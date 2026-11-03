/* Deliberate behaviour changes to the game (Chrono Trigger DS, YQUE), wired in via tools/recomp/overrides.txt. */
#include "dispatch.h"
#include "rt.h"
#include "single_screen.h"
#include "system_menu.h"

static void game_settings_defaults(CPU *cpu) { rt_call_wrapped(cpu, "ov007", 0x0212a3f0u); } /* the original */

/* New Game "Game Settings" scene (ov007): resets the three options, both when the scene opens and on
 * "Defaults" (SELECT). The original picks Game Mode DS, Battle Mode Active, Movies On; in single screen mode
 * the port defaults Game Mode to Classic (one-screen battle interface). The options are bits of the save configuration
 * that *(0x020708d0) points to: Game Mode byte 2 bit 7 (1 = Classic), Battle Mode byte 0 bit 6
 * (1 = Wait), Movies byte 2 bits 3-4 (0 = On). r0 is the scene object. */
void rt_ct_settings_defaults(CPU *cpu) {
    if (!ss_enabled()) {
        game_settings_defaults(cpu);
        return;
    }
    uint32_t cfg = rt_r32(0x020708d0u);
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
    else rt_call_wrapped(cpu, "ov007", 0x02127618u);
}
