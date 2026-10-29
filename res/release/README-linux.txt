Egg of Time
===========

A native PC port of Chrono Trigger (Nintendo DS), made by static recompilation.

This download contains no part of the game. You need your own dump of Chrono Trigger (USA) for the
Nintendo DS (game code YQUE, SHA-1 11f9e8204e484eab42181b210c9f214584be75bf).

Getting started (Linux x86-64)
------------------------------
1. Install Python 3 and a C compiler, if you do not have them:
     Debian/Ubuntu: sudo apt install python3 build-essential
     Fedora:        sudo dnf install python3 gcc
     Arch:          sudo pacman -S python gcc
2. Extract this folder anywhere you can write to:  tar xzf EggOfTime-*-linux-x64.tar.gz
3. Start "Egg of Time" (cd "Egg of Time" && "./Egg of Time") and choose your ROM file (.nds).
   It is copied to rom/CT.nds. You can also pass the ROM:  "./Egg of Time" path/to/rom.nds
4. The first start builds the game from your ROM: a few minutes, with a progress bar. Later starts
   begin at once. A new version of Egg of Time builds it again when it needs to.

The build uses kit/dsd and your system's python3 and cc (or clang or gcc); nothing is installed and
nothing is downloaded. What it makes stays in game/. If it fails, game/build.log has the details.
The file dialog needs xdg-desktop-portal or zenity; without one, pass the ROM on the command line.

Your saves (rom/CT.sav), save states (rom/CT.state, F5 / F7) and settings (eggoftime.ini) stay in
this folder. To update, extract the new version over this one.

Controls: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Backspace = Select,
F2 layout, F3 swap screens, F4 filter, F11 fullscreen, hold Esc to quit. Gamepads work too.
Mods: replacement cutscenes and music go in mods/cutscenes and mods/music (see the readme files there).

This is an unofficial fan project, not affiliated with or endorsed by Square Enix or Nintendo.
Chrono Trigger is a trademark of Square Enix. Third-party licenses are in kit/licenses.
