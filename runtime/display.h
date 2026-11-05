/* Window presentation of the two screens and the display settings (display.c). Host thread only. */
#pragma once

#include <stdint.h>

#include <SDL3/SDL.h>

#include "video.h"

int display_init(void); /* window, renderer, saved settings; 0 on failure */
/* Draws a window frame; new_frame uploads the latest screens: top, bottom (256x192), main (`height` rows of
 * `width` columns) and the main screen's party layer (NULL: none), as video_copy_frame returns them; ui: its UI
 * layer (NULL: none), drawn at the scale of 192 rows over the whole picture. slide_dx/dy:
 * camera smoothing, where the field picture is shown from (pixels; the party layer does not slide). still: the main
 * screen's still layer (NULL: none), drawn under the slid picture without the slide. */
/* hd: the screens' sources for HD graphics mods (NULL: none), drawn from HD art while Mods is on. */
void display_frame(const uint32_t *const screens[3], const uint32_t *party, const uint32_t *still, const uint32_t *ui,
                   int width, int height, int new_frame, float slide_dx, float slide_dy, const VideoHdFrame *hd);
/* Whether the framerate setting asks for camera smoothing (above 60). */
int display_camera_smoothing(void);
/* F2 layout, F3 swap screens, F4 filter, F11 fullscreen; returns 1 if the key was used. */
int display_key(SDL_Scancode sc);
/* Window events: the second screen window's closing, moves and monitor changes. */
void display_event(const SDL_Event *e);
/* Pointer position in the window it is over -> bottom-screen pixel, when the bottom screen is shown there. */
int display_touch(float wx, float wy, int *x, int *y);
/* Shows text in the window's message box for ms milliseconds (replacing any current message). */
void display_message(const char *text, unsigned ms);
/* Shows a popup in the window's top right corner for ms milliseconds (replacing any current one), sliding in and out:
 * an image (size pixels square, ABGR8888 with alpha; NULL: none) left of two lines of text, the first bright. */
void display_popup(const uint32_t *image, int size, const char *line1, const char *line2, unsigned ms);
/* Starts (1) or stops (0) text input in the main window: SDL_EVENT_TEXT_INPUT events, and the on-screen keyboard
 * where the system has one. */
void display_text_input(int on);
/* Saves the next presented window as a BMP file. */
void display_capture(const char *path);

/* Settings of the System Settings menu, in its order; values are indexes into each setting's choices.
 * Setting one applies it at once and saves eggoftime.ini. */
enum { SET_LANGUAGE, SET_WINDOW, SET_LAYOUT, SET_SECOND, SET_RESOLUTION, SET_ASPECT, SET_ZOOM, SET_FILL, SET_MINIMAP, SET_MINIMAP_SIZE, SET_MINIMAP_BG, SET_FRAMERATE, SET_VSYNC, SET_FILTER, SET_SHADER, SET_COLOR, SET_MODS, SET_FF_MUSIC, SET_FF_SOUND,
       SET_COUNT };
enum { LANGUAGE_ENGLISH, LANGUAGE_FRENCH }; /* SET_LANGUAGE values */
enum { FF_MUSIC_FAST, FF_MUSIC_NORMAL };    /* SET_FF_MUSIC values */
enum { FF_SOUND_ALL, FF_SOUND_MUSIC };      /* SET_FF_SOUND values */
const char *display_setting_title(int id);
int display_setting_count(int id);
int display_setting_get(int id);
/* The menu lists it: Fill screen sides only with a wide aspect ratio, Minimap only where the main screen of single
 * screen mode is shown, Minimap size and background only with Minimap on, Shader only with shaders in mods/shaders,
 * Internal resolution and Graphics filter only without a shader. */
int display_setting_shown(int id);
/* The chosen shader's path under mods/shaders (e.g. "glsl/crt/crt-geom.glslp"); NULL when Shader is Off. */
const char *display_shader_path(void);
const char *display_setting_label(int id, int value); /* may use a buffer shared by every call */
void display_setting_set(int id, int value);
/* Window frame interval for the chosen framerate, in nanoseconds; 0 for unlimited. */
uint64_t display_frame_ns(void);
/* The framerate is the game's own: a frame is presented as it arrives (60 fps). */
int display_follows_game(void);
