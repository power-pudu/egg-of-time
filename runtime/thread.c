/* NitroSDK threads as host coroutines.
 *
 * The SDK switches threads by saving/restoring the whole register file (OS_SaveContext/OS_LoadContext,
 * and the IRQ-exit switch in ITCM). Recompiled code keeps its state in C stack frames, so each game
 * thread runs on its own coroutine with its own CPU struct, and those three routines are replaced
 * (tools/recomp/overrides.txt) by coroutine switches. A thread is found by the OSContext address it was
 * suspended under; a context nobody is suspended under is a new thread, started from the registers the
 * SDK wrote into it (OS_CreateThread).
 *
 * Save states describe each thread by its registers and the address it continues at (thread_resume_point);
 * a loaded thread starts a fresh coroutine that re-enters the generated code there. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define MINICORO_IMPL
#define MCO_NO_DEBUG
#include "third_party/minicoro.h"

#include "dispatch.h"
#include "rt.h"

/* Game addresses (YQUE). */
#define CP_SAVE_CONTEXT 0x02010d18u     /* divider/sqrt state, called by the original routines */
#define CP_RESTORE_CONTEXT 0x02010d58u
#define IRQ_THREAD_QUEUE 0x027e0060u    /* threads waiting for an IRQ (OS_WaitIrq) */
#define THREAD_INFO 0x02067b00u         /* OSThreadInfo: +0 reschedule flag, +4 current, +8 list, +0xC switch callback */
#define THREAD_STATE 0x64u              /* OSThread fields */
#define THREAD_LINK_PRIO 0x68u

#define THREAD_STACK_SIZE (8u << 20)
#define MAX_THREADS 64
#define HOST_RETURN 0xFFFF0000u         /* return address for runtime -> game calls */
#define RESUME_BASE 0xFFFF0004u         /* bottom of a thread resumed from a save state */

typedef struct RtThread {
    mco_coro *co;
    CPU cpu;
    uint32_t ctx;   /* OSContext this thread is suspended under; 0 while running */
    int dead;       /* switched away without saving its context (exited) */
    int boot;       /* runs boot_fn instead of a context's entry point */
    uint32_t start_pc, start_lr;
    /* While switched away: in OS_LoadContext (continues at susp_lr) or in the IRQ-exit switch. */
    uint32_t susp_lr;
    int susp_irq;
    uint32_t resume_pc; /* loaded from a save state: re-enter here (0 = start normally) */
    int started;        /* its coroutine has run */
} RtThread;

CPU *rt_cur_cpu;
static RtThread *threads[MAX_THREADS];
static RtThread *cur, *next;
static void (*boot_fn)(CPU *cpu);
static int restart; /* RESTART_STATE: rebuild every thread from the loaded state; RESTART_BOOT: power on again */
enum { RESTART_STATE = 1, RESTART_BOOT };

uint32_t rt_call_game(CPU *cpu, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2) {
    uint32_t lr = R[14];
    R[0] = a0;
    R[1] = a1;
    R[2] = a2;
    RT_CALL_ADDR(addr, HOST_RETURN);
    R[14] = lr;
    return R[0];
}

static void thread_body(RtThread *t) {
    CPU *cpu = &t->cpu;
    rt_ret_push(cpu, HOST_RETURN);
    RT_CALL_ADDR(t->start_pc, t->start_lr);
    /* The entry function returned to its lr (OS_ExitThread). */
    RT_CALL_ADDR(t->start_lr, HOST_RETURN);
    rt_fatal(cpu, "game thread returned from its exit routine 0x%08x", t->start_lr);
}

/* A thread loaded from a save state. Returns past the re-entered function reach its emulated callers through
 * rt_dispatch, which re-enters each one at its return address. */
static void resumed_body(RtThread *t) {
    CPU *cpu = &t->cpu;
    rt_ret_push(cpu, RESUME_BASE);
    if (!rt_dispatch(cpu, t->resume_pc)) rt_fatal(cpu, "save state: no code at 0x%08x", t->resume_pc);
    rt_fatal(cpu, "game thread resumed at 0x%08x returned to the runtime", t->resume_pc);
}

static void coro_entry(mco_coro *co) {
    RtThread *t = mco_get_user_data(co);
    t->started = 1;
    if (t->resume_pc) resumed_body(t);
    else if (t->boot) boot_fn(&t->cpu);
    else thread_body(t);
}

static RtThread *thread_new(void) {
    int slot = 0;
    while (slot < MAX_THREADS && threads[slot]) slot++;
    if (slot == MAX_THREADS) rt_fatal(rt_cur_cpu, "too many game threads");
    RtThread *t = calloc(1, sizeof *t);
    mco_desc desc = mco_desc_init(coro_entry, THREAD_STACK_SIZE);
    desc.user_data = t;
    if (!t || mco_create(&t->co, &desc) != MCO_SUCCESS) rt_fatal(rt_cur_cpu, "cannot create coroutine");
    threads[slot] = t;
    return t;
}

