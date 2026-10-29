"""Assembles the Windows or Linux release: the runtime and the kit that builds the game on the player's machine.

The release holds no game code or data: runtime/game_build.c makes the game library from the player's ROM with
the tools in kit/ (see DESIGN.md, "Release builds"). This script copies

  <build>/Egg of Time.exe, SDL3 and FFmpeg DLLs, mods/   the runtime (a build with -DEGG_RUNTIME_ONLY=OFF or ON)
  kit/lib/libeggoftime.a                                  its import library, which the game library links against
  kit/include, kit/recomp, kit/config                     the recompiler and its inputs (runtime/recomp.h, dispatch.h)
  kit/dsd, kit/python, kit/llvm                           dsd, embeddable CPython, and the part of llvm-mingw that
                                                          compiles and links the game library (found by asking
                                                          clang for the headers and lld for the libraries it reads)
  kit/licenses, README.txt

and writes kit/kit.id, a hash of what decides the generated code, so a release with other code rebuilds it.

The Linux release (made on Linux) holds "Egg of Time" (SDL3 linked in statically), the FFmpeg shared libraries it
finds next to itself (RPATH $ORIGIN), kit/include, kit/recomp, kit/config and kit/dsd: the player's python3 and C
compiler do the rest, and the game library needs no import library. It is a .tar.gz, which keeps the executable bits.

Usage: python -I scripts/package.py --build <dir> --llvm <llvm-mingw dir> --python-embed <python-*-embed-amd64.zip>
           --dsd <dsd.exe> --dsd-license <file> --sdl <SDL3 dir> --ffmpeg <FFmpeg dir> --out <dir> [--zip <file>]
       python3 -I scripts/package.py --build <dir> --dsd <dsd> --dsd-license <file> --sdl <SDL3 source dir>
           --ffmpeg <FFmpeg shared build dir> --out <dir> [--tar <file.tar.gz>]
"""

import argparse
import glob
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile

WINDOWS = sys.platform == "win32"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FORBIDDEN = re.compile(r"(^|/)(gen|extract|asm|ghidra)/|\.(nds|sav|state)$", re.I)


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copy_tree(src, dst, pattern="**/*"):
    for f in glob.glob(os.path.join(src, pattern), recursive=True):
        if os.path.isfile(f) and "__pycache__" not in f:
            copy(f, os.path.join(dst, os.path.relpath(f, src)))


def llvm_subset(llvm, kit_include, implib):
    """Files of llvm-mingw (relative paths) needed to compile and link the game library."""
    # clang and lld report absolute paths: a relative llvm would match none of them and leave out every header,
    # crt object and import library.
    llvm = os.path.abspath(llvm)
    files = set()
    for pattern in ("clang.exe", "clang-[0-9]*.exe", "ld.lld.exe", "lld.exe", "libLLVM-*.dll", "libclang-cpp.dll",
                    "libc++.dll", "libunwind.dll", "libwinpthread-1.dll", "*.cfg"):
        files.update(os.path.relpath(f, llvm) for f in glob.glob(os.path.join(llvm, "bin", pattern)))
    clang = os.path.join(llvm, "bin", "clang.exe")
    flags = ["-std=c11", "-O1", "-w", "-fwrapv", "-DNDEBUG", "-DEGG_SINGLE_SCREEN=1", "-DRT_GAME_LIB", "-I", kit_include]
    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "probe.c")
        with open(probe, "w") as f:
            f.write('#include "dispatch.h"\nint probe(void) { return 0; }\n')
        deps = subprocess.run([clang, *flags, "-M", probe], check=True, capture_output=True, text=True).stdout
        for p in re.split(r"(?<!\\)\s+", deps.replace("\\\n", " ")):
            p = os.path.normpath(p.replace("\\ ", " "))
            if p.lower().startswith(os.path.normpath(llvm).lower()):
                files.add(os.path.relpath(p, llvm))
        obj = os.path.join(tmp, "probe.o")
        subprocess.run([clang, *flags, "-c", probe, "-o", obj], check=True)
        out = subprocess.run([clang, "-shared", "-o", os.path.join(tmp, "probe.dll"), obj, implib, "-Wl,--verbose"],
                             check=True, capture_output=True, text=True)
        for m in re.finditer(r"Reading (\S+\.(?:a|o))\s*$", out.stdout + out.stderr, re.M):
            p = os.path.normpath(m.group(1))
            if p.lower().startswith(os.path.normpath(llvm).lower()):
                files.add(os.path.relpath(p, llvm))
    for need in ("setjmp.h", "dllcrt2.o"):
        if not any(os.path.basename(f) == need for f in files):
            sys.exit(f"llvm-mingw subset lacks {need}: clang or lld did not report it under {llvm}")
    files.add("LICENSE.TXT")
    return sorted(files)


def anonymous(info):
    """A tar entry without the packager's user and group, executable only where it runs (copies from a Windows
    drive are all 0777)."""
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    name = os.path.basename(info.name)
    info.mode = 0o755 if info.isdir() or name in ("Egg of Time", "dsd") or ".so." in name else 0o644
    return info


