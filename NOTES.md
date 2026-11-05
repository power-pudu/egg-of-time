# EggOfTime — Chrono Trigger DS (USA, YQUE) decomp setup

The European and Japanese configs in `config/regions/` come from the same `dsd init` and `dsd sig apply` run on their
ROMs, with the fixes below applied where they recur (see DESIGN.md, "Versions of the game").

## Layout
- `rom/` — base ROM (untouched).
- `tools/dsd/dsd.exe` — ds-decomp v0.12.1.
- `extract/` — `dsd rom extract` output (ARM9/ARM7 binaries, 19 overlays, 10,381 asset files).
- `config/arm9/` — `dsd init` output: `config.yaml`, per-module `delinks.txt`, `symbols.txt`, `relocs.txt`.
- `build/delinks/` — relocatable ELF objects from `dsd delink`.
- `asm/` — informational disassembly from `dsd dis`.
- `objdiff.json` — objdiff project config.

## Commands used
```sh
dsd rom extract --rom rom/CT.nds --output-path extract
dsd init --rom-config extract/config.yaml --output-path config --build-path build --allow-unknown-function-calls
dsd sig apply --config-path config/arm9/config.yaml --all
dsd delink --config-path config/arm9/config.yaml
dsd dis --config-path config/arm9/config.yaml --asm-path asm
dsd objdiff --config-path config/arm9/config.yaml --output-path .
```

## Known issues / manual fixes
- `dsd init` without `--allow-unknown-function-calls` fails:
  `Local function call from 0x0210199a in overlay 2 to 0x02101b40 leads to no function`.
  Cause: `is_thumb_function` treats a Thumb function whose 2nd halfword is an unconditional
  Thumb `b` (0xE0xx) as ARM. The undiscovered-function pass drops the call's Thumb bit
  (`called.address & !1` in `lib/src/config/module.rs`), so the known mode is lost.
- Fixed by hand in `config/arm9/overlays/ov002/symbols.txt`: `func_ov002_02101b30` and
  `func_ov002_02101b40` are 0x10-byte Thumb functions (byte-wise little-endian u8/u16 store helpers).
  The bogus `_unk` heads and the `+4` tail functions were removed.
- `ov012` 0x0212c538: real Thumb function of size 0x4c (starts `lsrs r2, r1, #0xc`, then a jump table).
  dsd rejects that start instruction (`InvalidStart`, byte-table heuristic), so the auto split remains:
  `func_ov012_0212c539_unk` (2 bytes) + `func_ov012_0212c53a`. Bytes are preserved; treat as one function when decompiling.
- dsd-ghidra v0.7.0 embeds ds-decomp 0.12.0, which predates trampoline classification (0.12.1).
  `func_ov008_02123a24` (4-byte `b 0x02124430` trampoline) made SyncDsd fail with
  `IllegalIns { address: 2123a28 ... }`. It is marked `kind:function(arm,size=0x0,unknown)` in
  `config/arm9/overlays/ov008/symbols.txt`, which both 0.12.0 and 0.12.1 accept.
- Ghidra 11.2.1 cannot load scripts compiled by Java 25 (Bnd 7.0.0 writes `osgi.ee=UNKNOWN`).
  `support/launch.properties` sets `JAVA_HOME_OVERRIDE` to a portable JDK 21 at
  `%LOCALAPPDATA%\Programs\jdk-21.0.9+10`.
- `dsd rom build --config extract/config.yaml -7 "rom/[BIOS] DS ARM7 ROM.bin"` (ARM7 BIOS: 16 KB,
  CRC32 1280f0d5) matches the base ROM except 8 FAT bytes at 0x1b6291: files 9682–9685 are empty
  and the original places them at 0x5096400, dsd at 0x5096600. No file data differs.
  Without `-7`, the header secure-area CRC (0x6c) and header CRC (0x15e) also differ.

## Contents
- NitroSDK + `[SDK+Actimagine:Mobiclip SDK V1.0.2]` (FMV `.mods` files), SDAT sound, Nitro graphics (NCGR/NCER/NANR/NCLR).
- Functions: main 2816, ITCM 12, overlays 0–18 ~5,978. Only 2 dsd signatures exist (FS_UnloadOverlay matched).

## Ghidra
- Ghidra 11.2.1 at `%LOCALAPPDATA%\Programs\ghidra_11.2.1_PUBLIC` (runs on installed JDK 25).
- dsd-ghidra v0.7.0 installed to `%APPDATA%\ghidra\ghidra_11.2.1_PUBLIC\Extensions\dsd-ghidra`.
- Loader and sync are GUI-only:
  1. Run `ghidraRun.bat`, create a project.
  2. Drop the ROM into the project window, choose **ARM9**. Do NOT auto-analyze.
  3. Script Manager → `SyncDsd.java` → select `config/arm9/config.yaml`.
