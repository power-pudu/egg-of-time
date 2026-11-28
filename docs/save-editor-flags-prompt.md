# Prompt for the Save Editor agent: Story and quest flags

Add a way to turn the game's story and quest flags on and off in a save slot: whether the party ate the old man's
lunch at the Millennial Fair, whether Fiona's forest quest is complete, whether Lucca's mother was saved, and about
370 more. Testers use it to set up a save before a quest, or to undo one, without replaying.

The game logic below was worked out and tested by the Egg of Time agent (the Chrono Trigger DS recompilation
project). It counts as the answer that MEMORY.md rule 9 asks for. Do not change offsets or values. If anything
here does not fit what you find in the code, ask the user before guessing.

## The data file

Copy `C:\Users\sirgu\OneDrive\Documentos\Claude\EggOfTime\docs\saveFlags.ts` into `src/lib/save/saveFlags.ts`
unchanged. It exports:

```ts
export interface SaveFlag {
  group: { en: string; fr: string } // the game's own name for the area where the flag is set
  label: string                     // what the flag means, English only
  offset: number                    // slot-relative byte offset, 0x228-0x427
  bit: number                       // the bit's mask in that byte: 0x01, 0x02, ... 0x80
  setBy: readonly number[]          // location ids of the rooms whose event scripts set it
  verified: boolean                 // checked by hand in the game's scripts and in real saves
}
export const SAVE_FLAGS: readonly SaveFlag[]  // 389 entries; the 10 verified ones first
```

`group` comes from the game's text: English and French only. For `ja`, `es`, `pt` and `zh`, show the English
group. `label` is English in every language; never translate it yourself.

The 10 `verified` entries are the ones the user asked for (the fair lunch, Lucca's mother, and the quest-complete
flag of each side quest). Their labels are exact. The other labels come from outside research on the SNES game,
matched to the DS rooms that set each flag; the room always fits, but the wording can be rough ("Arris Scene 10").

## Save layout

A slot keeps the game's flag bank ($7F0000-$7F01FF) unchanged at slot-relative offsets 0x228-0x427: the flag at
$7F00NN is at slot offset 0x228 + 0xNN. Each `SaveFlag` is one bit: on when `byte[offset] & bit` is non-zero.

- Toggling a flag changes that one bit and nothing else in the byte. Many bytes hold flags of different quests.
- Do not expose offset 0x228 itself: it is the storyline counter, a number, not flags. No entry in the file uses it.
- The location fields at 0x428+ (the existing Save location card) are just past this range; they do not overlap.
- The slot checksum (sum32 of slot+4 .. slot+0x27FF at slot+0) covers these bytes; `serializeSave` must recompute
  it as it does now.

Tested: Egg of Time reads these bits from a loaded save for its own quest achievements. A save with every side quest
done shows all seven; a slot saved at Death Peak shows only the two it has done (Fiona, Lucca's mother). Loading a save
whose flags were changed by hand has not been tested in the game yet.

## What to build

1. `constants.ts`: add `FLAG_BANK: 0x0228` and `FLAG_BANK_SIZE: 0x200` to `SLOT_OFFSETS`, with short doc comments
   as the other fields have.
2. `types.ts` / `parser.ts`: add `flagBank: Uint8Array` (0x200 bytes, a copy) to `SlotData`. Parse it in the slot
   parser; in `serializeSave`, write it back at 0x228. A slot that was not edited must serialize byte-identical to
   the input. Add helpers `getFlag(slot, flag)` and `setFlag(slot, flag, on)` that read and change one bit. Add
   tests: round trip, setting a bit leaves the other 7 bits of its byte unchanged, clearing works, an untouched slot
   stays byte-identical, and every `SAVE_FLAGS` entry has 0x229 <= offset <= 0x427 and exactly one bit set.
3. UI: a "Story flags" section in the existing style (its own view or tab if the editor has a place for one, like
   the other large lists; an icon from `@mui/icons-material`):
   - A search box that filters by label and group.
   - The flags grouped by `group` (collapsible), each a checkbox with its label. Verified flags show a small
     "checked" marker; offer a "Checked flags only" toggle that shows just those 10, on by default.
   - Each row's secondary text: the location ids from `setBy` in hex, and the offset/bit (e.g. `0x3C6 · 0x02`).
   - One line of help text: turning on a "quest complete" flag marks the quest done but does not give its rewards,
     and turning it off does not take them away; some flags change what a room shows, so the game may look odd if
     they disagree with the story.
4. i18n: add the section's strings (title, search placeholder, "Checked flags only", the "checked" marker, the help
   text) to `types.ts` and all six locale files. These are UI strings, so translate them.
5. Update MEMORY.md's save layout section with the flag bank, and the README if it lists editable fields.

## Do not

- Do not add, remove or rename entries in `saveFlags.ts`, and do not merge entries that share a byte.
- Do not write whole bytes from the UI (no "set all"/"clear all" per byte or group): flags in one byte can belong to
  unrelated quests.
- Do not edit the storyline counter at 0x228.
