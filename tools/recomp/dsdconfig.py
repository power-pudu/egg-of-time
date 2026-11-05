"""Loads a dsd ARM9 project (config.yaml + symbols/relocs) and the matching extracted binaries."""

import bisect
import os
import re
from dataclasses import dataclass, field


@dataclass
class Function:
    module: "Module"
    name: str
    addr: int
    size: int
    thumb: bool
    unknown: bool

    @property
    def end(self):
        return self.addr + self.size

    @property
    def cname(self):
        return f"f_{self.module.name}_{self.addr:08x}"


@dataclass
class Reloc:
    kind: str
    to: int
    # Module names this reloc may target ("main", "itcm", "ov003", ...); several when ambiguous.
    modules: list


@dataclass
class Module:
    name: str
    base: int
    data: bytes
    functions: dict = field(default_factory=dict)  # addr -> Function
    labels: dict = field(default_factory=dict)  # addr -> thumb
    relocs: dict = field(default_factory=dict)  # from addr -> Reloc
    overlay_id: int | None = None
    whole: bool = False  # translated as one function with every instruction as an entry (extracode.py)

    @property
    def end(self):
        return self.base + len(self.data)

    def contains(self, addr):
        return self.base <= addr < self.end

    def u16(self, addr):
        o = addr - self.base
        return int.from_bytes(self.data[o:o + 2], "little")

    def u32(self, addr):
        o = addr - self.base
        return int.from_bytes(self.data[o:o + 4], "little")


_SYM_RE = re.compile(r"^(\S+) kind:(\w+)(?:\(([^)]*(?:\([^)]*\))*[^)]*)\))?.* addr:0x([0-9a-f]+)")
_REL_RE = re.compile(
    r"^from:0x([0-9a-f]+) kind:(\w+) to:0x([0-9a-f]+)(?: add:(-?0x[0-9a-f]+))? module:(\w+)(?:\(([\d,]+)\))?")


def _read_yaml_scalars(path):
    out = {}
    with open(path) as f:
        for line in f:
            m = re.match(r"^(\w+): *(.*)$", line)
            if m:
                out[m.group(1)] = m.group(2).strip()
    return out


def _parse_overlays_yaml(path):
    overlays, cur = [], None
    with open(path) as f:
        for line in f:
            m = re.match(r"^  - id: (\d+)", line)
            if m:
                cur = {"id": int(m.group(1))}
                overlays.append(cur)
                continue
            m = re.match(r"^    (\w+): *(.*)$", line)
            if m and cur is not None:
                cur[m.group(1)] = m.group(2).strip()
    return overlays


REGIONS = ("YQUE", "YQUP", "YQUJ")  # USA, Europe, Japan


class Project:
    def __init__(self, root, extract=None, region=None):
        self.root = root
        ext = extract or os.path.join(root, "extract")
        self.region = region or _read_yaml_scalars(os.path.join(ext, "header.yaml"))["gamecode"]
        if self.region not in REGIONS:
            raise SystemExit(f"game code {self.region} is not a known version of the game")
        self.modules = {}

        def config_file(rel):
            """A region's config holds the files that differ from the USA one in config/arm9."""
            own = os.path.join(root, "config", "regions", self.region, "arm9", rel)
            return own if os.path.exists(own) else os.path.join(root, "config", "arm9", rel)

        def add(name, base, binpath, cfgdir, overlay_id=None):
            with open(binpath, "rb") as f:
                data = f.read()
            m = Module(name, base, data, overlay_id=overlay_id)
            self._load_symbols(m, config_file(os.path.join(cfgdir, "symbols.txt")))
            self._load_relocs(m, config_file(os.path.join(cfgdir, "relocs.txt")))
            self.modules[name] = m

        arm9 = _read_yaml_scalars(os.path.join(ext, "arm9", "arm9.yaml"))
        self.entry = int(arm9["entry_function"])
        add("main", int(arm9["base_address"]), os.path.join(ext, "arm9", "arm9.bin"), "")
        for tcm in ("itcm", "dtcm"):
            y = _read_yaml_scalars(os.path.join(ext, "arm9", f"{tcm}.yaml"))
            add(tcm, int(y["base_address"]), os.path.join(ext, "arm9", f"{tcm}.bin"), tcm)
        for ov in _parse_overlays_yaml(os.path.join(ext, "arm9_overlays", "overlays.yaml")):
            name = f"ov{ov['id']:03d}"
            add(name, int(ov["base_address"]), os.path.join(ext, "arm9_overlays", ov["file_name"]),
                os.path.join("overlays", name), overlay_id=ov["id"])

        from extracode import load_extra_modules
        for m in load_extra_modules(self, os.path.join(os.path.dirname(os.path.abspath(__file__)), "extra_code.txt")):
            self.modules[m.name] = m

        # Sorted function index per module for containment lookups.
        self._sorted = {n: sorted(m.functions.values(), key=lambda f: f.addr) for n, m in self.modules.items()}
        # dsd 'unknown' functions have size 0: let them extend to the next function so they translate.
        for n, fs in self._sorted.items():
            for i, f in enumerate(fs):
                if f.size == 0:
                    f.size = (fs[i + 1].addr if i + 1 < len(fs) else self.modules[n].end) - f.addr
        self._starts = {n: [f.addr for f in fs] for n, fs in self._sorted.items()}

    @staticmethod
    def _load_symbols(m, path):
        with open(path) as f:
            for line in f:
                line = line.strip()
                mm = _SYM_RE.match(line)
                if not mm:
                    continue
                name, kind, args, addr = mm.group(1), mm.group(2), mm.group(3) or "", int(mm.group(4), 16)
                if kind == "function":
                    size = int(re.search(r"size=0x([0-9a-f]+)", args).group(1), 16)
                    m.functions[addr] = Function(m, name, addr, size, args.startswith("thumb"), "unknown" in args)
                elif kind == "label":
                    m.labels[addr] = args.startswith("thumb")

    @staticmethod
    def _load_relocs(m, path):
        with open(path) as f:
            for line in f:
                mm = _REL_RE.match(line.strip())
                if not mm:
                    continue
                frm, kind, to, mod, ids = int(mm.group(1), 16), mm.group(2), int(mm.group(3), 16), mm.group(5), mm.group(6)
                if mod in ("overlay", "overlays"):
                    mods = [f"ov{int(i):03d}" for i in ids.split(",")]
                elif mod == "none":
                    mods = []
                else:
                    mods = [mod]
                m.relocs[frm] = Reloc(kind, to, mods)

    def function_containing(self, module_name, addr):
        fs = self._sorted[module_name]
        i = bisect.bisect_right(self._starts[module_name], addr) - 1
        if i >= 0 and fs[i].addr <= addr < max(fs[i].end, fs[i].addr + 1):
            return fs[i]
        return None

    def static_modules_for(self, addr):
        """Modules that are always resident (main, ITCM, DTCM) containing addr."""
        return [m for n, m in self.modules.items() if m.overlay_id is None and m.contains(addr)]
