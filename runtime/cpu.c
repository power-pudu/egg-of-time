/* CPU modes, PSR access, coprocessor 15 and HLE BIOS calls. */
#include "rt.h"

enum { MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13, MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F };

volatile int rt_poll_pending;

static int bank_index(uint32_t mode) {
    switch (mode & 0x1F) {
    case MODE_FIQ: return 1;
    case MODE_IRQ: return 2;
    case MODE_SVC: return 3;
    case MODE_ABT: return 4;
    case MODE_UND: return 5;
    default: return 0; /* USR, SYS */
    }
}

void rt_switch_mode(CPU *cpu, uint32_t new_mode) {
    int from = bank_index(cpu->cpsr_ctl), to = bank_index(new_mode);
    if (from == to) return;
    cpu->bank_r13[from] = R[13];
    cpu->bank_r14[from] = R[14];
    cpu->bank_spsr[from] = cpu->spsr;
    if (from == 1 || to == 1) {
        uint32_t *save = from == 1 ? cpu->fiq_r8_12 : cpu->usr_r8_12;
        uint32_t *load = to == 1 ? cpu->fiq_r8_12 : cpu->usr_r8_12;
        memcpy(save, &R[8], sizeof cpu->fiq_r8_12);
        memcpy(&R[8], load, sizeof cpu->fiq_r8_12);
    }
    R[13] = cpu->bank_r13[to];
    R[14] = cpu->bank_r14[to];
    cpu->spsr = cpu->bank_spsr[to];
}

uint32_t rt_get_cpsr(CPU *cpu) {
    return cpu->n << 31 | cpu->z << 30 | cpu->c << 29 | cpu->v << 28 | cpu->q << 27 | cpu->cpsr_ctl;
}

static void set_cpsr(CPU *cpu, uint32_t v, int flags, int control) {
    if (flags) {
        cpu->n = v >> 31; cpu->z = (v >> 30) & 1; cpu->c = (v >> 29) & 1; cpu->v = (v >> 28) & 1; cpu->q = (v >> 27) & 1;
    }
    if (control && (cpu->cpsr_ctl & 0x1F) != MODE_USR) {
        rt_switch_mode(cpu, v & 0x1F);
        cpu->cpsr_ctl = (cpu->cpsr_ctl & 0x20) | (v & 0xDF); /* T is not writable by MSR */
        irq_changed();
    }
}

void rt_cpu_reset(CPU *cpu) {
    memset(cpu, 0, sizeof *cpu);
    cpu->cpsr_ctl = MODE_SYS | 0xC0; /* IRQ and FIQ disabled, as the firmware leaves it */
}

uint32_t rt_mrs(CPU *cpu, int spsr) {
    return spsr ? cpu->spsr : rt_get_cpsr(cpu);
}

void rt_msr(CPU *cpu, int spsr, uint32_t fields, uint32_t v) {
    if (spsr) {
        uint32_t mask = (fields & 8 ? 0xFF000000u : 0) | (fields & 1 ? 0xFFu : 0);
        cpu->spsr = (cpu->spsr & ~mask) | (v & mask);
        return;
    }
    set_cpsr(cpu, v, fields & 8, fields & 1);
}

void rt_exception_return(CPU *cpu, uint32_t target) {
    (void)target; /* execution continues in the C caller that delivered the exception */
    uint32_t spsr = cpu->spsr;
    rt_switch_mode(cpu, spsr & 0x1F);
    cpu->cpsr_ctl = spsr & 0xFF;
    set_cpsr(cpu, spsr, 1, 0);
}

/* ---- coprocessor 15 ---- */

static uint32_t cp15_control = 0x00012078u, cp15_dtcm = 0, cp15_itcm = 0;

void rt_mcr(CPU *cpu, int cp, int opc1, int crn, int crm, int opc2, uint32_t v) {
    if (cp != 15) rt_fatal(cpu, "mcr p%d", cp);
    (void)opc1;
    if (crn == 1 && crm == 0 && opc2 == 0) {
        cp15_control = v;
    } else if (crn == 9 && crm == 1 && opc2 == 0) {
        cp15_dtcm = v;
        rt_dtcm_base = v & 0xFFFFF000u;
    } else if (crn == 9 && crm == 1 && opc2 == 1) {
        cp15_itcm = v;
    } else if (crn == 7 && ((crm == 0 && opc2 == 4) || (crm == 8 && opc2 == 2))) {
        rt_halt(cpu); /* wait for interrupt */
    }
    /* Cache maintenance, protection unit and write buffer settings need no emulation. */
}

uint32_t rt_mrc(CPU *cpu, int cp, int opc1, int crn, int crm, int opc2) {
    if (cp != 15) rt_fatal(cpu, "mrc p%d", cp);
    (void)opc1;
    if (crn == 0) return opc2 == 1 ? 0x0F0D2112u : 0x41059461u; /* cache type, main ID (ARM946E-S) */
    if (crn == 1) return cp15_control;
    if (crn == 9 && crm == 1) return opc2 ? cp15_itcm : cp15_dtcm;
    return 0;
}

