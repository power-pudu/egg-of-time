# EggOfTime — static recompilation design

Goal: native Windows/Linux/macOS build of Chrono Trigger DS (USA, `YQUE`) by translating
the game's ARM9 machine code to C11 ahead of time, linked against a runtime that emulates
the DS hardware the code talks to. Rendering is enhanced (GPU, higher resolution), and the
game starts in Classic (one-screen) mode with the second screen hidden by default.

## Pipeline

```
ROM ──dsd rom extract──> extract/          (decompressed ARM9, overlays, ITCM/DTCM, assets)
    ──dsd init/delink──> config/arm9/       (function bounds, ARM/Thumb mode, relocations)
extract + config ──tools/recomp──> gen/*.c  (one C function per game function + dispatch tables)
gen/*.c + runtime/*.c ──CMake + clang──> Egg of Time(.exe)
```

The original ROM is still required at run time (assets, card reads, overlay loading).
Nothing derived from the ROM is meant to be committed or distributed: `extract/`, `config/`,
`gen/` stay local.

## Why dsd output drives the recompiler
- **Function boundaries and mode:** `symbols.txt` gives every function's address, size, and ARM/Thumb mode.
  Static recompilation needs exactly this, and dsd's analysis already handles constant pools,
  jump tables and DS Protect ranges.
- **Overlay-correct call targets:** ARM9 overlays share addresses (e.g. 0x020fd800 is used by ov000,
  ov001, ov002). `relocs.txt` records which overlay each call/load targets (`module:overlay(3)`),
  or several candidates (`module:overlays(7,13)`), which the recompiler turns into direct calls or
  runtime dispatch.

## Generated code model
- `CPU` context struct: `r[16]`, flags `n z c v q` as separate words, CPSR control bits,
  banked `sp/lr/spsr` for IRQ/SVC modes.
- One C function per game function: `void f_<module>_<addr>(CPU *cpu)`. Any game function can be
  called through the address-keyed dispatch table, so the signature is uniform.
- Intra-function branches become `goto`. Backward branches emit `RT_POLL(cpu)` (pending IRQ / frame
  pacing check).
- `bl`/`blx` to a known function: set `lr`, direct C call. Ambiguous overlay targets and register
  calls go through `rt_call(cpu, addr)`.
- Returns (`bx lr`, `pop {...,pc}`, `ldm ...,{...,pc}`, `mov pc, lr`) become C `return`. A shadow
  return-address stack in debug builds catches code that doesn't return to its caller.
- Branch to another function's start: tail call (`f(cpu); return;`). Branch into the middle of another
  function (dsd external labels): the target function gets an extra entry copy that starts with
  `goto` to that label.
- Computed jumps inside a function (`ldr pc, [pc, rN, lsl #2]`, `add pc, ...`): `switch` over the
  function's labels; unknown target → `rt_jump`.
- Literal pool loads (`ldr rX, [pc, #imm]`) are folded into constants at recompile time.
- Conditional ARM instructions → `if (COND_xx(cpu))`. Flag-setting ops call inline helpers;
  barrel-shifter carry-out is computed only when the instruction needs it.
- Memory: `rt_read8/16/32`, `rt_write8/16/32`. Fast path for main RAM, ITCM and DTCM; slow path
  dispatches to I/O, VRAM, palette, OAM and the card. ARMv5 rotated unaligned `ldr` semantics are kept.
- `tools/recomp/overrides.txt` routes calls to a game function to a runtime C function instead. With `wrap`, the
  game function is still recompiled under its usual name, for the runtime function to call (hooks).
- `swi` → `rt_swi` (HLE BIOS: Div, Sqrt, CpuSet, LZ77/Huffman/RLE, IntrWait, VBlankIntrWait, Halt, CRC16).
- `mcr/mrc p15` → `rt_cp15_*` (TCM setup, cache ops as no-ops, wait-for-interrupt).

## Overlay dispatch
Overlays are loaded by the game's own code (recompiled crt0/FS functions reading the card), so the
runtime doesn't track them through hooks. When a call can land in more than one overlay, the runtime
picks the resident one by comparing the first instruction words in RAM against each candidate's
original bytes, and caches the result per region until the region is written.

## Runtime (C11 + SDL3)
- **Boot:** emulate the post-firmware state. Load the ARM9 binary (still compressed, as on the
  cartridge) at 0x02000000, set the cartridge header copy at 0x027FFE00, then call the entry
  function 0x02000800. The recompiled crt0 decompresses itself and copies autoloads (ITCM 0x01ff8000,
  DTCM 0x027e0000).
- **Hardware emulated at register level:** IRQ controller (IE/IF/IME), timers, DMA, IPC FIFO,
  divider/sqrt units, card bus (KEY2 `0xB7` reads from the `.nds` file), keypad, 2D engines A/B,
  3D geometry/rendering engine, VRAM banking, sound channels.
- **IRQs:** delivered at `RT_POLL` points, `swi` waits and `cp15` WFI. Delivery mimics the BIOS:
  switch to IRQ mode banks, call the handler pointer at DTCM+0x3FFC, restore.
- **OS threads:** NitroSDK switches threads via `OS_SaveContext`/`OS_LoadContext`, which cannot be
  expressed as C calls. Each game thread runs on its own host coroutine (minicoro); these two SDK
  functions are replaced by coroutine switches. They are found by byte signature, as dsd does for
  `FS_LoadOverlay`.
