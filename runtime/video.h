/* 2D graphics engines, VRAM banking and frame output. */
#pragma once

#include <stdint.h>

#define SCREEN_W 256
#define SCREEN_H 192
/* Widescreen: the top screen's engine can draw up to VIDEO_MARGIN_MAX columns beyond each side (21:9 at 192
 * rows: 448 = 256 + 2 * 96, plus the hidden columns camera smoothing slides into, VIDEO_SLIDE_MAX a side). */
#define VIDEO_SLIDE_MAX 8
#define VIDEO_MARGIN_MAX (96 + VIDEO_SLIDE_MAX)
#define VIDEO_LINE_MAX (SCREEN_W + 2 * VIDEO_MARGIN_MAX)

/* VRAM as seen by the CPU (0x06000000-0x06FFFFFF); NULL where no bank is mapped. */
uint8_t *video_vram_ptr(uint32_t addr);
void video_vramcnt_written(void);
void video_affine_ref_written(int engine, int bg); /* BGxX/BGxY written: reload the internal reference */

void video_reset(void);
void video_render_line(int line); /* visible lines 0..191 */
void video_end_frame(void);       /* start of VBlank: publish the finished frame */

/* Columns to draw beyond each side of the top screen from the next frame on (0 = none; any thread). */
void video_set_wide_margin(int margin);
/* Widescreen: in the field, the margins show the room's map repeated beyond a room narrower than the view
 * (1), or black (0, the default; any thread). */
void video_set_fill_sides(int on);
/* Latest finished frame (ABGR8888): top screen, bottom screen (256x192 each), and the main screen of single
 * screen mode (single_screen.c; the top screen when that is off), 192 rows of the returned width (up to
 * VIDEO_LINE_MAX). Copies under a lock. */
/* party (when *party_on): the main screen's party layer, same size, transparent (0) where the party is not:
 * the sprites the camera follows, left out of main so camera smoothing slides only the scenery. */
int video_copy_frame(uint32_t *top, uint32_t *bottom, uint32_t *main, uint32_t *party, int *party_on);
/* Camera smoothing on: the field's camera-following sprites get the party layer (any thread). */
void video_set_camera_smoothing(int on);
/* Serial of the latest frame; also how far the field camera moved since the previous frame (pixels; 0 outside
 * the field, after a jump beyond VIDEO_SLIDE_MAX, or when the previous frame was not the field), and when the
 * frame was published (SDL_GetTicksNS). */
uint64_t video_frame_serial(int *camera_dx, int *camera_dy, uint64_t *time_ns);
