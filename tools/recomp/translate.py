"""ARMv5TE / Thumb (ARM946E-S) instruction decoder that emits C for the EggOfTime runtime.

Each game function becomes one C function `void f_<module>_<addr>(CPU *cpu)`. Register state lives
in `cpu`; memory goes through rt_* accessors. See DESIGN.md for the control-flow model.
"""

from dataclasses import dataclass, field

COND = ["EQ", "NE", "CS", "CC", "MI", "PL", "VS", "VC", "HI", "LS", "GE", "LT", "GT", "LE", "AL", "NV"]
DP_NAMES = ["AND", "EOR", "SUB", "RSB", "ADD", "ADC", "SBC", "RSC", "TST", "TEQ", "CMP", "CMN", "ORR", "MOV", "BIC", "MVN"]
LOGICAL = {0, 1, 8, 9, 12, 13, 14, 15}
NO_WRITE = {8, 9, 10, 11}

SHIFT_LSL, SHIFT_LSR, SHIFT_ASR, SHIFT_ROR, SHIFT_RRX = range(5)
SHIFT_FN = ["lsl", "lsr", "asr", "ror"]


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v & (m - 1)) - (v & m)


def h(v):
    return f"0x{v & 0xFFFFFFFF:08x}u"


@dataclass
class Decoded:
    addr: int
    size: int
    lines: list
    falls: bool = True  # execution may continue at addr + size
    local_targets: list = field(default_factory=list)
    computed: bool = False  # contains an intra-function computed jump (needs the dispatch switch)
    switch_targets: list = field(default_factory=list)  # resolved jump-table targets
    unimpl: bool = False