- **ARM7:** phase 1 uses HLE of the ARM7 side of the IPC FIFO protocol (PXI): sound (SND), touch
  screen/RTC/firmware (SPI), power management. Recompiling `arm7.bin` with the same pipeline is
  phase 2 if the HLE grows too large. Its function bounds would have to come from Ghidra, since dsd only covers ARM9.
- **Video, enhanced:** both 2D engines render per scanline in software at native 256×192; `runtime/display.c`
  enhances and presents them: each window frame draws the shown screens into an internal target of N times
  the layout's native size (Auto: the next integer multiple of the size on screen; up to 11x = 2112 rows, the
  largest within 4K), then fits that target into the window, pixels kept square. Filters (F4): Pixel (nearest
  throughout, integer scaling), Sharp (nearest to the internal size, then a smooth fit), Smooth (Scale2x applied
  twice for 4× edge-smoothed art, then smooth scaling), LCD (DS) (Sharp, with the DS screen's pixel grid
  multiplied in: Gigaherz's lcd3x shader, public domain, whose factors depend only on the place within a screen
  pixel, so one pixel's worth at the internal scale is a small texture tiled over each screen with
  SDL_BLENDMODE_MOD; no shader pipeline needed, and the GPU does it on any renderer).
- **Widescreen** (Aspect ratio: Original (4:3), 5:3, 16:9 (the default), Ultrawide (21:9), Fit screen = the window's shape): shows
  more of the world instead of stretching. The display asks the renderer for margins (`video_set_wide_margin`,
  up to 96 columns a side: 5:3 = 320 columns, 16:9 = 340, 21:9 = 448); the top screen's engine draws text and affine BGs,
  sprites (X wraps at 512: beyond the left margin is the right one) and windows (their edge columns extended;
  edges the game sets line by line through its software HDMA, such as the rays of Lightning II, carried on along
  their slope, or drawn from the two straight edges fitted to the window where it is a ray shape; their bolts, a
  256-wide SNES picture, reflected at the screen sides) across them. Single screen mode uses the wide picture in
  the field only (current scene: ov000 vtable 0x02115558); other scenes stay 4:3, centred.
  - The field keeps each BG in a 512x256 map used as a ring and only keeps the 34 columns around the 4:3 view
    current (scroll handler ov004 0x0213bcb0: per layer, generator 0x02137144 / 0x02137734 / 0x02137c4c into a
    staging buffer, uploaded into the ring at origin-1 / origin+32 by ov004 0x02137e2c and main 0x02046754).
    `runtime/widescreen.c` writes the margin columns itself before each frame, from the room map (metatile ids
    at `*(0x0207b518)` + 0x1000 + 0x40 * layer, rows 0x100 apart, 9th id bit at +0x10c0), the metatiles
    (`*(*(0x020708d4) + 0xc / + 0x10)`, 2x2 tiles of 3 bytes) and the scroll state `*(0x020708e0)` (camera column
    +4, row +8, map masks +0x1a, ring origins +0x8e); layer 2 goes to BG0 (palette through the table at
    0x02058714), layers 0 and 1 to BG2 and BG3, their priority tiles to BG1. That model reproduces every cell the
    game itself drew in the Millennial Fair. A layer is filled only while its BG scroll agrees with its ring origin.
  - Characters: two culls kept them inside the 4:3 view. (1) The field object loop (ov004 0x02130a10) draws an
    object only while ov004 0x02130dbc finds its tile within the camera columns `*(0x020708e0)+4 - 4 ..
    +6 + 4` (and rows); those fields are the camera's own, used by map streaming too, so the wrapper
    `rt_ws_object_visible` (`wrap` override) widens them by the margins only for the call. (2) NitroSDK's G2D
    renderer culls sprites to its view (top screen surface at 0x02058070: x, y, width, height in 20.12; normally
    0, -1, 256, 192) and places them relative to its top-left; in the field it is set to -margin, 256 + 2 *
    margin, so sprite X counts from the left margin (the renderer reads the view back, `ws_sprite_origin`), and
    put back elsewhere (also after loading a state saved with it widened).
  - Battle UI: battles run in the field scene, and their menus are sprites in screen coordinates (boxes with
    their text, the command cursor), while gauges, damage numbers and characters are world sprites. The scene's
    draw (main 0x02041940) uses one renderer surface (scene secondary base `*(0x0207088c)` + 0xd748): it saves
    the surface's own view (0, 0, 1024 wide: screen space, no culling), draws world sprites with the G2D view
    above, and puts the own view back for the screen-space ones. In the field the runtime starts that own view
    at -margin too (`set_screen_view`), so the UI keeps its 4:3 place, centred, lined up with the world sprites
    (found with a write watch on the OAM buffer at 0x0224a418, `EGG_WATCH_WRITE`).
  - Battle margins: a battle keeps the room on screen but reuses the room map (0x0207b518 -> + 0x1000, the
    metatile rows) for its own data, so filling the margins from it showed tiles from elsewhere. The runtime copies
    the room map's rows each frame while the field runs and fills from that copy while ov003 is resident, and after
    the battle until the field has put the map back (it matches the copy again; or the room changes, or 600 frames
    pass). Save states keep the copy in an optional last block (`ws_room_copy`, `rt_state_optional`): states
    saved before it load without it, and a battle loaded from one of those shows the reused data in the margins.
  - World map (ov002, scene vtable 0x02120a68): its own streaming (ov002 0x0211ac64, writers such as 0x0211af10,
    found with a write watch on a BG3 map cell) keeps 17 of the 32 metatile columns of each 512x256 BG1-3 ring
    current. The world object `*(0x0207088c)` + 0xddc0 has per layer the view's world position (+0x22a4 / +0x22ac),
    the 96 x 64 metatile map (+0x22b4) and the 2x2 metatile sheet (+0x2a4 + 0x800 * layer); the runtime fills the
    margins' columns from them (checked against every cell the game drew). Its sprites go through the game's
    SNES-style code (ov005 0x0212bcdc), which keeps a piece while X + 15 < 0x10f, writes a SNES OAM table with
    9-bit X (`*(0x0207b520)` + 0x700, high bits + 0x900), and ov002 0x0211cb44 / 0x0211cbb4 turn it into DS
    sprites reading X 256-511 as negative. The SNES 16-bit compare (main 0x020305ac) is replaced by
    `rt_ws_snes_cmp`, which widens that one test (return address 0x0212c0e4) by the margins, and the wrapper
    `rt_ws_world_sprite` (ov002 0x0211cc20) puts an X below -128 back at 256 and up: the right margin. Objects
    also keep an on-screen flag (ov002 0x0210cd24: X - camera + 16, negative values wrapped at 0x600, below
    0x120; the birds go away once it clears), widened the same way at its compare (return address 0x0210ce50).
  - Text boxes: for the box lines the game switches BG0/BG1 mid-frame to one-screen (256-wide) maps (BG0CNT
    0x0818, BG1CNT 0x0914, VOFS 384), which would repeat in the margins; the renderer draws a 256-wide text BG
    in the screen columns only. In the margins of those lines it keeps the layer as the frame started (BGxCNT,
    scroll, priority and BLDCNT/BLDALPHA from line 0, `margin_bg`), so the room goes on beside the box.
  - Not handled: rooms smaller than the wide view show what lies beside them in the map (other rooms of the same
    building, or void); the room's camera bounds were not found yet (the limits 0x02143fb0 the object cull clamps
    to hold the whole map, 0-255), so the margins are not masked. The game draws no 3D: the SDK sends a few housekeeping GX commands per frame but no geometry,
  and the 3D BG is off in the title, intro, world map, towns and the first time warp (an affine BG3 tunnel). The
  3D layer was seen on only briefly, while loading a save into the world map, so no 3D renderer exists yet.
