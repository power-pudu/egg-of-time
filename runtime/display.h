/* Window presentation of the two screens and the display settings (display.c). Host thread only. */
#pragma once

#include <stdint.h>

#include <SDL3/SDL.h>

int display_init(void); /* window, renderer, saved settings; 0 on failure */
/* Draws a window frame; new_frame uploads the latest screens: top, bottom (256x192), main (192 rows of
 * `width` columns) and the main screen's party layer (NULL: none), as video_copy_frame returns them. slide_dx/dy:
 * camera smoothing, where the field picture is shown from (pixels; the party layer does not slide). */
void display_frame(const uint32_t *const screens[3], const uint32_t *party, int width, int new_frame, float slide_dx,
                   float slide_dy);
/* Whether the framerate setting asks for camera smoothing (above 60). */
int display_camera_smoothing(void);
/* F2 layout, F3 swap screens, F4 filter, F11 fullscreen; returns 1 if the key was used. */
int display_key(SDL_Scancode sc);
/* Window position -> bottom-screen pixel, when the bottom screen is shown there. */
int display_touch(float wx, float wy, int *x, int *y);
/* Shows text in the window's message box for ms milliseconds (replacing any current message). */
void display_message(const char *text, unsigned ms);
/* Saves the next presented window as a BMP file. */
void display_capture(const char *path);

/* Settings of the System Settings menu, in its order; values are indexes into each setting's choices.
 * Setting one applies it at once and saves eggoftime.ini. */
enum { SET_LANGUAGE, SET_WINDOW, SET_RESOLUTION, SET_ASPECT, SET_FILL, SET_FRAMERATE, SET_VSYNC, SET_FILTER, SET_COLOR, SET_MODS, SET_FF_MUSIC,
       SET_COUNT };
enum { LANGUAGE_ENGLISH, LANGUAGE_FRENCH }; /* SET_LANGUAGE values */
enum { FF_MUSIC_FAST, FF_MUSIC_NORMAL };    /* SET_FF_MUSIC values */
const char *display_setting_title(int id);
int display_setting_count(int id);
int display_setting_get(int id);
int display_setting_shown(int id); /* the menu lists it (Fill screen sides: only with a wide aspect ratio) */
const char *display_setting_label(int id, int value); /* may use a buffer shared by every call */
void display_setting_set(int id, int value);
/* Window frame interval for the chosen framerate, in nanoseconds; 0 for unlimited. */
uint64_t display_frame_ns(void);
