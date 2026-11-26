/* Deliberate behaviour changes to the game (Chrono Trigger DS), wired in via tools/recomp/overrides.txt. */
#include <SDL3/SDL.h>

#include "dispatch.h"
#include "display.h"
#include "gfx_font.h"
#include "rt.h"
#include "single_screen.h"
#include "system_menu.h"
#include "ui_text.h"

static void game_settings_defaults(CPU *cpu) { rt_call_wrapped(cpu, "ov007", GAME_ADDR(0x0212a3f0u, 0x0212a2c8u)); } /* the original */

/* New Game "Game Settings" scene (ov007): resets the three options, both when the scene opens and on
 * "Defaults" (SELECT). The original picks Game Mode DS, Battle Mode Active, Movies On; with the main screen
 * shown alone (ss_classic_only) the port defaults Game Mode to Classic (one-screen battle interface). The options are bits of the save configuration
 * that *(0x020708d0) points to: Game Mode byte 2 bit 7 (1 = Classic), Battle Mode byte 0 bit 6
 * (1 = Wait), Movies byte 2 bits 3-4 (0 = On). r0 is the scene object. */
void rt_ct_settings_defaults(CPU *cpu) {
    if (!ss_classic_only()) {
        game_settings_defaults(cpu);
        return;
    }
    uint32_t cfg = rt_r32(GAME_ADDR(0x020708d0u, 0x020708f0u));
    rt_w8(cfg, rt_r8(cfg) & ~0x40u);
    rt_w8(cfg + 2, (rt_r8(cfg + 2) & ~0x18u) | 0x80u);
    /* The cursor column (+0x3f02) shows the value of the option on the cursor row (+0x3f00, kept by "Defaults"). */
    rt_w8(R[0] + 0x3f02, rt_r8(R[0] + 0x3f00) == 0);
    rt_w8(R[0] + 0x3f06, 1); /* redraw the description panel, which the scene first draws for DS */
}

/* Title scene (ov007), each frame on the formed logo and in the menu: counts idle frames at scene + 0x3a44
 * (Up/Down reset it) and after 0x708 (30 s) leaves for the intro movie. The System Settings menu keeps every key
 * from the game, so while it is open the count stays at 0: the title waits for it. r0 is the scene object. */
void rt_ct_title_idle(CPU *cpu) {
    if (sysmenu_is_open()) rt_w16(R[0] + 0x3a44, 0);
    else rt_call_wrapped(cpu, "ov007", GAME_ADDR(0x02127618u, 0x021274f0u));
}

/* The front end (main 0x0204bfcc) runs its scenes by state, from r0: 0 the save check (ov007, nothing shown
 * unless the save needs a question: it reads the save, its system settings and whether a game was saved, which the
 * title's Continue needs), 1 the logos ("Licensed by Nintendo", then Square Enix and Mobiclip), 2 the intro movie
 * (main 0x0204c668, which returns the next state: 3, the title), 3 the title. Power-on enters at 0. Each scene's
 * result, its first byte the next state, is read by ov007 0x02121704 (r0 the scene's result bytes) after it ends.
 * Skip intro logos turns the save check's next state 1 into 2; Skip intro movie leaves out the first intro of each
 * entry, returning 3 at once (the title idling into the intro still plays it). */
static int intro_first; /* the front end's first intro is still to come */
static int check_result; /* the front end entered at the save check, whose result is the next one read */

void rt_ct_front_end(CPU *cpu) {
    check_result = R[0] == 0;
    intro_first = 1;
    rt_call_wrapped(cpu, "main", GAME_ADDR(0x0204bfccu, 0x0204bffcu));
}

void rt_ct_scene_result(CPU *cpu) {
    rt_call_wrapped(cpu, "ov007", 0x02121704u);
    if (check_result && (R[0] & 0xFFu) == 1 && display_setting_get(SET_SKIP_LOGOS)) R[0] = (R[0] & ~0xFFu) | 2;
    check_result = 0;
}

void rt_ct_intro(CPU *cpu) {
    int first = intro_first;
    intro_first = 0;
    if (first && display_setting_get(SET_SKIP_MOVIE)) R[0] = 3;
    else rt_call_wrapped(cpu, "main", GAME_ADDR(0x0204c668u, 0x0204c698u));
}

