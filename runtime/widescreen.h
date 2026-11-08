/* Widescreen support for the field (widescreen.c). Game thread only. */
#pragma once

/* Before a frame is drawn: in the field, writes the background map columns the top screen's margins show
 * (margin columns each side, and rows above and below when zoomed out) and widens the sprite renderer's view to
 * them; elsewhere (or margin 0) puts the view back to the DS screen. limit: the columns a side actually shown, to
 * keep the view within the room's ends; 0 lets it pass them. limit_rows: the same for the rows above and below.
 * fill (Fill screen sides on): only at the ends beyond which the map is the void, with nothing to fill. */
void ws_fill_margins(int margin, int rows, int limit, int limit_rows, int fill);
/* This frame's shift of the view from the camera at the room's ends (pixels, 0 normally), and the BGs that move by it. */
void ws_view_shift(int *x, int *y, int *bgs);
/* How far this frame's sprites in the world are to be moved to go with that shift. */
void ws_sprite_lag(int *x, int *y);
/* Columns the game's sprite coordinates are shifted by (the widened view's left margin; 0 normally). */
int ws_sprite_origin(void);
/* Rows they are shifted by (the widened view's rows above the screen; 0 normally). */
int ws_sprite_rows(void);
/* Zoom out on the world map: a sprite's 8-bit Y from this row up means 256 less. */
int ws_world_sprite_limit(int rows);
/* Zoom out: bits of a sprite's attribute 1 (unused without rotation): its Y is 256 too high (it starts above the
 * view); it is in screen coordinates (the UI, not shifted by the rows). */
#define WS_OAM_ABOVE 0x200u
#define WS_OAM_UI 0x400u
/* In the field: the top screen's columns the room covers, x0..x1-1 (beyond them, other rooms or its map
 * repeated); 0 elsewhere. */
int ws_room_span(int *x0, int *x1);
/* In the field: the rows the room covers, y0..y1-1 from the top of the DS screen; 0 elsewhere. */
int ws_room_rows(int *y0, int *y1);
/* In the field: the BGs (bit per BG) whose 256 columns repeat into the margins, in a room one screen wide where
 * they scroll on their own (beyond the room the other BGs are then empty); 0 otherwise. */
int ws_repeat_bgs(void);
/* In the field: the BGs (bit per BG: BG0, BG3) at scroll 0 that hold a picture of their own, not the room map's
 * layer (the prison bridge's sky); their margins show the screen reflected. 0 otherwise. */
int ws_picture_bgs(void);
/* The field camera's position in the room (pixels); 0 outside the field. */
int ws_camera(int *x, int *y);
/* The window positions of each line of the coming frame (WIN0H | WIN1H << 16), when the game's software HDMA
 * sets them line by line (battle effects, Magus's portal; a window it leaves alone keeps its register); else 0. */
int ws_window_lines(uint32_t h[192]);
/* The BGs (bit per BG) whose scroll the game's software HDMA sets line by line: an effect's own picture. */
int ws_hdma_bgs(void);
/* Writes what the game's software HDMA writes before line `line` (registers, palette entries), keeping what was
 * there in *saved for ws_hdma_restore: to draw rows beyond the screen as that line. */
enum { WS_HDMA_MAX = 32, WS_HDMA_UNITS = 8 };
typedef struct WsHdmaWrite { uint32_t dst; int halves; uint16_t old[WS_HDMA_UNITS]; } WsHdmaWrite;
typedef struct WsHdmaSaved { int n; WsHdmaWrite w[WS_HDMA_MAX]; } WsHdmaSaved;
void ws_hdma_line(int line, WsHdmaSaved *saved);
/* The shortest period, in lines, of what the HDMA writes on lines top..bottom (the windows' channel left out),
 * checked over at least WS_PERIOD_LINES lines; 0 when there is none. */
enum { WS_PERIOD_LINES = 16 };
int ws_hdma_period(int top, int bottom);
void ws_hdma_restore(const WsHdmaSaved *saved);
/* Save states: the copy of the room map battles use (an optional block). */
typedef struct RtStateIO RtStateIO;
void ws_state(RtStateIO *s);
void ws_reset(void); /* power-on (rt_boot) */
