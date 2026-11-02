/* Single screen mode (single_screen.c): the game laid out for one screen. */
#pragma once

#include <stdint.h>

/* Build option (CMake EGG_SINGLE_SCREEN, default ON). Off keeps the DS's two-screen presentation and
 * defaults, for ports to devices with two screens. */
#ifndef EGG_SINGLE_SCREEN
#define EGG_SINGLE_SCREEN 1
#endif

/* Game thread only. */
uint32_t ss_game_keys(uint32_t keys); /* keys as the game reads them */
int ss_in_field(void);                /* the field (towns, maps) is running: widescreen applies */
int ss_in_warp(void);                 /* the warp through a time gate is running (widescreen, stretched sprites) */
int ss_in_overworld(void);            /* the world map is running (widescreen) */
int ss_in_race(void);                 /* the race against Johnny is running (widescreen, sky layers repeated) */
/* Columns the bottom screen's sprites starting above row *rows move left in a second picture for the main
 * screen (the age selector; 0: none). */
int ss_bottom_shift(int *rows);
/* Any thread: whether the System Settings menu (system_menu.c) may open now, and from where: the game's
 * Game Settings screen, the field paused, or the title screen's menu (New Game...). */
enum { SS_MENU_NONE, SS_MENU_GAME_SETTINGS, SS_MENU_PAUSE, SS_MENU_TITLE };
int ss_menu_place(void);
/* Builds the main screen from both screens of a finished frame (each 256x192 ABGR8888) and the top screen
 * with its widescreen margins (wide_w columns), and the bottom screen with sprites moved and the same margins
 * (ss_bottom_shift; NULL when not drawn) with which of its pixels are sprites; returns the main screen's width
 * (256, or wide_w in the field and the age selector). */
int ss_compose(uint32_t *main, const uint32_t *top, const uint32_t *bottom, const uint32_t *wide, int wide_w,
               const uint32_t *bottom_shifted, const uint8_t *shifted_sprite);
