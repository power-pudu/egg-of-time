"""EggOfTime static recompiler: dsd project + extracted ARM9 binaries -> C11 sources.

Usage: python -I tools/recomp/recomp.py <project_root> <out_dir> [--no-comments] [--extract <dir>] [--region <code>]
  --extract: the dsd extraction (default <project_root>/extract); --no-comments: no disassembly comments,
  which also leaves out capstone (the player-side build in runtime/game_build.c uses both); --region: the version
  of the game (YQUE, YQUP, YQUJ), when the extraction's header may not say (a translation patch can change it).
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from dsdconfig import Function, Project  # noqa: E402
from translate import FunctionTranslator, h  # noqa: E402

CHUNK_FUNCS = 250
SIG_BYTES = 64  # code bytes hashed to recognise which overlay is resident


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def load_overrides(path, region):
    """(module, addr) -> runtime C function name, and the set of (module, addr) marked `wrap`: their
    recompiled code is still emitted under its own name, for the runtime function to call."""
    out, wrapped = {}, set()
    with open(path) as f:
        for line in f:
            fields = line.split("#", 1)[0].split()
            if fields:
                addr = fields[1]
                for extra in fields[3:]:
                    if region == "YQUJ" and extra.startswith("jp="):
                        addr = extra[3:]
                key = (fields[0], int(addr, 16))
                out[key] = fields[2]
                if "wrap" in fields[3:]:
                    wrapped.add(key)
    return out, wrapped


class Recompiler:
    def __init__(self, project, overrides):
        self.p = project
        self.overrides = overrides
        self.entries = {}  # (module name, func addr, entry addr) -> C name
        self.pending = []

    def cname(self, f):
        return self.overrides.get((f.module.name, f.addr), f.cname)

    def entry_name(self, f, target):
        key = (f.module.name, f.addr, target)
        if key not in self.entries:
            self.entries[key] = f"{f.cname}_e{target:08x}"
            self.pending.append((f, target))
        return self.entries[key]

    def resolve(self, module, from_addr, target):
        """C function for a call/branch target, or None when it must be dispatched at run time."""
        reloc = module.relocs.get(from_addr)
        if reloc is not None:
            if len(reloc.modules) != 1:
                return None
            candidates = [self.p.modules[reloc.modules[0]]]
        else:
            candidates = ([module] if module.contains(target) else []) + self.p.static_modules_for(target)
        for m in candidates:
            if m.whole:
                return None  # reached through the module's any-entry at run time
            f = self.p.function_containing(m.name, target)
            if f is None:
                continue
            return self.cname(f) if f.addr == target else self.entry_name(f, target)
        return None

    @staticmethod
    def read_const(module, addr, size):
        if module.contains(addr) and addr + size <= module.end:
            o = addr - module.base
            return int.from_bytes(module.data[o:o + size], "little")
        return None


class Disasm:
    def __init__(self):
        import capstone

        self.arm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
        self.thumb = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)

    def __call__(self, module, addr, size, thumb):
        o = addr - module.base
        md = self.thumb if thumb else self.arm
        for i in md.disasm(module.data[o:o + size], addr, 1):
            return f"{i.mnemonic} {i.op_str}".strip().replace("*/", "* /")
        return ".word" if not thumb else ".hword"


def main():
    root, out = sys.argv[1], sys.argv[2]
    extract = sys.argv[sys.argv.index("--extract") + 1] if "--extract" in sys.argv else None
    comments = None if "--no-comments" in sys.argv else Disasm()
    t0 = time.time()
    region = sys.argv[sys.argv.index("--region") + 1] if "--region" in sys.argv else None
    p = Project(root, extract, region)
    overrides, wrapped = load_overrides(os.path.join(os.path.dirname(os.path.abspath(__file__)), "overrides.txt"),
                                        p.region)
    for mod, addr in overrides:
        if addr not in p.modules[mod].functions:
            sys.exit(f"overrides.txt: no function at {mod} 0x{addr:08x}")
    rc = Recompiler(p, overrides)
    os.makedirs(out, exist_ok=True)

    units = []  # (module name, C name, lines)
    stats = {"functions": 0, "entries": 0, "insns": 0, "unimpl": 0, "linear": 0}
    unimpl_funcs = []

    def run(f, entry, name, any_name=None):
        ft = FunctionTranslator(rc, f, entry, any_name)
        lines, insns = ft.emit(name, comments)
        stats["insns"] += len(insns)
        stats["unimpl"] += ft.unimpl
        if ft.unimpl:
            unimpl_funcs.append((name, ft.unimpl))
        if ft.has_dispatch and len(ft.labels) == len(insns):
            stats["linear"] += 1
        units.append((f.module.name, name, lines))

    any_funcs = {}  # module name -> C name of its any-entry function
    for m in p.modules.values():
        if m.whole:
            any_name = f"m_{m.name}_any"
            any_funcs[m.name] = any_name
            whole = Function(m, m.name, m.base, len(m.data), False, False)
            run(whole, None, any_name, any_name)
            for f in sorted(m.functions.values(), key=lambda f: f.addr):
                units.append((m.name, f.cname, [f"void {f.cname}(CPU *cpu) {{ {any_name}(cpu, {h(f.addr)}); }}"]))
                stats["functions"] += 1
            continue
        for f in sorted(m.functions.values(), key=lambda f: f.addr):
            if (m.name, f.addr) in overrides and (m.name, f.addr) not in wrapped:
                continue
            run(f, None, f.cname)
            stats["functions"] += 1
    while rc.pending:
        f, entry = rc.pending.pop()
        run(f, entry, rc.entries[(f.module.name, f.addr, entry)])
        stats["entries"] += 1

    emitted = {u[1] for u in units}

    # Write sources in chunks per module.
    with open(os.path.join(out, "funcs.h"), "w", newline="\n") as fh:
        fh.write("/* Generated by tools/recomp. Do not edit. */\n#pragma once\n#include \"recomp.h\"\n\n")
        for name in [u[1] for u in units]:
            if name in any_funcs.values():
                fh.write(f"void {name}(CPU *cpu, uint32_t entry);\n")
            else:
                fh.write(f"void {name}(CPU *cpu);\n")
        for name in sorted(set(overrides.values())):  # runtime functions, imported from the executable
            fh.write(f"RT_API void {name}(CPU *cpu);\n")
    files = []
    by_mod = {}
    for u in units:
        by_mod.setdefault(u[0], []).append(u)
    for mod, us in by_mod.items():
        for k in range(0, len(us), CHUNK_FUNCS):
            fn = f"{mod}_{k // CHUNK_FUNCS:02d}.c"
            files.append(fn)
            with open(os.path.join(out, fn), "w", newline="\n") as fc:
                fc.write("/* Generated by tools/recomp. Do not edit. */\n#include \"funcs.h\"\n\n")
                for _, _, lines in us[k:k + CHUNK_FUNCS]:
                    fc.write("\n".join(lines))
                    fc.write("\n\n")

    # Dispatch tables: every function and entry copy, keyed by address, with a hash of its first code
    # bytes for resident-overlay checks (overlays sharing an address often share a prologue too).
    with open(os.path.join(out, "dispatch.c"), "w", newline="\n") as fd:
        fd.write("/* Generated by tools/recomp. Do not edit. */\n#include \"funcs.h\"\n#include \"dispatch.h\"\n\n")
        mod_rows = []
        for mod in p.modules.values():
            rows = []
            for f in mod.functions.values():
                if f.cname in emitted or (mod.name, f.addr) in overrides:
                    rows.append((f.addr, rc.cname(f), f.end))
            for (mname, faddr, entry), name in rc.entries.items():
                if mname == mod.name:
                    rows.append((entry, name, mod.functions[faddr].end))
            rows.sort()
            fd.write(f"static const RtFuncEntry funcs_{mod.name}[] = {{\n")
            for a, name, end in rows:
                n = max(1, min(SIG_BYTES, end - a, mod.end - a))
                o = a - mod.base
                fd.write(f"    {{{h(a)}, {n}, {h(fnv1a(mod.data[o:o + n]))}, {name}}},\n")
            if not rows:
                fd.write("    {0, 0, 0, 0},\n")
            fd.write("};\n\n")
            ov = -1 if mod.overlay_id is None else mod.overlay_id
            any_fn = any_funcs.get(mod.name, "NULL")
            mod_rows.append(f"    {{\"{mod.name}\", {ov}, {h(mod.base)}, {h(mod.end)}, funcs_{mod.name}, {len(rows)}, "
                            f"{any_fn}}},")
        fd.write("static const RtModule modules[] = {\n" + "\n".join(mod_rows) + "\n};\n\n")
        # The recompiled originals of `wrap` overrides, which the runtime functions call.
        wrap_rows = [f"    {{\"{mod}\", {h(addr)}, {p.modules[mod].functions[addr].cname}}},"
                     for mod, addr in sorted(wrapped)]
        fd.write("static const RtWrapped wrapped[] = {\n" + "\n".join(wrap_rows or ["    {0, 0, 0},"]) + "\n};\n\n")
        fd.write(f"RT_GAME_EXPORT const RtGame egg_game = {{RT_API_VERSION, \"{p.region}\", modules, {len(mod_rows)}, "
                 f"{h(p.entry)}, wrapped, {len(wrap_rows)}}};\n")

    with open(os.path.join(out, "sources.cmake"), "w", newline="\n") as fs:
        fs.write("set(GEN_SOURCES\n" + "".join(f"    ${{GEN_DIR}}/{f}\n" for f in files + ["dispatch.c"]) + ")\n")

    unimpl_funcs.sort(key=lambda x: -x[1])
    print(f"functions={stats['functions']} entry_copies={stats['entries']} insns={stats['insns']} "
          f"unimpl={stats['unimpl']} linear_fallback={stats['linear']} files={len(files)} "
          f"time={time.time() - t0:.1f}s")
    for name, n in unimpl_funcs[:15]:
        print(f"  unimpl {n:4d} {name}")


if __name__ == "__main__":
    main()
