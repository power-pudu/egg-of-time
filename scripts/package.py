"""Assembles the Windows or Linux release: the runtime and the kit that builds the game on the player's machine.

The release holds no game code or data: runtime/game_build.c makes the game library from the player's ROM with
the tools in kit/ (see DESIGN.md, "Release builds"). This script copies

  <build>/Egg of Time.exe, SDL3, FFmpeg and libcurl DLLs,  the runtime (a build with -DEGG_RUNTIME_ONLY=OFF or ON)
  mods/
  librashader.dll (--librashader)                         librashader, which runs Slang shader mods
  kit/lib/libeggoftime.a                                  its import library, which the game library links against
  kit/include, kit/recomp, kit/config                     the recompiler and its inputs (runtime/recomp.h, dispatch.h)
  kit/dsd, kit/python, kit/llvm                           dsd, embeddable CPython, and the part of llvm-mingw that
                                                          compiles and links the game library (found by asking
                                                          clang for the headers and lld for the libraries it reads)
  kit/xdelta3 (--xdelta)                                  xdelta3, for translation patches in xdelta format
  kit/licenses, README.txt, LICENSE.md

and writes kit/kit.id, a hash of what decides the generated code, so a release with other code rebuilds it.

The Linux release (made on Linux) holds "Egg of Time" (SDL3 linked in statically), the FFmpeg shared libraries it
finds next to itself (RPATH $ORIGIN; libcurl is the system's), kit/include, kit/recomp, kit/config, kit/dsd and kit/zig
(the Zig executable and the headers it reads for the game library, so no system compiler or /usr/include is needed:
SteamOS has neither): the player's python3 does the rest, and the game library needs no import library. It is a .tar.gz, which keeps the executable bits,
and (--appimage) the same files as an AppImage: AppRun, a .desktop file and the icon added, packed by appimagetool
with the given type 2 runtime. The AppImage is read-only, so there the runtime keeps its data in
~/.local/share/EggOfTime/ (rt_data_dir, runtime/main.c).

Usage: python -I scripts/package.py --build <dir> --llvm <llvm-mingw dir> --python-embed <python-*-embed-amd64.zip>
           --dsd <dsd.exe> --dsd-license <file> --sdl <SDL3 dir> --ffmpeg <FFmpeg dir> --curl <curl dir>
           --rcheevos <rcheevos source dir> --out <dir> [--zip <file>]
           [--xdelta <xdelta3.exe> --xdelta-license <file>] [--librashader <dll> --librashader-license <file>]
       python3 -I scripts/package.py --build <dir> --dsd <dsd> --dsd-license <file> --zig <Zig dir> --sdl <SDL3 source dir>
           --ffmpeg <FFmpeg shared build dir> --rcheevos <rcheevos source dir> --out <dir> [--tar <file.tar.gz>] [--xdelta <xdelta3> --xdelta-license <file>]
           [--appimage <file.AppImage> --appimagetool <appimagetool> --appimage-runtime <runtime-x86_64>]
           [--librashader <librashader.so> --librashader-license <file>]
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


ZIG_FLAGS = ["cc", "-target", "x86_64-linux-gnu", "-g0"]  # runtime/game_build.c, cc_command


def zig_probe(zig, kit_include, tmp, deps=False):
    """Compiles and links a probe game library with zig as runtime/game_build.c does; its header list with deps."""
    flags = [*ZIG_FLAGS, "-std=c11", "-O1", "-w", "-fwrapv", "-DNDEBUG", "-DEGG_SINGLE_SCREEN=1", "-DRT_GAME_LIB",
             "-fPIC", "-Xclang", "-O1", "-I", kit_include]
    probe = os.path.join(tmp, "probe.c")
    with open(probe, "w") as f:
        f.write('#include "dispatch.h"\nint probe(void) { return 0; }\n')
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(tmp, "cache"), ZIG_LOCAL_CACHE_DIR=os.path.join(tmp, "cache"))
    headers = ""
    if deps:
        headers = subprocess.run([zig, *flags, "-M", probe], check=True, capture_output=True, text=True, env=env).stdout
    obj = os.path.join(tmp, "probe.o")
    subprocess.run([zig, *flags, "-c", probe, "-o", obj], check=True, env=env)
    subprocess.run([zig, *ZIG_FLAGS, "-shared", "-o", os.path.join(tmp, "probe.so"), "-nostdlib", obj], check=True,
                   env=env)
    return headers


def zig_subset(zig_dir, kit_include):
    """Files of Zig (relative paths) needed to compile and link the game library: the executable, the headers the
    probe reads, and lib/std/std.zig, by which zig finds its lib folder."""
    zig_dir = os.path.realpath(zig_dir)
    files = {"zig", "LICENSE", os.path.join("lib", "std", "std.zig")}
    with tempfile.TemporaryDirectory() as tmp:
        deps = zig_probe(os.path.join(zig_dir, "zig"), kit_include, tmp, deps=True)
    for p in re.split(r"(?<!\\)\s+", deps.replace("\\\n", " ")):
        if not p or p.endswith(":"):
            continue
        # zig reports a path under the working directory relative to it (CI's deps/zig-*)
        p = os.path.realpath(p.replace("\\ ", " "))
        if p.startswith(zig_dir + os.sep):
            files.add(os.path.relpath(p, zig_dir))
    if not any(os.path.basename(f) == "setjmp.h" for f in files):
        sys.exit(f"Zig subset lacks setjmp.h: zig did not report it under {zig_dir}")
    return sorted(files)


def mode(name, isdir):
    """Executable only where it runs (copies from a Windows drive are all 0777)."""
    return 0o755 if isdir or name in ("Egg of Time", "dsd", "xdelta3", "zig", "AppRun") or ".so." in name else 0o644


def anonymous(info):
    """A tar entry without the packager's user and group."""
    info.uid = info.gid = 0
    info.uname = info.gname = ""
    info.mode = mode(os.path.basename(info.name), info.isdir())
    return info


