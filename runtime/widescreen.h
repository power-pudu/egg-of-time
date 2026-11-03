/* Widescreen support for the field (widescreen.c). Game thread only. */
#pragma once

/* Before a frame is drawn: in the field, writes the background map columns the top screen's margins show
 * (margin columns each side) and widens the sprite renderer's view to them; elsewhere (or margin 0) puts the
 * view back to the DS screen. */
void ws_fill_margins(int margin);
/* Columns the game's sprite coordinates are shifted by (the widened view's left margin; 0 normally). */
int ws_sprite_origin(void);
/* In the field: the top screen's columns the room covers, x0..x1-1 (beyond them, other rooms or its map
 * repeated); 0 elsewhere. */
int ws_room_span(int *x0, int *x1);
/* In the field: the BGs (bit per BG) whose 256 columns repeat into the margins, in a room one screen wide where
 * they scroll on their own (beyond the room the other BGs are then empty); 0 otherwise. */
int ws_repeat_bgs(void);
/* The field camera's position in the room (pixels); 0 outside the field. */
int ws_camera(int *x, int *y);
/* The window positions of each line of the coming frame (WIN0H | WIN1H << 16), when the game's software HDMA
 * sets them line by line (battle effects); else 0. */
int ws_window_lines(uint32_t h[192]);
/* Save states: the copy of the room map battles use (an optional block). */
typedef struct RtStateIO RtStateIO;
void ws_state(RtStateIO *s);
void ws_reset(void); /* power-on (rt_boot) */
