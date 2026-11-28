# Prompt for the Save Editor agent: Save location picker

Add a way to choose where a save slot loads: a "Save location" picker listing every save point in the game. The
user picks one, and the next time the game loads that slot, the party starts standing on that save point. This
lets testers jump straight to a room before a quest without playing up to it.

The game logic below was worked out and tested by the Egg of Time agent (the Chrono Trigger DS recompilation
project). It counts as the answer that MEMORY.md rule 9 asks for. Do not change offsets or values. If anything
here does not fit what you find in the code, ask the user before guessing.

## The data file

Copy `C:\Users\sirgu\OneDrive\Documentos\Claude\EggOfTime\docs\saveLocations.ts` into
`src/lib/save/saveLocations.ts` unchanged. It exports:

```ts
export interface LocalizedName { en: string; fr: string; ja: string }
export interface SaveLocation {
  era: LocalizedName   // the game's own era name (Dimensional Vortex counts as one); all three empty where the game shows none (End of Time, Black Omen, ???, Arena of the Ages)
  area: LocalizedName  // the game's own area name, the same text the game's Load screen shows
  room: string         // the room's name, English only; DS-only rooms have none, so it reads e.g. "Lost Sanctum room 0x214"
  location: number     // location id
  x: number            // tile x
  y: number            // tile y
}
export const SAVE_LOCATIONS: readonly SaveLocation[]  // 63 entries, sorted by era, then location id
```

The era and area names in English, French and Japanese come from the game's own text (the USA ROM for English
and French, the Japanese ROM for Japanese). There are no official names in Spanish, Portuguese or Chinese: for
`es`, `pt` and `zh`, show the English names. Never translate them yourself. `room` is English in every language.
Some entries share a `location` (two save points in one room) but differ in `x`/`y`. Identify an entry by
`location` + `x` + `y`, never by `location` alone.

## Save layout (slot-relative, like the existing `SLOT_OFFSETS`)

| Offset | Size | Field |
|---|---|---|
| 0x428 | u16 LE | Location id |
| 0x42A | u8 | X, in tiles |
| 0x42B | u8 | Y, in tiles |
| 0x42C | u8 | Facing. Leave it unchanged. |
| 0x434 | u16 LE | Location id, second copy. Always write the same value as 0x428. |

The slot checksum (sum32 of slot+4 .. slot+0x27FF at slot+0) already covers these bytes; `serializeSave` must
recompute it as it does now.

Tested: a real save with slot 3 set to Algetty (0x17C, 42, 42) loads with the party in Algetty, on the save point.
The DS-only rooms (Lost Sanctum, Dimensional Vortex, Arena of the Ages, ids 0x214-0x29B) are included and load the
same way. Some of them play their quest scene when entered (0x249, the Middle Ages village, does); that is the game,
not a bug to work around.

## What to build

1. `constants.ts`: add `LOCATION: 0x0428`, `LOCATION_X: 0x042a`, `LOCATION_Y: 0x042b` and `LOCATION_COPY: 0x0434`
   to `SLOT_OFFSETS`, with short doc comments as the other fields have.
2. `types.ts` / `parser.ts`: add `location`, `locationX` and `locationY` to `SlotData`. Parse them in the slot
   parser. In `serializeSave`, write `location` to both 0x428 and 0x434, and `locationX`/`locationY`, next to the
   other Progress fields. Do not touch the facing byte or any other byte. A slot that was not edited must
   serialize byte-identical to the input. Add parser/serializer tests in the existing test files for the round
   trip, for the copy at 0x434, and for an untouched slot staying byte-identical.
3. UI (`MiscView`, a new card in the existing style, with an icon from `@mui/icons-material`), "Save location":
   - Show the slot's current location: the matching entry's era, area and room, in the current language. A save
     made somewhere else (on the world map, or anywhere that is not a listed save point) matches no entry. Show it
     as "Other location (0x1F1)" with the id in hex, and leave it unchanged unless the user picks a new one.
   - A searchable picker (MUI Autocomplete) of all 63 entries, grouped by era. Each option shows the area name in
     the current language, with the room name and the location id in hex as secondary text. Selecting an entry
     patches `location`, `locationX` and `locationY` together.
   - One line of help text: the party will start on that save point the next time the game loads this slot.
4. i18n: add the card's strings (title, subtitle, current-location label, "Other location", the help text, and a
   "No era" label for entries whose era is empty) to `types.ts` and all six locale files. These are UI strings, so
   translate them; the game's names stay as they are in the data file.
5. Update MEMORY.md's save layout section with the new offsets, and the README if it lists editable fields.

## Do not

- Do not add, remove or rename entries in `saveLocations.ts`. 0x1D7 was removed on purpose: its sparkles are fake
  save points. 0x00D (Truce) is a real save point.
- Do not write the facing byte, and do not "fix" other bytes near 0x428–0x437.
