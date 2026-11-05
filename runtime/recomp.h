/* Interface between generated code (gen/) and the runtime. */
#pragma once

#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "EggOfTime assumes a little-endian host"
#endif

/* The game code (gen/) is a library of its own, built on the player's machine from their ROM (runtime/game_build.c):
 * it imports the runtime functions and data marked RT_API from the executable, which exports them. Bump
 * RT_API_VERSION whenever this interface changes, so a game library built for another runtime is rebuilt. */
#if defined(RT_GAME_LIB) && defined(_WIN32)
#define RT_API __declspec(dllimport)
#else
#define RT_API
#endif
#define RT_API_VERSION 2

enum { RT_RET_STACK = 1024, RT_IRQ_NEST = 4 };

/* Interrupted state that rt_enter_irq would restore, kept where save states can read it. */
typedef struct RtIrqFrame { uint32_t pc, r[4], r12, irq_sp; } RtIrqFrame;

typedef struct CPU {
    uint32_t r[16];
    uint32_t n, z, c, v, q; /* CPSR flags, one word each (0/1) */
    uint32_t cpsr_ctl;      /* CPSR bits 0..7: mode, T, F, I */
    uint32_t spsr;          /* SPSR of the current mode */
    /* Banked registers, indexed by rt_bank_index(mode). */
    uint32_t bank_r13[6], bank_r14[6], bank_spsr[6];
    uint32_t fiq_r8_12[5], usr_r8_12[5];
    /* Save states: where the running code can be re-entered, and pending IRQ returns. */
    uint32_t resume_pc;     /* set before calling a function: start at this address instead of its entry */
    uint32_t poll_pc;       /* address the current poll or wait resumes at (0 = unknown) */
    uint32_t irq_depth;
    RtIrqFrame irq_frame[RT_IRQ_NEST];
    /* Expected return addresses of the C call chain (see rt_jump). */
    uint32_t ret_stack[RT_RET_STACK];
    void *unwind[RT_RET_STACK]; /* jmp_buf of call sites that can be returned to from deeper frames */
    uint32_t ret_depth;
} CPU;

#define R (cpu->r)

/* ---- conditions ---- */
#define COND_EQ (cpu->z)
#define COND_NE (!cpu->z)
#define COND_CS (cpu->c)
#define COND_CC (!cpu->c)
#define COND_MI (cpu->n)
#define COND_PL (!cpu->n)
#define COND_VS (cpu->v)
#define COND_VC (!cpu->v)
#define COND_HI (cpu->c && !cpu->z)
#define COND_LS (!cpu->c || cpu->z)
#define COND_GE (cpu->n == cpu->v)
#define COND_LT (cpu->n != cpu->v)
#define COND_GT (!cpu->z && cpu->n == cpu->v)
#define COND_LE (cpu->z || cpu->n != cpu->v)