- **Screens:** layouts (F2): top screen only (the default in single screen mode; it shows the main screen
  below), stacked (the default with single screen mode off), side by side; F3 swaps the screens
  (with top-only: bottom screen alone); F11 toggles between windowed and the last full-screen mode
  (`EGG_WINDOWED=1` starts windowed without changing the setting, for automated runs). Each change shows a short message in the window; Sharp is the default filter. Holding Escape shows
  "Exiting..." and quits after 1 s (releasing cancels). The mouse is the stylus wherever the bottom screen is
  shown. Layout, swap and the System Settings persist in `eggoftime.ini` next to the executable.
- **System Settings menu** (`runtime/system_menu.c`, settings in `runtime/display.c`): SELECT opens it on Game
  Settings, in the title screen's menu (phase 7; no hint is shown there) or while the field is paused
  (`ss_menu_place`), B or SELECT closes it (closed from the pause, START is held for the game for 100 ms, which
  ends the pause), and so does the game leaving that place (the title idling into the intro); Up/Down choose, Left/Right change (held: repeat
  after 400 ms, then every 110 ms); changes apply at once. While it is open the game gets no keys, and keys
  still held when it closes reach the game only after they are released (B would otherwise leave Game
  Settings). It is drawn over the main screen at native resolution, so the filter and internal resolution
  apply to it, in the look of Game Settings re-created by the runtime (bevel profiles and colours measured
  there: light bars, dark buttons, lit label and teal frame on the cursor row, striped backdrop, B sign),
  so it looks the same from the pause; text in the runtime's small font (`runtime/ui_font.c`).
  - Pause (field, ov000 func 0x0210155c, START): the game dims both screens (MASTER_BRIGHT darken 4) and loops
    waiting for START, with the byte at `*(0x0207b520) + 0x407` set to 0xff; single screen mode then writes
    "Press SELECT to open System Settings" at the bottom of the main screen.
  - Window mode: windowed; borderless windowed (SDL fullscreen at the desktop mode, the default); fullscreen
    (exclusive, at the closest mode to the desktop's).
  - Framerate paces only the host loop's presentation (`display_frame_ns`, `SDL_DelayPrecise`): the game thread
    keeps its emulated 60 Hz clock. Measured with `EGG_LOG_FPS=1`: 30/60/90/144 exact, unlimited ~1000-2000 with
    V-Sync off, the display's rate with it on; the game stays at 60 in every case.
  - Camera smoothing (framerates above 60): each published frame carries how far the field camera moved
    (`video_frame_serial`; camera at 0x020760f8, steps over 8 pixels count as cuts). The host shows the field
    picture slid back by that step times the share of the game frame still to come, timed from the frame's
    arrival and the measured game frame period, so the view moves on every window frame at even pace whatever
    the refresh rate, in sub-pixel steps of the internal resolution (one game frame of extra delay). Background
    and sprites slide together (nothing is blended or redrawn), except the party layer: sprites the camera follows
    (the party) hold still on screen while the world moves, so sliding them would shake them back and forth. A
    sprite counts as followed when, matched to the previous frame's OAM (same tile and palette, shape and size,
    nearest), its screen step is smaller than its step in the room (screen step plus camera step). The renderer
    leaves those out of the field picture and composes them a second time into a transparent layer (only where
    they win the top layer, so backgrounds in front still cover them), and the host draws it over the slid
    picture without the slide (`video_copy_frame`'s party output). The text box is screen-fixed too: on its
    lines (BG0/BG1 switched to one-screen maps, see Text boxes) its pixels go to the party layer, and the slid
    picture shows the room under it (those layers as the frame started), so the box holds still while walking
    with it open. The renderer draws 8 hidden
    columns each side for it (`VIDEO_SLIDE_MAX`, part of the widescreen margins); a vertical slide repeats the
    edge row for its pixel or two. Measured at 144 Hz while walking: the view advances ~0.83 px per window
    frame (the game's 2 px per frame over 2.4 refreshes), the game at 60. (Frame interpolation, blending
    sprites between frames, was tried first and dropped: it did not look right.)
  - Color (`color` in the ini: ds, light, moderate, strong): raises each channel of every uploaded screen to
    1.0, 1.1 (default), 1.2 or 1.4 through a 256-entry table, before the filters; black and white stay.
    (The full 2.4 / 1.5 ratio of the SNES on a CRT to the DS screen, 1.6, was tried: it crushes shadows.)
  - Mods: replacement cutscenes on (the default) or off (movie mods, below).
- **Single screen mode** (CMake option `EGG_SINGLE_SCREEN`, default ON; `runtime/single_screen.c`): the game laid
  out for one screen. `video_end_frame` builds a third output, the main screen, which the top-only layout shows;
  the other layouts show both DS screens as they are. The running scene is the game's: `Scene::run`
  (main 0x0202514c), the loop every scene runs in, stores the scene's secondary base (scene + 8) at 0x0207088c
  (func 0x02025d30) for the VBlank handler. Its vtable names the scene (ov007: 0x0212ad68 logos, 0x0212aed4
  title, 0x0212b300 Game Settings). Being game memory, it is also right after loading a save state. The front
  end (main 0x0204bfcc) creates the scenes in turn: logos, intro movie (ov017), title, Game Settings; all of
  them are allocated at the same heap address.
  - Logos: "Licensed by Nintendo" is on the bottom screen alone (shown as it is while the top screen is blank);
    then Square Enix (top rows 85-107) and Mobiclip (bottom rows 50-141) on one backdrop, shown stacked.
  - Title: the scene's state is the halfword at scene + 0x4a (+0x48: requested state): 8 = "Touch to Start",
    0x1e = menu. The copyright is engine B BG2, which the game turns on once the logo has formed. The main
    screen is the top screen plus the copyright (bottom rows 156-185, moved down 6 rows) and, in state 8,
    "Press any button to start" in a small built-in font, blinking with the game's prompt (its sprites are
    turned off and on) and following the bottom screen's fade; in the menu, the bottom screen's buttons
    (whatever rows they use above the copyright) are moved into the space left of the pendulum. In state 8
    any button pressed after the title started also presses START (`ss_game_keys`, applied to KEYINPUT and
    the X/Y buffer), which opens the menu, as a touch does.
  - Game Settings (after New Game): options on the bottom screen (rows: 0 Game Mode, 1 Battle Mode, 2 Movies;
    cursor row at scene + 0x3f00, the row shown +0x3f01, the column +0x3f02), the highlighted option's title and
    description box on the top screen. The main screen stacks full-width bands: the header, Battle Mode and
    Movies with the description box (top rows 119-184) under the highlighted one, the SELECT/START buttons and
    the footer. Game Mode is left out, since single screen mode plays Classic: while the cursor is on row 0
    the game gets a pulsed Down, and Up/Down wrap between rows 1 and 2 (the direction is decided when a press
    starts). SELECT, the game's "Defaults", is withheld from the game and its button reads "System settings"
    (painted over with the button's own texture, built-in font): it opens the System Settings menu.
  - Name entry (ov012, vtable 0x02156880, built only at 0x021224cc): the character on a patch of grass on the
    top screen; the name, portrait, keyboard and buttons fill the bottom screen. The word at scene + 0x4e48 is
    the scene's state: 0 while the character, the floor and the keyboard fade in, 1 entering the name, 2 after
    START (the keyboard and floor fade out while the character animates). The main screen shows the bottom
    screen in state 1, the top screen otherwise.
  - Shops (ov012, vtable 0x021575f0): every member's attack and defense (now and with the highlighted item) on
    the top screen (the party at the left, the others at the right, up to four each), everything else on the
    bottom one, whose help bar stays empty. The main screen shows the bottom screen moved up into the gap under
    the tabs and, in place of the help bar, a 3 x 3 grid of text lines (the party first): each member's name and
    only the stat the item changes (defense when only defense values are coloured, else attack), old -> new.
    The shop's Equip opens the field menu's Equipment page.
  - Classic battles by default, below.
- **Classic mode default** (single screen mode): the game's own "Game Mode: DS / Classic" option (New Game
  settings scene, ov007) defaults to Classic. `func_ov007_0212a3f0`, which resets the options when the scene opens and on
  "Defaults", is wrapped by `rt_ct_settings_defaults` (`runtime/patches.c`, `tools/recomp/overrides.txt`),
  which calls the original when single screen mode is off.
  The options are bits of the save configuration at `*(0x020708d0)`: Game Mode byte 2 bit 7 (1 = Classic),
  Battle Mode byte 0 bit 6 (1 = Wait), Movies byte 2 bits 3-4 (0 = On).
