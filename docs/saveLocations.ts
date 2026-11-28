// Save point locations: where a save slot can be set to load. Generated from the ROMs.
// Slot offsets: location u16 at 0x428 and 0x434, x u8 at 0x42A, y u8 at 0x42B, facing u8 at 0x42C.
// era and area are the game's own names (USA ROM: en, fr; Japanese ROM: ja). era is empty where the game shows none.
// room is the SNES location name, English only.

export interface LocalizedName { en: string; fr: string; ja: string }

export interface SaveLocation {
  era: LocalizedName
  area: LocalizedName
  room: string
  location: number
  x: number
  y: number
}

export const SAVE_LOCATIONS: readonly SaveLocation[] = [
  { era: { en: "Prehistory", fr: "Préhistoire", ja: "原始" }, area: { en: "Reptite Lair", fr: "Antre des sauriens", ja: "恐竜人アジト" }, room: "Reptite Lair Tunnel", location: 0x120, x: 13, y: 28 },
  { era: { en: "Prehistory", fr: "Préhistoire", ja: "原始" }, area: { en: "Tyranno Lair", fr: "Fort Tyrannos", ja: "ティラン城" }, room: "Tyrano Lair Storage", location: 0x12f, x: 8, y: 22 },
  { era: { en: "Prehistory", fr: "Préhistoire", ja: "原始" }, area: { en: "Tyranno Lair", fr: "Fort Tyrannos", ja: "ティラン城" }, room: "Tyrano Lair Nizbel's Room", location: 0x130, x: 7, y: 16 },
  { era: { en: "Prehistory", fr: "Préhistoire", ja: "原始" }, area: { en: "Mount Emerald", fr: "Mont verdoyant", ja: "緑山" }, room: "Lost Sanctum room 0x214", location: 0x214, x: 29, y: 3 },
  { era: { en: "Prehistory", fr: "Préhistoire", ja: "原始" }, area: { en: "Mount Emerald", fr: "Mont verdoyant", ja: "緑山" }, room: "Lost Sanctum room 0x216", location: 0x216, x: 9, y: 7 },
  { era: { en: "Prehistory", fr: "Préhistoire", ja: "原始" }, area: { en: "Mount Emerald", fr: "Mont verdoyant", ja: "緑山" }, room: "Lost Sanctum room 0x217", location: 0x217, x: 8, y: 11 },
  { era: { en: "Prehistory", fr: "Préhistoire", ja: "原始" }, area: { en: "Primeval Fortress", fr: "Forteresse antique", ja: "太古の城" }, room: "Lost Sanctum room 0x21C", location: 0x21c, x: 4, y: 26 },
  { era: { en: "Prehistory", fr: "Préhistoire", ja: "原始" }, area: { en: "Primeval Fortress", fr: "Forteresse antique", ja: "太古の城" }, room: "Lost Sanctum room 0x223", location: 0x223, x: 24, y: 9 },
  { era: { en: "Prehistory", fr: "Préhistoire", ja: "原始" }, area: { en: "Primeval Fortress", fr: "Forteresse antique", ja: "太古の城" }, room: "Lost Sanctum room 0x22A", location: 0x22a, x: 11, y: 47 },
  { era: { en: "Prehistory", fr: "Préhistoire", ja: "原始" }, area: { en: "Village", fr: "Village souterrain", ja: "竜の里" }, room: "Lost Sanctum room 0x247", location: 0x247, x: 30, y: 5 },
  { era: { en: "Prehistory", fr: "Préhistoire", ja: "原始" }, area: { en: "Village", fr: "Village souterrain", ja: "竜の里" }, room: "Lost Sanctum room 0x248", location: 0x248, x: 7, y: 7 },
  { era: { en: "Antiquity", fr: "Antiquité", ja: "古代" }, area: { en: "Zeal Palace", fr: "Palais de Zhyle", ja: "ジール宮殿" }, room: "Zeal Palace Regal Hall", location: 0x149, x: 6, y: 27 },
  { era: { en: "Antiquity", fr: "Antiquité", ja: "古代" }, area: { en: "Blackbird", fr: "Cygne noir", ja: "黒鳥号" }, room: "Blackbird Cell", location: 0x173, x: 21, y: 10 },
  { era: { en: "Antiquity", fr: "Antiquité", ja: "古代" }, area: { en: "Algetty", fr: "Alguetti", ja: "アルゲティ" }, room: "Algetty", location: 0x17c, x: 42, y: 42 },
  { era: { en: "Antiquity", fr: "Antiquité", ja: "古代" }, area: { en: "Mountain of Woe", fr: "Mont du tourment", ja: "なげきの山" }, room: "Mt. Woe Western Face", location: 0x188, x: 34, y: 27 },
  { era: { en: "Antiquity", fr: "Antiquité", ja: "古代" }, area: { en: "Mountain of Woe", fr: "Mont du tourment", ja: "なげきの山" }, room: "Mt. Woe Lower Eastern Face", location: 0x189, x: 27, y: 7 },
  { era: { en: "Antiquity", fr: "Antiquité", ja: "古代" }, area: { en: "Ocean Palace", fr: "Sanctuaire sous-marin", ja: "海底神殿" }, room: "Ocean Palace Entrance", location: 0x194, x: 10, y: 23 },
  { era: { en: "Antiquity", fr: "Antiquité", ja: "古代" }, area: { en: "Ocean Palace", fr: "Sanctuaire sous-marin", ja: "海底神殿" }, room: "Ocean Palace B20 Landing", location: 0x19a, x: 3, y: 10 },
  { era: { en: "Antiquity", fr: "Antiquité", ja: "古代" }, area: { en: "Ocean Palace", fr: "Sanctuaire sous-marin", ja: "海底神殿" }, room: "Ocean Palace Security Esplanade", location: 0x19d, x: 13, y: 6 },
  { era: { en: "Antiquity", fr: "Antiquité", ja: "古代" }, area: { en: "Surviving Village", fr: "Village des survivants", ja: "残された村" }, room: "Last Village Empty Hut", location: 0x1a9, x: 21, y: 27 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Giant's Claw", fr: "Griffe du géant", ja: "巨人のツメ" }, room: "Ancient Tyrano Lair", location: 0x06d, x: 61, y: 21 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Giant's Claw", fr: "Griffe du géant", ja: "巨人のツメ" }, room: "Ancient Tyrano Lair Traps", location: 0x06e, x: 7, y: 21 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Manolia Cathedral", fr: "Abbaye de Manolia", ja: "マノリア修道院" }, room: "Manoria Headquarters", location: 0x083, x: 17, y: 41 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Manolia Cathedral", fr: "Abbaye de Manolia", ja: "マノリア修道院" }, room: "Manoria Royal Guard Hall", location: 0x084, x: 3, y: 39 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Denadoro Mountains", fr: "Monts Dénadoro", ja: "デナドロ山" }, room: "Denadoro West Face", location: 0x095, x: 59, y: 10 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Fiendlord's Keep", fr: "Palais du Magistère", ja: "魔王城" }, room: "Castle Magus Entrance", location: 0x0a6, x: 11, y: 9 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Fiendlord's Keep", fr: "Palais du Magistère", ja: "魔王城" }, room: "Castle Magus Throne of Strength", location: 0x0a9, x: 8, y: 35 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Fiendlord's Keep", fr: "Palais du Magistère", ja: "魔王城" }, room: "Castle Magus Hall of Aggression", location: 0x0aa, x: 39, y: 61 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Fiendlord's Keep", fr: "Palais du Magistère", ja: "魔王城" }, room: "Castle Magus Throne of Defense", location: 0x0ae, x: 50, y: 52 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Fiendlord's Keep", fr: "Palais du Magistère", ja: "魔王城" }, room: "Castle Magus Throne of Defense", location: 0x0ae, x: 56, y: 52 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Fiendlord's Keep", fr: "Palais du Magistère", ja: "魔王城" }, room: "Castle Magus Grand Stairway", location: 0x1bf, x: 62, y: 9 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Mount Emerald", fr: "Mont verdoyant", ja: "緑山" }, room: "Lost Sanctum room 0x218", location: 0x218, x: 29, y: 3 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Mount Emerald", fr: "Mont verdoyant", ja: "緑山" }, room: "Lost Sanctum room 0x21A", location: 0x21a, x: 9, y: 25 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Mount Emerald", fr: "Mont verdoyant", ja: "緑山" }, room: "Lost Sanctum room 0x21B", location: 0x21b, x: 8, y: 11 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Tower of the Ancients", fr: "Tour séculaire", ja: "いにしえの塔" }, room: "Lost Sanctum room 0x22F", location: 0x22f, x: 4, y: 18 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Tower of the Ancients", fr: "Tour séculaire", ja: "いにしえの塔" }, room: "Lost Sanctum room 0x234", location: 0x234, x: 21, y: 6 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Tower of the Ancients", fr: "Tour séculaire", ja: "いにしえの塔" }, room: "Lost Sanctum room 0x23E", location: 0x23e, x: 9, y: 7 },
  { era: { en: "Middle Ages", fr: "Moyen-Âge", ja: "中世" }, area: { en: "Village", fr: "Village souterrain", ja: "竜の里" }, room: "Lost Sanctum room 0x249", location: 0x249, x: 30, y: 5 },
  { era: { en: "Present", fr: "Présent", ja: "現代" }, area: { en: "Truce", fr: "Truce", ja: "トルース町" }, room: "Truce Mayor's Manor 1F", location: 0x00d, x: 4, y: 24 },
  { era: { en: "Present", fr: "Présent", ja: "現代" }, area: { en: "Prison Towers", fr: "Prison", ja: "空中刑務所" }, room: "Prison Supervisor's Office", location: 0x01d, x: 13, y: 10 },
  { era: { en: "Present", fr: "Présent", ja: "現代" }, area: { en: "Heckran Cave", fr: "Grottes d'Héclan", ja: "ヘケランの洞窟" }, room: "Heckran Cave Underground River", location: 0x031, x: 22, y: 37 },
  { era: { en: "Present", fr: "Présent", ja: "現代" }, area: { en: "Prison Towers", fr: "Prison", ja: "空中刑務所" }, room: "Prison Cells", location: 0x047, x: 17, y: 32 },
  { era: { en: "Future", fr: "Futur", ja: "未来" }, area: { en: "Arris Dome", fr: "Dôme d'Arris", ja: "アリスドーム" }, room: "Arris Dome", location: 0x0d6, x: 38, y: 9 },
  { era: { en: "Future", fr: "Futur", ja: "未来" }, area: { en: "Derelict Factory", fr: "Usine désaffectée", ja: "工場跡" }, room: "Factory Ruins Auxiliary Console", location: 0x0e5, x: 32, y: 22 },
  { era: { en: "Future", fr: "Futur", ja: "未来" }, area: { en: "Abandoned Sewers", fr: "Ancien égout", ja: "地下水道跡" }, room: "Sewer Access B2", location: 0x0ed, x: 16, y: 29 },
  { era: { en: "Future", fr: "Futur", ja: "未来" }, area: { en: "Death Peak", fr: "Mont des défunts", ja: "死の山" }, room: "Death Peak Southeast Face", location: 0x0f6, x: 53, y: 27 },
  { era: { en: "Future", fr: "Futur", ja: "未来" }, area: { en: "Geno Dome", fr: "Dôme de Génos", ja: "ジェノサイドーム" }, room: "Geno Dome Mainframe", location: 0x10c, x: 44, y: 35 },
  { era: { en: "Future", fr: "Futur", ja: "未来" }, area: { en: "Death Peak", fr: "Mont des défunts", ja: "死の山" }, room: "Death Peak Guardian Spawn", location: 0x1ef, x: 6, y: 39 },
  { era: { en: "Dimensional Vortex", fr: "Vortex dimensionnel", ja: "次元のゆがみ" }, area: { en: "Nameless Cave", fr: "Grotte sans nom", ja: "名も無き洞窟" }, room: "Dimensional Vortex room 0x259", location: 0x259, x: 29, y: 20 },
  { era: { en: "Dimensional Vortex", fr: "Vortex dimensionnel", ja: "次元のゆがみ" }, area: { en: "Twilight Grotto", fr: "Passage obscur", ja: "黒の洞窟" }, room: "Dimensional Vortex room 0x270", location: 0x270, x: 24, y: 41 },
  { era: { en: "Dimensional Vortex", fr: "Vortex dimensionnel", ja: "次元のゆがみ" }, area: { en: "Abandoned Sewers", fr: "Ancien égout", ja: "地下水道跡" }, room: "Dimensional Vortex room 0x27C", location: 0x27c, x: 16, y: 29 },
  { era: { en: "Dimensional Vortex", fr: "Vortex dimensionnel", ja: "次元のゆがみ" }, area: { en: "Nameless Cave", fr: "Grotte sans nom", ja: "名も無き洞窟" }, room: "Dimensional Vortex room 0x297", location: 0x297, x: 29, y: 20 },
  { era: { en: "Dimensional Vortex", fr: "Vortex dimensionnel", ja: "次元のゆがみ" }, area: { en: "Nameless Cave", fr: "Grotte sans nom", ja: "名も無き洞窟" }, room: "Dimensional Vortex room 0x298", location: 0x298, x: 29, y: 20 },
  { era: { en: "Dimensional Vortex", fr: "Vortex dimensionnel", ja: "次元のゆがみ" }, area: { en: "Twilight Grotto", fr: "Passage obscur", ja: "黒の洞窟" }, room: "Dimensional Vortex room 0x299", location: 0x299, x: 24, y: 41 },
  { era: { en: "Dimensional Vortex", fr: "Vortex dimensionnel", ja: "次元のゆがみ" }, area: { en: "Twilight Grotto", fr: "Passage obscur", ja: "黒の洞窟" }, room: "Dimensional Vortex room 0x29A", location: 0x29a, x: 24, y: 41 },
  { era: { en: "", fr: "", ja: "" }, area: { en: "Black Omen", fr: "Songe obscur", ja: "黒の夢" }, room: "Black Omen 1F Walkway", location: 0x135, x: 14, y: 17 },
  { era: { en: "", fr: "", ja: "" }, area: { en: "Black Omen", fr: "Songe obscur", ja: "黒の夢" }, room: "Black Omen room 0x13B", location: 0x13b, x: 4, y: 17 },
  { era: { en: "", fr: "", ja: "" }, area: { en: "Black Omen", fr: "Songe obscur", ja: "黒の夢" }, room: "Black Omen room 0x143", location: 0x143, x: 38, y: 44 },
  { era: { en: "", fr: "", ja: "" }, area: { en: "Black Omen", fr: "Songe obscur", ja: "黒の夢" }, room: "Black Omen room 0x145", location: 0x145, x: 2, y: 23 },
  { era: { en: "", fr: "", ja: "" }, area: { en: "Black Omen", fr: "Songe obscur", ja: "黒の夢" }, room: "Black Omen Omega Defense", location: 0x1c2, x: 3, y: 9 },
  { era: { en: "", fr: "", ja: "" }, area: { en: "The End of Time", fr: "Fin des temps", ja: "時の最果て" }, room: "End of Time", location: 0x1d0, x: 24, y: 22 },
  { era: { en: "", fr: "", ja: "" }, area: { en: "???", fr: "???", ja: "？？？" }, room: "Lavos Tunnel", location: 0x1da, x: 11, y: 60 },
  { era: { en: "", fr: "", ja: "" }, area: { en: "Arena of the Ages", fr: "Arène", ja: "次元の闘技場" }, room: "Arena of the Ages room 0x29B", location: 0x29b, x: 30, y: 15 },
]
