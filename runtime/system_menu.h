/* System Settings menu (system_menu.c): the runtime's display settings, opened with SELECT from the game's
 * Game Settings screen, the title screen's menu or the paused field. Host thread only, except sysmenu_is_open. */
#pragma once

#include <stdint.h>

#include <SDL3/SDL.h>

/* Takes this frame's DS keys (keyboard, gamepad, automation) and returns the keys the game gets: none while
 * the menu is open, and none of the keys still held from closing it. */
uint32_t sysmenu_input(uint32_t keys);
int sysmenu_is_open(void); /* any thread */
/* Keyboard text, keys and gamepad buttons while a text field of the menu is being typed in (RetroAchievements
 * login); returns 1 if the event was used. */
int sysmenu_text_event(const SDL_Event *e);
/* Draws the menu over a 256x192 screen. */
void sysmenu_draw(uint32_t *screen);
/* A small button reading SELECT, in the menu's look, ending left of column x1 and above row y1: the hint that
 * SELECT opens the menu. */
#define SELECT_BUTTON_W 36
#define SELECT_BUTTON_H 13
void sysmenu_draw_select_button(uint32_t *screen, int x1, int y1);