- **Audio:** the ARM7 sound driver (NitroSDK SND) is emulated at a high level in `runtime/snd.c`. It takes the
  ARM9's command lists from PXI and runs sequence players (SSEQ with SBNK instruments and SWAR waves, read from
  main RAM), ex channels (ADSR envelope, LFO, sweep/portamento, priority allocation), raw channel setup for
  streams, alarms, and the shared work block (player/channel status, sequence variables, tick counters). A
  driver frame runs every 2728 × 64 bus cycles (5.2 ms), as on hardware. `runtime/spu.c` mixes the 16
  channels (PCM8/16, IMA-ADPCM, PSG square, noise) at 48 kHz float with linear interpolation and queues the
  output to an SDL3 audio stream (40-150 ms latency). Command lists run as soon as the ARM9 sends them, so the
  shared player/channel status is refreshed before each list's finishCommandTag is advanced: NNS checks a started
  player's status once its tag finishes and releases the handle of a player not yet marked active, after which
  all volume changes for that song are dropped (music cut off instead of fading; Game Over music stayed silent).
- **Video playback:** the Mobiclip `.mods` cutscenes are decoded by the recompiled Mobiclip
  SDK code itself. That code is ordinary ARM9 code, so it needs no special handling.
- **Movie mods** (`runtime/movie.c`, FFmpeg): `mods/cutscenes/<name>.mp4` next to the executable replaces the ROM's
  `/movie/<name>.mods` while Mods is on (the build creates the folder and copies `res/mods/cutscenes/readme.txt`,
  the list of names). Every cutscene is played by ov017 0x02198d20 (r0 path, r1 per-frame callback; the in-game
  movies, main 0x02045c3c with the movie table at 0x02064968) or 0x02198da0 (also r1 start / r2 per-frame / r3 end
  callbacks with the scene's data at [sp]: the intro, the Extras theater, the ending variants); both are wrapped
  and return after the replacement without running the game's player. The emulated machine pauses meanwhile (the
  scheduler and sound driver skip the time, as after a host stall). The 0x02198da0 start callback must still run:
  the intro's sets the scene's result (data + 0x7a0 = 3), which the front end reads next (without it, a black
  screen); the end callback frees what it set up; the per-frame one (bottom-screen animation) is left out.
  Skipping (START or a touch, as the game's per-frame callbacks do; for the intro, whose callback ov017 0x02199930
  takes any newly pressed button (mask 0xfff, 0x02199894), any button) sets their "skipped" byte at 0x0207b3d0.
  Decoding: the codec's hardware decoder (D3D11VA, VideoToolbox, VAAPI, CUDA, ... in that order) or software on
  every core; frames are copied to YUV420P/NV12 (others, e.g. 10-bit, through swscale) and drawn by `movie_draw`
  over the whole window instead of the screens. Pacing follows the audio played, falling back to the wall clock
  while under 20 ms is queued: SDL's resampler keeps a few samples until flushed, and waiting on them stalled
  playback. Not handled: HDR (PQ/HLG) colors are shown as SDR.
