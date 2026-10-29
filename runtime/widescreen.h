/* Widescreen support for the field (widescreen.c). Game thread only. */
#pragma once

/* Before a frame is drawn: in the field, writes the background map columns the top screen's margins show
 * (margin columns each side) and widens the sprite renderer's view to them; elsewhere (or margin 0) puts the
 * view back to the DS screen. */
void ws_fill_margins(int margin);
/* Columns the game's sprite coordinates are shifted by (the widened view's left margin; 0 normally). */
int ws_sprite_origin(void);
/* The field camera's position in the room (pixels); 0 outside the field. */
int ws_camera(int *x, int *y);
/* The window positions of each line of the coming frame (WIN0H | WIN1H << 16), when the game's software HDMA
 * sets them line by line (battle effects); else 0. */
int ws_window_lines(uint32_t h[192]);
/* Save states: the copy of the room map battles use (an optional block). */
typedef struct RtStateIO RtStateIO;
void ws_state(RtStateIO *s);
