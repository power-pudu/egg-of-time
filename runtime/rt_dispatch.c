#include <SDL3/SDL.h>

#include "dispatch.h"
#include "rt.h"

const RtGame *rt_game;

int rt_game_load(const char *path, const char **error) {
    SDL_SharedObject *lib = SDL_LoadObject(path);
    if (!lib) {
        *error = SDL_GetError();
        return 0;
    }
    const RtGame *g = (const RtGame *)(void *)SDL_LoadFunction(lib, RT_GAME_SYMBOL);
    if (!g || g->api_version != RT_API_VERSION) {
        *error = g ? "the game library was built for another version of the runtime" : "not a game library";
        SDL_UnloadObject(lib);
        return 0;
    }
    if (strcmp(g->game_code, rt_rom.code) != 0) {
        *error = "the game library was built from another version of the game than this ROM";
        SDL_UnloadObject(lib);
        return 0;
    }
    rt_game = g; /* stays loaded */
    return 1;
}

void rt_call_wrapped(CPU *cpu, const char *module, uint32_t addr) {
    for (unsigned i = 0; i < rt_game->wrapped_count; i++) {
        const RtWrapped *w = &rt_game->wrapped[i];
        if (w->addr == addr && !strcmp(w->module, module)) {
            w->fn(cpu);
            return;
        }
    }
    rt_fatal(cpu, "no recompiled original for %s 0x%08x", module, addr);
}

static int code_matches(uint32_t addr, const RtFuncEntry *e) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; i < e->sig_len; i++) h = (h ^ rt_r8(addr + i)) * 0x01000193u;
    return h == e->sig;
}

/* Index of the last entry with addr <= target, or -1. */
static int find_at_or_before(const RtModule *m, uint32_t addr) {
    unsigned lo = 0, hi = m->count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (m->funcs[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    return (int)lo - 1;
}

int rt_overlay_resident(int id) {
    for (unsigned i = 0; i < rt_game->module_count; i++) {
        const RtModule *m = &rt_game->modules[i];
        if (m->overlay_id == id) return m->count && code_matches(m->funcs[0].addr, &m->funcs[0]);
    }
    return 0;
}

int rt_dispatch(CPU *cpu, uint32_t addr) {
    addr &= ~1u;
    const RtFuncEntry *inside = NULL;
    for (unsigned i = 0; i < rt_game->module_count; i++) {
        const RtModule *m = &rt_game->modules[i];
        if (addr < m->base || addr >= m->end) continue;
        int k = find_at_or_before(m, addr);
        if (k < 0) continue;
        const RtFuncEntry *e = &m->funcs[k];
        /* Overlays share address ranges: only the one whose code is in RAM matches. */
        if (m->overlay_id >= 0 && !code_matches(e->addr, e)) continue;
        if (e->addr == addr) {
            e->fn(cpu);
            return 1;
        }
        if (m->any) {
            m->any(cpu, addr);
            return 1;
        }
        if (!inside) inside = e;
    }
    if (inside) {
        /* Inside a function (a thread resumed from a save state returning to its callers): re-enter there. */
        cpu->resume_pc = addr;
        inside->fn(cpu);
        return 1;
    }
    return 0;
}

void rt_call_unwindable(CPU *cpu, uint32_t ret, void (*fn)(CPU *), void (*any)(CPU *, uint32_t), uint32_t target) {
    jmp_buf jb;
    R[14] = ret;
    rt_ret_push(cpu, ret);
    cpu->unwind[cpu->ret_depth - 1] = &jb;
    if (!setjmp(jb)) {
        if (fn) fn(cpu);
        else if (any) any(cpu, target);
        else rt_call_addr(cpu, target);
    }
    rt_ret_pop(cpu); /* after a longjmp here, rt_return_mismatch has cut ret_depth back to this call */
}

void rt_resume_failed(CPU *cpu, uint32_t target) {
    rt_fatal(cpu, "cannot re-enter code at 0x%08x (not a resume point of its function)", target);
}

/* The code at addr runs in a module translated as one function (rt_dispatch's choice). */
static int runs_whole_module(uint32_t addr) {
    addr &= ~1u;
    for (unsigned i = 0; i < rt_game->module_count; i++) {
        const RtModule *m = &rt_game->modules[i];
        if (addr < m->base || addr >= m->end) continue;
        int k = find_at_or_before(m, addr);
        if (k < 0) continue;
        const RtFuncEntry *e = &m->funcs[k];
        if (m->overlay_id >= 0 && !code_matches(e->addr, e)) continue;
        if (e->addr == addr) return m->any != NULL;
        if (m->any) return 1;
    }
    return 0;
}

/* Out of line, so that only these calls have a jmp_buf in their frame. */
static __attribute__((noinline)) void call_unwindable_here(CPU *cpu, uint32_t target) {
    jmp_buf jb;
    cpu->unwind[cpu->ret_depth - 1] = &jb;
    if (!setjmp(jb)) rt_call_addr(cpu, target);
    cpu->unwind[cpu->ret_depth - 1] = NULL; /* after a longjmp here, rt_return_mismatch has cut ret_depth back to this call */
}

void rt_call_addr(CPU *cpu, uint32_t target) {
    /* Hand-written code (whole modules) may return several frames up at once, as far as its caller: the Mobiclip
     * decoder's bit reader returns with `mov pc, r6`, a jump within the module that leaves its C frame, and the
     * decoder's last return then skips those frames. The call into that code needs an unwind point too (the
     * in-game movies call the decoder from ov008 0x02122358, code dsd analysed: without one, a fatal error). */
    if (cpu->ret_depth && !cpu->unwind[cpu->ret_depth - 1] && runs_whole_module(target)) {
        call_unwindable_here(cpu, target);
        return;
    }
    if (!rt_dispatch(cpu, target)) rt_fatal(cpu, "call to unknown code address 0x%08x", target);
}

void rt_jump(CPU *cpu, uint32_t target) {
    if (cpu->ret_depth && ((target ^ cpu->ret_stack[cpu->ret_depth - 1]) & ~1u) == 0) {
        return; /* e.g. `pop {r3}; bx r3`: a return to the caller */
    }
    if (!rt_dispatch(cpu, target)) rt_fatal(cpu, "jump to unknown code address 0x%08x", target);
}

void rt_return_mismatch(CPU *cpu, uint32_t target) {
    for (uint32_t i = cpu->ret_depth - 1; i-- > 0;) {
        if (((target ^ cpu->ret_stack[i]) & ~1u) == 0) {
            if (!cpu->unwind[i]) {
                rt_fatal(cpu, "return to 0x%08x unwinds %u C frames from a call site without an unwind point",
                         target, cpu->ret_depth - 1 - i);
            }
            cpu->ret_depth = i + 1;
            longjmp(*(jmp_buf *)cpu->unwind[i], 1);
        }
    }
    if (!rt_dispatch(cpu, target)) rt_fatal(cpu, "return to unknown code address 0x%08x", target);
}

void rt_ret_overflow(CPU *cpu) {
    rt_fatal(cpu, "call depth exceeds %d", RT_RET_STACK);
}

void rt_bad_alloc(CPU *cpu) {
    rt_fatal(cpu, "game ran out of heap memory: operator new(%u) failed (called from 0x%08x)", R[6],
             cpu->ret_depth > 1 ? cpu->ret_stack[cpu->ret_depth - 2] : 0);
}