static void thread_free(RtThread *t) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i] == t) threads[i] = NULL;
    }
    mco_destroy(t->co);
    free(t);
}

/* Thread suspended under ctx, or a new one started from the registers stored in ctx. */
static RtThread *thread_for_ctx(CPU *from, uint32_t ctx) {
    for (int i = 0; i < MAX_THREADS; i++) {
        RtThread *t = threads[i];
        if (t && t != cur && !t->dead && t->ctx == ctx) return t;
    }
    RtThread *t = thread_new();
    CPU *c = &t->cpu;
    *c = *from; /* inherit banked registers (IRQ/SVC stacks) */
    c->ret_depth = 0;
    /* What OS_LoadContext does: SVC sp from the context, then the thread's mode, user-bank registers,
     * and a return to pc - 4. */
    uint32_t cpsr = rt_r32(ctx);
    rt_switch_mode(c, 0x13);
    c->cpsr_ctl = (c->cpsr_ctl & ~0x1Fu) | 0x13u;
    c->r[13] = rt_r32(ctx + 0x44);
    rt_switch_mode(c, cpsr & 0x1F);
    c->cpsr_ctl = cpsr & 0xFF;
    c->n = cpsr >> 31; c->z = (cpsr >> 30) & 1; c->c = (cpsr >> 29) & 1; c->v = (cpsr >> 28) & 1; c->q = (cpsr >> 27) & 1;
    for (int i = 0; i < 15; i++) c->r[i] = rt_r32(ctx + 4 + 4 * i);
    t->start_pc = (rt_r32(ctx + 0x40) - 4) | ((cpsr >> 5) & 1);
    t->start_lr = c->r[14];
    return t;
}

static void switch_to(RtThread *t) {
    next = t;
    mco_yield(cur->co);
}

/* ---- save states ---- */

typedef struct ThreadState {
    uint32_t used, running, ctx, pc;
    uint8_t cpu[offsetof(CPU, ret_stack)]; /* registers; the host call chain is not saved */
} ThreadState;
static ThreadState tstate[MAX_THREADS];

/* A thread's registers and the address it continues at, without its host call chain. */
static int thread_resume_point(const RtThread *t, CPU *cpu, uint32_t *pc) {
    memcpy(cpu, &t->cpu, offsetof(CPU, ret_stack));
    if (t->resume_pc && !t->started) {
        *pc = t->resume_pc; /* loaded from a state and not run since */
    } else if (t == cur) {
        if (cpu->irq_depth) return 0;
        *pc = cpu->poll_pc; /* the poll that is saving */
    } else if (!t->susp_irq) {
        if (cpu->irq_depth) return 0;
        *pc = t->susp_lr; /* return from OS_LoadContext */
    } else {
        /* Switched away at the end of an interrupt: finish the BIOS IRQ return here. */
        if (cpu->irq_depth != 1) return 0;
        const RtIrqFrame *f = &cpu->irq_frame[0];
        R[13] = f->irq_sp;
        for (int i = 0; i < 4; i++) R[i] = f->r[i];
        R[12] = f->r12;
        rt_exception_return(cpu, 0);
        cpu->irq_depth = 0;
        *pc = f->pc;
    }
    cpu->resume_pc = cpu->poll_pc = 0;
    return *pc != 0;
}

int rt_threads_can_save(void) {
    static CPU tmp;
    uint32_t pc;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i] && !threads[i]->dead && !thread_resume_point(threads[i], &tmp, &pc)) return 0;
    }
    return 1;
}

void threads_state(RtStateIO *s) {
    if (rt_state_saving(s)) {
        static CPU tmp;
        memset(tstate, 0, sizeof tstate);
        for (int i = 0; i < MAX_THREADS; i++) {
            RtThread *t = threads[i];
            if (!t || t->dead) continue;
            ThreadState *ts = &tstate[i];
            ts->used = 1;
            ts->running = t == cur;
            ts->ctx = t->ctx;
            thread_resume_point(t, &tmp, &ts->pc);
            memcpy(ts->cpu, &tmp, sizeof ts->cpu);
        }
    }
    RT_STATE(s, tstate);
}

_Noreturn void rt_threads_restart(int power_on) {
    restart = power_on ? RESTART_BOOT : RESTART_STATE;
    next = NULL;
    mco_yield(cur->co);
    rt_fatal(NULL, "dropped game thread was resumed");
}

static void threads_drop(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        if (threads[i]) thread_free(threads[i]);
    }
    cur = NULL;
}

/* A reboot: memory as at power-on and a new boot thread (rt_boot sets the hardware up again). */
static RtThread *threads_power_on(void) {
    threads_drop();
    memory_power_on();
    cpu_power_on();
    RtThread *t = thread_new();
    t->boot = 1;
    return t;
}

