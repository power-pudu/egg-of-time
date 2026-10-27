#!/bin/sh
# Build EggOfTime on Linux or macOS into build/ (or the directory given as the first argument).
#
# Needs the dsd project (extract/ and config/, see NOTES.md), CMake >= 3.25, a C11 compiler (clang or gcc),
# Python 3 with capstone (pip install capstone), and SDL3: Homebrew `brew install sdl3` on macOS; on Linux
# an SDL3 package if the distro has one, otherwise CMake builds SDL 3.4.18 from source, which needs the
# X11/Wayland and ALSA/PulseAudio development packages for a window and sound. Ninja is used when installed.
#
# Run the game from the build directory with the ROM in its rom/ folder:  cd build && "./Egg of Time"
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-"$ROOT/build"}

command -v cmake >/dev/null || { echo "cmake not found" >&2; exit 1; }
PYTHON=$(command -v python3 || command -v python)
"$PYTHON" -c "import capstone" 2>/dev/null || { echo "Python module capstone missing: pip install capstone" >&2; exit 1; }
[ -d "$ROOT/extract" ] && [ -d "$ROOT/config" ] || { echo "extract/ and config/ missing: see NOTES.md" >&2; exit 1; }

GENERATOR=
command -v ninja >/dev/null && GENERATOR="-G Ninja"
# shellcheck disable=SC2086
cmake -S "$ROOT" -B "$BUILD" $GENERATOR -DPython3_EXECUTABLE="$PYTHON"
cmake --build "$BUILD" --parallel
echo "Built $BUILD/Egg of Time"