void rt_mrc_flags(CPU *cpu, int cp, int opc1, int crn, int crm, int opc2) {
    set_cpsr(cpu, rt_mrc(cpu, cp, opc1, crn, crm, opc2), 1, 0);
}

/* ---- HLE BIOS ---- */

static void cpu_set(uint32_t src, uint32_t dst, uint32_t ctl, int fast) {
    uint32_t count = ctl & 0x1FFFFF;
    int fill = (ctl >> 24) & 1, words = fast || ((ctl >> 26) & 1);
    if (fast) count = (count + 7) & ~7u;
    if (words) {
        uint32_t v = rt_r32(src);
        for (uint32_t i = 0; i < count; i++) {
            rt_w32(dst + i * 4, fill ? v : rt_r32(src + i * 4));
        }
    } else {
        uint32_t v = rt_r16(src);
        for (uint32_t i = 0; i < count; i++) {
            rt_w16(dst + i * 2, fill ? v : rt_r16(src + i * 2));
        }
    }
}

/* Decompression. The "ReadNormal" variants read memory and write 8 bits at a time; the DS-specific
 * "ReadByCallback" variants (SWI 12h/15h) fetch bytes through game callbacks and write 16 bits at a time.
 * Callback structure: +0 Open(src, dst, param) -> header, +4 Close(src), +8 Get8bit(src). */
typedef struct In { CPU *cpu; uint32_t src, cb; } In;
typedef struct Out { uint32_t dst, pos; int wide; uint32_t pend; } Out;

static uint32_t in_byte(In *in) {
    if (!in->cb) return rt_r8(in->src++);
    return rt_call_game(in->cpu, rt_r32(in->cb + 8), in->src++, 0, 0) & 0xFF;
}

/* Reads the 4-byte header; returns 0 if the BIOS would refuse the call. */
static int in_open(In *in, uint32_t dst, uint32_t param, uint32_t *hdr) {
    if ((in->src & 0x0E000000u) == 0) return 0; /* the BIOS refuses sources in the BIOS/TCM area */
    if (in->cb) {
        *hdr = rt_call_game(in->cpu, rt_r32(in->cb), in->src, dst, param);
        if ((int32_t)*hdr < 0) return 0;
    } else {
        *hdr = rt_r32(in->src);
    }
    in->src += 4;
    return 1;
}

static void in_close(In *in) {
    if (in->cb && rt_r32(in->cb + 4)) rt_call_game(in->cpu, rt_r32(in->cb + 4), in->src, 0, 0);
}

static void out_byte(Out *o, uint32_t b) {
    if (!o->wide) { rt_w8(o->dst + o->pos++, b); return; }
    if (o->pos & 1) rt_w16(o->dst + o->pos - 1, o->pend | (b << 8));
    else o->pend = b;
    o->pos++;
}
static uint32_t out_peek(Out *o, uint32_t back) {
    uint32_t p = o->pos - back;
    if (o->wide && p == o->pos - 1 && (o->pos & 1)) return o->pend;
    return rt_r8(o->dst + p);
}

static void lz77(CPU *cpu, uint32_t cb) {
    In in = {cpu, R[0], cb};
    uint32_t hdr;
    if (!in_open(&in, R[1], R[2], &hdr)) return;
    uint32_t size = hdr >> 8;
    Out o = {R[1], 0, cb != 0, 0};
    while (o.pos < size) {
        uint32_t flags = in_byte(&in);
        for (int i = 0; i < 8 && o.pos < size; i++, flags <<= 1) {
            if (flags & 0x80) {
                uint32_t b0 = in_byte(&in), b1 = in_byte(&in);
                uint32_t len = (b0 >> 4) + 3, disp = (((b0 & 0xF) << 8) | b1) + 1;
                while (len-- && o.pos < size) out_byte(&o, out_peek(&o, disp));
            } else {
                out_byte(&o, in_byte(&in));
            }
        }
    }
    in_close(&in);
}

static void rl(CPU *cpu, uint32_t cb) {
    In in = {cpu, R[0], cb};
    uint32_t hdr;
    if (!in_open(&in, R[1], R[2], &hdr)) return;
    uint32_t size = hdr >> 8;
    Out o = {R[1], 0, cb != 0, 0};
    while (o.pos < size) {
        uint32_t f = in_byte(&in);
        if (f & 0x80) {
            uint32_t len = (f & 0x7F) + 3, b = in_byte(&in);
            while (len-- && o.pos < size) out_byte(&o, b);
        } else {
            for (uint32_t len = (f & 0x7F) + 1; len-- && o.pos < size;) out_byte(&o, in_byte(&in));
        }
    }
    in_close(&in);
}

static uint32_t crc16(uint32_t crc, uint32_t ptr, uint32_t len) {
    static const uint16_t tbl[] = {0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001};
    for (uint32_t i = 0; i < len; i++) {
        crc ^= rt_r8(ptr + i);
        for (int j = 0; j < 8; j++) {
            int carry = crc & 1;
            crc >>= 1;
            if (carry) crc ^= (uint32_t)tbl[j] << (7 - j);
        }
    }
    return crc & 0xFFFF;
}

