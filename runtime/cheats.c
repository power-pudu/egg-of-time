/* Cheats, chosen on the System Settings menu's Cheats page (their settings are display settings, kept in
 * eggoftime.ini):
 *   - EXP and TP multipliers: what each defeated enemy gives is multiplied as the battle adds it to its rewards
 *     (rt_ct_battle_rewards), so the results, level ups and techs learned all follow from the multiplied total.
 *   - Infinite HP and MP: each frame, the party's HP and MP are put back to their maximum, in battle and out of it.
 *     A member who is down (HP 0) stays down: only a hit bigger than the maximum HP knocks one out.
 *   - Infinite money: each frame, the gold is put back to the most the game allows, 9,999,999.
 *
 * The game's data, Chrono Trigger DS: the characters' stats, 0x60 bytes each (Crono, Marle, Lucca, Robo, Frog, Ayla,
 * Magus): +0 max HP, +2 HP, +4 max MP, +6 MP (main 0x0202b244 and 0x0202b304 set HP and MP, capped at the maximum).
 * 0x8e0 bytes after them, the gold, 32-bit (main 0x0202c874 adds to it, up to 9,999,999).
 * 0x20 bytes before them, a pointer to the party: a character per slot, 0x80 set for none (main 0x0202a834).
 * In battle (overlays 0 and 3), two pointers in overlay 0's data: the members' battle copies, 0x80 bytes per party
 * slot (+3 HP, +7 MP, 16-bit and unaligned; +0x1d bit 7: down), written back to the stats when the battle ends
 * (ov003 0x021433c4); and the rewards, +0x4ee the EXP and +0x548 the TP (16-bit, as they are shared out: ov003
 * 0x0213044c, 0x0212fa50). */
#include <SDL3/SDL.h>

#include "cheats.h"
#include "dispatch.h"
#include "display.h"
#include "rt.h"

#define STATS GAME_ADDR(0x0207291cu, 0x0207293cu)
#define PARTY (STATS - 0x20u)
#define GOLD (STATS + 0x8e0u)
#define GOLD_MAX 9999999u
#define BATTLE_MEMBERS GAME_ADDR(0x02118c0cu, 0x02118c2cu)
#define BATTLE_REWARDS GAME_ADDR(0x02118c10u, 0x02118c30u)
#define REWARD_EXP 0x4eeu
#define REWARD_TP 0x548u

static const int multipliers[] = {1, 2, 4, 8, 10}; /* SET_CHEAT_EXP and SET_CHEAT_TP values */

static int in_ram(uint32_t a) { return a >= 0x02000000u && a < 0x02400000u; }

/* What the enemy just added to the 16-bit reward at a (from was there before) multiplied, up to 0xFFFF. */
static void multiply_reward(uint32_t a, uint32_t from, int setting) {
    uint32_t now = rt_r16(a), m = (uint32_t)multipliers[display_setting_get(setting)];
    if (now <= from || m == 1) return;
    rt_w16(a, SDL_min(now + (now - from) * (m - 1), 0xFFFFu));
}

/* ov000 0x021111f4 (Japanese 0x02111214), when an enemy is defeated: adds its EXP, gold and TP to the battle's
 * rewards (and its item to the drops). */
void rt_ct_battle_rewards(CPU *cpu) {
    uint32_t rewards = rt_r32(BATTLE_REWARDS);
    uint32_t exp = rt_r16(rewards + REWARD_EXP), tp = rt_r16(rewards + REWARD_TP);
    rt_call_wrapped(cpu, "ov000", GAME_ADDR(0x021111f4u, 0x02111214u));
    if (!in_ram(rewards)) return;
    multiply_reward(rewards + REWARD_EXP, exp, SET_CHEAT_EXP);
    multiply_reward(rewards + REWARD_TP, tp, SET_CHEAT_TP);
}

/* 16-bit values in the battle copies are unaligned. */
static uint32_t r16u(uint32_t a) { return rt_r8(a) | rt_r8(a + 1) << 8; }
static void w16u(uint32_t a, uint32_t v) { rt_w8(a, v & 0xFF), rt_w8(a + 1, v >> 8); }

void cheats_frame(void) {
    if (display_setting_get(SET_CHEAT_GOLD)) rt_w32(GOLD, GOLD_MAX);
    int hp = display_setting_get(SET_CHEAT_HP), mp = display_setting_get(SET_CHEAT_MP);
    if (!hp && !mp) return;
    for (uint32_t c = 0; c < 7; c++) {
        uint32_t s = STATS + 0x60u * c;
        if (hp && rt_r16(s + 2)) rt_w16(s + 2, rt_r16(s));
        if (mp) rt_w16(s + 6, rt_r16(s + 4));
    }
    uint32_t members = rt_r32(BATTLE_MEMBERS), party = rt_r32(PARTY);
    if (!rt_overlay_resident(0) || !rt_overlay_resident(3) || !in_ram(members) || !in_ram(party)) return;
    for (uint32_t slot = 0; slot < 3; slot++) {
        uint32_t c = rt_r8(party + slot), b = members + 0x80u * slot, s = STATS + 0x60u * c;
        if (c >= 7 || (rt_r8(b + 0x1d) & 0x80)) continue;
        if (hp && r16u(b + 3)) w16u(b + 3, rt_r16(s));
        if (mp) w16u(b + 7, rt_r16(s + 4));
    }
}
