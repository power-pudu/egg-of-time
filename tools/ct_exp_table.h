/* Chrono Trigger DS (USA) level-up EXP, read from arm9.bin 0x0205540C (data_0205540c).
 * Used by func_0202bee0: when a character gains EXP, the game subtracts it from the u16 at
 * record + 0x3A ("EXP to next level"). When that counter would reach 0 or below, the level
 * (u8 at record + 0x19) goes up by 1 and the counter is reloaded with ct_exp_to_next[new level].
 * At level 99 the counter is 0. The u32 total EXP at record + 0x1C is only added to (capped at
 * 9999999 by func_0202be5c); the game never uses it to decide the level.
 * Record = 0x60 bytes per character; Crono starts at 0x484 in save slot 1. */
#ifndef CT_EXP_TABLE_H
#define CT_EXP_TABLE_H
#include <stdint.h>

/* EXP needed to go from level L to level L+1. Index 0 is unused; index 99 is 0 (max level). */
static const uint16_t ct_exp_to_next[100] = {
         0,     20,     40,     70,    110,    160,    220,    300,    400,    520,
       650,    790,    940,   1100,   1270,   1450,   1640,   1840,   2050,   2270,
      2500,   2740,   2990,   3250,   3520,   3800,   4090,   4390,   4700,   5020,
      5350,   5690,   6040,   6400,   6770,   7150,   7540,   7940,   8350,   8770,
      9200,   9640,  10090,  10550,  11020,  11500,  11990,  12490,  13000,  13520,
     14050,  14590,  15140,  15700,  16270,  16850,  17440,  18040,  18650,  19270,
     19900,  20540,  21190,  21850,  22520,  23200,  23890,  24590,  25300,  26020,
     26750,  27490,  28240,  29000,  29770,  30550,  31340,  32140,  32950,  33770,
     34600,  35440,  36290,  37150,  38020,  38900,  39790,  40690,  41600,  42520,
     43450,  44390,  45340,  46300,  47270,  48250,  49240,  50240,  51250,      0
};

/* Total EXP when level L is first reached: sum of ct_exp_to_next[1..L-1]. Index 0 unused. */
static const uint32_t ct_exp_total[100] = {
         0,      0,     20,     60,    130,    240,    400,    620,    920,   1320,
      1840,   2490,   3280,   4220,   5320,   6590,   8040,   9680,  11520,  13570,
     15840,  18340,  21080,  24070,  27320,  30840,  34640,  38730,  43120,  47820,
     52840,  58190,  63880,  69920,  76320,  83090,  90240,  97780, 105720, 114070,
    122840, 132040, 141680, 151770, 162320, 173340, 184840, 196830, 209320, 222320,
    235840, 249890, 264480, 279620, 295320, 311590, 328440, 345880, 363920, 382570,
    401840, 421740, 442280, 463470, 485320, 507840, 531040, 554930, 579520, 604820,
    630840, 657590, 685080, 713320, 742320, 772090, 802640, 833980, 866120, 899070,
    932840, 967440, 1002880, 1039170, 1076320, 1114340, 1153240, 1193030, 1233720, 1275320,
    1317840, 1361290, 1405680, 1451020, 1497320, 1544590, 1592840, 1642080, 1692320, 1743570
};

#endif