void rt_swi(CPU *cpu, uint32_t n) {
    switch (n) {
    case 0x03: /* WaitByLoop */
        break;
    case 0x04: /* IntrWait */
    case 0x05: { /* VBlankIntrWait */
        /* The BIOS waits with IRQs enabled in the CPU, whatever the caller's CPSR. */
        uint32_t ctl = cpu->cpsr_ctl;
        cpu->cpsr_ctl &= ~0x80u;
        if (n == 0x04) rt_intr_wait(cpu, R[0] != 0, R[1]);
        else rt_intr_wait(cpu, 1, IRQ_VBLANK);
        cpu->cpsr_ctl = (cpu->cpsr_ctl & ~0x80u) | (ctl & 0x80u);
        break;
    }
    case 0x06: /* Halt */
        rt_halt(cpu);
        break;
    case 0x09: { /* Div */
        int32_t num = (int32_t)R[0], den = (int32_t)R[1];
        if (den == 0) rt_fatal(cpu, "SWI Div by zero");
        int32_t q = (num == INT32_MIN && den == -1) ? INT32_MIN : num / den;
        R[0] = (uint32_t)q;
        R[1] = (uint32_t)(num - q * den);
        R[3] = (uint32_t)(q < 0 ? -q : q);
        break;
    }
    case 0x0B: cpu_set(R[0], R[1], R[2], 0); break;
    case 0x0C: cpu_set(R[0], R[1], R[2], 1); break;
    case 0x0D: { /* Sqrt */
        uint32_t v = R[0], r = 0, bit = 1u << 30;
        while (bit > v) bit >>= 2;
        while (bit) {
            if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
            else r >>= 1;
            bit >>= 2;
        }
        R[0] = r;
        break;
    }
    case 0x0E: R[0] = crc16(R[0], R[1], R[2]); break;
    case 0x0F: R[0] = 0; break; /* IsDebugger */
    case 0x11: lz77(cpu, 0); break;
    case 0x12: lz77(cpu, R[3]); break;
    case 0x14: rl(cpu, 0); break;
    case 0x15: rl(cpu, R[3]); break;
    default: rt_fatal(cpu, "unimplemented SWI 0x%02x (lr=0x%08x)", n, R[14]);
    }
}

/* BIOS IRQ vector: push r0-r3, r12, lr on the IRQ stack, call the handler at DTCM+0x3FFC, restore. */
#define BIOS_IRQ_RETURN 0xFFFF0290u

void rt_enter_irq(CPU *cpu) {
    uint32_t cpsr = rt_get_cpsr(cpu);
    uint32_t saved[5] = {R[0], R[1], R[2], R[3], R[12]};
    rt_switch_mode(cpu, MODE_IRQ);
    cpu->spsr = cpsr;
    cpu->cpsr_ctl = (cpsr & 0x40) | 0x80 | MODE_IRQ;
    uint32_t irq_sp = R[13];
    R[14] = 0; /* interrupted PC is not observable in recompiled code */
    R[13] = irq_sp - 24;
    for (int i = 0; i < 4; i++) rt_w32(R[13] + 4 * i, saved[i]);
    rt_w32(R[13] + 16, saved[4]);
    rt_w32(R[13] + 20, R[14]);
    uint32_t handler = rt_r32(rt_dtcm_base + 0x3FFCu);
    if (cpu->irq_depth < RT_IRQ_NEST) {
        cpu->irq_frame[cpu->irq_depth] = (RtIrqFrame){cpu->poll_pc, {saved[0], saved[1], saved[2], saved[3]}, saved[4], irq_sp};
    }
    cpu->irq_depth++;
    R[0] = 0x04000000u;
    RT_CALL_ADDR(handler, BIOS_IRQ_RETURN);
    cpu->irq_depth--;
    /* Registers come back from C locals, not the IRQ stack: a game thread switch inside the handler
     * may let other threads reuse that stack area before this one resumes. */
    R[13] = irq_sp;
    for (int i = 0; i < 4; i++) R[i] = saved[i];
    R[12] = saved[4];
    rt_exception_return(cpu, 0);
}

void rt_bkpt(CPU *cpu, uint32_t addr) {
    rt_fatal(cpu, "bkpt at 0x%08x", addr);
}

void rt_unimpl(CPU *cpu, uint32_t addr, uint32_t ins) {
    rt_fatal(cpu, "unimplemented instruction 0x%08x at 0x%08x", ins, addr);
}

void cpu_power_on(void) {
    cp15_control = 0x00012078u;
    cp15_dtcm = cp15_itcm = 0;
}

void cpu_state(RtStateIO *s) {
    RT_STATE(s, cp15_control);
    RT_STATE(s, cp15_dtcm);
    RT_STATE(s, cp15_itcm);
}
