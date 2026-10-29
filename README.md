# Egg of Time

[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/powerpudu)

**A native PC port of *Chrono Trigger* for Nintendo DS**, for Windows, Linux and macOS.

Egg of Time is not an emulator. It is a **static recompilation**: every function of the game's code is translated
into C and compiled into a native program, with a small runtime standing in for the DS hardware. You get the DS
version of the game running at native speed, with widescreen, high resolutions, high framerates, gamepad support
and mods.

> [!IMPORTANT]
> **You need your own copy of the game.** Egg of Time contains no ROM, no game code and no game assets. The game
> is built on your computer from your own ROM dump of *Chrono Trigger* (USA). See [Your ROM](#your-rom).

## Disclaimers

- This is an unofficial fan project, not affiliated with or endorsed by Square Enix or Nintendo.
  *Chrono Trigger* is a trademark of Square Enix.
- **No copyrighted game data is distributed.** You need your own copy of the game. Do not share the files made
  from it: `rom/`, `extract/`, `game/` and the generated code in `build/`.
- **AI assistance:** this project was developed with Claude (Anthropic) through Claude Code. Claude did the
  reverse-engineering analysis and wrote the recompiler, the runtime and the documentation, under the direction
  of the repository owner.

## Playing on Windows

> [!WARNING]
> **Windows may block the first launch** with a blue or red "Windows protected your PC" screen. Egg of Time is
> not signed with a paid certificate, so Windows SmartScreen doesn't recognise it yet. Click **More info**, then
> **Run anyway**. Windows remembers your choice for this version.

1. Download the latest `EggOfTime-<version>-windows-x64.zip` from the
   [Releases page](https://github.com/power-pudu/egg-of-time/releases).
2. Unzip it into a folder you can write to, such as `Documents\Egg of Time`. Avoid `Program Files`.
3. Start **`Egg of Time.exe`** and choose your ROM (a `.nds` file) when asked.
4. Wait for the first-start build. Egg of Time translates the game from your ROM into native code. This
   takes a few minutes and shows a progress bar. Later starts take about two seconds.

> [!NOTE]
> Nothing is installed and nothing is downloaded: the tools the build uses (dsd, Python, clang) come in the zip's
> `kit\` folder. If the build fails, `game\build.log` has the details.

**Where your files are.** Everything stays in the Egg of Time folder:

| What | Where |
|---|---|
| Your ROM (copied there on first start) | `rom\CT.nds` |
| In-game saves | `rom\CT.sav` |
| Save state (F5 / F7) | `rom\CT.state` |
| Settings | `eggoftime.ini` |
| Mods | `mods\cutscenes\`, `mods\music\` |

> [!TIP]
> **Updating:** unzip the new version over the old folder. Your ROM, saves and settings are kept. If the new
> version changed the recompiler, the game is built again on its first start.

## Playing on Linux

1. Install Python 3 and a C compiler if you don't have them. The first-start build uses your system's
   `python3` and `cc` (or `clang` / `gcc`):
   - Debian/Ubuntu: `sudo apt install python3 build-essential`
   - Fedora: `sudo dnf install python3 gcc`
   - Arch: `sudo pacman -S python gcc`
2. Download the latest `EggOfTime-<version>-linux-x64.tar.gz` from the
   [Releases page](https://github.com/power-pudu/egg-of-time/releases) and extract it:
   `tar xzf EggOfTime-*-linux-x64.tar.gz`.
3. Start `Egg of Time` in the extracted folder and choose your ROM, or pass it: `"./Egg of Time" path/to/rom.nds`.
4. Wait for the first-start build (about two minutes), as on Windows. Your files go in the same places, with `/`.

> [!NOTE]
> The download runs on x86-64 distros with glibc 2.35 or newer (Ubuntu 22.04, Debian 12, Fedora 36 and later).
> The file dialog needs xdg-desktop-portal or zenity. On other systems, and on macOS (the runtime compiles there,
> but a full game build is untested), build from source (see [Building from source](#building-from-source)).

## Your ROM

Egg of Time runs one version of the game only:

- *Chrono Trigger*, Nintendo DS, **USA** release (game code `YQUE`, English/French)
- SHA-1 `11f9e8204e484eab42181b210c9f214584be75bf`

> [!CAUTION]
> Dump it from your own cartridge. The Windows release refuses any other file (other regions, modified ROMs). A
> source build only warns, but the game will not work with them.

## Features

### Made for one screen

The DS shows the game on two screens. Egg of Time shows it on one by default:

- The boot logos, title screen, Game Settings and name entry are laid out on a single screen. Any button
  starts the game from the title screen.
- New games use the game's own Classic battle mode, the one-screen layout of the original SNES game.
- The two-screen layouts are still there (F2): stacked, side by side, or swapped (F3).

### Graphics

- **Widescreen**: 16:9 by default, also 5:3, 21:9 or the window's own shape. You see more of the world while
  exploring and in battle, with nothing stretched. Menus and text boxes stay in the centred 4:3 area.
- **Resolution**: 1x (256×192) up to 11x (2816×2112), or Auto.
- **Framerate**: 30 to 144 fps or unlimited. Above 60 fps, scrolling is smoothed: the camera glides between the
  game's frames. The game itself still runs at 60, so timing, animation, sound and input are unchanged.
- **Filters**: Pixel, Sharp (default), Smooth, or LCD (the DS screen's pixel grid).
- **Colour**: the DS version's colours look washed out on a PC screen. The SNES settings (light, the default;
  moderate; strong) bring them closer to the SNES look. Original keeps the DS colours.
- **Window**: windowed, borderless (default) or fullscreen; V-Sync on or off.

### Mods

- **Cutscenes**: put an MP4 video (H.264 or H.265, up to 4K) in `mods\cutscenes\` to replace one of the anime
  cutscenes, for example `op_ds.mp4` for the opening. The `readme.txt` there lists the file names.
- **Music**: put an audio file in `mods\music\` to replace a music track, for example `BGM_18.ogg` for
  "Chrono Trigger". Use OGG, Opus, FLAC, MP3, WAV or M4A, looped with `LOOPSTART`/`LOOPEND` tags, or MSU-1
  `.pcm` files. The `readme.txt` there lists every track with its Music Box title.
- Mods can be turned off in System Settings > Mods.

### System Settings

Open it with **SELECT** on the title menu or the Game Settings screen, or while the game is paused (START). Close
it with B or SELECT. Changes apply immediately and are saved in `eggoftime.ini`.

### Save states

**F5** saves and **F7** loads. There is one slot, `rom\CT.state`, next to your in-game saves. States keep working
after updates and move between Windows and Linux.

## Controls

Gamepads work out of the box. The DS face buttons map by position: A is the right button, B the bottom one.

| DS | Keyboard | Gamepad |
|---|---|---|
| D-pad | Arrow keys | D-pad or left stick |
| A / B | X / Z | East / South button |
| X / Y | S / A | North / West button |
| L / R | Q / W | Shoulder buttons |
| START / SELECT | Enter / Backspace or Right Shift | Start / Back |
| Touch screen | Mouse, when the bottom screen is shown | — |

| Key | Action |
|---|---|
| F2 | Layout: top screen only / stacked / side by side |
| F3 | Swap the screens |
| F4 | Filter |
| F11 | Windowed / fullscreen |
| F5 / F7 | Save / load state |
| Hold Esc for 1 second | Quit |

## Status

- Playable from the start: tested through the opening, the Millennial Fair, the first time warp, battles and
  Game Over. Sound, music and cutscenes work.
- Windows (clang) and Linux (gcc) are tested. macOS compiles but is untested.
- Not implemented: Wi-Fi, the microphone, and the DS 3D engine (no 3D has been seen in the tested parts).

> [!TIP]
> Found a bug? Open an issue. A save state (`rom\CT.state`) taken just before the bug helps a lot.

## How it works

The release contains a runtime (the DS hardware, written for this project) and a toolkit, but none of the game.
On the first start:

1. [ds-decomp](https://github.com/AetiasHax/ds-decomp) extracts the game's code from your ROM.
2. The recompiler (`tools/recomp/`) translates each of the game's ~9,000 ARM and Thumb functions into one C
   function.
3. clang compiles them into the game library, `game\eggoftime_game.dll`, which the runtime loads.

The runtime provides what the code expects from a DS:

- both 2D graphics engines;
- the sound driver, reimplemented so the game's own music and sound data play;
- threads, interrupts, saves, touch and the clock.

The game's anime cutscenes play through its own recompiled decoder.

[DESIGN.md](DESIGN.md) explains the architecture and the problems solved along the way.

## Building from source

For development, or to play on Linux and macOS.

### Requirements

- CMake 3.25 or newer, and Ninja.
- A C11 compiler: clang (llvm-mingw on Windows) or gcc.
- Python 3 with Capstone (`pip install capstone`).
- SDL3:
  - Windows: the SDL3 development zip.
  - macOS: `brew install sdl3`.
  - Linux: your distro's package if it has one. Otherwise CMake builds SDL 3.4.18 from source; install the
    X11/Wayland and ALSA/PulseAudio development packages first.
- FFmpeg (libavformat, libavcodec, libavutil, libswscale, libswresample), for movie and music mods:
  - Windows: a shared build such as `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` from
    [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases). The build copies its DLLs next to the
    executable.
  - macOS: `brew install ffmpeg`.
  - Linux (Debian/Ubuntu): `libavformat-dev libavcodec-dev libswscale-dev libswresample-dev`.
- [ds-decomp (dsd) v0.12.1](https://github.com/AetiasHax/ds-decomp/releases), in `tools/dsd/` or on your `PATH`.

### Steps

1. **Add your ROM** as `rom/CT.nds` (see [Your ROM](#your-rom)).
2. **Extract it.** This creates `extract/`, which the recompiler reads. It holds game data: it stays on your
   machine, **never commit it**.

   ```sh
   dsd rom extract --rom rom/CT.nds --output-path extract
   ```

3. **Build.** The build generates the C code from `extract/` and `config/` (the dsd analysis, included in this
   repository), then compiles it. The first build takes a few minutes.
   - Linux / macOS: `scripts/build.sh` (output in `build/`).
   - Windows (llvm-mingw, SDL3 zip, FFmpeg build):

     ```sh
     cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang "-DCMAKE_PREFIX_PATH=<SDL3>/x86_64-w64-mingw32;<FFmpeg>"
     cmake --build build
     ```

4. **Run.** Copy your ROM to `build/rom/CT.nds`. Then run `Egg of Time` (`Egg of Time.exe` on Windows) from
   `build/`. You can also pass a ROM path: `"Egg of Time" path/to/rom.nds`. Saves go next to the ROM.

### Build options

- `-DEGG_SINGLE_SCREEN=OFF` keeps the DS's two-screen presentation and defaults, for devices with two screens.
- `-DEGG_RUNTIME_ONLY=ON` builds only the runtime, without the game, which needs no ROM. CI and the release
  use it.

### Making a release

Pushing a `v*` tag runs `.github/workflows/release.yml`. It builds the runtime for Windows and Linux, packages
each with the toolkit (`scripts/package.py`) and publishes the Windows zip and the Linux tar.gz. The packager refuses ROMs, extracted files and generated code.

## Credits

- [ds-decomp](https://github.com/AetiasHax/ds-decomp) and [dsd-ghidra](https://github.com/AetiasHax/dsd-ghidra)
  by AetiasHax, for the analysis the recompiler builds on.
- [SDL3](https://libsdl.org), [FFmpeg](https://ffmpeg.org) (LGPL, linked dynamically),
  [minicoro](https://github.com/edubart/minicoro) (bundled in `runtime/third_party/`, under its own license),
  [Capstone](https://www.capstone-engine.org), and [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) and
  Python in the Windows release.
- Hardware details from GBATEK.
- The LCD filter is based on the [lcd3x shader](https://github.com/libretro/slang-shaders/blob/master/handheld/shaders/lcd3x.slang)
  by Gigaherz (public domain).