APP_RUN = """#!/bin/sh
here="$(dirname "$(readlink -f "$0")")"
exec "$here/Egg of Time" "$@"
"""

DESKTOP = """[Desktop Entry]
Type=Application
Name=Egg of Time
Comment=Native PC port of Chrono Trigger (Nintendo DS)
Exec=eggoftime %f
Icon=eggoftime
Categories=Game;RolePlaying;
Terminal=false
"""


def appimage(out, target, tool, runtime):
    """Packs the release folder as an AppImage."""
    with tempfile.TemporaryDirectory() as tmp:
        appdir = os.path.join(tmp, "EggOfTime.AppDir")
        shutil.copytree(out, appdir)
        for name, text in (("AppRun", APP_RUN), ("eggoftime.desktop", DESKTOP)):
            with open(os.path.join(appdir, name), "w", newline="\n") as f:
                f.write(text)
        copy(os.path.join(ROOT, "res", "icon.png"), os.path.join(appdir, "eggoftime.png"))
        copy(os.path.join(ROOT, "res", "icon.png"), os.path.join(appdir, ".DirIcon"))
        for d, dirs, files in os.walk(appdir):
            for n in dirs + files:
                os.chmod(os.path.join(d, n), mode(n, n in dirs))
        # appimagetool is itself an AppImage: extracting it avoids needing FUSE on the build machine.
        env = dict(os.environ, ARCH="x86_64", APPIMAGE_EXTRACT_AND_RUN="1")
        subprocess.run([tool, "--no-appstream", "--runtime-file", runtime, appdir, target], check=True, env=env)


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
    for a in ("build", "dsd", "dsd-license", "sdl", "ffmpeg", "rcheevos", "out"):
        ap.add_argument("--" + a, required=True)
    for a in ("llvm", "python-embed", "curl"):
        ap.add_argument("--" + a, required=WINDOWS)
    ap.add_argument("--zig", required=not WINDOWS)
    ap.add_argument("--xdelta")
    ap.add_argument("--xdelta-license")
    ap.add_argument("--librashader")
    ap.add_argument("--librashader-license")
    ap.add_argument("--zip")
    ap.add_argument("--tar")
    for a in ("appimage", "appimagetool", "appimage-runtime"):
        ap.add_argument("--" + a)
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    kit = os.path.join(out, "kit")

    # The runtime.
    exe = "Egg of Time.exe" if WINDOWS else "Egg of Time"
    if WINDOWS:
        for pattern in (exe, "SDL3.dll", "av*.dll", "sw*.dll", "libcurl*.dll"):
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
    if args.librashader:
        copy(args.librashader, os.path.join(out, "librashader" + (".dll" if WINDOWS else ".so")))
    for d in ("cutscenes", "music", "translation", "shaders", "graphics"):
        copy(os.path.join(ROOT, "res", "mods", d, "readme.txt"), os.path.join(out, "mods", d, "readme.txt"))
    readme = "README.txt" if WINDOWS else "README-linux.txt"
    copy(os.path.join(ROOT, "res", "release", readme), os.path.join(out, "README.txt"))
    copy(os.path.join(ROOT, "LICENSE.md"), os.path.join(out, "LICENSE.md"))
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
    if args.xdelta:
        xdelta = os.path.join(kit, "xdelta3", "xdelta3" + (".exe" if WINDOWS else ""))
        copy(args.xdelta, xdelta)
        if not WINDOWS:
            os.chmod(xdelta, 0o755)
        if args.xdelta_license:
            copy(args.xdelta_license, os.path.join(lic, "xdelta3.txt"))
    if WINDOWS:
        with zipfile.ZipFile(args.python_embed) as z:
            z.extractall(os.path.join(kit, "python"))
        # xdelta3.exe is built with the MSVC runtime: keep its DLLs beside it so a machine without the
        # redistributable can still apply xdelta translation patches.
        if args.xdelta:
            for dll in glob.glob(os.path.join(kit, "python", "vcruntime140*.dll")):
                copy(dll, os.path.join(kit, "xdelta3", os.path.basename(dll)))
        for rel in llvm_subset(args.llvm, os.path.join(kit, "include"), implib):
            copy(os.path.join(args.llvm, rel), os.path.join(kit, "llvm", rel))
        copy(os.path.join(args.llvm, "LICENSE.TXT"), os.path.join(lic, "llvm-mingw.txt"))
        copy(os.path.join(kit, "python", "LICENSE.txt"), os.path.join(lic, "python.txt"))
        copy(os.path.join(args.curl, "COPYING.txt"), os.path.join(lic, "curl.txt"))
    else:
        os.chmod(os.path.join(kit, "dsd", "dsd"), 0o755)
        for rel in zig_subset(args.zig, os.path.join(kit, "include")):
            copy(os.path.join(args.zig, rel), os.path.join(kit, "zig", rel))
        os.chmod(os.path.join(kit, "zig", "zig"), 0o755)
        copy(os.path.join(args.zig, "LICENSE"), os.path.join(lic, "zig.txt"))
        # The subset alone must do what the player's first start does.
        with tempfile.TemporaryDirectory() as tmp:
            zig_probe(os.path.join(kit, "zig", "zig"), os.path.join(kit, "include"), tmp)
    copy(args.dsd_license, os.path.join(lic, "dsd.txt"))
    copy(os.path.join(args.sdl, "LICENSE.txt"), os.path.join(lic, "SDL3.txt"))
    copy(os.path.join(args.ffmpeg, "LICENSE.txt"), os.path.join(lic, "FFmpeg.txt"))
    copy(os.path.join(args.rcheevos, "LICENSE"), os.path.join(lic, "rcheevos.txt"))
    if args.librashader_license:
        copy(args.librashader_license, os.path.join(lic, "librashader.txt"))

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
    if args.appimage:
        appimage(out, os.path.abspath(args.appimage), args.appimagetool, args.appimage_runtime)
        print(f"{args.appimage}: {os.path.getsize(args.appimage) / 1e6:.0f} MB")


if __name__ == "__main__":
    main()
