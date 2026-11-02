/* Button mapping (controls.c): which keyboard key and which gamepad button press each DS button, and fast
 * forward; changed in System Settings > Controls and saved in controls.ini. Host thread only. */
#pragma once

#include <stdint.h>

#include <SDL3/SDL.h>

/* The mapped actions, in the menu's order. */
enum { CTL_A, CTL_B, CTL_X, CTL_Y, CTL_L, CTL_R, CTL_START, CTL_SELECT, CTL_UP, CTL_DOWN, CTL_LEFT, CTL_RIGHT,
       CTL_FAST_FORWARD, CTL_COUNT };

void controls_init(void); /* loads controls.ini */
/* The DS keys (KEY_*) held on the keyboard and on pad (NULL: none); the left stick also moves the D-pad. */
uint32_t controls_read(SDL_Gamepad *pad);
int controls_fast_forward(SDL_Gamepad *pad); /* the fast forward key or button is held */

const char *controls_name(int action);
const char *controls_key_label(int action);
const char *controls_pad_label(int action);

/* Rebinding: the next key press (pad 0) or gamepad button or trigger press (pad 1), taken by controls_capture
 * from the event loop, becomes the action's. An input another action had swaps with this action's old one, so
 * every action keeps an input. Escape, or CAPTURE_MS without a press, cancels. Function keys (the hotkeys)
 * are not taken. */
void controls_capture_start(int action, int pad);
void controls_capture_cancel(void);
int controls_capturing(void);
int controls_capture(const SDL_Event *e); /* returns 1 if it used the event */
void controls_reset(void);                /* the default mapping */
