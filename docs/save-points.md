# Save point locations

Every save point in the game files, for setting where a save slot loads. Generated from the ROMs: event scripts
(`PS_BIN/Atel`), the location table (`PS_BIN/bankf6.bin`), and the game's own era and area names (`msg/big/menu.msg`,
`msg/big/map.msg`, by the per-location table in ov012 at file offset 0x374B8). English and French come from the USA
ROM (language slots 1 and 3), Japanese from the Japanese ROM (slot 0). Room names are the SNES location names and
exist in English only.

A location's event script is the u16 at arm9.bin file offset 0x59E10 + 20 x location id (a table that also
covers the DS-only rooms).

## Fields in a save slot

Offsets are relative to the start of a slot (0x0028, 0x2828, 0x5028 in the 64 KB save).

| Offset | Size | Field |
|---|---|---|
| 0x428 | u16 | Location id |
| 0x42A | u8 | X, in 16-px tiles |
| 0x42B | u8 | Y, in 16-px tiles |
| 0x42C | u8 | Facing (0 is fine) |
| 0x434 | u16 | Location id again (write the same value) |

Recompute the slot checksum after editing (u32 sum of slot+4 .. slot+0x27FF, stored at slot+0). Tested: a slot
set to Algetty loads with the party standing there.

DS-only rooms (Lost Sanctum, Dimensional Vortex, Arena of the Ages) are included. Their location ids are 0x214-0x29C;
their rooms have no names of their own, so Room names the place and the id. Some of them play their quest scene
when entered (0x249, the Middle Ages village, does).

## Save points