/* The game's cursor sound (sound effect 0, as its menus play it when the cursor moves) for the runtime's menus
 * (System Settings), asked from the host thread and played on the game's main thread at a point where it runs no
 * other code: NitroSDK's OS_WaitVBlankIntr (main 0x02008fa4), once a frame, calling SVC WaitByLoop(1) before it
 * sleeps (other WaitByLoop callers, such as the sound library's waits, are left alone). It is told by its code at
 * the return address: mov r0, #1; mov r1, r0 after the call, push {r3, lr}; mov r0, #1 before. The sound is played
 * through the game's own function (main 0x020445f0, r0 the effect, r1 0: ov012 0x0214b938 in the field menu), found
 * by its code, the same in either region: ldr r1, =0x18008000; ldr ip, =<sound command>; orr r0, r1, r0, lsl #16;
 * bx ip (the sound command word 0x18 plays an effect). */
static SDL_AtomicInt cursor_sound;

void rt_menu_cursor_sound(void) { SDL_SetAtomicInt(&cursor_sound, 1); }

static uint32_t sound_effect_fn(void) {
    static uint32_t found;
    static const uint32_t code[] = {0xe59f1008u, 0xe59fc008u, 0xe1810800u, 0xe12fff1cu, 0x18008000u};
    for (uint32_t a = 0x02000000u; !found && a < 0x020a0000u; a += 4) {
        size_t i = 0;
        while (i < SDL_arraysize(code) && rt_r32(a + 4 * (uint32_t)i) == code[i]) i++;
        if (i == SDL_arraysize(code)) found = a;
    }
    return found;
}

void rt_menu_sounds(CPU *cpu) {
    uint32_t lr = R[14];
    if (!SDL_GetAtomicInt(&cursor_sound) || lr >> 24 != 0x02 || rt_r32(lr) != 0xe3a00001u || rt_r32(lr + 4) != 0xe1a01000u ||
        rt_r32(lr - 8) != 0xe3a00001u || rt_r32(lr - 12) != 0xe92d4008u)
        return;
    SDL_SetAtomicInt(&cursor_sound, 0);
    uint32_t fn = sound_effect_fn();
    if (!fn) return;
    uint32_t saved[5] = {R[0], R[1], R[2], R[3], R[12]};
    rt_call_game(cpu, fn, 0, 0, 0);
    for (int i = 0; i < 4; i++) R[i] = saved[i];
    R[12] = saved[4];
}

/* Settings (the field menu's page and New Game's Game Settings), main screen alone: SELECT opens the runtime's System
 * Settings, and the game itself draws its label there, so font mods and HD letters take it as they take the rest. Its
 * text reader (main 0x020380b8: one letter a call, the box r1, the reader r2, 0 when done) draws "Defaults", entry 100
 * of the scene's text, into its own 8 x 2 tile buffer: once done, the buffer is emptied and "System settings" drawn
 * there in the small font (the big one is wider than the button's 64 columns). A character the small font lacks
 * leaves "Defaults", painted over as before (single_screen.c relabel_select). */
#define TEXT_DEFAULTS 100
#define FONT_SMALL_SLOT 0x020711b0u /* USA / Europe; the Japanese game keeps the painted label */

void rt_ct_text_step(CPU *cpu) {
    uint32_t box = R[1], reader = R[2];
    rt_call_wrapped(cpu, "main", 0x020380b8u);
    if (RT_JAPAN || R[0] || rt_r32(reader + 8) != TEXT_DEFAULTS || !ss_select_opens_menu()) return;
    uint32_t buf = rt_r32(box + 4), size = (uint32_t)(rt_r16(box + 0xc) * rt_r16(box + 0xe) * 32);
    uint8_t was[1024];
    if (size > sizeof was) return;
    for (uint32_t a = 0; a < size; a++) was[a] = rt_r8(buf + a), rt_w8(buf + a, 0);
    int drawn = gfxfont_game_text(cpu, FONT_SMALL_SLOT, buf, rt_r16(box + 0xc), ui_text_in(rt_language(), "System settings"), 0, 2, 0);
    if (!drawn) for (uint32_t a = 0; a < size; a++) rt_w8(buf + a, was[a]); /* the small font cannot: "Defaults" */
    ss_set_select_drawn(drawn);
    R[0] = 0;
}