/* ---- ALU ---- */
static inline uint32_t rt_logic_s(CPU *cpu, uint32_t r) {
    cpu->n = r >> 31;
    cpu->z = r == 0;
    return r;
}
static inline uint32_t rt_add_s(CPU *cpu, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    cpu->n = r >> 31; cpu->z = r == 0; cpu->c = r < a; cpu->v = (~(a ^ b) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t rt_adc_s(CPU *cpu, uint32_t a, uint32_t b) {
    uint64_t w = (uint64_t)a + b + cpu->c;
    uint32_t r = (uint32_t)w;
    cpu->n = r >> 31; cpu->z = r == 0; cpu->c = (uint32_t)(w >> 32); cpu->v = (~(a ^ b) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t rt_sub_s(CPU *cpu, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    cpu->n = r >> 31; cpu->z = r == 0; cpu->c = a >= b; cpu->v = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}
static inline uint32_t rt_sbc_s(CPU *cpu, uint32_t a, uint32_t b) {
    uint32_t borrow = cpu->c ^ 1;
    uint32_t r = a - b - borrow;
    cpu->n = r >> 31; cpu->z = r == 0; cpu->c = (uint64_t)a >= (uint64_t)b + borrow;
    cpu->v = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}

static inline uint32_t rt_ror(uint32_t v, uint32_t n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
static inline uint32_t rt_clz(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }

/* Register-specified shifts (amount 0..255), without and with carry-out. */
static inline uint32_t rt_lsl_r(uint32_t v, uint32_t a) { return a >= 32 ? 0 : v << a; }
static inline uint32_t rt_lsr_r(uint32_t v, uint32_t a) { return a >= 32 ? 0 : v >> a; }
static inline uint32_t rt_asr_r(uint32_t v, uint32_t a) { return (uint32_t)((int32_t)v >> (a >= 32 ? 31 : a)); }
static inline uint32_t rt_ror_r(uint32_t v, uint32_t a) { return rt_ror(v, a); }
static inline uint32_t rt_lsl_rc(uint32_t v, uint32_t a, uint32_t *c) {
    if (a == 0) return v;
    if (a < 32) { *c = (v >> (32 - a)) & 1; return v << a; }
    *c = a == 32 ? (v & 1) : 0;
    return 0;
}
static inline uint32_t rt_lsr_rc(uint32_t v, uint32_t a, uint32_t *c) {
    if (a == 0) return v;
    if (a < 32) { *c = (v >> (a - 1)) & 1; return v >> a; }
    *c = a == 32 ? (v >> 31) : 0;
    return 0;
}
static inline uint32_t rt_asr_rc(uint32_t v, uint32_t a, uint32_t *c) {
    if (a == 0) return v;
    if (a < 32) { *c = (v >> (a - 1)) & 1; return (uint32_t)((int32_t)v >> a); }
    *c = v >> 31;
    return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t rt_ror_rc(uint32_t v, uint32_t a, uint32_t *c) {
    if (a == 0) return v;
    a &= 31;
    if (a == 0) { *c = v >> 31; return v; }
    *c = (v >> (a - 1)) & 1;
    return rt_ror(v, a);
}

/* Saturating ops (ARMv5TE). */
static inline uint32_t rt_qadd(CPU *cpu, uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a + (int32_t)b;
    if (r > INT32_MAX) { cpu->q = 1; return INT32_MAX; }
    if (r < INT32_MIN) { cpu->q = 1; return (uint32_t)INT32_MIN; }
    return (uint32_t)r;
}
static inline uint32_t rt_qsub(CPU *cpu, uint32_t a, uint32_t b) {
    int64_t r = (int64_t)(int32_t)a - (int32_t)b;
    if (r > INT32_MAX) { cpu->q = 1; return INT32_MAX; }
    if (r < INT32_MIN) { cpu->q = 1; return (uint32_t)INT32_MIN; }
    return (uint32_t)r;
}
/* Wrapping add that sets Q on signed overflow (SMLAxy / SMLAWy). */
static inline uint32_t rt_qadd_flag(CPU *cpu, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    if ((~(a ^ b) & (a ^ r)) >> 31) cpu->q = 1;
    return r;
}

/* ---- memory ---- */
#define RT_MAIN_RAM_SIZE 0x400000u
#define RT_ITCM_SIZE 0x8000u
#define RT_DTCM_SIZE 0x4000u
extern RT_API uint8_t rt_main_ram[RT_MAIN_RAM_SIZE];
extern RT_API uint8_t rt_dtcm[RT_DTCM_SIZE];
extern RT_API uint32_t rt_dtcm_base;

RT_API uint32_t rt_r32_slow(uint32_t a);
RT_API uint32_t rt_r16_slow(uint32_t a);
RT_API uint32_t rt_r8_slow(uint32_t a);
RT_API void rt_w32_slow(uint32_t a, uint32_t v);
RT_API void rt_w16_slow(uint32_t a, uint32_t v);
RT_API void rt_w8_slow(uint32_t a, uint32_t v);

/* Debug write watch (EGG_WATCH_WRITE=<addr>): logs CPU stores to one word. Disabled = ~0u, never matches. */
extern RT_API uint32_t rt_watch_word;
RT_API void rt_watch_hit(uint32_t a, uint32_t v, int size);
#define RT_WATCH(a, v, n) do { if (__builtin_expect(((a) & ~3u) == rt_watch_word, 0)) rt_watch_hit(a, v, n); } while (0)

/* Main RAM fast path (0x02000000-0x02FFFFFF mirrors 4 MB), excluding the DTCM window inside it. */
#define RT_IS_MAIN(a) (((a) >> 24) == 2 && (uint32_t)((a) - rt_dtcm_base) >= RT_DTCM_SIZE)

static inline uint32_t rt_r32(uint32_t a) {
    a &= ~3u;
    if (RT_IS_MAIN(a)) { uint32_t v; memcpy(&v, &rt_main_ram[a & (RT_MAIN_RAM_SIZE - 1)], 4); return v; }
    return rt_r32_slow(a);
}
static inline uint32_t rt_ldr32(uint32_t a) { return rt_ror(rt_r32(a), (a & 3) * 8); }
static inline uint32_t rt_r16(uint32_t a) {
    a &= ~1u;
    if (RT_IS_MAIN(a)) { uint16_t v; memcpy(&v, &rt_main_ram[a & (RT_MAIN_RAM_SIZE - 1)], 2); return v; }
    return rt_r16_slow(a);
}
static inline uint32_t rt_ldrsh(uint32_t a) { return (uint32_t)(int32_t)(int16_t)rt_r16(a); }
static inline uint32_t rt_r8(uint32_t a) {
    if (RT_IS_MAIN(a)) return rt_main_ram[a & (RT_MAIN_RAM_SIZE - 1)];
    return rt_r8_slow(a);
}
static inline void rt_w32(uint32_t a, uint32_t v) {
    a &= ~3u;
    RT_WATCH(a, v, 4);
    if (RT_IS_MAIN(a)) { memcpy(&rt_main_ram[a & (RT_MAIN_RAM_SIZE - 1)], &v, 4); return; }
    rt_w32_slow(a, v);
}
static inline void rt_w16(uint32_t a, uint32_t v) {
    a &= ~1u;
    RT_WATCH(a, v, 2);
    if (RT_IS_MAIN(a)) { uint16_t h = (uint16_t)v; memcpy(&rt_main_ram[a & (RT_MAIN_RAM_SIZE - 1)], &h, 2); return; }
    rt_w16_slow(a, v);
}
static inline void rt_w8(uint32_t a, uint32_t v) {
    RT_WATCH(a, v, 1);
    if (RT_IS_MAIN(a)) { rt_main_ram[a & (RT_MAIN_RAM_SIZE - 1)] = (uint8_t)v; return; }
    rt_w8_slow(a, v);
}

/* ---- system ---- */
RT_API void rt_swi(CPU *cpu, uint32_t n);
RT_API uint32_t rt_mrs(CPU *cpu, int spsr);
RT_API void rt_msr(CPU *cpu, int spsr, uint32_t fields, uint32_t v);
RT_API void rt_exception_return(CPU *cpu, uint32_t target);
RT_API void rt_mcr(CPU *cpu, int cp, int opc1, int crn, int crm, int opc2, uint32_t v);
RT_API uint32_t rt_mrc(CPU *cpu, int cp, int opc1, int crn, int crm, int opc2);
RT_API void rt_mrc_flags(CPU *cpu, int cp, int opc1, int crn, int crm, int opc2);
RT_API void rt_bkpt(CPU *cpu, uint32_t addr);
RT_API void rt_unimpl(CPU *cpu, uint32_t addr, uint32_t ins);

/* ---- control flow ---- */
extern RT_API volatile int rt_poll_pending;
RT_API void rt_poll(CPU *cpu);
#define RT_POLL(cpu) do { if (rt_poll_pending) rt_poll(cpu); } while (0)
/* Poll at a loop back-edge or after enabling interrupts; pc is where execution continues. */
#define RT_POLL_AT(cpu, pc) do { if (rt_poll_pending) { uint32_t sv_ = cpu->poll_pc; cpu->poll_pc = (pc);     rt_poll(cpu); cpu->poll_pc = sv_; } } while (0)
/* SWI / cp15 write that may wait for an interrupt: a save state taken while waiting re-executes it. */
#define RT_SWI(cpu, n, pc) do { uint32_t sv_ = cpu->poll_pc; cpu->poll_pc = (pc); rt_swi(cpu, (n)); cpu->poll_pc = sv_; } while (0)
#define RT_MCR(cpu, cp, o1, crn, crm, o2, v, pc) do { uint32_t sv_ = cpu->poll_pc; cpu->poll_pc = (pc);     rt_mcr(cpu, cp, o1, crn, crm, o2, v); cpu->poll_pc = sv_; } while (0)
/* A resume address that is not a label of the function it falls in. */
RT_API void rt_resume_failed(CPU *cpu, uint32_t target);

RT_API void rt_ret_overflow(CPU *cpu);
static inline void rt_ret_push(CPU *cpu, uint32_t ret) {
    if (cpu->ret_depth == RT_RET_STACK) rt_ret_overflow(cpu);
    cpu->unwind[cpu->ret_depth] = 0;
    cpu->ret_stack[cpu->ret_depth++] = ret;
}
static inline void rt_ret_pop(CPU *cpu) { cpu->ret_depth--; }

#define RT_CALL(fn, ret) do { R[14] = (ret); rt_ret_push(cpu, (ret)); fn(cpu); rt_ret_pop(cpu); } while (0)
/* Call through an address (register calls, ambiguous overlay targets). */
RT_API void rt_call_addr(CPU *cpu, uint32_t target);
#define RT_CALL_ADDR(target, ret) do { uint32_t t_ = (target); R[14] = (ret); rt_ret_push(cpu, (ret)); \
    rt_call_addr(cpu, t_); rt_ret_pop(cpu); } while (0)
/* Call into a module translated as one function (code copied at run time), at any instruction. */
#define RT_CALL_ANY(fn, target, ret) do { R[14] = (ret); rt_ret_push(cpu, (ret)); fn(cpu, (target));     rt_ret_pop(cpu); } while (0)
/* Calls from hand-written code (whole modules) record an unwind point: that code may return several
 * frames up at once, which rt_return_mismatch turns into a longjmp to the matching call site. The setjmp
 * lives in this helper, not in the (huge) calling function, whose stack frame compilers would otherwise
 * not compact (gcc: about 9 KB per frame); this code nests deeply through those returns. Calls fn, else
 * any(cpu, target), else the code at target. */
RT_API void rt_call_unwindable(CPU *cpu, uint32_t ret, void (*fn)(CPU *), void (*any)(CPU *, uint32_t), uint32_t target);
/* Tail jump through an address. Jumping to the expected return address is a return. */
RT_API void rt_jump(CPU *cpu, uint32_t target);

/* Function return (bx lr, pop {pc}, ...). Code that returns somewhere other than its caller
 * (e.g. NitroSDK sets lr to the IRQ thread-switch routine before calling an IRQ handler) jumps there. */
RT_API void rt_return_mismatch(CPU *cpu, uint32_t target);
#define RT_RETURN(target) do { uint32_t rr_ = (target); \
    if ((rr_ ^ cpu->ret_stack[cpu->ret_depth - 1]) & ~1u) rt_return_mismatch(cpu, rr_); \
    return; } while (0)