class FunctionTranslator:
    """Translates one function (or one extra entry copy of it)."""

    def __init__(self, ctx, func, entry=None, any_name=None):
        self.ctx = ctx  # Recompiler: resolves calls/branches and literal reads
        self.f = func
        self.entry = entry if entry is not None else func.addr
        # Whole-module mode (hand-written code copied at run time): one C function with every
        # instruction as an entry, `void any_name(CPU *cpu, uint32_t entry)`.
        self.any_name = any_name
        self.thumb = func.thumb
        self.labels = set()
        # Addresses where a saved thread can be resumed (save states): call return sites, SWI/WFI, MSR.
        self.resume_sites = set()
        self.has_dispatch = False
        self.unimpl = 0

    # ---------------- helpers ----------------
    def in_func(self, a):
        return self.f.addr <= a < self.f.end

    def reg(self, n, pc):
        return h(pc) if n == 15 else f"R[{n}]"

    def u16(self, a):
        return self.f.module.u16(a)

    def u32(self, a):
        return self.f.module.u32(a)

    # Branch / call emission. `target` already has the Thumb bit stripped; `thumb` is the target mode.
    def branch(self, addr, target, link, ret, backward_poll=True, same_mode=True):
        """Returns (lines, local_target_or_None)."""
        if not link and self.in_func(target):
            lines = []
            if backward_poll and target <= addr:
                lines.append(f"RT_POLL_AT(cpu, {h(target)});")
            lines.append(f"goto L_{target:08x};")
            return lines, target
        if link and same_mode and not self.any_name and self.in_func(target) and target != self.f.addr:
            # Thumb `bl` as a long branch within the function (beyond the +-2 KB of `b`), e.g. to a shared
            # epilogue: lr is clobbered, nothing returns to it.
            lines = [f"R[14] = {h(ret)};"]
            if backward_poll and target <= addr:
                lines.append(f"RT_POLL_AT(cpu, {h(target)});")
            lines.append(f"goto L_{target:08x};")
            return lines, target
        if link:
            self.resume_sites.add(ret & ~1)
        if link and self.any_name:
            # Hand-written code: every call can be returned to from deeper frames.
            if self.in_func(target):
                args = f"NULL, {self.any_name}, {h(target)}"
            else:
                name = self.ctx.resolve(self.f.module, addr, target)
                args = f"{name}, NULL, 0" if name else f"NULL, NULL, {h(target)}"
            return [f"rt_call_unwindable(cpu, {h(ret)}, {args});"], None
        name = self.ctx.resolve(self.f.module, addr, target)
        if link:
            if name:
                return [f"RT_CALL({name}, {h(ret)});"], None
            return [f"RT_CALL_ADDR({h(target)}, {h(ret)});"], None
        if name:
            return [f"{name}(cpu); return;"], None
        return [f"rt_jump(cpu, {h(target)}); return;"], None

    def reg_call(self, expr, ret):
        """Call through a register value."""
        self.resume_sites.add(ret & ~1)
        if self.any_name:
            return [f"uint32_t t = {expr};", f"rt_call_unwindable(cpu, {h(ret)}, NULL, NULL, t);"]
        return [f"uint32_t t = {expr};", f"RT_CALL_ADDR(t, {h(ret)});"]

    def computed_jump(self, expr):
        """Jump to a register value. Intra-function targets use the dispatch switch."""
        self.has_dispatch = True
        return [f"tgt = {expr};", "goto L__dispatch;"]

    # ---------------- shifter operands ----------------
    @staticmethod
    def shift_imm_expr(v, typ, n):
        """Value of an immediate-amount shift (n already normalised: LSR/ASR 1..32, ROR 1..31)."""
        if typ == SHIFT_LSL:
            return v if n == 0 else f"({v} << {n})"
        if typ == SHIFT_LSR:
            return "0u" if n == 32 else f"({v} >> {n})"
        if typ == SHIFT_ASR:
            return f"(uint32_t)((int32_t){v} >> {min(n, 31)})"
        if typ == SHIFT_ROR:
            return f"rt_ror({v}, {n})"
        return f"((cpu->c << 31) | ({v} >> 1))"  # RRX

    @staticmethod
    def shift_imm_carry(v, typ, n):
        if typ == SHIFT_LSL:
            return None if n == 0 else f"(({v} >> {32 - n}) & 1)"
        if typ in (SHIFT_LSR, SHIFT_ASR, SHIFT_ROR):
            return f"(({v} >> {min(n, 32) - 1}) & 1)"
        return f"({v} & 1)"

    def arm_op2(self, ins, pc, need_carry):
        """Returns (setup_lines, value_expr, carry_expr_or_None). Carry None = unchanged."""
        if ins & (1 << 25):
            rot = ((ins >> 8) & 0xF) * 2
            imm = ins & 0xFF
            val = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
            carry = (str(val >> 31) if rot else None) if need_carry else None
            return [], h(val), carry
        rm = ins & 0xF
        typ = (ins >> 5) & 3
        if ins & 0x10:  # register-specified shift
            rs = (ins >> 8) & 0xF
            src = self.reg(rm, pc + 4)
            lines = [f"uint32_t m = {src}, sa = {self.reg(rs, pc + 4)} & 0xFF;"]
            if need_carry:
                lines.append(f"uint32_t sc = cpu->c; uint32_t o2 = rt_{SHIFT_FN[typ]}_rc(m, sa, &sc);")
                return lines, "o2", "sc"
            lines.append(f"uint32_t o2 = rt_{SHIFT_FN[typ]}_r(m, sa);")
            return lines, "o2", None
        n = (ins >> 7) & 0x1F
        if typ in (SHIFT_LSR, SHIFT_ASR) and n == 0:
            n = 32
        if typ == SHIFT_ROR and n == 0:
            typ = SHIFT_RRX
        src = self.reg(rm, pc)
        if typ == SHIFT_LSL and n == 0:
            return [], src, None
        lines = [f"uint32_t m = {src};"]
        carry = self.shift_imm_carry("m", typ, n) if need_carry else None
        return lines, self.shift_imm_expr("m", typ, n), carry

    # ---------------- data processing (shared by ARM and Thumb) ----------------
    def dp(self, addr, op, s, rd, a_expr, b_lines, b_expr, b_carry, ret_ok=True):
        """Emits a data-processing op. a_expr = Rn value, b = operand 2. Returns Decoded fields."""
        lines = list(b_lines)
        if len(b_lines):
            lines.append(f"uint32_t o2v = {b_expr};")
            b_expr = "o2v"
            if b_carry:
                lines.append(f"uint32_t scv = {b_carry};")
                b_carry = "scv"
        a, b = a_expr, b_expr
        if s:
            arith = {2: f"rt_sub_s(cpu, {a}, {b})", 3: f"rt_sub_s(cpu, {b}, {a})", 4: f"rt_add_s(cpu, {a}, {b})",
                     5: f"rt_adc_s(cpu, {a}, {b})", 6: f"rt_sbc_s(cpu, {a}, {b})", 7: f"rt_sbc_s(cpu, {b}, {a})",
                     10: f"rt_sub_s(cpu, {a}, {b})", 11: f"rt_add_s(cpu, {a}, {b})"}
            if op in arith:
                val = arith[op]
            else:
                logic = {0: f"{a} & {b}", 1: f"{a} ^ {b}", 8: f"{a} & {b}", 9: f"{a} ^ {b}", 12: f"{a} | {b}",
                         13: f"{b}", 14: f"{a} & ~{b}", 15: f"~{b}"}[op]
                val = f"rt_logic_s(cpu, {logic})"
                if b_carry:
                    lines.append(f"cpu->c = {b_carry};")
        else:
            val = {0: f"{a} & {b}", 1: f"{a} ^ {b}", 2: f"{a} - {b}", 3: f"{b} - {a}", 4: f"{a} + {b}",
                   5: f"{a} + {b} + cpu->c", 6: f"{a} - {b} - (cpu->c ^ 1)", 7: f"{b} - {a} - (cpu->c ^ 1)",
                   12: f"{a} | {b}", 13: f"{b}", 14: f"{a} & ~{b}", 15: f"~{b}"}.get(op)
            if val is None:  # TST/TEQ/CMP/CMN without S do not exist (MRS/MSR space); caller filters these
                return lines + [f"rt_unimpl(cpu, {h(addr)}, 0);"], True
        if op in NO_WRITE:
            lines.append(f"(void)({val});")
            return lines, True
        if rd != 15:
            lines.append(f"R[{rd}] = {val};")
            return lines, True
        # Writes to PC.
        if s:
            lines += [f"rt_exception_return(cpu, {val});", "return;"]
            return lines, False
        if op == 13 and b_expr == "R[14]" and not b_lines:
            lines.append("RT_RETURN(R[14]);")
            return lines, False
        lines += self.computed_jump(val)
        return lines, False

    # ---------------- loads / stores ----------------
    def literal(self, a, size):
        return self.ctx.read_const(self.f.module, a, size)

    def load_expr(self, kind, addr_expr):
        return {"w": f"rt_ldr32({addr_expr})", "b": f"rt_r8({addr_expr})", "h": f"rt_r16({addr_expr})",
                "sb": f"(uint32_t)(int8_t)rt_r8({addr_expr})", "sh": f"rt_ldrsh({addr_expr})"}[kind]

    def store_stmt(self, kind, addr_expr, val):
        return {"w": f"rt_w32({addr_expr}, {val});", "b": f"rt_w8({addr_expr}, {val});",
                "h": f"rt_w16({addr_expr}, {val});"}[kind]

    def single_transfer(self, addr, load, kind, rd, rn, pre, up, wb, off_lines, off_expr, pc, is_const_off,
                        const_off=0):
        """Generic LDR/STR family. off_expr is the unsigned offset magnitude."""
        sign = "+" if up else "-"
        lines = list(off_lines)
        # PC-relative literal: fold to a constant when possible.
        if rn == 15 and load and pre and not wb and is_const_off and rd != 15:
            a = (pc + const_off) if up else (pc - const_off)
            size = {"w": 4, "b": 1, "h": 2, "sb": 1, "sh": 2}[kind]
            v = self.literal(a, size)
            if v is not None:
                if kind == "sb":
                    v = sext(v, 8)
                elif kind == "sh":
                    v = sext(v, 16)
                return [f"R[{rd}] = {h(v)};"], True
        base = self.reg(rn, pc)
        lines.append(f"uint32_t ad = {base};")
        if pre:
            lines.append(f"ad = ad {sign} {off_expr};")
            new_base = "ad"
        else:
            lines.append(f"uint32_t nb = ad {sign} {off_expr};")
            new_base = "nb"
        do_wb = (wb or not pre) and rn != 15
        if load:
            lines.append(f"uint32_t v = {self.load_expr(kind, 'ad')};")
            if do_wb:
                lines.append(f"R[{rn}] = {new_base};")
            if rd == 15:
                if rn == 13 and not pre and up and kind == "w":
                    lines.append("RT_RETURN(v);")  # ldr pc, [sp], #4  (pop pc)
                    return lines, False
                lines += self.computed_jump("v")
                return lines, False
            lines.append(f"R[{rd}] = v;")
        else:
            val = h(pc + 4) if rd == 15 else f"R[{rd}]"
            lines.append(self.store_stmt(kind, "ad", val))
            if do_wb:
                lines.append(f"R[{rn}] = {new_base};")
        return lines, True

    def block_transfer(self, addr, load, rn, regs, pre, up, wb, user, pc_val, thumb_ldm=False):
        n = len(regs)
        lines = []
        if user and not (load and 15 in regs):
            lines.append(f"rt_unimpl(cpu, {h(addr)}, 1); /* LDM/STM ^ user-bank transfer */")
            return lines, True, True
        if up:
            start = "4" if pre else "0"
        else:
            start = f"-{4 * n}" if pre else f"-{4 * n - 4}"
        lines.append(f"uint32_t ob = R[{rn}], ad = ob + (uint32_t)({start});")
        wb_val = f"ob + {4 * n}u" if up else f"ob - {4 * n}u"
        if load:
            pc_loaded = False
            for i, r in enumerate(regs):
                if r == 15:
                    lines.append(f"uint32_t npc = rt_r32(ad + {4 * i});")
                    pc_loaded = True
                else:
                    lines.append(f"R[{r}] = rt_r32(ad + {4 * i});")
            if wb:
                if rn not in regs:
                    lines.append(f"R[{rn}] = {wb_val};")
                elif not thumb_ldm:
                    # ARM9: writeback still happens if Rn is the only register or not the last one.
                    if regs == [rn] or any(r > rn for r in regs):
                        lines.append(f"R[{rn}] = {wb_val};")
            if pc_loaded:
                if user:  # ldm ..., {..., pc}^ : exception return, CPSR = SPSR
                    lines += ["rt_exception_return(cpu, npc);", "return;"]
                elif rn == 13:
                    lines.append("RT_RETURN(npc);")
                else:
                    lines += self.computed_jump("npc")
                return lines, False, False
            return lines, True, False
        for i, r in enumerate(regs):
            val = "ob" if r == rn else (h(pc_val) if r == 15 else f"R[{r}]")
            lines.append(f"rt_w32(ad + {4 * i}, {val});")
        if wb:
            lines.append(f"R[{rn}] = {wb_val};")
        return lines, True, False

    # ---------------- ARM ----------------
    def decode_arm(self, addr):
        ins = self.u32(addr)
        pc = addr + 8
        cond = ins >> 28
        d = Decoded(addr, 4, [])
        if cond == 0xF:
            if (ins >> 25) & 7 == 5:  # BLX imm
                target = (pc + (sext(ins & 0xFFFFFF, 24) << 2) + ((ins >> 23) & 2)) & 0xFFFFFFFF
                d.lines, _ = self.branch(addr, target, True, addr + 4, same_mode=False)
                return d
            if (ins & 0x0D70F000) == 0x0550F000:  # PLD
                d.lines = ["/* pld */"]
                return d
            return self.unimpl_ins(d, ins)
        body, falls = self.arm_body(d, ins, addr, pc)
        if cond != 14:
            d.lines = [f"if (COND_{COND[cond]}) {{"] + ["    " + l for l in body] + ["}"]
            d.falls = True
        else:
            d.lines = body
            d.falls = falls
        return d

    def unimpl_ins(self, d, ins):
        d.lines = [f"rt_unimpl(cpu, {h(d.addr)}, {h(ins)});"]
        d.unimpl = True
        self.unimpl += 1
        return d

    def arm_body(self, d, ins, addr, pc):
        """Returns (lines, falls_through_when_executed)."""
        top = (ins >> 25) & 7
        rd = (ins >> 12) & 0xF
        rn = (ins >> 16) & 0xF
        if top == 0:
            if (ins & 0x0FFFFFF0) == 0x012FFF10:  # BX
                return self.arm_bx(d, ins & 0xF, addr, link=False)
            if (ins & 0x0FFFFFF0) == 0x012FFF30:  # BLX reg
                return self.arm_bx(d, ins & 0xF, addr, link=True)
            if (ins & 0x0FFF0FF0) == 0x016F0F10:
                return [f"R[{rd}] = rt_clz(R[{ins & 0xF}]);"], True
            if (ins & 0x0F900FF0) == 0x01000050:
                op = ["rt_qadd(cpu, R[{m}], R[{n}])", "rt_qsub(cpu, R[{m}], R[{n}])",
                      "rt_qadd(cpu, R[{m}], rt_qadd(cpu, R[{n}], R[{n}]))",
                      "rt_qsub(cpu, R[{m}], rt_qadd(cpu, R[{n}], R[{n}]))"][(ins >> 21) & 3]
                return [f"R[{rd}] = {op.format(m=ins & 0xF, n=rn)};"], True
            if (ins & 0x0F900090) == 0x01000080:
                return self.arm_dsp_mul(ins), True
            if (ins & 0x0FB00FF0) == 0x01000090:
                b = "8" if ins & (1 << 22) else "32"
                rm = ins & 0xF
                rd_, rn_ = rd, rn
                if b == "8":
                    return [f"uint32_t t = rt_r8(R[{rn_}]); rt_w8(R[{rn_}], R[{rm}]); R[{rd_}] = t;"], True
                return [f"uint32_t t = rt_ldr32(R[{rn_}]); rt_w32(R[{rn_}], R[{rm}]); R[{rd_}] = t;"], True
            if (ins & 0x0FC000F0) == 0x00000090:
                rd_, rn_, rs, rm = (ins >> 16) & 0xF, (ins >> 12) & 0xF, (ins >> 8) & 0xF, ins & 0xF
                val = f"R[{rm}] * R[{rs}]" + (f" + R[{rn_}]" if ins & (1 << 21) else "")
                lines = [f"R[{rd_}] = {val};"]
                if ins & (1 << 20):
                    lines.append(f"cpu->n = R[{rd_}] >> 31; cpu->z = R[{rd_}] == 0;")
                return lines, True
            if (ins & 0x0F8000F0) == 0x00800090:
                hi, lo, rs, rm = (ins >> 16) & 0xF, (ins >> 12) & 0xF, (ins >> 8) & 0xF, ins & 0xF
                signed = ins & (1 << 22)
                ty = "int64_t" if signed else "uint64_t"
                cast = "(int64_t)(int32_t)" if signed else "(uint64_t)"
                lines = [f"uint64_t r = (uint64_t)(({ty}){cast}R[{rm}] * {cast}R[{rs}]);"]
                if ins & (1 << 21):
                    lines.append(f"r += ((uint64_t)R[{hi}] << 32) | R[{lo}];")
                lines.append(f"R[{lo}] = (uint32_t)r; R[{hi}] = (uint32_t)(r >> 32);")
                if ins & (1 << 20):
                    lines.append("cpu->n = (uint32_t)(r >> 63); cpu->z = r == 0;")
                return lines, True
            if (ins & 0x0E000090) == 0x00000090 and (ins & 0x60):
                return self.arm_halfword(d, ins, addr, pc)
            if (ins & 0x0FBF0FFF) == 0x010F0000:
                return [f"R[{rd}] = rt_mrs(cpu, {(ins >> 22) & 1});"], True
            if (ins & 0x0FB0FFF0) == 0x0120F000:
                self.resume_sites.add(addr + 4)
                return [f"rt_msr(cpu, {(ins >> 22) & 1}, {(ins >> 16) & 0xF}, R[{ins & 0xF}]);",
                        f"RT_POLL_AT(cpu, {h(addr + 4)});"], True
            if (ins & 0x0FF000F0) == 0x01200070:
                return [f"rt_bkpt(cpu, {h(addr)});"], True
            return self.arm_dp(d, ins, addr, pc)
        if top == 1:
            if (ins & 0x0FB0F000) == 0x0320F000:
                rot = ((ins >> 8) & 0xF) * 2
                imm = ins & 0xFF
                val = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
                self.resume_sites.add(addr + 4)
                return [f"rt_msr(cpu, {(ins >> 22) & 1}, {(ins >> 16) & 0xF}, {h(val)});", f"RT_POLL_AT(cpu, {h(addr + 4)});"], True
            return self.arm_dp(d, ins, addr, pc)
        if top in (2, 3):
            if top == 3 and ins & 0x10:
                self.unimpl_ins(d, ins)
                return d.lines, True
            load, byte = bool(ins & (1 << 20)), bool(ins & (1 << 22))
            pre, up, wb = bool(ins & (1 << 24)), bool(ins & (1 << 23)), bool(ins & (1 << 21))
            if top == 2:
                off = ins & 0xFFF
                off_lines, off_expr, const = [], h(off), True
            else:
                rm, typ, n = ins & 0xF, (ins >> 5) & 3, (ins >> 7) & 0x1F
                if typ in (SHIFT_LSR, SHIFT_ASR) and n == 0:
                    n = 32
                if typ == SHIFT_ROR and n == 0:
                    typ = SHIFT_RRX
                off_lines = [f"uint32_t om = {self.reg(rm, pc)};"]
                off_expr = self.shift_imm_expr("om", typ, n)
                const, off = False, 0
            return self.single_transfer(addr, load, "b" if byte else "w", rd, rn, pre, up, wb, off_lines, off_expr,
                                        pc, const, off)
        if top == 4:
            regs = [r for r in range(16) if ins & (1 << r)]
            if not regs:
                self.unimpl_ins(d, ins)
                return d.lines, True
            lines, falls, unimpl = self.block_transfer(addr, bool(ins & (1 << 20)), rn, regs, bool(ins & (1 << 24)),
                                                       bool(ins & (1 << 23)), bool(ins & (1 << 21)),
                                                       bool(ins & (1 << 22)), addr + 12)
            if unimpl:
                self.unimpl += 1
                d.unimpl = True
            return lines, falls
        if top == 5:
            target = (pc + (sext(ins & 0xFFFFFF, 24) << 2)) & 0xFFFFFFFF
            link = bool(ins & (1 << 24))
            lines, local = self.branch(addr, target, link, addr + 4)
            if local is not None:
                d.local_targets.append(local)
            return lines, link
        if top == 7:
            if ins & (1 << 24):
                self.resume_sites.add(addr)
                return [f"RT_SWI(cpu, {(ins >> 16) & 0xFF}, {h(addr)});"], True
            if ins & 0x10:
                cp, opc1, crn, crm, opc2 = (ins >> 8) & 0xF, (ins >> 21) & 7, rn, ins & 0xF, (ins >> 5) & 7
                if ins & (1 << 20):
                    if rd == 15:
                        return [f"rt_mrc_flags(cpu, {cp}, {opc1}, {crn}, {crm}, {opc2});"], True
                    return [f"R[{rd}] = rt_mrc(cpu, {cp}, {opc1}, {crn}, {crm}, {opc2});"], True
                self.resume_sites.add(addr)  # cp15 wait-for-interrupt halts here
                return [f"RT_MCR(cpu, {cp}, {opc1}, {crn}, {crm}, {opc2}, {self.reg(rd, pc + 4)}, {h(addr)});"], True
        self.unimpl_ins(d, ins)
        return d.lines, True

    def arm_bx(self, d, rm, addr, link):
        if link:
            return self.reg_call(f"R[{rm}]", addr + 4), True
        if rm == 14:
            return ["RT_RETURN(R[14]);"], False
        prev = self.u32(addr - 4) if self.in_func(addr - 4) else None
        if prev is not None and (prev & 0x0FFFFFFF) == 0x01A0E00F:  # mov lr, pc ; bx rX  => call
            return self.reg_call(f"R[{rm}]", addr + 4), True
        return [f"rt_jump(cpu, R[{rm}]); return;"], False

    def arm_dp(self, d, ins, addr, pc):
        op = (ins >> 21) & 0xF
        s = bool(ins & (1 << 20))
        rd, rn = (ins >> 12) & 0xF, (ins >> 16) & 0xF
        if op in NO_WRITE and not s:
            self.unimpl_ins(d, ins)
            return d.lines, True
        need_carry = s and op in LOGICAL
        # mov lr, pc before a register call: lr = address after the following instruction.
        b_lines, b_expr, b_carry = self.arm_op2(ins, pc, need_carry)
        a_expr = self.reg(rn, pc + 4 if (ins & 0x2000010) == 0x10 else pc)
        # Jump tables: addls pc, pc, rX, lsl #2 followed by a branch table.
        if rd == 15 and op == 4 and rn == 15 and not s and (ins & 0x2000FF0) == 0x100:
            count = self.arm_table_entries(addr, ins & 0xF, ins >> 28)
            targets = [addr + 8 + 4 * i for i in range(count)]
            d.switch_targets += targets
            d.local_targets += targets
        if (prev := self.mov_lr_pc_call(addr, ins, rd, op, s)) is not None:
            return prev, True
        lines, falls = self.dp(addr, op, s, rd, a_expr, b_lines, b_expr, b_carry)
        if rd == 15 and any("L__dispatch" in l for l in lines):
            d.computed = True
        return lines, falls

    def mov_lr_pc_call(self, addr, ins, rd, op, s):
        """`mov lr, pc ; mov pc, rX` call idiom: handle the second instruction as a call."""
        if rd == 15 and op == 13 and not s and not (ins & (1 << 25)) and (ins & 0xFF0) == 0:
            prev = self.u32(addr - 4) if self.in_func(addr - 4) else None
            if prev is not None and (prev & 0x0FFFFFFF) == 0x01A0E00F and (ins & 0xF) != 14:
                return self.reg_call(f"R[{ins & 0xF}]", addr + 4)
        return None

    @staticmethod
    def arm_cmp_imm(w, reg):
        """N if w is `cmp reg, #N`."""
        if (w & 0x0FFFF000) != (0x03500000 | (reg << 16)):
            return None
        rot = ((w >> 8) & 0xF) * 2
        imm = w & 0xFF
        return ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm

    def arm_table_entries(self, addr, reg, cond):
        """Entry count of an MWCC `add<cond> pc, pc, reg, lsl #2` branch table."""
        LS, LE, CC, LT, HI, GT, CS, GE = 9, 13, 3, 11, 8, 12, 2, 10
        # `cmp reg, #N` just before, or with flag-preserving moves in between (`cmp r2, #4 ; mov r0, #0 ; addls`).
        for back in range(1, 5):
            a = addr - back * 4
            if not self.in_func(a):
                break
            w = self.u32(a)
            n = self.arm_cmp_imm(w, reg)
            if n is not None:
                if cond in (LS, LE, CC, LT):
                    return n + 1 if cond in (LS, LE) else n
                break
            # Only unconditional `mov rd, #imm` (no S) to another register may sit in between.
            if (w & 0xFFF00000) != 0xE3A00000 or ((w >> 12) & 0xF) in (reg, 15):
                break
        # Upper bound checked earlier: `cmp reg, #N ; b{hi,gt,cs,ge} default`.
        for back in range(1, 9):
            a = addr - back * 4
            if not self.in_func(a) or not self.in_func(a + 4):
                break
            n = self.arm_cmp_imm(self.u32(a), reg)
            nxt = self.u32(a + 4)
            if n is not None and (nxt & 0x0F000000) == 0x0A000000:
                bc = nxt >> 28
                if bc in (HI, GT):
                    return n + 1
                if bc in (CS, GE):
                    return n
        # Fallback: the table is a run of unconditional branches after the default branch.
        count, a = 0, addr + 8
        while self.in_func(a) and (self.u32(a) & 0xFF000000) == 0xEA000000:
            count += 1
            a += 4
        return count

    def arm_halfword(self, d, ins, addr, pc):
        load = bool(ins & (1 << 20))
        sh = (ins >> 5) & 3
        pre, up, wb = bool(ins & (1 << 24)), bool(ins & (1 << 23)), bool(ins & (1 << 21))
        rd, rn = (ins >> 12) & 0xF, (ins >> 16) & 0xF
        if ins & (1 << 22):
            off = ((ins >> 4) & 0xF0) | (ins & 0xF)
            off_lines, off_expr, const = [], h(off), True
        else:
            off_lines, off_expr, const, off = [], self.reg(ins & 0xF, pc), False, 0
        if not load and sh in (2, 3):  # LDRD / STRD
            if rd & 1 or rd == 14:
                self.unimpl_ins(d, ins)
                return d.lines, True
            sign = "+" if up else "-"
            lines = list(off_lines) + [f"uint32_t ad = {self.reg(rn, pc)};"]
            if pre:
                lines.append(f"ad = ad {sign} {off_expr};")
            nb = "ad" if pre else f"ad {sign} {off_expr}"
            if sh == 2:
                lines += [f"uint32_t v0 = rt_r32(ad), v1 = rt_r32(ad + 4);"]
                if wb or not pre:
                    lines.append(f"R[{rn}] = {nb};")
                lines.append(f"R[{rd}] = v0; R[{rd + 1}] = v1;")
            else:
                lines += [f"rt_w32(ad, R[{rd}]); rt_w32(ad + 4, R[{rd + 1}]);"]
                if wb or not pre:
                    lines.append(f"R[{rn}] = {nb};")
            return lines, True
        kind = {1: "h", 2: "sb", 3: "sh"}[sh]
        return self.single_transfer(addr, load, kind, rd, rn, pre, up, wb, off_lines, off_expr, pc, const, off)

    def arm_dsp_mul(self, ins):
        op = (ins >> 21) & 3
        rd, rn, rs, rm = (ins >> 16) & 0xF, (ins >> 12) & 0xF, (ins >> 8) & 0xF, ins & 0xF
        x, y = (ins >> 5) & 1, (ins >> 6) & 1
        half = lambda r, top: f"(int32_t)(int16_t)(R[{r}] >> 16)" if top else f"(int32_t)(int16_t)R[{r}]"
        if op == 0:
            return [f"R[{rd}] = rt_qadd_flag(cpu, (uint32_t)({half(rm, x)} * {half(rs, y)}), R[{rn}]);"]
        if op == 1:
            prod = f"(uint32_t)(((int64_t)(int32_t)R[{rm}] * {half(rs, y)}) >> 16)"
            if x == 0:
                return [f"R[{rd}] = rt_qadd_flag(cpu, {prod}, R[{rn}]);"]
            return [f"R[{rd}] = {prod};"]
        if op == 2:
            return [f"uint64_t acc = (((uint64_t)R[{rd}] << 32) | R[{rn}]) + (uint64_t)(int64_t)({half(rm, x)} * {half(rs, y)});",
                    f"R[{rn}] = (uint32_t)acc; R[{rd}] = (uint32_t)(acc >> 32);"]
        return [f"R[{rd}] = (uint32_t)({half(rm, x)} * {half(rs, y)});"]

    # ---------------- Thumb ----------------
    def decode_thumb(self, addr):
        hw = self.u16(addr)
        pc = addr + 4
        d = Decoded(addr, 2, [])
        top5 = hw >> 11
        lo3 = hw & 7
        mid3 = (hw >> 3) & 7

        if top5 <= 2:  # shift by immediate
            typ = top5
            n = (hw >> 6) & 0x1F
            if typ in (SHIFT_LSR, SHIFT_ASR) and n == 0:
                n = 32
            lines = [f"uint32_t m = R[{mid3}];"]
            carry = self.shift_imm_carry("m", typ, n)
            if carry:
                lines.append(f"uint32_t scv = {carry};")
            lines.append(f"R[{lo3}] = rt_logic_s(cpu, {self.shift_imm_expr('m', typ, n)});")
            if carry:
                lines.append("cpu->c = scv;")
            d.lines = lines
            return d
        if top5 == 3:  # add/sub register or imm3
            imm, sub = bool(hw & (1 << 10)), bool(hw & (1 << 9))
            v = (hw >> 6) & 7
            b = h(v) if imm else f"R[{v}]"
            d.lines, _ = self.dp(addr, 2 if sub else 4, True, lo3, f"R[{mid3}]", [], b, None)
            return d
        if top5 <= 7:  # mov/cmp/add/sub imm8
            op = [13, 10, 4, 2][top5 - 4]
            rd = (hw >> 8) & 7
            d.lines, _ = self.dp(addr, op, True, rd, f"R[{rd}]", [], h(hw & 0xFF), None)
            return d
        if (hw & 0xFC00) == 0x4000:  # ALU
            op = (hw >> 6) & 0xF
            rd, rs = lo3, mid3
            if op in (2, 3, 4, 7):  # register shifts
                fn = {2: "lsl", 3: "lsr", 4: "asr", 7: "ror"}[op]
                d.lines = [f"uint32_t sc = cpu->c; R[{rd}] = rt_logic_s(cpu, rt_{fn}_rc(R[{rd}], R[{rs}] & 0xFF, &sc)); cpu->c = sc;"]
                return d
            if op == 9:  # NEG
                d.lines = [f"R[{rd}] = rt_sub_s(cpu, 0, R[{rs}]);"]
                return d
            if op == 13:  # MUL
                d.lines = [f"R[{rd}] = rt_logic_s(cpu, R[{rd}] * R[{rs}]);"]
                return d
            dpop = {0: 0, 1: 1, 5: 5, 6: 6, 8: 8, 10: 10, 11: 11, 12: 12, 14: 14, 15: 15}[op]
            d.lines, _ = self.dp(addr, dpop, True, rd, f"R[{rd}]", [], f"R[{rs}]", None)
            return d
        if (hw & 0xFC00) == 0x4400:  # hi register ops / BX
            op = (hw >> 8) & 3
            rd = lo3 | ((hw >> 4) & 8)
            rs = ((hw >> 3) & 0xF)
            src = self.reg(rs, pc)
            if op == 3:
                if hw & 0x80:  # BLX reg
                    d.lines = self.reg_call(f"R[{rs}]", addr + 2 | 1)
                    return d
                if rs == 14:
                    d.lines, d.falls = ["RT_RETURN(R[14]);"], False
                    return d
                if rs == 15:  # bx pc: continue in ARM mode at (pc & ~3), via the dispatch table
                    d.lines, d.falls = [f"rt_jump(cpu, {h(pc & ~3)}); return;"], False
                    return d
                d.lines, d.falls = [f"rt_jump(cpu, R[{rs}]); return;"], False
                return d
            if op == 1:
                d.lines, _ = self.dp(addr, 10, True, rd, self.reg(rd, pc), [], src, None)
                return d
            dpop = 4 if op == 0 else 13
            if rd == 15:
                if dpop == 4:
                    targets = self.thumb_jump_table(addr, rs)
                    if targets:
                        d.switch_targets += targets
                        d.local_targets += targets
                    val = f"{h(pc)} + {src}"
                else:
                    if rs == 14:
                        d.lines, d.falls = ["RT_RETURN(R[14]);"], False
                        return d
                    val = src
                d.lines = self.computed_jump(f"({val}) & ~1u")
                d.computed, d.falls = True, False
                return d
            a = self.reg(rd, pc)
            d.lines = [f"R[{rd}] = {a} + {src};"] if dpop == 4 else [f"R[{rd}] = {src};"]
            return d
        if (hw & 0xF800) == 0x4800:  # ldr rd, [pc, #imm]
            rd = (hw >> 8) & 7
            a = (pc & ~3) + (hw & 0xFF) * 4
            v = self.literal(a, 4)
            d.lines = [f"R[{rd}] = {h(v)};"] if v is not None else [f"R[{rd}] = rt_ldr32({h(a)});"]
            return d
        if (hw & 0xF000) == 0x5000:  # register offset
            op = (hw >> 9) & 7
            ro = (hw >> 6) & 7
            kind, load = [("w", False), ("h", False), ("b", False), ("sb", True), ("w", True), ("h", True),
                          ("b", True), ("sh", True)][op]
            d.lines, _ = self.single_transfer(addr, load, kind, lo3, mid3, True, True, False, [], f"R[{ro}]", pc, False)
            return d
        if (hw & 0xE000) == 0x6000:  # imm5 word/byte
            byte, load = bool(hw & (1 << 12)), bool(hw & (1 << 11))
            off = ((hw >> 6) & 0x1F) * (1 if byte else 4)
            d.lines, _ = self.single_transfer(addr, load, "b" if byte else "w", lo3, mid3, True, True, False, [],
                                              h(off), pc, True, off)
            return d
        if (hw & 0xF000) == 0x8000:  # imm5 halfword
            off = ((hw >> 6) & 0x1F) * 2
            d.lines, _ = self.single_transfer(addr, bool(hw & (1 << 11)), "h", lo3, mid3, True, True, False, [],
                                              h(off), pc, True, off)
            return d
        if (hw & 0xF000) == 0x9000:  # sp-relative
            rd, off = (hw >> 8) & 7, (hw & 0xFF) * 4
            d.lines, _ = self.single_transfer(addr, bool(hw & (1 << 11)), "w", rd, 13, True, True, False, [], h(off),
                                              pc, True, off)
            return d
        if (hw & 0xF000) == 0xA000:  # add rd, pc/sp, #imm
            rd, off = (hw >> 8) & 7, (hw & 0xFF) * 4
            d.lines = [f"R[{rd}] = R[13] + {h(off)};"] if hw & (1 << 11) else [f"R[{rd}] = {h((pc & ~3) + off)};"]
            return d
        if (hw & 0xFF00) == 0xB000:
            off = (hw & 0x7F) * 4
            d.lines = [f"R[13] {'-' if hw & 0x80 else '+'}= {h(off)};"]
            return d
        if (hw & 0xF600) == 0xB400:  # push / pop
            pop = bool(hw & (1 << 11))
            regs = [r for r in range(8) if hw & (1 << r)]
            if hw & 0x100:
                regs.append(15 if pop else 14)
            if not regs:
                return self.unimpl_ins(d, hw)
            if pop:
                d.lines, d.falls, _ = self.block_transfer(addr, True, 13, regs, False, True, True, False, 0, True)
            else:
                d.lines, d.falls, _ = self.block_transfer(addr, False, 13, regs, True, False, True, False, 0, True)
            return d
        if (hw & 0xFF00) == 0xBE00:
            d.lines = [f"rt_bkpt(cpu, {h(addr)});"]
            return d
        if (hw & 0xF000) == 0xC000:  # ldmia / stmia
            load = bool(hw & (1 << 11))
            rb = (hw >> 8) & 7
            regs = [r for r in range(8) if hw & (1 << r)]
            if not regs:
                return self.unimpl_ins(d, hw)
            d.lines, d.falls, _ = self.block_transfer(addr, load, rb, regs, False, True, True, False, 0, True)
            return d
        if (hw & 0xF000) == 0xD000:
            cond = (hw >> 8) & 0xF
            if cond == 0xF:
                self.resume_sites.add(addr)
                d.lines = [f"RT_SWI(cpu, {hw & 0xFF}, {h(addr)});"]
                return d
            if cond == 0xE:
                return self.unimpl_ins(d, hw)
            target = (pc + sext(hw & 0xFF, 8) * 2) & 0xFFFFFFFF
            body, local = self.branch(addr, target, False, 0)
            if local is not None:
                d.local_targets.append(local)
            d.lines = [f"if (COND_{COND[cond]}) {{"] + ["    " + l for l in body] + ["}"]
            return d
        if (hw & 0xF800) == 0xE000:
            target = (pc + sext(hw & 0x7FF, 11) * 2) & 0xFFFFFFFF
            d.lines, local = self.branch(addr, target, False, 0)
            if local is not None:
                d.local_targets.append(local)
            d.falls = False
            return d
        if (hw & 0xF800) == 0xF000:  # BL/BLX prefix
            hw2 = self.u16(addr + 2) if self.in_func(addr + 2) or addr + 2 < self.f.module.end else 0
            if (hw2 & 0xE800) == 0xE800:
                off = (sext(hw & 0x7FF, 11) << 12) + ((hw2 & 0x7FF) << 1)
                target = (pc + off) & 0xFFFFFFFF
                if not (hw2 & 0x1000):  # BLX: target is ARM, word-aligned
                    target &= ~3
                d.size = 4
                d.lines, local = self.branch(addr, target, True, (addr + 4) | 1, same_mode=bool(hw2 & 0x1000))
                if local is not None:
                    d.local_targets.append(local)
                return d
            d.lines = [f"R[14] = {h(pc + (sext(hw & 0x7FF, 11) << 12))};"]
            return d
        if (hw & 0xE800) == 0xE800:  # lone BL/BLX suffix
            off = (hw & 0x7FF) << 1
            mask = "" if hw & 0x1000 else " & ~3u"
            d.lines = self.reg_call(f"(R[14] + {h(off)}){mask}", (addr + 2) | 1)
            return d
        return self.unimpl_ins(d, hw)

    def thumb_jump_table(self, addr, rx):
        """MWCC Thumb switch: `add rY, pc ; ldrh rX, [rY, #k] ; lsl/asr ; add pc, rX` with `cmp rZ, #N`."""
        for back in range(1, 6):
            a = addr - back * 2
            if not self.in_func(a):
                return None
            hw = self.u16(a)
            if (hw & 0xF800) == 0x8800 and (hw & 7) == rx:  # ldrh rx, [ry, #k]
                ry, k = (hw >> 3) & 7, ((hw >> 6) & 0x1F) * 2
                b = a - 2
                if not self.in_func(b) or self.u16(b) != (0x4478 | ry):  # add ry, pc
                    return None
                n = None
                for back2 in range(1, 6):
                    c = b - back2 * 2
                    if self.in_func(c) and (self.u16(c) & 0xF800) == 0x2800:
                        n = self.u16(c) & 0xFF
                        break
                if n is None:
                    return None
                base = b + 4 + k
                return [(addr + 4 + sext(self.u16(base + 2 * i), 16)) & ~1 for i in range(n + 1)]
        return None

    # ---------------- function emission ----------------
    def decode(self, addr):
        return self.decode_thumb(addr) if self.thumb else self.decode_arm(addr)

    def translate(self):
        insns = {}
        computed = False
        work = [self.entry]
        while work:
            a = work.pop()
            if a in insns or not self.in_func(a):
                continue
            d = self.decode(a)
            insns[a] = d
            computed |= d.computed and not d.switch_targets
            for t in d.local_targets:
                self.labels.add(t)
                work.append(t)
            if d.falls:
                work.append(a + d.size)
        if computed or self.any_name:
            # Unresolved computed jump: decode the whole range linearly and make every instruction a target.
            a = self.f.addr
            while a < self.f.end:
                if a not in insns:
                    insns[a] = self.decode(a)
                a += insns[a].size
            self.labels |= set(insns)
        if self.any_name or any(d.computed for d in insns.values()):
            self.has_dispatch = True
        self.labels |= {a for a in self.resume_sites if a in insns}
        return insns, computed

    def emit(self, name, comments):
        insns, _ = self.translate()
        if self.entry != min(insns, default=self.entry):
            self.labels.add(self.entry)
        cases = sorted(t for t in self.labels if t in insns) if self.has_dispatch else []
        if self.any_name:
            out = [f"void {name}(CPU *cpu, uint32_t entry) {{", "    uint32_t tgt = entry;",
                   "    goto L__dispatch;"]
        else:
            out = [f"void {name}(CPU *cpu) {{", "    uint32_t tgt;",
                   "    if (__builtin_expect(cpu->resume_pc != 0, 0)) { tgt = cpu->resume_pc; cpu->resume_pc = 0; goto L__resume; }"]
            if self.entry != min(insns, default=self.entry):
                out.append(f"    goto L_{self.entry:08x};")
        covered_end = None
        for a in sorted(insns):
            d = insns[a]
            if covered_end is not None and a < covered_end:
                continue  # second half of a 4-byte Thumb BL
            if a in self.labels:
                out.append(f"L_{a:08x}:;")
            text = f" /* {a:08x}: {comments(self.f.module, a, d.size, self.thumb)} */" if comments else ""
            out.append("    {" + text)
            out += ["        " + l for l in d.lines]
            nxt = a + d.size
            if d.falls and nxt not in insns:
                # Falls off the end of the function (dsd split) or into undecoded bytes: continue there.
                lines, _ = self.branch(a, nxt, False, 0, backward_poll=False)
                out += ["        " + l for l in lines]
            out.append("    }")
            covered_end = nxt
        if self.has_dispatch:
            out.append("L__dispatch:")
            out.append("    switch (tgt & ~1u) {")
            out += [f"    case {h(t)}: goto L_{t:08x};" for t in cases]
            out.append("    default: rt_jump(cpu, tgt); return;")
            out.append("    }")
        if not self.any_name:
            # Save states: re-enter a thread at a resume site, poll target or branch target.
            out.append("L__resume:")
            out.append("    switch (tgt & ~1u) {")
            out += [f"    case {h(t)}: goto L_{t:08x};" for t in sorted(t for t in self.labels if t in insns)]
            out.append("    default: rt_resume_failed(cpu, tgt); return;")
            out.append("    }")
        out.append("}")
        return out, insns
