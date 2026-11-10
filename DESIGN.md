# EggOfTime — static recompilation design

Goal: native Windows/Linux/macOS build of Chrono Trigger DS (USA `YQUE`, Europe `YQUP`, Japan `YQUJ`) by translating
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
  256-wide SNES picture, reflected at the screen sides; only unscrolled: the ice of Ice II, one picture on such a
  map slid onto the enemy by the scroll, landed at scroll 0, 464 and was reflected beside itself) across them. Single screen mode uses the wide picture in
  the field (current scene: ov000 vtable 0x02115558), the warp (ov010), the world map (ov002) and the race (ov011);
  other scenes stay 4:3, centred.
  - A 256x256 text BG whose on-screen map is one fully opaque tile throughout (`solid_256`) is a flash of colour:
    the field's red flash when the party takes damage switches BG0 to such a map (BG0CNT 0x0e00) blended over the
    rest (BLDCNT 0x1e41). It fills the margins and the zoom out's rows with that tile, beyond the room's ends too,
    unless it is blended over the room there (with Fill screen sides off, beyond the room stays black): the
    Mammon Machine's hall keeps such a map on BG0 as a steady glow. Its text box switches BG0 from that map to the
    box's, which counts as a box layer like a switch from a 512-wide map (`frame_solid`), so the box goes to the
    zoom out's UI layer and the glow goes on beside and under it.
  - The field keeps each BG in a 512x256 map used as a ring and only keeps the 34 columns around the 4:3 view
    current (scroll handler ov004 0x0213bcb0: per layer, generator 0x02137144 / 0x02137734 / 0x02137c4c into a
    staging buffer, uploaded into the ring at origin-1 / origin+32 by ov004 0x02137e2c and main 0x02046754).
    `runtime/widescreen.c` writes the margin columns itself before each frame, from the room map (metatile ids
    at `*(0x0207b518)` + 0x1000 + 0x40 * layer, rows 0x100 apart, 9th id bit at +0x10c0), the metatiles
    (`*(*(0x020708d4) + 0xc / + 0x10)`, 2x2 tiles of 3 bytes) and the scroll state `*(0x020708e0)` (camera column
    +4, row +8, map masks +0x1a, ring origins +0x8e); layer 2 goes to BG0 (palette through the table at
    0x02058714), layers 0 and 1 to BG2 and BG3, their priority tiles to BG1. That model reproduces every cell the
    game itself drew in the Millennial Fair. A layer is filled only while its BG scroll is within a tile of its ring
    origin (walking, the origin in RAM is a tile ahead of the scroll for a frame at each tile, and that frame's entering
    column shows too; without hidden guard columns, at 60 fps, a stale one was seen at the view's edge;
    the margins take the tiles every filled layer shows at its own fine scroll; on Zenan Bridge BG0
    stays at 0, 0 while BG2 and BG3 sit 6 rows lower and show a 25th tile row).
    A layer is also filled only while the model reproduces what the game itself drew in the 4:3 view (9 cells in
    10, priority tiles left out; `bg_holds_layer`): some rooms put a picture of their own on BG0 or BG3 that never
    scrolls, so its ring origin stays 0, 0 and agrees with its scroll. The prison bridge: BG3 holds a fixed
    256-wide sky (tiles 0x199.. in order), BG0 is mostly empty, the room on BG2 pans; filled from the room map,
    the margins showed that map scrolling with the camera beside the still sky. Such a BG at scroll 0
    (`ws_picture_bgs`) shows the screen reflected in its margins (`mirror_bg`, as the battle bolts), so the sky
    and its mountains go on at both sides and hold still. A shaking screen (the Dragon Tank's charge: HOFS 0-1, VOFS
    0-3 on every BG) moves it a few pixels: it still counts, reflected at the picture's own sides; at any scroll but
    0 the margins had shown the map's other half, which blinked.
  - Rooms narrower than the view: the margins show what the map holds beside the room, its tiles repeated (the
    map wraps at its masks) or neighbouring rooms (a house's map holds several). Fill screen sides (System
    Settings, shown with a wide aspect ratio): Yes keeps that; No (the default) draws black beyond the room
    (`ws_room_span`: the room's bounds in metatiles at `*(0x020708e0)` + 0x16 left, + 0x17 right, inclusive, the
    ones the camera stays within, against the camera's x at 0x020760f8; BG0's scroll is no measure, since some
    rooms animate it, e.g. the Geno Dome's, whose edge then wobbled and let sprites show beside the room), so the
    room shows as the original. That camera is already the next frame's, so `video.c` moves the bounds to the frame's
    own: when a shake stepped it, a column beyond the room stayed unclipped, and with camera smoothing the prison
    bridge's sky (still layer) showed through it.
    Beyond the room is black unless the screen's own side columns (8 each side, sprites left out) showed one
    colour alone the frame before (`void_col`): the Blackbird's halls are one screen wide, their map dark grey around
    them within the room's bounds, and the screen showed a grey box between black margins.
  - Rooms one screen wide with a layer scrolling on its own (the Epoch in flight: a 16-metatile room, the Epoch on
    BG2, the clouds on BG3 scrolling down while the camera stays; BG3's 512-wide map holds a sea of fire in its
    right half. The Epoch parked, location 0x1d9 with Take off / Get off: the water on BG0 drifting both ways):
    such a layer (BG0 or BG3 off its ring origin while BG2, the room's own, is on it, so a shaking screen does
    not count; `ws_repeat_bgs`) is a 256-wide picture, so its screen columns repeat into the margins, and beyond
    the room the other BGs are left out, with Fill screen sides on or off; BG0 or BG3 alpha-blended onto such a
    layer repeats with it (the parked Epoch's water is lit by a grey BG3 added onto it, BLDCNT 0x014a, 16/16;
    without it the sides were dimmer), and keeps repeating while the room stays: as the Epoch warps the game
    brightens instead (BLDCNT 0x00bf, BLDY up to 8), the opaque grey then covers the water, and the sides had kept
    the bare water). The clouds tile without a seam; the
    water shows a faint one, its map not quite periodic. A layer once seen off its origin keeps repeating while
    the room stays: in passing, its scroll meets the origin (the clouds at VOFS 0 and 256, a frame each), which
    blanked the sides. So did BG2's own steps: walking, the ring origin in RAM is a tile ahead of the frame's scroll
    for one frame at each tile, so what already repeats carries on through a BG2 a tile off. Only a layer the screen shows at its sides repeats (seen in the 4:3 picture's 8 side
    columns the frame before, on top or blended under the top, text box lines left out): at location 0x71 (Marle
    back, beside the gate) BG0 holds bubbles drifting at priority 3 under the opaque room, seen only through the
    time gate's hole mid-screen, and they had filled the sides.
    BGs that are not the room's (its rings are 512x256 text maps) go on as drawn beyond the room, with Fill screen
    sides on or off: a battle effect on a 512x512 map (an enemy's "Rainbow Storm: Poison Cloud" over the starry
    one-screen room: BG0's black bubbles, scrolled line by line, over the stars of BG3 coloured line by line through
    palette entries 0xc1-0xc3) had stopped at the 4:3 picture's sides.
  - Rooms whose bounds hold more than the room (`narrow_rooms`, by location id, the halfword at 0x020720a0: 0x1d0
    the End of Time, 0x1d9 the parked Epoch, 0x1b0 the Epoch in flight): the parked Epoch's bounds are metatiles
    0-31, but its map's right half holds a second Epoch (its wing showed in the right margin), so the room ends at
    15. It stays narrow while the room holds (its bounds and first metatile row, `room_print`): taking off, the
    location reads the destination (0x1f7) for a few frames before the scene changes, and the wing came back.
  - Rooms looked at from their left end (`left_rooms`): the view stops at the room's left end alone, the scene at
    the view's left border. The Blackbird's wing (0x16c) is one screen wide, the plane's side and door at its left
    end; centred, the party stood out of the door mid-screen with clouds beyond the plane too.
  - With Fill screen sides on, a layer repeating into the margins (`ws_repeat_bgs`: the clouds there, the Epoch's
    flight) leaves the rest black beyond the room (room_clip) all the same, so the view stops at the room's ends as
    with it off (the rows above the wing were black once the text box no longer covered them).
  - A 4:3 view that has stayed outside the room's bounds for 8 frames (`view_outside`) has no room ends: the
    Blackbird seen from above (0x16e) is a cutscene the game pans by scrolling the BGs, its camera held below the
    bounds, whose ends had shifted the view 85 and 232 pixels off and cut the plane at them. A room loading has the
    camera outside the new bounds for a few frames only (after the Lab 32 race), so the view's shift holds then; a
    camera beyond the map itself (the Blackbird's, row 528 of a 512-row map) has none at once, or the scene's first
    frames had them.
  - Characters: two culls kept them inside the 4:3 view. (1) The field object loop (ov004 0x02130a10) draws an
    object only while ov004 0x02130dbc finds its tile within the camera columns `*(0x020708e0)+4 - 4 ..
    +6 + 4` (and rows); those fields are the camera's own, used by map streaming too, so the wrapper
    `rt_ws_object_visible` (`wrap` override) widens them by the margins only for the call. (2) NitroSDK's G2D
    renderer culls sprites to its view (top screen surface at 0x02058070: x, y, width, height in 20.12; normally
    0, -1, 256, 192) and places them relative to its top-left; in the field it is set to -margin, 256 + 2 *
    margin, so sprite X counts from the left margin (the renderer reads the view back, `ws_sprite_origin`), and
    put back elsewhere (also after loading a state saved with it widened).
    Both go with the view's shift at a room's ends: the object cull is widened to the shown view (the margins moved
    by the shift), and the game places an object's cell at its X from the camera in 9 bits (-128..383, the DS
    screen's middle +-256), so `rt_ws_draw_cell` takes, of X and X +- 512, the one nearest the view's middle. At a
    room's left end the shift (85 px) and the right margin reached past 383: creatures there wrapped to the left,
    culled, until the camera came closer.
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
  - Race against Johnny (Lab 32, ov011, scene vtable 0x02148574): mode 1, the road an affine BG3 (1024x1024,
    wraparound) shown below line 57 through WIN0/WIN1 (left and right halves), so its margins are drawn as they
    are. Above it, outside the windows, BG0 (skyline), BG1 (sky) and BG2 (mountains) are 256-wide text maps that
    tile seamlessly; in the race they repeat into the margins (`repeat_256`) instead of staying empty. Sprites
    (bikes, HUD) keep their 4:3 place, centred: logged OAM showed the game keeps Crono's bike at X >= 5 and
    Johnny well inside the screen, so no cull needed widening.
  - **Zoom out** (System Settings, Off / 90% / 85% / 77%; `video_set_zoom`): the field and the world map draw more rows and
    columns into the same place in the window: 214 (89.7%, the nearest to 90%), 226 or 248 rows at the aspect ratio's
    shape, in the layouts that show the wide picture; other scenes keep 192 rows.
    - Limits: BG maps are 32 tile rows tall, and a picture at a fine scroll shows one tile row more than rows / 8, so
      248 rows (77%) is the most that does not show a tile row at both edges (256 would be 75%). They are 512 wide, so
      a ratio beyond 496 columns (21:9 at 212 rows) zooms out less.
    - Rows: `video_render_line` draws the rows above the screen with line 0 and those below with line 191 (`Engine.rows`,
      `FROW`), using the registers those lines have, the frame's BG0 / BG1 where the text box switched them, and the
      windows of the nearest line; affine references are stepped back to match. A reflected picture
      (`mirror_bg`) reflects at the top and bottom too.
    - Maps: `ws_fill_margins` writes every ring cell outside the game's own (its 32 columns, the 4:3 view's rows and the
      25th), so the ring holds the taller view; the world map fills by tile row, since a metatile row can show at both
      ring edges. The object cull is widened in rows too. Rows beyond a layer's map top or bottom, which the camera
      never reaches, repeat its first or last row instead of wrapping round (`map_row`: the sky behind Azala's tower,
      layer 1 on its own camera near its map's top, had the canyon's rocks above it); a layer whose own 4:3 rows
      cross its map's end wraps as before.
      Layer 1 drifting on its own (BG3's scroll moving while the camera and BG2's hold still) is not filled while the
      room holds: past the Blackbird's wing the clouds loop through BG3's own map, which wraps seamlessly, and the fill
      took it over only for the ~30 frames a loop its scroll passed near the layer's place, the rows beyond the screen
      jumping then (clamped at the map's last row).
      Layer 2 the same with BG0: the Blackbird seen from above (0x16e) scrolls BG0's clouds 4 rows a frame over
      BG3's, and every 64 frames, for 3, the fill wrote the room into them, the clouds jumping.
      Neither a repeating layer nor a drifting one starts in a room's first 8 frames (`ROOM_SETTLE`): entering the
      Blackbird's hall from its wing, BG3 kept the wing's VOFS 403 for 3 frames and then went to 0 with the camera
      still, taken for both, so BG3 was not filled for as long as the room held: the floor slid up over the walls as
      the camera went down, and black blocks stood above the room. The room is told by its map and its four bounds
      (`room_bounds`; read as one word at +0x16, the aligned read had only left and right, the wing's and the hall's
      alike).    - Sprites: the G2D view is widened in rows too (`ws_sprite_rows`), and 8-bit Y no longer tells a sprite at the
      bottom from one above the view, as the DS's 64 unused rows did. `rt_ws_draw_cell` (main 0x02019fd4) knows the cell's
      place, and `rt_ws_enter_oam` (main 0x02048098) marks each sprite while it is entered, in attribute 1 bits that
      sprites without rotation do not use: `WS_OAM_ABOVE`. That entry also picks a priority from Y against a threshold
      (0x020fd618, when 0x020fd614 is set), in DS rows, so the threshold is moved for the call. World map sprites
      are not marked: Y from `SCREEN_H + rows` up counts as above.
    - Actors parked off the DS screen: the game leaves an actor it is done with just past the screen's edge (the
      dactyls flying off with the party, 16 rows above it until the fade), hidden on the DS but in the extra rows
      here, where they hung at the view's top. `rt_ws_draw_cell` follows each actor's sprite instance (its r1, when
      the call returns to 0x020482d4; `drift_by`): one that was on the DS screen, moved off it and stopped there
      (6 frames without a move) is drawn going on at its speed over its last 8 moves until it leaves the view, the
      drawing only. One pacing beyond the screen without having been on it stays put; moved again, it is drawn where
      the game has it. Its moves are counted in the room (its place from the camera plus the camera), and only in the
      dactyls' scene (location 0x127, `DRIFT_LOCATION`): elsewhere, a woman in a castle room (0x191) left beyond
      the screen as the camera stopped at the room's left end had moved with the scroll, and flew off to the right.
    - Battle effects drawn line by line (windows from the game's software HDMA, `ws_window_lines`): the game blanks
      them on the lines under the menu and the text box, so the rows above and below, drawn as lines 0 and 191,
      showed none of them (Rainbow Storm's clouds stopped at the 4:3 picture's top and bottom). An effect is made for
      its lines alone (per-line scroll, palette entries and windows; its map's other rows empty), and repeated beyond
      them it looked wrong, so the band of lines it is on is stretched over the whole picture, the screen's rows and
      those under the menu and the text box included (`effect_rows`, at least 48 lines; a few at its start streaked):
      each row is drawn as the band's line at the same height, its windows and the rows of the BGs that are not the
      room's (the effect's 512x512 map), with what the HDMA writes for that line put in place for the row and then
      put back (`ws_hdma_line`); sprites and the room's BGs keep their own rows (the stars, stretched with the rest,
      had streaked; their colours come with the line's palette entries). Between the battle menu and the text box
      the stretch is from those lines (46-165) whatever the effect fills of them: from the effect's own lines, the
      clouds slid as their oval opened from the middle and jumped when the text box closed. The band spans this
      frame's lines and the last one's (`Engine.lines_top`): Lightning II's rays alternate frame by frame between two
      sets, one clear of the top lines, and stretched from each in turn the bolts jumped up and down.
      Rays are not stretched (their centre, on the target, moved with the stretch: Lightning II's met below Crono):
      once a window fits rays (`fit_rays`), the band stays unstretched while the effect lasts (`Engine.rays_seen`;
      Lightning II's fit on some frames only, and switching between the two the bolts jumped). The rows beyond its
      lines take each fitted window's span from its two edges carried on, the part of the nearest line
      (`Engine.rays`, `ray_fit`), a window that fits none that frame its nearest line's; the effect's BGs keep their
      own rows (the bolts' SNES picture has 16 more above the DS's 192 and below). They go on only past band ends
      the game blanks (the screen's edge, under the top menu from line 46, the bottom menu from 146, the text box from
      166: `rays_up`, `rays_down`; the bottom menu's only when the lines start above 46, the menu then not at the top,
      as the pillar passed 145 while growing and was drawn to the bottom); elsewhere the effect ends on its lines (Azala's red pillar, a WIN0 column from
      under the menu down to just below Marle, went on to the picture's bottom). An effect hanging from the battle
      menu's line 46, or rising from the text box's 165, that ends short of the other is unstretched the same way from
      its first frame: the pillar, before its rounded end fit as rays, was stretched from lines 46-165 and reached
      lower than its end.
      A ring is not stretched (its circle would turn into an ellipse), nor are its edges carried on beyond the
      screen's sides (its arcs became straight bars): an enemy's "Rainbow Beam: Blind" grows a ring made of WIN0
      (the left arc) and WIN1 (the right arc), coloured line by line through the backdrop. When the window edges
      that do not stop at a screen side lie on one circle or two concentric ones (`fit_ring`: RANSAC over triples,
      then least squares), its windows in the margins and on the rows beyond its band are drawn from the circles
      (`ring_spans`), and those rows take the HDMA's line one period of its tables away (`ws_hdma_period`), so its
      colours go on. As it grows past the screen's sides only its hole's edge shows: which side of the edge the
      window is on tells a hole from a disc, and the ring keeps the width it last had. A ring just shown keeps its
      centre, its radii alone fitted from however few edges show (a corner as it leaves). A fit counts only when
      the circles give the game's windows on the band's lines (not some edges of another shape), are centred on
      the screen and fit two frames running: the clouds' oval passed for one now and then.
      The portal Magus opens in his castle sets the windows with one halfword channel each (`ws_window_lines` takes
      those too): WIN0 a disc growing from (128, 112) with BG0 in it, its scroll set line by line (the ripples),
      WIN1 a frame with rounded corners showing the room. Where the two windows show different layers the ring is
      WIN0 alone (`ring_wins`), WIN1 drawn as the game has it around it; without that the disc's bottom, past
      line 191, went on as a straight bar. A BG whose scroll the HDMA changes line by line (`ws_hdma_bgs`) is the
      effect's, not the room's, so it goes on beyond the room's ends; battles write BG1-BG3's scroll through the HDMA
      too, the same on every line, and taken for the effect's, the room was stretched with its band (a zoom in).
    - UI: the text box and sprites in screen coordinates zoom out with the rest, on the screen's own rows in the
      middle of the picture, but are not slid by camera smoothing: `ui_frame`, 192 rows, composed in a third pass
      (`draw_ui`). Kept at the original scale over the whole picture, as they first were, their pixels fell between
      the picture's and the text was scaled unevenly (worse at 77%), and the boxes did not shrink with the world; that
      stays a choice (Zoomed out text: Original size), for screens where the smaller text is too small. With the smaller text the layer is as tall as the picture (`ui_to_edges`): a band of UI rows in the DS screen's upper half goes to the view's top, one in its lower half to its bottom, where the DS screen has them (the battle's message and panels, the field's text box with the hand of its choices); a band across the middle stays. Left on the screen's own rows in the middle, the boxes covered the room around the party with the view's edges empty. The numbers placed at an actor (`WS_OAM_PLACED`, from `cell_world`) stay over it. Sprites drawn with the surface's own view are marked `WS_OAM_UI`, as is the battle HUD, which uses
      the G2D view: in a battle, one-sprite cells on the rows of the panel, at the top (above 60) or the bottom (from
      132), of the list the scene's draw calls returning to 0x02041a34 (gauges, HP / MP, the menu's hand; found by
      logging cells while fighting). That list's target bracket (4 sprites) and the hand over a target (the next cell
      in the table) stay in the world, with the actors. The HUD is also put back with zoom out off: the view's shift at
      a room's ends (Fill screen sides off) is not its own. The battle's numbers over the actors ("Miss!", damage) are
      the other way round: UI objects (the scene's layer-1 list, main 0x0203ce98) that ov003 0x0214a2a2 places at the
      actor's screen place (main 0x0203cf4c, wrapped: `rt_ws_place_ui` marks the slot), so the view's shift is theirs:
      their cells are moved by it while one of the slot's entries is drawn (main 0x020345dc, `rt_ws_draw_ui_entry`); at
      a forest's west end "Miss!" stood ~50 px right of its target. The field's draw (main 0x0204171c) has such a list too, its
      widgets drawn returning to 0x02041800: the hand of a text box's choices, also UI and put back. Under the box,
      the picture has the room (the frame's BG0 / BG1, as camera smoothing's box). UI pixels have no HD sources. The
      world map's labels (year, place name) are in the world's sprites, so they zoom.
    - The world map's sprites: the game's tests of Y go as those of X (`rt_ws_snes_cmp`): a piece's Y + 15 below 0xef
      (return address ov005 0x0212c178), an object's on-screen flag at Y - camera + 16, wrapped at 0x400, below 0x110
      (ov002 0x0210ce80). Widened by the rows, the Epoch and the birds go on to the view's edge. Y stays 8 bits in the SNES
      table and in OAM, too few for 248 rows and a piece above them, so `ws_world_sprite_limit` splits them: from that
      row up it means 256 less, for the test, `rt_ws_world_sprite` and `render_objs` alike. At 77% a few rows of a
      piece at the top and bottom edges are lost.
    - The room's ends: with Fill screen sides off, a view wider than the 4:3 one (widescreen too, not only zoomed out)
      stops at them instead of passing them, and a room smaller than the view stays centred in it. With it on, the view
      stops at the ends beyond which the map is the void over the rows shown (`void_beyond_ends`, `void_column`; above
      and below, `void_row`), with nothing to fill: a forest's west end, tile 0 on every layer where its map
      wraps round, showed black there. Above and below the bounds, layer 2 may hold stray tiles on under half the
      row: the Millennial Fair's south end has specks under the black, and the view passed the trees. Bounds at the
      map's own first or last row end there too (beyond, row -1 wraps to the last, and the fill repeats the edge
      row): a Zeal building whose roof is row 0 showed rows above it. The game clamps
      its camera for 256 x 192 and is left alone (`ws_fill_margins`, `view_shift`, `ws_view_shift`): the picture is
      shown shifted from the camera, by the BG scroll of the BGs on their ring origin (`Engine.view_bgs`; not a
      picture layer such as the prison bridge's sky, not a box's 256-wide map; each by its share of the shift, its
      scroll's rate to the room's, measured by axis since the room was entered (`measure_rates`, `ws_bg_shift`,
      `Engine.bg_dx`): the sky behind Azala's tower scrolls at half, and moved by the whole shift it crept the wrong way
      while the view held still at the room's bottom end), the G2D view's top-left (OAM X and Y
      already count from it, so `ws_sprite_origin` and `ws_sprite_rows` now read its size), the fills and the room's
      clip. The camera in RAM is a frame ahead of the BG scroll, and the game moves it after the sprite view is set:
      a frame's sprites have the view of two frames' starts ago, its BGs that of the last one, so the renderer moves the
      sprites by the difference while the shift changes (`ws_sprite_lag`, `Engine.obj_dx`, also in `find_party`), or
      they shake as the view reaches an end. Camera smoothing follows the shifted view, so the party walks on a still
      picture at the ends. A battle effect on a 512x512 map (the room's BGs are 512x256 rings) moves with it too, and
      the windows are tested at the game's screen column the shifted picture shows: Frog's Water (BG0, blended, shown
      in WIN0 only) is placed on its target in the game's screen, and at a forest's east end, with the shift left out,
      it fell 85 px left of the enemy, on Marle. Screen coordinates stay: the text box and the UI; the battle HUD, in
      the G2D view, has the shift taken out as it enters OAM (`rt_ws_enter_oam`).
    - Rooms: with Fill screen sides off, the rows beyond the room are black (`ws_room_rows`: bounds +0x18 top, +0x19
      bottom). Bounds can hold several rooms one above the other (the prison towers, location 0x1d: metatile rows
      0-32 with others stacked), which the camera never shows on the DS; the rows also end at the void between them:
      a metatile row that is one black metatile on layer 0 across the bounds and empty on the others (`void_row`),
      looking out from the view's middle, never inside the 192 rows the game draws.
  - Not handled: rooms smaller than the wide view show what lies beside them in the map (other rooms of the same
    building, or void); the room's camera bounds were not found yet (the limits 0x02143fb0 the object cull clamps
    to hold the whole map, 0-255), so the margins are not masked. The game draws no 3D: the SDK sends a few housekeeping GX commands per frame but no geometry,
  and the 3D BG is off in the title, intro, world map, towns and the first time warp (an affine BG3 tunnel). The
  3D layer was seen on only briefly, while loading a save into the world map, so no 3D renderer exists yet.
- **Screens:** layouts (F2, or System Settings > Layout, for gamepads; hidden with the second screen window): top screen only (the default in single screen mode; it shows the main screen
  below), stacked (the default with single screen mode off), side by side; F3 swaps the screens
  (with top-only: bottom screen alone); F11 toggles between windowed and the last full-screen mode
  (`EGG_WINDOWED=1` starts windowed without changing the setting, for automated runs). Each change shows a short message in the window; Sharp is the default filter. Holding Escape shows
  "Exiting..." and quits after 1 s (releasing cancels). The mouse is the stylus wherever the bottom screen is
  shown. Layout, swap and the System Settings persist in `eggoftime.ini` next to the executable.
- **Minimap** (System Settings, on by default): in single screen mode, the bottom screen's map panel (x 26-229,
  y 26-165; ov016's `cFieldMenu`, `cWorldMenu`) in the top left of the main window, at 1/2, 3/8 (the default) or 1/4 size (Minimap
  size). `runtime/display.c`
  draws it into the internal target after the picture, not into the main screen: the picture slides with camera
  smoothing and the map must not, and with enough internal resolution no pixel of the map is lost (nearest at a whole
  multiple of its pixels, else linear). Minimap background (off by default) keeps the map's parchment; off,
  `runtime/minimap.c` cuts it out with what the renderer says is on top at each pixel (`video_bottom_top`): the game has
  the parchment baked into BG3, so it goes by BG1 and by BG3's pixels in the plain parchment's colour (`minimap_000`). `ss_compose` decides whether it may show: the field or world map, no battle
  (ov003) and the panel on the bottom screen (by its pixels); the display also needs its rows free of the text box
  (`video_box_top`). The display takes the panel from `video_bottom_top`, not from the bottom screen: while a choice's
  buttons cover the panel (a sprite wider or taller than 16 within it, `panel_has_ui`) the game also takes the player's
  marker away, so the renderer gives BG1 and BG3 alone with the marker as it last was. HD art does not apply to it.
- **Second screen window** (System Settings, `runtime/display.c`): the screen the main window does not show,
  in a window of its own (a `View`: window, renderer and the screen textures it draws with, since SDL
  textures belong to one renderer). While it is open single screen mode is off at run time
  (`ss_set_enabled`): the main screen is the top screen, the keys reach the game unchanged, Game Settings
  defaults are the game's and its SELECT stays "Defaults"; scenes are still told apart, so SELECT opens System
  Settings on the title menu and while the field is paused, and the widescreen scenes (the field, the warp, the
  world map, the race) keep their margins in the main window while it shows the top screen. The
  main window then uses the top-only layout; the second is top-only
  with the swap inverted, so F3 exchanges the windows' screens and F2 does nothing. Each screen is uploaded to the window showing it, through the same
  pipeline (internal resolution, filter, HD art); shaders run on the main window only. The second window has no
  V-Sync (the main window's paces both) and is black while a cutscene plays. It follows Window mode on a monitor
  of its own and stays windowed on the main window's monitor (re-checked when either window changes monitor);
  its windowed place is saved as `secondplace` and reused while it is mostly on a monitor; the first time it
  opens centred on another monitor, or beside the main window. Closing it turns the setting off; closing the
  main window quits (SDL itself quits only on the last window). Touch maps through the window under the
  pointer. While it is open, `SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS` is 0, so exclusive fullscreen survives
  the focus moving between the windows.
- **System Settings menu** (`runtime/system_menu.c`, settings in `runtime/display.c`): SELECT opens it on Game
  Settings, in the title screen's menu (phase 7; a small SELECT button sits bottom right, above the copyright)
  or while the field is paused
  (`ss_menu_place`), B or SELECT closes it (closed from the pause, START is held for the game for 100 ms, which
  ends the pause), and so does the game leaving that place (the title idling into the intro); Up/Down choose, Left/Right change (held: repeat
  after 400 ms, then every 110 ms); changes apply at once. While it is open the game gets no keys, and keys
  still held when it closes reach the game only after they are released (B would otherwise leave Game
  Settings). It is drawn over the main screen at native resolution, so the filter and internal resolution
  apply to it, in the look of Game Settings re-created by the runtime (bevel profiles and colours measured
  there: light bars, dark buttons, lit label and teal frame on the cursor row, striped backdrop, B sign),
  so it looks the same from the pause; text in the runtime's small font (`runtime/ui_font.c`).
  - Controls (last row, A): the button mapping (`runtime/controls.c`), one keyboard key and one gamepad input
    (button or trigger) per DS button, for fast forward and for Save state and Load state, saved in `controls.ini` (SDL key and gamepad button
    names). A on a cell takes the next key or gamepad press from the event loop (`controls_capture`, before the
    hotkeys; function keys are refused, Escape or 5 s cancels); an input another action had swaps with this
    one's, so no action is left without one. The two state slots start unbound and stay optional: one taken from
    a slot leaves it unbound, and an unbound slot refuses (and keeps waiting on) an input in use, as the swap would
    unbind that action. They act on the press, outside the menu, beside F5 and F7, which capture cannot take. Exit game (Esc, the gamepad's
    Back) quits after 3 s held, also in the menu; being held, it shares its input with any action, outside the swap.
    Afterwards the menu ignores input until every key is released.
  - Pause (field, ov000 func 0x0210155c, START): the game dims both screens (MASTER_BRIGHT darken 4) and loops
    waiting for START, with the byte at `*(0x0207b520) + 0x407` set to 0xff; single screen mode then writes
    "Press SELECT to open System Settings" at the bottom of the main screen.
  - Window mode: windowed; borderless windowed (SDL fullscreen at the desktop mode, the default); fullscreen
    (exclusive, at the closest mode to the desktop's).
  - Framerate paces only the host loop's presentation (`display_frame_ns`, `SDL_DelayPrecise`): the game thread
    keeps its emulated 60 Hz clock. Measured with `EGG_LOG_FPS=1`: 30/60/90/144 exact, unlimited ~1000-2000 with
    V-Sync off, the display's rate with it on; the game stays at 60 in every case.
  - At 60 fps the host loop presents each game frame as it arrives (`display_follows_game`), up to a quarter frame
    late, not on a timer of its own: the game's 59.83 Hz against 60 Hz drifted through each other every 5.8 s, a burst of
    repeated and skipped frames each time, seen as a camera stutter. A state load moves the next burst to about 1.6 s
    after it. Fast forward and the menu keep the timer.
  - Camera smoothing (framerates above 60): each published frame carries how far the field camera moved
    (`video_frame_serial`; camera at 0x020760f8, steps over 8 pixels, and frames outside the field and the world
    map, count as cuts). In the world map the camera is the view's place in the world, layer 3's (world object
    +0x22a4 / +0x22ac + 6, x wrapping at 0x600, y at 0x400; read a frame early like the field's; layers 1-3 share it
    unless one scrolls on its own: Zeal's layers 1 and 2 are clouds drifting a pixel every few frames over its still
    land, and following layer 1 the picture shook): without it the
    world map moved a whole step on every other refresh and none between (walking 2 px a frame). The game moves that camera a frame before the BG scroll and OAM that show it (written at the next
    VBlank: in the trial's pan, camera y 23 to 24 one frame, VOFS 7 to 8 the next), so a frame's camera is the one
    read a frame earlier; taken from the same frame, every step slid a picture that had not moved yet, back a
    pixel then forward. The host keeps the view's lag behind the shown frame's camera per axis (main.c `Glide`): a camera step
    leaves the view where it was (the lag grows by the step), then the lag glides linearly to 0 over the time the
    camera took for its previous step, in whole game frames (measured period) up to `VIDEO_GLIDE_FRAMES` (8;
    longer: the camera was at rest, one frame), timed from the frames' arrival; a cut drops the lag. Both the frames'
    arrival and the presents are taken on an even beat (main.c `beat`: each tick pulls it an eighth of the way to
    itself, a tick off by a quarter period or more restarts it): the game thread's frames come 15.7-17.5 ms apart and
    the presents are composed 7-9.5 ms apart while the screen refreshes every 8.3 ms, and timed from the raw ticks the
    view's steps while walking at 144 fps (120 Hz screen) ranged 0.85-1.14 px a refresh instead of 1, a judder; on the
    beats, 0.98-1.02. So the view
    moves on every window frame at even pace whatever the refresh rate, in sub-pixel steps of the internal
    resolution: walking (2 px every frame) glides each step over one frame (one game frame of extra delay), and
    slow scripted pans glide over their own cadence (the trial's pan down moves 1 px every fourth frame: gliding
    over one frame showed a quarter-second rhythm of 1 px moves and three-frame stops; now 0.125 px every
    refresh at 120 Hz, stdev 0.02, at up to four frames of delay). Background
    and sprites slide together (nothing is blended or redrawn), except the party layer: sprites the camera follows
    (the party) hold still on screen while the world moves, so sliding them would shake them back and forth. A
    sprite counts as followed when, matched to the previous frame's OAM (same tile and palette, shape and size,
    nearest), its screen step is smaller than its step in the room (screen step plus camera step); in frames
    without a camera step within `VIDEO_GLIDE_FRAMES` of the last one (the view still gliding), when its match
    was followed. The renderer
    leaves those out of the field picture and composes them a second time into a transparent layer (only where
    they win the top layer, so backgrounds in front still cover them, or are seen blended under a translucent
    top layer: dark rooms lay a darkness BG0 at priority 0 over
    everything, alpha-blended, and without this the party vanished whenever it counted as followed while
    running), and the host draws it over the slid
    picture without the slide (`video_copy_frame`'s party output). The text box is screen-fixed too: on its
    lines (BG0/BG1 switched to one-screen maps, see Text boxes; in every frame of a glide) its pixels go to the party layer, and the slid
    picture shows the room under it (those layers as the frame started), so the box holds still while walking
    with it open. BGs that hold still on screen while the room pans (`ws_picture_bgs`, the prison bridge's sky:
    see Widescreen) get a still layer the same way: where such a BG or the backdrop is on top, not blended with a
    layer that slides, the pixel goes to the still layer and the main picture is transparent there; the host draws
    the still layer without the slide under the slid picture (all main textures blend), so the sky no longer
    moves a fraction of a pixel with the bridge and jumps back each game frame. The renderer draws 8 hidden
    columns each side for it (`VIDEO_SLIDE_MAX`, part of the widescreen margins) and `VIDEO_SLIDE_ROWS` (4) hidden rows
    above and below (part of `Engine.rows`, not zoomed out too); a vertical slide beyond those repeats the edge row. The BG
    maps' 256 rows leave no room for them at 77%, so with smoothing that setting shows 240 rows, and the menu calls it 80%; 85% keeps its rows. Measured at 144 Hz while walking: the view advances ~0.83 px per window
    frame (the game's 2 px per frame over 2.4 refreshes), the game at 60. (Frame interpolation, blending
    sprites between frames, was tried first and dropped: it did not look right.)
  - Color (`color` in the ini: ds, light, moderate, strong): raises each channel of every uploaded screen to
    1.0, 1.1 (default), 1.2 or 1.4 through a 256-entry table, before the filters; black and white stay.
    (The full 2.4 / 1.5 ratio of the SNES on a CRT to the DS screen, 1.6, was tried: it crushes shadows.)
  - Language (`language` in the ini: english, french): the game picks its language from the firmware user
    settings (language bits of offset 0x64, 1 English, 2 French), which `rt_boot` writes from the setting, so a
    change needs a reboot. Choosing a language other than the running one asks "Restart the game now?" (Yes / No,
    No by default); Yes requests `RT_STATE_REBOOT`, which the game thread takes at its next poll like a state
    load: `rt_threads_restart(1)` drops every thread, the scheduler clears what a save state holds (main RAM,
    TCMs, shared WRAM, palette, OAM, VRAM, I/O; CP15) and starts a new boot thread, and `rt_boot` resets the
    hardware modules (hw, video, widescreen, pxi with snd and spu, card, sched with the frame count) and boots
    again. The save memory stays as it is, as a cartridge's would. Verified from the title menu and mid-cutscene
    (autokey `REBOOT`): the logos, title and a loaded save play in the new language. The runtime's own texts in the game's screens (title prompt, relabelled
    SELECT, pause hint) follow the language in RAM (`rt_french`), so a save state keeps its own; the System
    Settings menu stays English. `ui_font.c` reads UTF-8 and has è and é for them.
  - Mods: replacement cutscenes on (the default) or off (movie mods, below).
- **Fast forward**: holding Left Shift or the gamepad's right trigger (autokey `FF`) runs emulated time at 2x the
  host clock (`rt_set_speed`; the game thread rebases `sched_now` when it sees the change). Everything on emulated
  time speeds up; the window keeps presenting at its own rate. The sound driver's frames halve in host time and
  the mixer steps the channels (and the music mods) twice as fast, so the output stays at the device rate, an
  octave higher. System Settings > Fast forward music at Normal speed (Sped up is the default) instead runs the
  players whose sequence is a BGM (`music_is_bgm`) and the channels their tracks hold one driver frame per frame
  of host time, at their own pitch; the game then sees those players advance slower than itself. The output then
  goes by the host's clock too (`snd_frame`): the game reaches less than 2x when busy (`ff_hold` drops the time it
  holds; 1.86x walking in a forest), and output by emulated time fell short of the device's rate, the queue running
  dry every 0.6 s, which stuttered the music. The other channels still step at 2x. System Settings >
  Fast forward sound at Music only (All is the default) mixes every other channel at zero volume while fast
  forwarding: they still play, so the driver sees them end on time. The music mods play either way. Only 2x: from about 3x the host cannot keep up in busy scenes (a Zenan Bridge battle) and the
  game freezes, its lower priority threads starved, while VBlanks go on.
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
    (whatever rows they use above the copyright) are moved into the space left of the pendulum. In phase 7 a
    small SELECT button (`sysmenu_draw_select_button`, the System Settings menu's button look) sits at the
    bottom right, right above the copyright: SELECT opens System Settings there. In state 8
    any button pressed after the title started also presses START (`ss_game_keys`, applied to KEYINPUT and
    the X/Y buffer), which opens the menu, as a touch does.
  - Game Settings (after New Game): options on the bottom screen (rows: 0 Game Mode, 1 Battle Mode, 2 Movies;
    cursor row at scene + 0x3f00, the row shown +0x3f01, the column +0x3f02), the highlighted option's title and
    description box on the top screen. The main screen stacks full-width bands: the header, Battle Mode and
    Movies with the description box (top rows 119-184) under the highlighted one, the SELECT/START buttons and
    the footer. Game Mode is left out, since single screen mode plays Classic: while the cursor is on row 0
    the game gets a pulsed Down, and Up/Down wrap between rows 1 and 2 (the direction is decided when a press
    starts). SELECT, the game's "Defaults", is withheld from the game and its button reads "System settings"
    (painted over with the button's own texture, built-in font, on the bottom screen, so in every layout): it opens
    the System Settings menu.
  - Name entry (ov012, vtable 0x02156880, built only at 0x021224cc): the character on a patch of grass on the
    top screen; the name, portrait, keyboard and buttons fill the bottom screen. The word at scene + 0x4e48 is
    the scene's state: 0 while the character, the floor and the keyboard fade in, 1 entering the name, 2 after
    START (the keyboard and floor fade out while the character animates). The main screen shows the bottom
    screen in state 1, the top screen otherwise.
  - Shops (ov012, vtable 0x021575f0): every member's attack and defense (now and with the highlighted item) on
    the top screen (the party at the left, the others at the right, up to four each), everything else on the
    bottom one, the item's description in its help bar. The main screen shows the bottom screen moved up into the gap
    under the tabs and, in place of the help bar, a 3 x 3 grid of text lines (the party first): each member's name and
    only the stat the item changes (defense when only defense values are coloured, else attack), old -> new. The
    item's panel is packed (its text lines 8 rows apart, not 16) to make room for a box under it with the description,
    the help bar's pixels in up to two lines of 110, broken at word gaps; a longer text would be cut.
    The shop's Equip opens the field menu's Equipment page.
  - Age selector (time travel; ov012, vtable 0x02158290): the chosen age's map on the top screen; on the bottom
    one the ring of ages, the age's name with a line to it, A Warp and B, all sprites over a swirl (affine BG3
    with wraparound). The main screen centres the ring and a half-size map (box-filtered) side by side: in this
    scene the renderer composes the bottom engine's lines a second time, with the sprites that start above row
    148 (ring, line and name; the buttons start below) drawn 44 columns left (`ss_bottom_shift`), the swirl
    and buttons in place, and the main screen is that picture with the map put under its sprites (the renderer
    marks them). The DS bottom screen of the other layouts stays as it is. The map takes rows 53-122, centred on
    the ring (rows 40-135), 10 columns right of it; the name sits above the ring, beside it (Present rows 37-42,
    Middle Ages rows 112-118, whose end then lies on the map's edge) or under it (rows 131-147).
    Widescreen: in this scene the bottom engine is drawn with the top screen's margins too, so the second
    picture is as wide as the field's, the swirl (wrapping) filling the sides and the rest centred.
    (Shifting whole rows instead, from a bottom screen drawn with margins, needed a shear between the moved
    rows and the buttons, and the names under the ring fall in it.)
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
  SDK code itself. That code is ordinary ARM9 code. Its ITCM decoder is hand-written: its bit reader returns with
  `mov pc, r6`, a jump within the whole module that leaves its C frame, and its last return skips those frames to its
  caller. So every call into a whole module records an unwind point, also those from analysed code (`rt_call_addr`;
  the in-game movies call the decoder from ov008 0x02122358, which ended in a fatal error without one).
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
- **Graphics dump** (`runtime/gfx_dump.c`, System Settings > Dump graphics): every tile sheet (`.NCGR`,
  `_ncg.bin`), every `.NCER` cell (a sprite frame) and every `_nsc.bin` screen of the ROM in memory, as indexed
  PNGs (`SDL_SavePNG`, palette indexes kept, colour 0 of each 16-colour row transparent) in
  `mods/graphics/dump/{sheets,frames,screens}/<ROM path>.png`, on a thread of its own while the game runs. The
  ROM's files come from its FNT/FAT; most are LZ10-compressed, some twice. The game's background formats have
  8-byte headers: NCG (tile count, 8 bpp flag), NCL (colour count), NSC (byte 6 bit 0 affine: one byte per map
  entry, else BG text entries; width and height in tiles at bytes 8, 9). Palettes: the file of the same name,
  else the only one in the folder, else for `Chara/Chara_xxxx` `Chara/ObjPlt_xxxx` (the game pairs them in code:
  main 0x020483d0 loads the sheet, 0x02048570 the palette, both called from ov000 0x02109420 through a function
  pointer; the same number held for every sprite checked), else a grey ramp (`palettes.txt` lists which; `Map/Bg`
  sheets are paired through the map tables). Progress and the time left are weighted by file size, a sheet's cells counting 32 times theirs.
  The message fonts (`msg/<size>/msgcmn.fnt`, `"\0\0\0\0FONT"`, 2 bits per pixel, a variable-width record per
  glyph; format in `runtime/gfx_rom.c`) go to `fonts/<ROM path>.png`, 16 glyphs to a row in cells as wide as the
  widest glyph's rows (12x10 big, 8x8 small), with `.txt` advance widths and ASCII characters by glyph; their
  palette is the only one in the folder above (`msg/font.NCLR`), its colours reordered as text shows on screen:
  letter (1) in colour 2, white; shadow (3) in colour 1, black (seen in screenshots, not traced in the code).
  The ROM reading, decoding and layouts (sheet, cell in reverse OAM order, screen) are `runtime/gfx_rom.c`,
  shared with the loader.
- **Graphics mods** (`runtime/gfx_mod.c`, Mods on, read once after the game build is prepared, so the build id
  is the ROM's): PNGs in `mods/graphics/{sheets,frames,screens}/<ROM path>.png` (the dump's paths), in path
  order. Each image is mapped back onto its 8x8 tiles through the same layouts as the dump; a pixel belongs to
  the top-most tile covering it. At 1x the tiles are written into the ROM in memory (indexed pixels, or RGBA
  matched to the tile's palette row), the file recompressed (LZ10) and, when it grows, appended after the ROM's
  end with its FAT entry moved. Indexed PNGs whose palette differs write the ROM palette's colours. At 2x to 8x
  each tile's HD block (native pixels upscaled where another tile owns them; blank tiles skipped) goes into a
  hash table keyed on the native tile's bytes and bpp. The renderer (`runtime/video.c`) records each pixel's
  source (`VideoHd`: tile number from that hash, cached per frame by VRAM address, texel, flips, palette row,
  blend weight, added colour and the colour under it for transparent HD texels); affine layers and extended
  palettes have none. `runtime/display.c` then draws each shown screen at min(HD scale, internal scale) from
  those records on worker threads: indexed art through the frame's palette RAM, RGBA as painted, both through
  the recorded blend and the Color setting. Not with a shader; the one-screen menus (built by
  `runtime/single_screen.c`) keep native tiles, the field, warp, world map and race passing their sources on.
  Fonts (`fonts/<ROM path>.png` and `.txt`, first pass) rebuild the font file: glyph pixels at 1x only (text is
  drawn into tiles at run time), advance widths, new glyphs where none were; each glyph keeps its bytes per row
  unless its pixels need more (some originals are wider than their pixels), so unchanged files rebuild byte for
  byte.
- **Translation mods** (`runtime/translation.c`, `runtime/rompatch.c`): every `.ips`, `.bps`, `.xdelta`,
  `.vcdiff`, `.xdelta3` or `.delta` file in `mods/translation/` patches the verified ROM before it is extracted, in
  file-name order (a split translation, or a base patch and a fix). IPS and BPS are decoded here; BPS checks its
  source, result and own CRC-32, so a patch made for another ROM is refused. VCDIFF goes through xdelta3 (Apache
  2.0, in `kit/xdelta3/`; a kit assembled without it uses one from `PATH`) because modern encoders often
  LZMA-compress their sections. xdelta 1 patches (`%XDZ004%`, usually `.delta`; xdelta3 cannot read them) go
  through `kit/recomp/xdelta1.py`, run by the build's Python, which checks the source and result MD5s the patch
  holds. When xdelta3 refuses a patch (or is missing), `kit/recomp/vcdiff.py` tries it: a pure-Python VCDIFF
  decoder (default code table, no secondary compression) that applies it to the dump, then to the dump rebuilt the
  way ndstool 1.x's `-x` then `-c` lays it out, which many patchers do before applying their patch: header's
  0x200 bytes, zeros to 0x4000, ARM9, overlay table (4-aligned), ARM7 and banner (0x200-aligned), a rebuilt FNT
  (names in Windows' upper-cased order, dir IDs depth-first, files then subfolders), FAT, then the files 4-aligned
  and zero-padded in the new ID order, the end 4-aligned. A dump with a decrypted secure area cannot rebuild an
  encrypted one, so a checksum failure in the window(s) starting below 0x8000 is accepted only in that case (those
  bytes are copied as they are). Only release builds apply patches. `rt_rom.data` holds the patched ROM while `rt_rom.path` and
  `save_path` stay on `rom/CT.nds`, so saves keep their place and the dump is never changed. `game/build.id` gains
  the patched SHA-1 as a third line, so changing patches rebuilds; dsd reads `game/translation.nds` for that build
  and it is deleted afterwards. After a native patch, an xdelta or xdelta 1 one writes the current ROM to
  `game/.translation-source.nds` and decodes to `game/.translation-target.nds`.
  A `.nds` next to the patches is the version they were made for (Chrono Trigger (Japan) or (Europe)): they are
  applied to it, and `kit/recomp/retarget.py` carries what they changed over to the USA ROM, by path (a repacked
  ROM numbers its files differently). Code cannot cross versions, so a change to ARM9 code that differs between
  them is refused; a changed overlay or program identical in both (the European overlays are the USA ones and hold
  some text) is carried like a file. ARM7 changes are dropped: the ARM7 is emulated. A changed
  file whose original is the same in both versions is copied. Text (`msg/<font>/*.msg`: languages (u8), entries
  (u16), then a u32 offset per entry and language; characters are glyph numbers of that folder's `msgcmn.fnt`,
  UTF-8-style, and control codes follow the font's last glyph at fixed distances) moves string by string into the
  same entry and slot (Japan's English slot 1 is the USA's English slot 1); characters map by identical glyph
  records, controls by their distance from the glyph count. Glyphs the USA font lacks (Portuguese ã, õ, ...) are
  drawn over glyphs no USA text uses, so the glyph count and the control codes stay put. Changed files go past the
  last file in the ROM, the FAT points at them; the header and code are untouched.
- **Versions of the game**: the ROM's game code (`rt_rom.code`, kept as loaded: a patch may change the header's) picks
  what the game is built from. Europe is the USA game but for its secure area, where the 18 BIOS call stubs sit in
  another order, and a constant in ov013 (no address): `config/regions/YQUP/` holds only its `symbols.txt`. A region's
  config holds the files that differ from `config/arm9/`, each falling back to the USA one (`dsdconfig.py`). Japan is
  the same source shifted around (main by up to +0x40 in rodata, overlay base +0x20, ov007 -0x128 to -0x180, ov012 up
  to +0x60): `config/regions/YQUJ/` is its own `dsd init` output with the same hand fixes as NOTES.md (ov002's two
  Thumb helpers at +0x20, ov008's trampoline). `overrides.txt` carries `jp=` where a hook moved, and the runtime names
  each game address with `GAME_ADDR(usa, japan)` where it is used (`rt.h`); struct offsets matched where checked. They
  were mapped by matching functions in order (USA and Japan have the same list but for dsd's split in ov002 and one in
  ov007), and checked by content: functions byte-identical at the address, vtables by their mapped entries, rodata by
  its bytes, BSS by the code's literals. The game library records the code it was built for and `rt_game_load` refuses
  another's ROM, so a developer build (`-DEGG_EXTRACT`) can't run a mismatched one. System Settings > Language offers each game's two languages
  (`SET_LANGUAGE`, or `SET_LANGUAGE_JP` on the Japanese game, its own ini key), given to the game as firmware language
  (0 Japanese, 1 English, 2 French) at power-on; the menu's own text follows the choice at once (`runtime/ui_text.c`,
  English keys; Japanese glyphs from PixelMplus10, only those used, `tools/font/ui_font_ja.py`). Translations for other versions aren't carried over to it, as its text
  has 2 language slots against 4 and other fonts. Saves share `rom/CT.sav`; save states check the game code.
- **RetroAchievements** (`runtime/cheevos.c`): rcheevos' `rc_client` (v12.5.0, fetched and built by CMake as a
  static library, cartridge hashes only) with libcurl for its server calls, each on a detached thread of its own.
  `rc_client_do_frame` runs on the game thread at VBlank (`sched.c`), so achievements read memory between frames;
  background reads are off. rcheevos' DS addresses map to main RAM (0) and DTCM (0x1000000). The game is
  identified by rcheevos' hash of the ROM file (`rt_rom.path`, unmodified by mods). Softcore only, so save
  states, fast forward and mods need no lockout; a reboot (`rt_boot`) or loaded state calls `rc_client_reset`.
  The login (name and token, never the password) is in `retroachievements.ini`, not `eggoftime.ini`, which
  display.c rewrites whole; a password written there by hand is swapped for the token after logging in.
  Events queue messages for the host thread (`cheevos_poll`), shown one at a time in the message box; unlocks queue
  popups instead (`display_popup`: badge, title, points, top right, sliding in), each shown once its badge is in or
  after 3 s without it. The
  System Settings page types its fields with SDL text input (`display_text_input`, `sysmenu_text_event`); logged in, it
  lists the set's achievements (`cheevos_list`, a copy refreshed twice a second) with their badges, downloaded a few
  at a time when first shown and cached as the server's PNGs in the data folder's `cache/badges`.
- **Save states** (F5 save, F7 load, or the inputs assigned under Controls; one slot: `<rom>.state` next to the ROM; `EGG_LOAD_STATE=<file>` starts
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
- CMake ≥ 3.25 + Ninja, clang or gcc (llvm-mingw on Windows), SDL3, FFmpeg, libcurl, Python 3 + capstone. SDL3 comes from an
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
  the ROM (a file dialog; copied to `rom/CT.nds`), checks its SHA-1 against the three known dumps, then runs `kit/dsd/dsd.exe rom extract`,
  `kit/python/python.exe kit/recomp/recomp.py kit <gen> --no-comments --extract <dir> --region <code>` (no capstone needed
  without the comments), one `kit/llvm/bin/clang.exe` per core over the generated files (the developer build's
  flags without `-g`) and the link against `kit/lib/libeggoftime.a`, showing progress in the window, the tools'
  output in `game/build.log`. `game/build.id` holds `kit/kit.id` (a hash of the recompiler, config and
  interface headers, written by the package script) and the ROM's SHA-1 (and the patched ROM's, when a
  translation patch applies): a release with other ones builds again, one with the same ones starts at once. The
  temporary `game/build/` (extract, C, objects) is deleted.
  `scripts/package.py` assembles the release: the runtime build, the recompiler and `config/`, dsd, embeddable
  CPython 3.11, xdelta3 (for translation mods), and the part of llvm-mingw the build needs (found by asking clang
  for the headers and lld for the libraries it reads: 50 files), third-party licenses; it refuses generated code,
  extracts, ROMs and saves.
  `.github/workflows/release.yml` builds and publishes it for a `v*` tag. The Linux release (built on Ubuntu 22.04
  for glibc 2.35) bundles dsd and Zig 0.17.1 in the kit and uses the system's `python3` from `PATH` (a screen names
  the package when it is missing). Zig (`kit/zig/zig cc -target x86_64-linux-gnu`) brings its own glibc headers, so
  no system compiler or `/usr/include` is needed: SteamOS has neither, and a Steam Deck's gcc, once installed,
  failed on `setjmp.h`. The kit keeps the executable, `lib/std/std.zig` (by which zig finds its lib folder) and the
  headers a probe compile reads (`zig_subset`), and the packager builds a probe library with that subset alone. The
  glibc headers matter, not musl's: `RtCpu.unwind` holds `jmp_buf`s the runtime and the game library share. The
  link is `-nostdlib` (no glibc stubs to build). `zig cc` makes any `-O` clang's `-O2` and adds debug info, so the
  compile also passes `-Xclang -O1` and `-g0`: 152 s for the game on 8 threads instead of 757 s (gcc: 73 s). The
  subset is 178 MB, 60 MB of the `.tar.gz`, nearly all the zig executable. Zig's caches go in
  `game/build/zig-cache`, as the kit can be read-only. Without `kit/zig` the system's `cc`/`clang`/`gcc` compile.
  The game library is compiled with `-fPIC` and linked without an import library, its runtime symbols resolved
  from the executable (`ENABLE_EXPORTS`, i.e. `-rdynamic`). SDL3 is linked statically and BtbN's FFmpeg shared libraries sit next to the executable (RPATH `$ORIGIN` only); the package is a `.tar.gz`
  so the executable bits survive. The same files also go out as an AppImage (`--appimage`: AppRun, a `.desktop`
  file and `res/icon.png` added, packed by appimagetool 1.9.1 with the static type 2 runtime, so players need no
  FUSE 2). An AppImage is mounted read-only, so when the executable runs under `$APPDIR` the runtime splits its
  folders (`rt_install_dir`/`rt_data_dir`, `runtime/main.c`): `kit/` stays in the mount, while `rom/`, `game/`,
  `eggoftime.ini` and `mods/` go to `SDL_GetPrefPath` (`$XDG_DATA_HOME/EggOfTime/`), whose `mods/` folders and
  readme files are made on the first start. Elsewhere both are the executable's folder. Measured in WSL Ubuntu 24.04: 66 MB download, first start 120 s. Measured: zip 125 MB (FFmpeg 112 MB
  of DLLs, the kit 171 MB unpacked), first start about 3 minutes on this machine, later starts 2 s. Zig as the
  bundled compiler was dropped: `zig cc` compiles at -O2 with debug info for the host CPU, and took 21 minutes
  (0.14.1) / 8 minutes (0.17.0) for the largest file, against 106 s with llvm-mingw's clang 23.
- CMake runs `tools/recomp/recomp.py` at configure time (and again when the dsd config or recompiler
  changes). Output goes to `<build>/gen/`, split into ~250-function chunks that compile in parallel.
- Windows (this machine):

```sh
export PATH="$LOCALAPPDATA/Programs/llvm-mingw-20261020-ucrt-x86_64/bin:$APPDATA/Python/Python311/Scripts:$PATH"
cmake -S . -B "$LOCALAPPDATA/EggOfTime-build" -G Ninja -DCMAKE_C_COMPILER=clang \
      "-DCMAKE_PREFIX_PATH=$LOCALAPPDATA/Programs/SDL3-3.4.18/x86_64-w64-mingw32;$LOCALAPPDATA/Programs/ffmpeg-8.1;$LOCALAPPDATA/Programs/curl-8.23.0_8-win64-mingw"
cmake --build "$LOCALAPPDATA/EggOfTime-build"
"$LOCALAPPDATA/EggOfTime-build/Egg of Time.exe" [rom.nds]   # run from the project root
```

  For testing, the same commands with `-B build` put `build/Egg of Time.exe` (and `SDL3.dll`) in the project; it can
  be started by double-click when `build/rom/` holds `CT.nds` (the default ROM is `rom/CT.nds` next to the
  executable, else relative to the working directory). Alongside dsd's `build/delinks`, that adds
  about 60 MB of generated C and a 140 MB executable to the OneDrive folder.
- Controls: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q = L, W = R, Enter = Start, Backspace = Select (defaults; System Settings > Controls remaps them),
  F2 = layout, F3 = swap screens, F4 = filter, F11 = fullscreen, F5 = save state, F7 = load state, hold Left Shift = fast forward, hold Esc 3 s = quit. Gamepads use button positions (east = A, south = B,
  north = X, west = Y), shoulders = L/R, Start/Back, D-pad or left stick. X/Y reach the game through the ARM7's
  `HW_BUTTON_XY_BUF` (0x027FFFA8, X = bit 10, Y = bit 11), which `PAD_Read` ORs into KEYINPUT.
- Play time: the ARM7's VBlank count (0x027FFC3C, `OS_GetVBlankCount`) is kept at each VBlank (`video_line_start`).
  The game's play time goes by it (main 0x0202c95c adds the seconds it advanced, count / 60, since its last call);
  never counted, the TIME stayed 00:00. A save made from another emulator holds it as seconds (u32, 109816 for
  30:30), in its slot and the save's summary.
- Video timing: each scanline steps through start (VBlank/VCount IRQs) -> draw -> HBlank (IRQ, HBlank DMA), with
  VCOUNT reading that line throughout. Lines follow host time, but a step waits while an enabled interrupt is pending
  (IE & IF, also while CPSR/IME mask it) or while the CPU is in an interrupt handler, up to 192 lines behind; further
  behind, one line per call (so code waiting on VCOUNT with interrupts off still sees time pass). The game's text
  boxes are a mid-frame split fed by HBlank DMA tables that the VBlank handler re-arms each frame; without this
  ordering the split jittered by up to ~30 lines (sprites vanished every other frame), then by one line (the box's
  top row flickered). In the field the split is the game's software HDMA (HBlank handler), which reads VCOUNT: with
  the old 64-line limit, a game thread held up by the host (often in fast forward, where a line takes half the host
  time) ran lines past their HBlank handler inside it, the line ending the box was missed, and the rest of the frame
  showed the box's BG0/BG1 and no window (the trees and the time gate gone for a frame; 77 of 2700 frames in fast
  forward beside the gate, none after). Time more than 192 lines ahead of video is dropped (`sched_now`), the game
  held up with it: catching it up stepped past pending interrupts for a whole frame, the HBlank handler at line 262
  that writes line 0's entry was merged, and a frame after a host stall (a screenshot written, in the Blackbird seen
  from above) began with the box's BG0/BG1, the box drawn into the picture with a line above it (1-2 frames in 700,
  none in 2300 after). A frame also starts (line 0) only once the game waits for an interrupt
  (`rt_halt`), up to 128 lines late: in fast forward the game was sometimes still updating at line 0, and the
  frame showed half of it (sprites gone, the gate's OBJ window square). Held lines catch up after, so speed holds.
  In fast forward the game also loses whole frames: it waits for VBlank discarding any already raised
  (`OS_WaitVBlankIntr`), and a frame has half the host time, so heavy ones (map columns streamed in while walking)
  were still busy at line 192 and skipped a VBlank; walking in fast forward went at 1.0-1.7x (prison corridor,
  measured by the camera), slower than it felt at normal speed. Time now holds at the end of line 191 while the game
  is busy (`ff_hold`), up to a normal-speed frame's host time (16.7 ms), and the held time is dropped: fast forward
  runs as fast as the host allows up to 2x, never below 1x (the same walk: 1.9x in every run).
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
   `EGG_DUMP_AT="ms,ms..."`, `EGG_LOAD_STATE=<file>`, autokeys `SAVESTATE`/`LOADSTATE`/`REBOOT`, `EGG_SELFTEST=1`.
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