/* Replaces every thread with the ones in the loaded state; returns the one that was running. */
static RtThread *threads_from_state(void) {
    threads_drop();
    rt_state_apply_requested();
    RtThread *run = NULL;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!tstate[i].used) continue;
        RtThread *t = thread_new();
        memset(&t->cpu, 0, sizeof t->cpu);
        memcpy(&t->cpu, tstate[i].cpu, sizeof tstate[i].cpu);
        t->ctx = tstate[i].ctx;
        t->resume_pc = tstate[i].pc;
        if (tstate[i].running) run = t;
    }
    if (!run) rt_fatal(NULL, "save state has no running thread");
    return run;
}

void rt_threads_run(void (*boot)(CPU *cpu)) {
    boot_fn = boot;
    next = thread_new(); /* the boot thread, which becomes the SDK's main thread */
    next->boot = 1;
    while (next) {
        RtThread *prev = cur;
        cur = next;
        next = NULL;
        rt_cur_cpu = &cur->cpu;
        if (prev && prev->dead) thread_free(prev);
        mco_resume(cur->co);
        if (restart) {
            next = restart == RESTART_BOOT ? threads_power_on() : threads_from_state();
            restart = 0;
            continue;
        }
        if (mco_status(cur->co) == MCO_DEAD) break;
        if (!next) rt_fatal(&cur->cpu, "game thread suspended with nothing to run");
    }
}

void rt_threads_log(void) {
    for (int i = 0; i < MAX_THREADS; i++) {
        RtThread *t = threads[i];
        if (!t) continue;
        CPU *c = &t->cpu;
        char buf[200];
        int n = 0;
        for (uint32_t k = c->ret_depth; k > 0 && c->ret_depth - k < 10 && n < (int)sizeof buf - 12; k--) {
            n += snprintf(buf + n, sizeof buf - (size_t)n, " %08x", c->ret_stack[k - 1]);
        }
        rt_log("thread %d%s ctx %08x cpsr %08x:%s", i, t == cur ? " (running)" : "", t->ctx, rt_get_cpsr(c),
               n ? buf : " -");
    }
}

/* ---- overridden SDK routines ---- */

void rt_os_save_context(CPU *cpu) {
    uint32_t ctx = R[0];
    rt_call_game(cpu, CP_SAVE_CONTEXT, ctx + 0x48, 0, 0);
    /* Keep the SDK-visible context plausible (stack checks read sp from it). */
    rt_w32(ctx, rt_get_cpsr(cpu));
    rt_w32(ctx + 4, 1);
    for (int i = 1; i < 15; i++) rt_w32(ctx + 4 + 4 * i, R[i]);
    cur->ctx = ctx;
    R[0] = 0;
}

void rt_os_load_context(CPU *cpu) {
    uint32_t ctx = R[0];
    rt_call_game(cpu, CP_RESTORE_CONTEXT, ctx + 0x48, 0, 0);
    RtThread *self = cur, *t = thread_for_ctx(cpu, ctx);
    if (!self->ctx) self->dead = 1;
    self->susp_lr = R[14];
    self->susp_irq = 0;
    switch_to(t);
    self->ctx = 0;
    /* Resumed: the caller (OS_RescheduleThread) returns as if OS_SaveContext had returned 1. */
}

void rt_os_irq_thread_switch(CPU *cpu) {
    /* Wake the threads waiting for an interrupt. */
    uint32_t q = rt_r32(IRQ_THREAD_QUEUE);
    if (q) {
        while (q) {
            rt_w32(q + THREAD_STATE, 1);
            rt_w32(q + 0x78, 0);
            rt_w32(q + 0x7C, 0);
            uint32_t nx = rt_r32(q + 0x80);
            rt_w32(q + 0x80, 0);
            q = nx;
        }
        rt_w32(IRQ_THREAD_QUEUE, 0);
        rt_w32(IRQ_THREAD_QUEUE + 4, 0);
        rt_w16(THREAD_INFO, 1);
    }
    if (rt_r16(THREAD_INFO)) {
        rt_w16(THREAD_INFO, 0);
        uint32_t t = rt_r32(THREAD_INFO + 8);
        while (t && rt_r16(t + THREAD_STATE) != 1) t = rt_r32(t + THREAD_LINK_PRIO);
        uint32_t old = rt_r32(THREAD_INFO + 4);
        if (t && t != old) {
            uint32_t cb = rt_r32(THREAD_INFO + 0xC);
            if (cb) rt_call_game(cpu, cb, old, t, 0);
            rt_w32(THREAD_INFO + 4, t);
            rt_call_game(cpu, CP_SAVE_CONTEXT, old + 0x48, 0, 0);
            rt_call_game(cpu, CP_RESTORE_CONTEXT, t + 0x48, 0, 0);
            RtThread *self = cur, *target = thread_for_ctx(cpu, t); /* OSContext is at offset 0 of OSThread */
            self->ctx = old;
            self->susp_irq = 1;
            switch_to(target);
            self->ctx = 0;
        }
    }
    R[13] += 4; /* pop {pc}: the lr OS_IrqHandler pushed; returning ends the IRQ */
}