| Era | Area | Area (French) | Area (Japanese) | Room | Location | X | Y |
|---|---|---|---|---|---|---|---|
| Prehistory | Reptite Lair | Antre des sauriens | 恐竜人アジト | Reptite Lair Tunnel | 0x120 | 13 | 28 |
| Prehistory | Tyranno Lair | Fort Tyrannos | ティラン城 | Tyrano Lair Storage | 0x12F | 8 | 22 |
| Prehistory | Tyranno Lair | Fort Tyrannos | ティラン城 | Tyrano Lair Nizbel's Room | 0x130 | 7 | 16 |
| Prehistory | Mount Emerald | Mont verdoyant | 緑山 | Lost Sanctum room 0x214 | 0x214 | 29 | 3 |
| Prehistory | Mount Emerald | Mont verdoyant | 緑山 | Lost Sanctum room 0x216 | 0x216 | 9 | 7 |
| Prehistory | Mount Emerald | Mont verdoyant | 緑山 | Lost Sanctum room 0x217 | 0x217 | 8 | 11 |
| Prehistory | Primeval Fortress | Forteresse antique | 太古の城 | Lost Sanctum room 0x21C | 0x21C | 4 | 26 |
| Prehistory | Primeval Fortress | Forteresse antique | 太古の城 | Lost Sanctum room 0x223 | 0x223 | 24 | 9 |
| Prehistory | Primeval Fortress | Forteresse antique | 太古の城 | Lost Sanctum room 0x22A | 0x22A | 11 | 47 |
| Prehistory | Village | Village souterrain | 竜の里 | Lost Sanctum room 0x247 | 0x247 | 30 | 5 |
| Prehistory | Village | Village souterrain | 竜の里 | Lost Sanctum room 0x248 | 0x248 | 7 | 7 |
| Antiquity | Zeal Palace | Palais de Zhyle | ジール宮殿 | Zeal Palace Regal Hall | 0x149 | 6 | 27 |
| Antiquity | Blackbird | Cygne noir | 黒鳥号 | Blackbird Cell | 0x173 | 21 | 10 |
| Antiquity | Algetty | Alguetti | アルゲティ | Algetty | 0x17C | 42 | 42 |
| Antiquity | Mountain of Woe | Mont du tourment | なげきの山 | Mt. Woe Western Face | 0x188 | 34 | 27 |
| Antiquity | Mountain of Woe | Mont du tourment | なげきの山 | Mt. Woe Lower Eastern Face | 0x189 | 27 | 7 |
| Antiquity | Ocean Palace | Sanctuaire sous-marin | 海底神殿 | Ocean Palace Entrance | 0x194 | 10 | 23 |
| Antiquity | Ocean Palace | Sanctuaire sous-marin | 海底神殿 | Ocean Palace B20 Landing | 0x19A | 3 | 10 |
| Antiquity | Ocean Palace | Sanctuaire sous-marin | 海底神殿 | Ocean Palace Security Esplanade | 0x19D | 13 | 6 |
| Antiquity | Surviving Village | Village des survivants | 残された村 | Last Village Empty Hut | 0x1A9 | 21 | 27 |
| Middle Ages | Giant's Claw | Griffe du géant | 巨人のツメ | Ancient Tyrano Lair | 0x06D | 61 | 21 |
| Middle Ages | Giant's Claw | Griffe du géant | 巨人のツメ | Ancient Tyrano Lair Traps | 0x06E | 7 | 21 |
| Middle Ages | Manolia Cathedral | Abbaye de Manolia | マノリア修道院 | Manoria Headquarters | 0x083 | 17 | 41 |
| Middle Ages | Manolia Cathedral | Abbaye de Manolia | マノリア修道院 | Manoria Royal Guard Hall | 0x084 | 3 | 39 |
| Middle Ages | Denadoro Mountains | Monts Dénadoro | デナドロ山 | Denadoro West Face | 0x095 | 59 | 10 |
| Middle Ages | Fiendlord's Keep | Palais du Magistère | 魔王城 | Castle Magus Entrance | 0x0A6 | 11 | 9 |
| Middle Ages | Fiendlord's Keep | Palais du Magistère | 魔王城 | Castle Magus Throne of Strength | 0x0A9 | 8 | 35 |
| Middle Ages | Fiendlord's Keep | Palais du Magistère | 魔王城 | Castle Magus Hall of Aggression | 0x0AA | 39 | 61 |
| Middle Ages | Fiendlord's Keep | Palais du Magistère | 魔王城 | Castle Magus Throne of Defense | 0x0AE | 50 | 52 |
| Middle Ages | Fiendlord's Keep | Palais du Magistère | 魔王城 | Castle Magus Throne of Defense | 0x0AE | 56 | 52 |
| Middle Ages | Fiendlord's Keep | Palais du Magistère | 魔王城 | Castle Magus Grand Stairway | 0x1BF | 62 | 9 |
| Middle Ages | Mount Emerald | Mont verdoyant | 緑山 | Lost Sanctum room 0x218 | 0x218 | 29 | 3 |
| Middle Ages | Mount Emerald | Mont verdoyant | 緑山 | Lost Sanctum room 0x21A | 0x21A | 9 | 25 |
| Middle Ages | Mount Emerald | Mont verdoyant | 緑山 | Lost Sanctum room 0x21B | 0x21B | 8 | 11 |
| Middle Ages | Tower of the Ancients | Tour séculaire | いにしえの塔 | Lost Sanctum room 0x22F | 0x22F | 4 | 18 |
| Middle Ages | Tower of the Ancients | Tour séculaire | いにしえの塔 | Lost Sanctum room 0x234 | 0x234 | 21 | 6 |
| Middle Ages | Tower of the Ancients | Tour séculaire | いにしえの塔 | Lost Sanctum room 0x23E | 0x23E | 9 | 7 |
| Middle Ages | Village | Village souterrain | 竜の里 | Lost Sanctum room 0x249 | 0x249 | 30 | 5 |
| Present | Truce | Truce | トルース町 | Truce Mayor's Manor 1F | 0x00D | 4 | 24 |
| Present | Prison Towers | Prison | 空中刑務所 | Prison Supervisor's Office | 0x01D | 13 | 10 |
| Present | Heckran Cave | Grottes d'Héclan | ヘケランの洞窟 | Heckran Cave Underground River | 0x031 | 22 | 37 |
| Present | Prison Towers | Prison | 空中刑務所 | Prison Cells | 0x047 | 17 | 32 |
| Future | Arris Dome | Dôme d'Arris | アリスドーム | Arris Dome | 0x0D6 | 38 | 9 |
| Future | Derelict Factory | Usine désaffectée | 工場跡 | Factory Ruins Auxiliary Console | 0x0E5 | 32 | 22 |
| Future | Abandoned Sewers | Ancien égout | 地下水道跡 | Sewer Access B2 | 0x0ED | 16 | 29 |
| Future | Death Peak | Mont des défunts | 死の山 | Death Peak Southeast Face | 0x0F6 | 53 | 27 |
| Future | Geno Dome | Dôme de Génos | ジェノサイドーム | Geno Dome Mainframe | 0x10C | 44 | 35 |
| Future | Death Peak | Mont des défunts | 死の山 | Death Peak Guardian Spawn | 0x1EF | 6 | 39 |
| Dimensional Vortex | Nameless Cave | Grotte sans nom | 名も無き洞窟 | Dimensional Vortex room 0x259 | 0x259 | 29 | 20 |
| Dimensional Vortex | Twilight Grotto | Passage obscur | 黒の洞窟 | Dimensional Vortex room 0x270 | 0x270 | 24 | 41 |
| Dimensional Vortex | Abandoned Sewers | Ancien égout | 地下水道跡 | Dimensional Vortex room 0x27C | 0x27C | 16 | 29 |
| Dimensional Vortex | Nameless Cave | Grotte sans nom | 名も無き洞窟 | Dimensional Vortex room 0x297 | 0x297 | 29 | 20 |
| Dimensional Vortex | Nameless Cave | Grotte sans nom | 名も無き洞窟 | Dimensional Vortex room 0x298 | 0x298 | 29 | 20 |
| Dimensional Vortex | Twilight Grotto | Passage obscur | 黒の洞窟 | Dimensional Vortex room 0x299 | 0x299 | 24 | 41 |
| Dimensional Vortex | Twilight Grotto | Passage obscur | 黒の洞窟 | Dimensional Vortex room 0x29A | 0x29A | 24 | 41 |
| (none) | Black Omen | Songe obscur | 黒の夢 | Black Omen 1F Walkway | 0x135 | 14 | 17 |
| (none) | Black Omen | Songe obscur | 黒の夢 | Black Omen room 0x13B | 0x13B | 4 | 17 |
| (none) | Black Omen | Songe obscur | 黒の夢 | Black Omen room 0x143 | 0x143 | 38 | 44 |
| (none) | Black Omen | Songe obscur | 黒の夢 | Black Omen room 0x145 | 0x145 | 2 | 23 |
| (none) | Black Omen | Songe obscur | 黒の夢 | Black Omen Omega Defense | 0x1C2 | 3 | 9 |
| (none) | The End of Time | Fin des temps | 時の最果て | End of Time | 0x1D0 | 24 | 22 |
| (none) | ??? | ??? | ？？？ | Lavos Tunnel | 0x1DA | 11 | 60 |
| (none) | Arena of the Ages | Arène | 次元の闘技場 | Arena of the Ages room 0x29B | 0x29B | 30 | 15 |
