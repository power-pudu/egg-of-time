/* Button mapping (controls.c): which keyboard key and which gamepad button press each DS button, fast forward, the
 * save state slots and Exit game; changed in System Settings > Controls and saved in controls.ini. Host thread only. */
#pragma once

#include <stdint.h>

#include <SDL3/SDL.h>

/* The mapped actions, in the menu's order. */
enum { CTL_A, CTL_B, CTL_X, CTL_Y, CTL_L, CTL_R, CTL_START, CTL_SELECT, CTL_UP, CTL_DOWN, CTL_LEFT, CTL_RIGHT,
       CTL_FAST_FORWARD, CTL_SAVE_STATE, CTL_LOAD_STATE, CTL_EXIT_GAME, CTL_COUNT };

void controls_init(void); /* loads controls.ini */
/* The DS keys (KEY_*) held on the keyboard and on pad (NULL: none); the left stick also moves the D-pad. */
uint32_t controls_read(SDL_Gamepad *pad);
int controls_held(SDL_Gamepad *pad, int action); /* the action's key or button is held */

const char *controls_name(int action);
const char *controls_key_label(int action);
const char *controls_pad_label(int action);

/* Rebinding: the next key press (pad 0) or gamepad button or trigger press (pad 1), taken by controls_capture
 * from the event loop, becomes the action's. An input another action had swaps with this action's old one, so
 * every action keeps an input. The save state slots are the exception: they have no input until one is given to
 * them, so one taken from a slot leaves it empty, and an empty slot cannot take an input from an action that would
 * be left without one (the capture keeps waiting; controls_capture_conflict names that action until the next
 * capture). Exit game is held, not pressed, so it shares its input with any action: neither takes from the other.
 * Escape, or CAPTURE_MS without a press, cancels. Function keys (the hotkeys) are not taken. */
void controls_capture_start(int action, int pad);
void controls_capture_cancel(void);
int controls_capturing(void);
const char *controls_capture_conflict(void); /* NULL: none */
int controls_capture(const SDL_Event *e); /* returns 1 if it used the event */
void controls_reset(void);                /* the default mapping */