def kit_id(kit):
    """Hash of everything that decides the game library's code."""
    h = hashlib.sha1()
    for part in ("recomp", "config", "include"):
        for f in sorted(glob.glob(os.path.join(kit, part, "**", "*"), recursive=True)):
            if os.path.isfile(f):
                h.update(os.path.relpath(f, kit).replace("\\", "/").encode() + b"\0")
                with open(f, "rb") as fh:
                    h.update(fh.read().replace(b"\r\n", b"\n") + b"\0")
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    for a in ("build", "dsd", "dsd-license", "sdl", "ffmpeg", "out"):
        ap.add_argument("--" + a, required=True)
    for a in ("llvm", "python-embed"):
        ap.add_argument("--" + a, required=WINDOWS)
    ap.add_argument("--zip")
    ap.add_argument("--tar")
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    kit = os.path.join(out, "kit")

    # The runtime.
    exe = "Egg of Time.exe" if WINDOWS else "Egg of Time"
    if WINDOWS:
        for pattern in (exe, "SDL3.dll", "av*.dll", "sw*.dll"):
            for f in glob.glob(os.path.join(args.build, pattern)):
                copy(f, os.path.join(out, os.path.basename(f)))
    else:
        copy(os.path.join(args.build, exe), os.path.join(out, exe))
        # Each library under its soname (libavcodec.so.62), the name the executable asks for.
        for f in glob.glob(os.path.join(args.ffmpeg, "lib", "lib*.so.*")):
            if re.search(r"/lib(avformat|avcodec|avutil|swscale|swresample)\.so\.\d+$", f.replace("\\", "/")):
                copy(os.path.realpath(f), os.path.join(out, os.path.basename(f)))
    if not os.path.exists(os.path.join(out, exe)):
        sys.exit(f"no '{exe}' in " + args.build)
    for d in ("cutscenes", "music"):
        copy(os.path.join(ROOT, "res", "mods", d, "readme.txt"), os.path.join(out, "mods", d, "readme.txt"))
    readme = "README.txt" if WINDOWS else "README-linux.txt"
    copy(os.path.join(ROOT, "res", "release", readme), os.path.join(out, "README.txt"))
    implib = os.path.join(args.build, "libeggoftime.a")
    if WINDOWS:
        copy(implib, os.path.join(kit, "lib", "libeggoftime.a"))

    # The recompiler and its inputs.
    for h in ("recomp.h", "dispatch.h"):
        copy(os.path.join(ROOT, "runtime", h), os.path.join(kit, "include", h))
    for f in glob.glob(os.path.join(ROOT, "tools", "recomp", "*")):
        if f.endswith((".py", ".txt")) and not f.endswith("disfunc.py"):
            copy(f, os.path.join(kit, "recomp", os.path.basename(f)))
    copy_tree(os.path.join(ROOT, "config"), os.path.join(kit, "config"), "**/*.txt")
    copy_tree(os.path.join(ROOT, "config"), os.path.join(kit, "config"), "**/*.yaml")

    # The tools.
    copy(args.dsd, os.path.join(kit, "dsd", "dsd" + (".exe" if WINDOWS else "")))
    lic = os.path.join(kit, "licenses")
    if WINDOWS:
        with zipfile.ZipFile(args.python_embed) as z:
            z.extractall(os.path.join(kit, "python"))
        for rel in llvm_subset(args.llvm, os.path.join(kit, "include"), implib):
            copy(os.path.join(args.llvm, rel), os.path.join(kit, "llvm", rel))
        copy(os.path.join(args.llvm, "LICENSE.TXT"), os.path.join(lic, "llvm-mingw.txt"))
        copy(os.path.join(kit, "python", "LICENSE.txt"), os.path.join(lic, "python.txt"))
    else:
        os.chmod(os.path.join(kit, "dsd", "dsd"), 0o755)
    copy(args.dsd_license, os.path.join(lic, "dsd.txt"))
    copy(os.path.join(args.sdl, "LICENSE.txt"), os.path.join(lic, "SDL3.txt"))
    copy(os.path.join(args.ffmpeg, "LICENSE.txt"), os.path.join(lic, "FFmpeg.txt"))

    with open(os.path.join(kit, "kit.id"), "w", newline="\n") as f:
        f.write(kit_id(kit) + "\n")

    # Nothing of the game may be in the release.
    bad = [p for p in (os.path.relpath(f, out).replace("\\", "/") for f in glob.glob(os.path.join(out, "**", "*"),
                                                                                    recursive=True))
           if FORBIDDEN.search(p)]
    if bad:
        sys.exit("release contains game files: " + ", ".join(bad[:10]))

    size = sum(os.path.getsize(f) for f in glob.glob(os.path.join(out, "**", "*"), recursive=True) if os.path.isfile(f))
    print(f"{out}: {size / 1e6:.0f} MB")
    if args.zip:
        with zipfile.ZipFile(args.zip, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for f in sorted(glob.glob(os.path.join(out, "**", "*"), recursive=True)):
                if os.path.isfile(f):
                    z.write(f, os.path.join("Egg of Time", os.path.relpath(f, out)))
        print(f"{args.zip}: {os.path.getsize(args.zip) / 1e6:.0f} MB")
    if args.tar:
        with tarfile.open(args.tar, "w:gz", compresslevel=9) as t:
            t.add(out, "Egg of Time", filter=anonymous)
        print(f"{args.tar}: {os.path.getsize(args.tar) / 1e6:.0f} MB")


if __name__ == "__main__":
    main()
