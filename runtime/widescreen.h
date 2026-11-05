/* Widescreen support for the field (widescreen.c). Game thread only. */
#pragma once

/* Before a frame is drawn: in the field, writes the background map columns the top screen's margins show
 * (margin columns each side, and rows above and below when zoomed out) and widens the sprite renderer's view to
 * them; elsewhere (or margin 0) puts the view back to the DS screen. limit: the columns a side actually shown, to
 * keep the view within the room's ends (Fill screen sides off); 0 lets it pass them. */
void ws_fill_margins(int margin, int rows, int limit);
/* This frame's shift of the view from the camera at the room's ends (pixels, 0 normally), and the BGs that move by it. */
void ws_view_shift(int *x, int *y, int *bgs);
/* Columns the game's sprite coordinates are shifted by (the widened view's left margin; 0 normally). */
int ws_sprite_origin(void);
/* Rows they are shifted by (the widened view's rows above the screen; 0 normally). */
int ws_sprite_rows(void);
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
 * sets them line by line (battle effects); else 0. */
int ws_window_lines(uint32_t h[192]);
/* Save states: the copy of the room map battles use (an optional block). */
typedef struct RtStateIO RtStateIO;
void ws_state(RtStateIO *s);
void ws_reset(void); /* power-on (rt_boot) */