- **Music mods** (`runtime/music.c`, FFmpeg): `mods/music/BGM_xx.<ext>` replaces the sound archive's sequence
  `BGM_xx` (symbols from `/sound/sound_data.sdat`; 97, BGM_00-BGM_60) while Mods is on. The driver (`snd.c`) only
  gets a sequence's data address, so `player_prepare` identifies it by comparing that RAM with each BGM
  sequence's data in the ROM's SDAT (found by its header; INFO for the file and volume, FAT for the data). A
  replaced player keeps running its sequence with its channels at VOL_MIN (status, variables and tick counts stay
  the game's); the replacement plays while the player is active, started and not paused, at the player's volume
  (player volume + extFader, which NNS fades use) relative to the archive's volume for the sequence, and stops
  with it (so jingles end with the game's). Decoded on the game thread inside `spu_render`, resampled to 48 kHz
  stereo, mixed before the master volume. Loop: LOOPSTART..LOOPEND/LOOPLENGTH tags in samples (else the whole
  file); MSU-1 `.pcm` through FFmpeg's s16le demuxer (8-byte header skipped), looping at its end to the header's
  point. After a state load the active players' replacements restart. Track titles in
  `res/mods/music/readme.txt` come from the Music Box: titles in `/msg/big/bgm.msg` (one byte per character in
  the game's font order; English is every 4th entry from 1, French follows each) and the BGM numbers in the u32
  table at main 0x020558d4 (also in ov009); 28 of the 97 are not in the Music Box.
- **Save states** (F5 save, F7 load; one slot: `<rom>.state` next to the ROM; `EGG_LOAD_STATE=<file>` starts
  from a state): `runtime/state.c`. A state holds emulated state only (memory, device state, and per game thread its
  ARM registers and the address it continues at), so it stays loadable after rebuilding, as long as the tagged
  blocks keep their sizes; loading checks every block before changing anything. Every generated function can be
  re-entered (`cpu->resume_pc`, a switch over its labels: call return sites, loop heads, SWI/WFI, MSR). A loaded
  thread starts a fresh coroutine at its address; returns beyond it reach its emulated callers through
  `rt_dispatch`, which re-enters each at the return address. Saving waits for a poll where the running thread is
  outside interrupt handlers (usually within a frame). Known limit: a state saved during a Mobiclip movie loses the
  movie decoder's setjmp unwind points.
- **Anti-piracy (DS Protect):** the game's checks pass because the runtime behaves like a retail DS, not through
  code patches. DS Protect (encrypted at rest, decrypted around each call; dsd decrypts it for analysis, so the
  generated code is correct) has two detectors behind four entry points:
  - `func_0204e694` reads cartridge addresses 0, 0x200, 0x400, 0x8000, 0x8200, 0x8400 with raw B7 commands; a retail
    card redirects reads below 0x8000 to 0x8000-0x81FF (`card.c`), a flashcart does not.
  - `func_0204e334` reads the console MAC address (`OS_GetMacAddress`, 0x027FFCF4) and owner info; an all-zero MAC,
    or the emulator MAC 00:09:BF:00:00:31 with a 1/1 birthday and no nickname, means "emulator". The runtime now
    writes a MAC at boot (`rt_write_console_mac`) and into loaded states that lack one.
  Entry points: `func_0204e7a4` / `func_0204e880` use the cartridge check (main 0x0202ef00, ov000 0x0210c090 /
  0x0210c210, ov004 0x0212769e, ov010 0x021222e0); `func_0204e95c` uses the emulator check (ov000 0x020fe20e, and
  ov004 0x0212fa00, the time-warp script command: on failure it skips `func_ov004_0212bb6c`, which selects the
  normal warp routines 0x0204eb04.., leaving the copies 0x0204f990.. that loop forever); `func_0204ea38` has no
  detectors. The field trap (ov000 0x020fe118) swaps an in-memory object and does not reach the save file.
- **Saves:** cartridge backup (`[SDK+NINTENDO:BACKUP]`) is emulated via the SPI backup protocol
  and stored as a file next to the executable.

## Build
- CMake ≥ 3.25 + Ninja, clang or gcc (llvm-mingw on Windows), SDL3, Python 3 + capstone. SDL3 comes from an
  installed package (Windows SDK zip, Homebrew, distro); if none is found CMake builds SDL 3.4.18 from its pinned
  source release (e.g. Ubuntu 24.04, which has no SDL3 package; that needs the X11/Wayland and ALSA/PulseAudio
  development packages for a window and sound).
- Linux/macOS: `scripts/build.sh [build-dir]` (default `build/`), then run `"./Egg of Time"` from the build directory with
  the ROM in its `rom/` folder.
- The recompiled code is a library of its own: `game/eggoftime_game.dll` (`.so` elsewhere, a CMake `MODULE`) next to the
  executable, which loads it at start (`rt_game_load`, `runtime/rt_dispatch.c`). It imports the runtime
  functions and data marked `RT_API` in `runtime/recomp.h` from the executable (built with `ENABLE_EXPORTS` and,
  on Windows, `--export-all-symbols`, which also writes its import library `libeggoftime.a`), and exports one
  `RtGame` descriptor, `egg_game` (`runtime/dispatch.h`, generated in `gen/dispatch.c`): `RT_API_VERSION`, the
  module and function tables, the ARM9 entry point and the recompiled originals of `wrap` overrides, which the
  runtime reaches through `rt_call_wrapped`. Measured: no CPU cost against the single executable (the game's own
  Mobiclip decoder, 15-20 s runs, within run-to-run noise).
- `-DEGG_RUNTIME_ONLY=ON` builds only the executable, with `-Werror`, without the ROM-derived inputs; CI uses it
  (`.github/workflows/runtime.yml`, Linux and macOS; `release.yml`, Windows), because CI machines must not hold
  ROM-derived data.
- **Release builds** (Windows, Linux): the release holds the executable and `kit/`, and builds the game library on the
  player's machine from their ROM on the first start (`runtime/game_build.c`): it finds `rom/CT.nds` or asks for
  the ROM (a file dialog; copied to `rom/CT.nds`), checks its SHA-1, then runs `kit/dsd/dsd.exe rom extract`,
  `kit/python/python.exe kit/recomp/recomp.py kit <gen> --no-comments --extract <dir>` (no capstone needed
  without the comments), one `kit/llvm/bin/clang.exe` per core over the generated files (the developer build's
  flags without `-g`) and the link against `kit/lib/libeggoftime.a`, showing progress in the window, the tools'
  output in `game/build.log`. `game/build.id` holds `kit/kit.id` (a hash of the recompiler, config and
  interface headers, written by the package script) and the ROM's SHA-1: a release with other ones builds
  again, one with the same ones starts at once. The temporary `game/build/` (extract, C, objects) is deleted.
  `scripts/package.py` assembles the release: the runtime build, the recompiler and `config/`, dsd, embeddable
  CPython 3.11, and the part of llvm-mingw the build needs (found by asking clang for the headers and lld for
  the libraries it reads: 50 files), third-party licenses; it refuses generated code, extracts, ROMs and saves.
  `.github/workflows/release.yml` builds and publishes it for a `v*` tag. The Linux release (built on Ubuntu 22.04
  for glibc 2.35) bundles only dsd in the kit and uses the system's `python3` and `cc`/`clang`/`gcc` from `PATH`
  (a screen names the packages when one is missing): bundling clang would still need the distro's libc headers
  and crt files. The game library is compiled with `-fPIC` and linked without an import library, its runtime
  symbols resolved from the executable (`ENABLE_EXPORTS`, i.e. `-rdynamic`). SDL3 is linked statically and
  BtbN's FFmpeg shared libraries sit next to the executable (RPATH `$ORIGIN` only); the package is a `.tar.gz`
  so the executable bits survive. Measured in WSL Ubuntu 24.04: 66 MB download, first start 120 s. Measured: zip 125 MB (FFmpeg 112 MB
  of DLLs, the kit 171 MB unpacked), first start about 3 minutes on this machine, later starts 2 s. Zig as the
  bundled compiler was dropped: `zig cc` compiles at -O2 with debug info for the host CPU, and took 21 minutes
  (0.14.1) / 8 minutes (0.17.0) for the largest file, against 106 s with llvm-mingw's clang 23.
- CMake runs `tools/recomp/recomp.py` at configure time (and again when the dsd config or recompiler
  changes). Output goes to `<build>/gen/`, split into ~250-function chunks that compile in parallel.
- Windows (this machine):

```sh
export PATH="$LOCALAPPDATA/Programs/llvm-mingw-20261020-ucrt-x86_64/bin:$APPDATA/Python/Python311/Scripts:$PATH"
cmake -S . -B "$LOCALAPPDATA/EggOfTime-build" -G Ninja -DCMAKE_C_COMPILER=clang \
      -DCMAKE_PREFIX_PATH="$LOCALAPPDATA/Programs/SDL3-3.4.18/x86_64-w64-mingw32"
cmake --build "$LOCALAPPDATA/EggOfTime-build"
"$LOCALAPPDATA/EggOfTime-build/Egg of Time.exe" [rom.nds]   # run from the project root
```

  For testing, the same commands with `-B build` put `build/Egg of Time.exe` (and `SDL3.dll`) in the project; it can
  be started by double-click when `build/rom/` holds `CT.nds` (the default ROM is `rom/CT.nds` next to the
  executable, else relative to the working directory). Alongside dsd's `build/delinks`, that adds
  about 60 MB of generated C and a 140 MB executable to the OneDrive folder.
- Controls: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Right Shift/Backspace = Select,
  F2 = layout, F3 = swap screens, F4 = filter, F11 = fullscreen, F5 = save state, F7 = load state, hold Esc = quit. Gamepads use button positions (east = A, south = B,
  north = X, west = Y), shoulders = L/R, Start/Back, D-pad or left stick. X/Y reach the game through the ARM7's
  `HW_BUTTON_XY_BUF` (0x027FFFA8, X = bit 10, Y = bit 11), which `PAD_Read` ORs into KEYINPUT.
- Video timing: each scanline steps through start (VBlank/VCount IRQs) -> draw -> HBlank (IRQ, HBlank DMA), with
  VCOUNT reading that line throughout. Lines follow host time, but a step waits while an enabled interrupt is pending
  (IE & IF, also while CPSR/IME mask it) or while the CPU is in an interrupt handler, up to 64 lines behind (so code
  waiting on VCOUNT with interrupts off still sees time pass). The game's text boxes are a mid-frame split fed by
  HBlank DMA tables that the VBlank handler re-arms each frame; without this ordering the split jittered by up to
  ~30 lines (sprites vanished every other frame), then by one line (the box's top row flickered).
- Audio: `EGG_MUTE=1` skips the audio device; `EGG_WAV=<file>` records the mixed output.
- Debug environment variables: `EGG_LOG_IO=1` logs the first read/write of each I/O register; `EGG_LOG_FPS=1`
  logs window and game frames per second;
  `EGG_LOG_PXI=1` logs ARM9 -> ARM7 messages and sound commands; `EGG_WATCH=1` logs the running game
  thread's return-address stack every 2 s; `EGG_DUMP_RAM=<file>` writes main RAM to a file 3 s after boot.
- `python -I tools/recomp/disfunc.py . <module> <addr>...` disassembles the dsd function containing an
  address, with relocation targets (useful together with `EGG_WATCH`).
- The ARM7 BIOS in `rom/` is only needed for byte-exact `dsd rom build`; the runtime emulates the ARM7 at a
  high level and does not execute ARM7 code.

## Phases
1. **Done.** Recompiler covers the full ARM9 instruction set used by the game; generated code compiles.
   8,806 functions, 542,549 instructions. The only unimplemented instructions are 3 user-bank `ldm/stm ^`, in
   `f_itcm_01ff8058` and `f_main_02007924` (OS thread context save/load, to be replaced by coroutines).
   Boot check: crt0 decompresses the ARM9 binary and the result matches dsd's `arm9.bin`. The only differences
   are the cleared compressed-size field and the still-encrypted DS Protect ranges.
   Execution then stops in the NitroSDK PXI handshake, polling `IPCSYNC` (0x04000180) for the ARM7.
2. **Done.** Runtime memory map, boot, card reads, IRQ/timers/DMA, threads, PXI handshake; game reaches its main loop.
   The game boots, loads overlays from the card, formats/loads its 64 KB EEPROM save (`<rom>.sav`) and runs
   its per-frame loop at 60 Hz (touch panel polled, RTC read) without errors for 40+ s. What it implements:
   - `sched.c`: host-clock time base, scanline events (VBlank/HBlank/VCount IRQs), timers with cascade,
     DMA (immediate, VBlank, HBlank, card, GX FIFO), Halt/IntrWait. A ticker thread requests polls every 0.5 ms.
   - `hw.c`: I/O dispatch, divider and square-root units, KEYINPUT, GXSTAT (idle).
   - `pxi.c` (ARM7 HLE): IPCSYNC handshake, FIFO, callback-ready mask; RTC (host clock), touch panel
     (not touched), sound commands acknowledged (no audio), CARD library requests.
   - `card.c`: ROM reads (`B7`), chip ID, card DMA; EEPROM backup (read/write/program/verify).
   - `thread.c`: one minicoro coroutine per NitroSDK thread, replacing `OS_SaveContext`,
     `OS_LoadContext` and the IRQ thread switch (`tools/recomp/overrides.txt`).
   - BIOS HLE matches DS semantics: decompression refuses sources below 0x02000000, which turns the game's
     per-frame `svc #0x123456` (semihosting call) into a no-op, as on hardware; SWI 12h/15h use read callbacks.
   - Recompiler: returns check the expected return address (`RT_RETURN`); MWCC jump tables are bounded by
     the condition codes of `cmp`/`b<cond>`.
3. **Mostly done.** 2D video + input; title screen visible. VRAM banking, both 2D engines per scanline
   (text/affine/extended/bitmap BGs, sprites, windows, blending, master brightness, VRAM display mode),
   keyboard/gamepad/mouse-touch, Classic one-screen default. Intro movie
   (Mobiclip, recompiled from runtime-copied ITCM code via `extra_code.txt`) plays, paced by sound alarms;
   title screen, "New Game" (tap twice: select, then confirm), Game Settings and name entry all work.
   Fix along the way: `rt_poll` re-enters the IRQ handler while interrupts remain pending; the SDK handler
   services one source per entry, and enabled HBlank IRQs otherwise starved the IPC (touch reply) IRQ.
   Debug aids: `EGG_SHOTS=<dir>` (top, bottom and main screen stacked; with `EGG_SHOT_MS=<interval>`, `EGG_SHOT_FROM=<ms>`; `EGG_SHOT_MS=1` = every frame), `EGG_AUTOKEYS`, `EGG_TOUCH_POS="x,y;x,y..."` (one per TOUCH),
   `EGG_DUMP_AT="ms,ms..."`, `EGG_LOAD_STATE=<file>`, autokeys `SAVESTATE`/`LOADSTATE`, `EGG_SELFTEST=1`.
   Classic mode: the game's own option "Game Mode: DS / Classic" on the Game Settings screen shown after
   New Game (bottom-screen buttons at about (163,33) DS and (219,33) Classic) now defaults to Classic (see
   Runtime). Verified: the scene opens on Classic with its description, DS can still be picked, "Defaults"
   returns to Classic, and the choice survives Accept. Found with alternating in-scene RAM dumps plus
   `EGG_WATCH_WRITE=<hex addr>`, which logs every CPU store to that word with the return-address stack.
4. **Done (sound).** The SND driver HLE and mixer above. Verified by recording (`EGG_WAV`) and screenshots: the intro
   movie's PCM16 stream, the title music, menu sounds, the opening world-map music and the pendulum ambience in
   Crono's house; the output is tonal at musical pitches and never clips. Not implemented (logged once if used):
   raw PSG/noise channel setup, sound capture (used for reverb), driver info reads.
   Recompiler fix found on the way: Thumb `bl` into the middle of its own function is a long branch (beyond the
   ±2 KB of `b`, e.g. to a shared epilogue), now translated as `lr = ret; goto`; this crashed new games when
   the opening scene started.
5. **Done.** Enhanced 2D presentation (Pixel / Sharp / Smooth / LCD filters) and screen layouts (top only, stacked,
   side by side, swap), persisted in `eggoftime.ini`; checked with window captures (`EGG_SHOTS` now also saves
   `window_NNNN.bmp`). 3D rendering was dropped from this phase after measuring that the game draws no 3D.
6. **Done (Linux verified; macOS: runtime compiles in CI, full build untested).** Portable CMake (SDL3 package or pinned source build, libm on Unix),
   `scripts/build.sh`, and a runtime-only CI workflow for Linux and macOS (`EGG_RUNTIME_ONLY`; dormant until the
   project is pushed to GitHub). Verified on Ubuntu 24.04 (WSL, gcc 13, SDL built from source with
   `-DSDL_UNIX_CONSOLE_BUILD=ON` because the X11/Wayland headers were not installable there): boots at 60 fps, plays
   the intro movie, and loads a save state made on Windows (states are portable). gcc warnings in the runtime: none.
   Fix found on the way: calls in hand-written code kept a `setjmp` (and `jmp_buf`) in the huge any-entry
   function; gcc does not compact the frames of functions that call `setjmp` (9 KB per Mobiclip decoder frame vs
   712 bytes with clang), and the decoder's nested returns overflowed the 8 MB coroutine stack. The `setjmp` now
   lives in `rt_call_unwindable` (frame: 24 bytes with gcc).
