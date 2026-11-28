# Story and side quest flags in a save slot

The game keeps its story and quest progress in the SNES game's flag bank, $7F0000-$7F01FF. A save slot stores
those 0x200 bytes unchanged at slot offset 0x228-0x427: the flag at $7F00NN is at slot offset 0x228 + 0xNN
(found by matching a running game's copy of the bank against the save it was loaded from). Slot offset 0x228 is
the storyline counter ($7F0000).

Each flag below was confirmed in the DS game's own event scripts (`PS_BIN/Atel`): the room listed is the one whose
script sets it, and other rooms read it. The values in the save files on hand agree. Recompute the slot checksum
after editing, as for any other field.

Event script commands used to find them: `65 bs aa` sets bit `1 << (bs & 7)` of $7F0000 + aa (+0x100 when bs has
0x80); `66 bs aa` clears it; `16 aa vv oo jj` tests $7F0000 + aa (+0x100 when oo has 0x80) against vv.

## Flags

| Quest | What the flag means | Address | Slot offset | Bit | Set by (location) |
|---|---|---|---|---|---|
| Millennial Fair | Ate the old man's lunch. The trial brings it up. | $7F0054 | 0x27C | 0x04 | Leene Square 0x1B7 |
| Millennial Fair | Returned the lost cat to the girl. The trial reads it. | $7F0054 | 0x27C | 0x01 | Leene Square 0x1B7 |
| Millennial Fair | Talked to the girl who lost her cat. The trial reads it. | $7F0054 | 0x27C | 0x02 | Leene Square 0x1B7 |
| Fiona's forest | Robo stays behind to help Fiona (the 400-year wait starts) | $7F019E | 0x3C6 | 0x01 | Fiona's Villa 0x09E |
| Fiona's forest | Robo met again at Fiona's Shrine in 1000 AD: quest complete | $7F019E | 0x3C6 | 0x02 | Fiona's Shrine 0x039 |
| Rainbow Shell | Rainbow Shell taken from the Giant's Claw (600 AD) | $7F01AF | 0x3D7 | 0x02 | 0x0C5 |
| Rainbow Shell | Rainbow Shell shown at Guardia Castle in 1000 AD: quest complete | $7F00A7 | 0x2CF | 0x40 | Guardia Throneroom 0x015 |
| Sun Stone | Charged Sun Stone collected in 2300 AD: quest complete | $7F013A | 0x362 | 0x40 | Sun Keep 2300 AD 0x0FF |
| Northern Ruins | Cyrus laid to rest at the Hero's Grave: quest complete | $7F01F4 | 0x41C | 0x40 | Hero's Grave 0x049 |
| Ozzie's Fort | Ozzie driven out of his fort: quest complete | $7F01F4 | 0x41C | 0x20 | Ozzie's Fort throne 0x0B5 |
| Geno Dome | Mother Brain defeated: quest complete | $7F013B | 0x363 | 0x10 | Geno Dome Mainframe 0x10C |
| Lucca's mother | Lara saved from the machine (she can walk) | $7F007C | 0x2A4 | 0x80 | Lucca's Workshop 0x004 |

Notes:

- Lucca's mother: when Lucca's flashback to the accident starts, the game sets bit 0x20 of $7F007C. Both endings then
  clear 0x20 and set 0x40, and only the rescue also sets 0x80, which Lara's room and later scenes read. Lucca's
  room clears 0x40 again once the scene there plays, so a save shows the result in 0x80 alone. Edit only 0x80.
- Ozzie and Cyrus share a byte ($7F01F4) of flags that the world maps also read. Change only the bit you mean.
- The fair has more flags the trial reads, in $7F0054 and $7F0055 (the pendant, the candy, waiting for Marle). The
  ones above are confirmed by where the fair sets them; the meaning of each $7F0055 bit is not yet confirmed.
- A completion flag records that a quest's last scene happened. Setting it does not give the quest's rewards
  (items, the Sun Stone's weapons), and clearing it does not take them away.

## All flags

Every flag that the DS event scripts set and that has a known meaning, 389 in all, grouped by the area of the room that sets it. The first 10 are the ones above, checked by hand. The others are named from outside research on the SNES game, matched to the DS rooms that set them: the room always fits the name, but the exact wording may be off. Left out: flags the game sets and clears within one scene (cutscene in progress, saving, entering a room), and bits no source names.

| Area | Flag | Slot offset | Bit | Set by (location) |
|---|---|---|---|---|
| World map | Ozzie driven out of his fort (quest complete) (checked) | 0x41C | 0x20 | 0x0B5 |
| World map | Cyrus laid to rest in the Northern Ruins (quest complete) (checked) | 0x41C | 0x40 | 0x049 |
| Fiona's Shrine | Fiona's forest: Robo met again at the shrine in 1000 AD (quest complete) (checked) | 0x3C6 | 0x02 | 0x039 |
| Fiona's Villa | Robo stays behind to help Fiona (checked) | 0x3C6 | 0x01 | 0x09E |
| Geno Dome | Mother Brain defeated in the Geno Dome (quest complete) (checked) | 0x363 | 0x10 | 0x10C |
| Giant's Claw | Rainbow Shell taken from the Giant's Claw (checked) | 0x3D7 | 0x02 | 0x0C5 |
| Guardia Castle | Rainbow Shell shown at Guardia Castle in 1000 AD (quest complete) (checked) | 0x2CF | 0x40 | 0x015 |
| Leene Square | Ate the old man's lunch at the fair (the trial brings it up) (checked) | 0x27C | 0x04 | 0x1B7 |
| Lucca's House | Lucca's mother saved from the machine (checked) | 0x2A4 | 0x80 | 0x004 |
| Sun Shrine | Charged Sun Stone collected in 2300 AD (quest complete) (checked) | 0x362 | 0x40 | 0x0FF |
| World map | Fiona Shrine | 0x418 | 0x02 | 0x09E |
| World map | Ferry to Porre | 0x418 | 0x04 | 0x012 |
| World map | Ferry to Truce | 0x418 | 0x08 | 0x038 |
| World map | Arris Scene 10 | 0x418 | 0x10 | 0x0DA |
| World map | Arris Scene 20 | 0x418 | 0x20 | 0x0DA |
| World map | Arris Scene 40 | 0x418 | 0x40 | 0x0DA |
| World map | Factory Drag Robo | 0x418 | 0x80 | 0x0E6 |
| World map | Magic Cave Beam | 0x419 | 0x01 | 0x0A3 |
| World map | Apocalypse Lavos Emerge | 0x419 | 0x02 | 0x1D2 |
| World map | Apocalypse Btfrtc | 0x419 | 0x04 | 0x1D3, 0x1DB, 0x1DF |
| World map | Omen Appears | 0x419 | 0x08 | 0x1DD |
| World map | Darkages Lavos Emerge | 0x419 | 0x40 | 0x14E |
| World map | Sunken Desert Complete | 0x419 | 0x80 | 0x09E |
| World map | Woe Chains Up | 0x41A | 0x01 | 0x183, 0x184 |
| World map | Woe Chains Down | 0x41A | 0x02 | 0x18A |
| World map | Apocalypse Lavos Arris Scene | 0x41A | 0x40 | 0x0DB |
| World map | Woe Fall | 0x41B | 0x02 | 0x183, 0x184 |
| World map | Ride Wind | 0x41B | 0x10 | 0x097 |
| World map | Zeal Fall Kajar | 0x41B | 0x40 | 0x165 |
| World map | Zeal Fall Lavos Emerge | 0x41B | 0x80 | 0x19F |
| World map | Btfrtc Truce Dome | 0x41C | 0x01 | 0x1DC |
| World map | Crash Epoch | 0x41C | 0x02 | 0x1D9 |
| World map | Zeal Fall Paradise Lost | 0x41C | 0x04 | 0x1EE |
| World map | Apocalypse Run Away | 0x41C | 0x08 | 0x1B0, 0x1D8 |
| World map | Apocalypse Something | 0x41C | 0x10 | 0x1B0, 0x1D8 |
| World map | Skyway Enhasa S | 0x41D | 0x04 | 0x151 |
| World map | Landbridge Enhasa S | 0x41D | 0x08 | 0x161 |
| World map | Skyway Enhasa N | 0x41D | 0x10 | 0x162 |
| World map | Landbridge Enhasa N | 0x41D | 0x20 | 0x150 |
| World map | Skyway Kajar | 0x41D | 0x40 | 0x164 |
| World map | Landbridge Kajar | 0x41D | 0x80 | 0x160 |
| World map | 600ad Year Known | 0x41E | 0x01 | 0x078 |
| World map | Darkages Fly to Last Village | 0x41E | 0x02 | 0x179 |
| World map | Darkages Omen Appears | 0x41E | 0x04 | 0x066, 0x067, 0x068, 0x069, 0x06A, 0x06B |
| World map | Omen Activate Lavos | 0x41E | 0x08 | 0x066, 0x067, 0x068, 0x069, 0x06A, 0x06B |
| World map | Omen Activate Lavos 2 | 0x41E | 0x10 | 0x066, 0x067, 0x068, 0x069, 0x06A, 0x06B |
| World map | Lavos Some Cutscene | 0x41E | 0x20 | 0x1D3 |
| World map | Zeal Show Elemental Weapons | 0x41E | 0x40 | 0x14F, 0x163 |
| World map | Giants Claw Open | 0x41E | 0x80 | 0x03F |
| World map | Zeal Show Sun Keep | 0x41F | 0x01 | 0x165 |
| World map | Future Show Crest | 0x41F | 0x20 | 0x133 |
| World map | End of Time Show Ozzie Fort | 0x41F | 0x40 | 0x1D0 |
| World map | End of Time Show Geno Dome | 0x41F | 0x80 | 0x1D0 |
| World map | End of Time Show Cyrus Grave | 0x420 | 0x01 | 0x1D0 |
| World map | Toma Show Giants Claw | 0x420 | 0x02 | 0x03F |
| World map | End of Time Show Sun Keep | 0x420 | 0x04 | 0x1D0 |
| ??? | Has Seen Lavos Emerge 1999 | 0x2D2 | 0x02 | 0x1B0, 0x1D2, 0x1D8 |
| ??? | Entered Lavos from Bucket | 0x2D2 | 0x10 | 0x1D2 |
| ??? | Hard Lavos Defeated | 0x305 | 0x02 | 0x1D3 |
| ??? | Has Seen Lavos Apocalypse Scene | 0x305 | 0x04 | 0x1D3 |
| Abandoned Sewers | Fought Sewers B1 Entrance Nereid Battle | 0x298 | 0x20 | 0x0EC |
| Abandoned Sewers | Viewed Sewers Intro Scene | 0x315 | 0x01 | 0x0EC |
| Abandoned Sewers | Viewed First Boss Underling Scene | 0x315 | 0x02 | 0x0EC |
| Abandoned Sewers | Viewed Sewers B2 Scene | 0x315 | 0x04 | 0x0EC |
| Abandoned Sewers | Viewed Second Boss Underling Scene | 0x315 | 0x08 | 0x0EC |
| Arris Dome Substructure | Obtained Arris Food Item | 0x2CC | 0x01 | 0x158 |
| Arris Dome Substructure | Arris Lavos Scene 02 | 0x2CC | 0x02 | 0x0DA |
| Arris Dome Substructure | Arris Lavos Scene 04 | 0x2CC | 0x04 | 0x0DA |
| Arris Dome Substructure | Arris Lavos Scene 08 | 0x2CC | 0x08 | 0x0DA |
| Arris Dome Substructure | Arris Lavos Scene 10 | 0x2CC | 0x10 | 0x0DA |
| Arris Dome Substructure | Arris auxiliary Console Left Switch | 0x2CC | 0x20 | 0x0D8 |
| Arris Dome Substructure | Arris auxiliary Console Right Switch | 0x2CC | 0x40 | 0x0D8 |
| Arris Dome Substructure | Arris Lower Commons Switch | 0x2CC | 0x80 | 0x0D9 |
| Arris Dome Substructure | Arris Dome Sealed Power tab | 0x2FD | 0x08 | 0x0DC |
| Arris Dome Substructure | Guardian Defeated | 0x314 | 0x01 | 0x0DB |
| Arris Dome Substructure | Arris Rat Catchable | 0x314 | 0x10 | 0x158 |
| Arris Dome Substructure | Has Seen Arris Rat Instructions | 0x314 | 0x20 | 0x0DD |
| Arris Dome Substructure | Has Caught Arris Rat | 0x314 | 0x40 | 0x0DD |
| Bangor Dome | Seen Bangor Scene | 0x2CD | 0x01 | 0x0D0 |
| Black Omen | Terra Mutant Defeated | 0x382 | 0x01 | 0x145 |
| Black Omen | Black Omen Final Panels Defeated | 0x382 | 0x04 | 0x1C2 |
| Black Omen | Black Omen 1F Panels Bottom | 0x3CD | 0x40 | 0x136 |
| Black Omen | Black Omen 1F Panels Top | 0x3CD | 0x80 | 0x136 |
| Black Omen | Black Omen Mega Mutant Battle | 0x3D0 | 0x01 | 0x134 |
| Black Omen | Black Omen 1F Incognito Battle | 0x3D0 | 0x20 | 0x134 |
| Black Omen | Black Omen Elder Spawn | 0x3D1 | 0x01 | 0x04C, 0x04D, 0x04E, 0x060 |
| Blackbird | Blackbird Found PC1 Gear | 0x2D7 | 0x01 | 0x1BC |
| Blackbird | Blackbird Found PC2 Gear | 0x2D7 | 0x02 | 0x1BB |
| Blackbird | Blackbird Found PC3 Gear | 0x2D7 | 0x04 | 0x175 |
| Blackbird | Blackbird Played Sick | 0x2D7 | 0x10 | 0x173 |
| Blackbird | Blackbird Seen Exterior | 0x2D7 | 0x20 | 0x16E |
| Blackbird | Blackbird Gear Taken | 0x2D7 | 0x40 | 0x173 |
| Blackbird | Blackbird Noticed Ducts | 0x2DB | 0x40 | 0x173 |
| Blackbird | Blackbird Items Taken | 0x2E2 | 0x01 | 0x173 |
| Blackbird | Blackbird Items Recovered | 0x2E2 | 0x02 | 0x176, 0x179 |
| Blackbird | Blackbird Money Recovered | 0x2E2 | 0x04 | 0x172 |
| Blackbird | Blackbird Knocked Out Guard Removed | 0x2E2 | 0x08 | 0x170, 0x171, 0x172, 0x174, 0x175, 0x176, 0x177, 0x1BA, 0x1BB, 0x1BC |
| Blackbird | Blackbird Duct Found | 0x2E2 | 0x40 | 0x178 |
| Blackbird | Has Seen Dalton on Blackbird Scaffolding | 0x2F9 | 0x40 | 0x16A, 0x197 |
| Blackbird | Can Fight on Blackbird | 0x2F9 | 0x80 | 0x173, 0x175, 0x1BB, 0x1BC |
| Blackbird | Returned to Blackbird Cell | 0x2FB | 0x01 | 0x170, 0x171, 0x172, 0x174, 0x175, 0x176, 0x177, 0x1BA, 0x1BB, 0x1BC |
| Blackbird | Blackbird Inventory Battle | 0x2FC | 0x08 | 0x176 |
| Blackbird | Blackbird Ducts Magic tab | 0x2FD | 0x10 | 0x178 |
| Choras | Choras 600 AD Talked to Carpenter | 0x3C6 | 0x10 | 0x0BC |
| Choras | Choras 1000 AD Tools Permission | 0x3C6 | 0x20 | 0x03E |
| Choras | Choras 600 AD Gave Carpenter Tools | 0x3C6 | 0x40 | 0x0BC |
| Choras | Choras 1000 AD Received Tools | 0x3C6 | 0x80 | 0x03D |
| Choras | Choras 600 AD Carpenter at Ruins | 0x3C7 | 0x01 | 0x0BA |
| Choras | Northern Ruins Landing Repaired | 0x3C7 | 0x02 | 0x0BA |
| Choras | Northern Ruins Basement Corridor Repaired | 0x3C7 | 0x04 | 0x0BA |
| Choras | Northern Ruins Antechamber Repaired | 0x3C7 | 0x08 | 0x0BA |
| Choras | Obtained Toma Item | 0x3C8 | 0x02 | 0x0BC |
| Choras | Talked to Choras 1000 AD Carpenter | 0x3CA | 0x08 | 0x03E |
| Choras | Saw Choras 1000 AD Carpenter Drink | 0x3CA | 0x80 | 0x03E |
| Crono's House | Mom Gave Money | 0x368 | 0x02 | 0x001 |
| Crono's House | Mom Met Marle | 0x368 | 0x04 | 0x001 |
| Crono's House | Mom Met Lucca | 0x368 | 0x08 | 0x001 |
| Crono's House | Mom Met Robo | 0x368 | 0x10 | 0x001 |
| Crono's House | Mom Met Frog | 0x368 | 0x20 | 0x001 |
| Crono's House | Mom Met Ayla | 0x368 | 0x40 | 0x001 |
| Crono's House | Mom Met Magus | 0x368 | 0x80 | 0x001 |
| Cursed Woods | Keep Song on Switch | 0x3F7 | 0x40 | 0x08D, 0x1D0, 0x1DA |
| Dactyl Nest | Obtained Dactyls | 0x388 | 0x10 | 0x127 |
| Death Peak | Crono Revived | 0x27F | 0x40 | 0x1AD |
| Death Peak | Death Peak Cave Lavos Spawn | 0x28C | 0x08 | 0x108 |
| Death Peak | Death Peak Power tab | 0x2A4 | 0x08 | 0x0F4 |
| Death Peak | Epoch in End of Time | 0x2D3 | 0x80 | 0x1AD, 0x1D0, 0x1D3, 0x1DA |
| Denadoro Mountains | Denadoro Mts. Speed tab | 0x28C | 0x40 | 0x095 |
| Denadoro Mountains | Mountains Re Nice Magic tab | 0x298 | 0x01 | 0x094 |
| Denadoro Mountains | Masamune Defeated | 0x31B | 0x20 | 0x097 |
| Denadoro Mountains | Obtained Gold Rock | 0x31F | 0x08 | 0x08F |
| Derelict Factory | Factory Defense Lasers Off | 0x280 | 0x01 | 0x0E6, 0x1EF |
| Derelict Factory | Robo Used Factory Ruins Entrance Computer | 0x280 | 0x04 | 0x0E4 |
| Derelict Factory | Factory Xaby Entered | 0x3F8 | 0x01 | 0x0EB |
| Derelict Factory | Factory Walls Closed | 0x3F8 | 0x02 | 0x0EB |
| Dorino | Dorino Bromide Magic tab | 0x31E | 0x02 | 0x088 |
| Enhasa | Has Viewed Enhasa Janus Scene | 0x31C | 0x04 | 0x14F, 0x163 |
| Enhasa | Enhasa Nu Battle | 0x31C | 0x08 | 0x169 |
| Fiendlord's Keep | Magus Castle Flea Defeated | 0x2CB | 0x08 | 0x0AD |
| Fiendlord's Keep | Magus Castle Visited Slash Room | 0x2CB | 0x10 | 0x0A9 |
| Fiendlord's Keep | Magus Castle Visited Flea Room | 0x2CB | 0x20 | 0x0AD |
| Fiendlord's Keep | Magus Castle Ghost Kids Battle | 0x2D4 | 0x01 | 0x0AB |
| Fiendlord's Keep | Magus Castle Ozzie Left Hall Aggression | 0x2D4 | 0x02 | 0x0AA |
| Fiendlord's Keep | Magus Castle Ozzie Left Guillotines | 0x2D4 | 0x04 | 0x0A7 |
| Fiendlord's Keep | Magus Castle Ozzie Left Pits | 0x2D4 | 0x08 | 0x0A8 |
| Fiendlord's Keep | Magus Castle Ozzie Left Hall Apprehension | 0x2D4 | 0x10 | 0x0AF |
| Fiendlord's Keep | Magus Castle Ozzie Welcome Guillotines | 0x2D4 | 0x20 | 0x0A7 |
| Fiendlord's Keep | Magus Castle Ozzie Faster Guillotines | 0x2D4 | 0x40 | 0x0A7 |
| Fiendlord's Keep | Magus Castle Slash Grabbed Sword | 0x2D6 | 0x04 | 0x0A9 |
| Fiendlord's Keep | Magus Castle Slash Sword Treasure | 0x2D6 | 0x08 | 0x0A9 |
| Fiendlord's Keep | Magus Castle Ghost Kids Chest | 0x2D6 | 0x80 | 0x0AB |
| Fiendlord's Keep | Magus Castle Flea Magic tab | 0x2FD | 0x01 | 0x0AD |
| Fiendlord's Keep | Magus Castle Dungeons Magic tab | 0x2FD | 0x02 | 0x1D7 |
| Fiona's Shrine | Lucca Changing Past | 0x282 | 0x02 | 0x1B4 |
| Fiona's Villa | Robo Missing from Helping | 0x3C6 | 0x08 | 0x09E |
| Forest Ruins | Pyramid Unlocked | 0x3C4 | 0x04 | 0x02A |
| Forest Ruins | Pyramid Left Chest | 0x3C8 | 0x01 | 0x02A |
| Geno Dome | Geno Dome Atropos Defeated | 0x363 | 0x20 | 0x10C |
| Geno Dome | Geno Dome Waste Scene Viewed | 0x363 | 0x40 | 0x10D |
| Geno Dome | Geno Dome Entrance Scene Viewed | 0x364 | 0x01 | 0x0F8 |
| Geno Dome | Geno Dome PC Has Charge | 0x364 | 0x40 | 0x100 |
| Geno Dome | Geno Dome Labs Magic tab | 0x372 | 0x04 | 0x100 |
| Geno Dome | Geno Dome Corridor Power tab | 0x372 | 0x08 | 0x080 |
| Geno Dome | Geno Dome Labs Speed tab | 0x373 | 0x01 | 0x100 |
| Geno Dome | Geno Dome Atropos Magic tab | 0x373 | 0x02 | 0x10C |
| Geno Dome | Geno Dome Conveyor Entrance Mother Brain Text | 0x376 | 0x01 | 0x0F9 |
| Geno Dome | Geno Dome Conveyor Exit Mother Brain Text | 0x376 | 0x02 | 0x0FA |
| Geno Dome | Geno Dome Labs Mother Brain Text | 0x376 | 0x04 | 0x100 |
| Geno Dome | Geno Dome Corridor Mother Brain Text | 0x376 | 0x08 | 0x080 |
| Geno Dome | Geno Dome Mainframe Mother Brain Text | 0x376 | 0x10 | 0x10C |
| Giant's Claw | Giants Claw Viewed Throne Scene | 0x3FA | 0x08 | 0x129 |
| Giant's Claw | Giants Claw Boss Defeated | 0x3FA | 0x40 | 0x0C5 |
| Giant's Claw | Giants Claw Entrance Power tab | 0x3FB | 0x02 | 0x128 |
| Giant's Claw | Giants Claw Traps Power tab | 0x3FB | 0x40 | 0x06E |
| Giant's Claw | Giants Claw Caverns Power tab | 0x3FB | 0x80 | 0x0C4 |
| Guardia Castle | Guardia Basement Gnashers Battle | 0x278 | 0x08 | 0x01A |
| Guardia Castle | Kings Trial Complete | 0x278 | 0x40 | 0x1B6 |
| Guardia Castle | Obtained Yakra Key | 0x278 | 0x80 | 0x1B6 |
| Guardia Castle | Fair Pendant Not Found | 0x27C | 0x10 | 0x01B, 0x1B7 |
| Guardia Castle | Guardia Throne 1000 AD Blocked | 0x292 | 0x80 | 0x1B6 |
| Guardia Castle | Melchior in Treasury | 0x295 | 0x10 | 0x1B6 |
| Guardia Castle | Queen 600 AD Guard Moved | 0x2C8 | 0x20 | 0x07A |
| Guardia Castle | Marle Dismiss Attendants | 0x2C9 | 0x01 | 0x07A |
| Guardia Castle | Marle Disappeared | 0x2C9 | 0x02 | 0x07A |
| Guardia Castle | Guardia Treasury Exists | 0x2C9 | 0x40 | 0x078 |
| Guardia Castle | Barracks Escape | 0x2CA | 0x01 | 0x019 |
| Guardia Castle | Has Escaped Guardia Prison | 0x2CA | 0x02 | 0x018 |
| Guardia Castle | Lucca Rescued Crono | 0x2CA | 0x04 | 0x015 |
| Guardia Castle | Obtained Rainbow Shell Item | 0x2CA | 0x80 | 0x1B8 |
| Guardia Castle | Rescue Chancellor 1000 AD | 0x2CF | 0x80 | 0x1E8 |
| Guardia Castle | Chef Pipe Down | 0x2D1 | 0x08 | 0x07B |
| Guardia Castle | Chef Gives Jerky | 0x2D1 | 0x10 | 0x078 |
| Guardia Castle | Guardia Castle Sealed Chest Charged | 0x301 | 0x01 | 0x1E0 |
| Guardia Castle | Guardia Castle Sealed 600 AD Obtained | 0x301 | 0x02 | 0x1E0 |
| Guardia Castle | Guardia Castle Sealed 1000 AD Obtained | 0x301 | 0x04 | 0x1E6 |
| Guardia Forest | Has Bangor Portal | 0x2CE | 0x02 | 0x014, 0x0D0 |
| Guardia Forest | Guardia Forest Dead End Sealed Chest | 0x3F9 | 0x20 | 0x014 |
| Guardia Forest | Guardia Forest Sealed 600 AD Obtained | 0x3FA | 0x80 | 0x077 |
| Guardia Forest | Obtained Guardia Forest 1000 AD tab | 0x3FB | 0x04 | 0x013 |
| Guardia Forest | Obtained Guardia Forest 600 AD tab | 0x3FB | 0x08 | 0x077 |
| Heckran Cave | Heckran Sealed Obtained | 0x3C8 | 0x04 | 0x02F |
| Heckran Cave | Heckran Cave Entrance Initial Battle | 0x3CB | 0x02 | 0x02F, 0x030 |
| Heckran Cave | Heckran Cave Entrance Jinn Bottle Battle | 0x3D6 | 0x01 | 0x030 |
| Hunting Range | Hunting Range Nu Reward | 0x3F9 | 0x08 | 0x123 |
| Ioka | Obtained Sweetwater Hut Tonics | 0x388 | 0x40 | 0x116 |
| Kajar | Obtained Black Rock | 0x31C | 0x20 | 0x167 |
| Kajar | Kajar Magic Lab Speed tab | 0x31E | 0x04 | 0x168 |
| Kajar | Nu Scratch Magic tab | 0x31E | 0x08 | 0x168 |
| Keeper's Dome | Obtained Keepers Dome Magic tab | 0x298 | 0x02 | 0x0F2 |
| Keeper's Dome | Keepers Nu Complete | 0x298 | 0x04 | 0x0F2 |
| Keeper's Dome | Keepers Nu Sent Poyozos | 0x2A4 | 0x02 | 0x0F2 |
| Keeper's Dome | Epoch Out of Hangar | 0x314 | 0x04 | 0x0F3 |
| Keeper's Dome | Viewed Keepers Plot Sparkles | 0x314 | 0x08 | 0x0F2 |
| Keeper's Dome | Nu Entered Keepers Hangar | 0x316 | 0x02 | 0x0F3 |
| Keeper's Dome | Viewed Name Epoch Scene | 0x316 | 0x80 | 0x0F3 |
| Keeper's Dome | Viewed Epoch First Flight Scene | 0x317 | 0x10 | 0x0F3 |
| Laruba Ruins | Has Seen Laruba Take Kino Scene | 0x3C4 | 0x08 | 0x124 |
| Laruba Ruins | Received Silver Rock | 0x3D4 | 0x20 | 0x124 |
| Leene Square | Fair Has Returned Cat | 0x27C | 0x01 | 0x1B7 |
| Leene Square | Fair Talked to Cat Girl | 0x27C | 0x02 | 0x1B7 |
| Leene Square | Fair Lucca Asked Crono Telepod | 0x27C | 0x08 | 0x008 |
| Leene Square | Fair Pendant Picked Up | 0x27C | 0x20 | 0x1B7 |
| Leene Square | Fair Not Given Pendant | 0x27C | 0x40 | 0x1B7 |
| Leene Square | Marle Pendant Sold | 0x27D | 0x01 | 0x005 |
| Leene Square | Pendant Before Marle | 0x27D | 0x02 | 0x1B7 |
| Leene Square | Marle Talk Pendant | 0x27D | 0x04 | 0x1B7 |
| Leene Square | Marle Wants Candy | 0x27D | 0x08 | 0x1B7 |
| Leene Square | Marle Fortune | 0x27D | 0x10 | 0x005, 0x1B7 |
| Leene Square | Marle Yelling Kidnapped | 0x27D | 0x20 | 0x1B7 |
| Leene Square | Marle Given Pendant | 0x27D | 0x40 | 0x1B7 |
| Leene Square | Shown Marle Fair | 0x27D | 0x80 | 0x005 |
| Leene Square | Won Crono Clone | 0x286 | 0x01 | 0x1B2 |
| Leene Square | Won Marle Clone | 0x286 | 0x02 | 0x1B2 |
| Leene Square | Won Lucca Clone | 0x286 | 0x04 | 0x1B2 |
| Leene Square | Won Robo Clone | 0x286 | 0x08 | 0x1B2 |
| Leene Square | Won Frog Clone | 0x286 | 0x10 | 0x1B2 |
| Leene Square | Won Ayla Clone | 0x286 | 0x20 | 0x1B2 |
| Leene Square | Won Magus Clone | 0x286 | 0x40 | 0x1B2 |
| Leene Square | Has Truce Portal | 0x2CE | 0x01 | 0x008, 0x071, 0x1A8 |
| Lucca's House | Lucca Tried Changing Past | 0x282 | 0x20 | 0x00A |
| Lucca's House | Lara Machine Started | 0x282 | 0x40 | 0x004 |
| Lucca's House | Taban Vest Given | 0x2A2 | 0x01 | 0x004 |
| Lucca's House | Taban Helm Given | 0x2A2 | 0x02 | 0x004 |
| Lucca's House | Taban Suit Given | 0x2A2 | 0x04 | 0x004 |
| Lucca's House | Lara Accident Started | 0x2A4 | 0x20 | 0x004 |
| Lucca's House | Lara Loses Legs | 0x2A4 | 0x40 | 0x004 |
| Lucca's House | Wondershot Sunshades Received | 0x362 | 0x80 | 0x004 |
| Magic Cave | Zenan Cyrus Scene | 0x27F | 0x02 | 0x0A3 |
| Magic Cave | Magic Cave First Battle | 0x292 | 0x20 | 0x0A4 |
| Magic Cave | Magic Cave Sealed Chest | 0x2A1 | 0x01 | 0x0A4 |
| Manolia Cathedral | Manoria Confinement Power tab | 0x31E | 0x10 | 0x0C7 |
| Manolia Cathedral | Obtained Nagaette Bromide | 0x326 | 0x02 | 0x0C9 |
| Manolia Cathedral | Manoria Sanctuary Organ | 0x327 | 0x10 | 0x081 |
| Medina | Medina Portal First Use | 0x3C1 | 0x80 | 0x024 |
| Medina | Initial Medina Imp Dialog | 0x3CA | 0x01 | 0x024 |
| Medina | First Visit Medina Square | 0x3CA | 0x20 | 0x01F |
| Medina | Medina Elder Magic tab | 0x3CC | 0x02 | 0x022 |
| Medina | Medina Elder Speed tab | 0x3CC | 0x04 | 0x021 |
| Melchior's Cabin | Melchior Workshop Forge Scene | 0x3C3 | 0x10 | 0x028 |
| Mountain of Woe | Mt. Woe Magic tab | 0x3CC | 0x20 | 0x18B |
| Mudbeast Den | Beast Nest Power tab | 0x3CC | 0x10 | 0x183, 0x184, 0x280 |
| North Cape | North Cape Recruit Obtained | 0x360 | 0x01 | 0x1AC |
| North Cape | North Cape Magus Defeated | 0x360 | 0x02 | 0x1AC |
| North Cape | North Cape Magus Item Obtained | 0x360 | 0x04 | 0x1AC |
| Northern Ruins | Northern Ruins Basement Sentries Cleared | 0x3C7 | 0x20 | 0x042 |
| Northern Ruins | Masamune Upgraded | 0x3CB | 0x40 | 0x049 |
| Northern Ruins | Cyrus Grave Magic tab | 0x3CC | 0x80 | 0x049 |
| Northern Ruins | Northern Ruins Antechamber Sealed 600 AD Obtained | 0x3CE | 0x01 | 0x044 |
| Northern Ruins | Northern Ruins Back Left Sealed 600 AD Obtained | 0x3CE | 0x02 | 0x046 |
| Northern Ruins | Northern Ruins Back Right Sealed 600 AD Obtained | 0x3CE | 0x04 | 0x046 |
| Northern Ruins | Northern Ruins Antechamber Sealed Chest Charged | 0x3CE | 0x08 | 0x044 |
| Northern Ruins | Northern Ruins Back Left Sealed Chest Charged | 0x3CE | 0x10 | 0x046 |
| Northern Ruins | Northern Ruins Back Right Sealed Chest Charged | 0x3CE | 0x20 | 0x046 |
| Northern Ruins | Northern Ruins Landing Power tab | 0x3D1 | 0x10 | 0x043 |
| Northern Ruins | Northern Ruins Antechamber Sealed 1000 AD Obtained | 0x3D1 | 0x20 | 0x044 |
| Northern Ruins | Northern Ruins Back Left Sealed 1000 AD Obtained | 0x3D1 | 0x40 | 0x046 |
| Northern Ruins | Northern Ruins Back Right Sealed 1000 AD Obtained | 0x3D1 | 0x80 | 0x046 |
| Northern Ruins | Northern Ruins Basement Chest 1000 AD Obtained | 0x3D4 | 0x01 | 0x042 |
| Northern Ruins | Northern Ruins Basement Chest 600 AD Obtained | 0x3D4 | 0x02 | 0x042 |
| Northern Ruins | Northern Ruins Antechamber Chest 1000 AD Obtained | 0x3D4 | 0x04 | 0x044 |
| Northern Ruins | Northern Ruins Antechamber Chest 600 AD Obtained | 0x3D4 | 0x08 | 0x044 |
| Northern Ruins | Northern Ruins Repairs Complete | 0x3D4 | 0x80 | 0x041 |
| Ocean Palace | Ocean Palace Disaster Scene | 0x292 | 0x01 | 0x19F |
| Ocean Palace | Starting Lucca Forest Scene | 0x2A4 | 0x10 | 0x194, 0x1B4 |
| Ocean Palace | Ocean Palace Elevator Magic tab | 0x31E | 0x01 | 0x1A1 |
| Ocean Palace | Ocean Palace Platform Down | 0x382 | 0x08 | 0x19C |
| Ocean Palace | Black Omen Mammon M Battle | 0x3D0 | 0x80 | 0x1A6 |
| Other | Lucca Explained Paradoxes | 0x2D1 | 0x01 | 0x06C |
| Other | Obtained Epoch Flight | 0x2E2 | 0x80 | 0x179 |
| Ozzie's Fort | Medina Redeemed | 0x3C9 | 0x80 | 0x0B5 |
| Ozzie's Fort | Ozzies Fort Guillotines tab | 0x3CC | 0x40 | 0x0B3 |
| Porre | Recovered Moonstone | 0x362 | 0x10 | 0x032 |
| Porre | Obtained Snail Stop Item | 0x3F8 | 0x10 | 0x035 |
| Porre | Porre Mayor Sealed 1 Obtained | 0x3F9 | 0x40 | 0x033, 0x09A |
| Porre | Porre Mayor Sealed 2 Obtained | 0x3F9 | 0x80 | 0x033, 0x09A |
| Porre | Porre Mayor Sealed 1 Charged | 0x3FA | 0x01 | 0x09A |
| Porre | Porre Mayor Sealed 2 Charged | 0x3FA | 0x02 | 0x09A |
| Porre | Gave Away Jerky Porre | 0x3FA | 0x04 | 0x09A |
| Porre | Porre Market 600 AD tab | 0x3FB | 0x01 | 0x09D |
| Porre | Porre Elder Sealed 1 Obtained | 0x3FB | 0x10 | 0x09A |
| Porre | Porre Elder Sealed 2 Obtained | 0x3FB | 0x20 | 0x09A |
| Prison Towers | Crono Has Been Imprisoned | 0x3B8 | 0x01 | 0x01D |
| Prison Towers | Crono Wakes in Cell | 0x3B8 | 0x02 | 0x047 |
| Prison Towers | Crono Free from Cell | 0x3B8 | 0x04 | 0x01E, 0x047 |
| Prison Towers | Prison Blue Shield Battle W | 0x3B8 | 0x10 | 0x048 |
| Prison Towers | Prison Blue Shield Battle E | 0x3B8 | 0x20 | 0x048 |
| Prison Towers | Prison Omnicrone Battle | 0x3B8 | 0x40 | 0x047 |
| Prison Towers | Prison Stairs Guard SW Out | 0x3B9 | 0x01 | 0x048 |
| Prison Towers | Prison Stairs Guard SE Out | 0x3B9 | 0x02 | 0x01E, 0x048 |
| Prison Towers | Prison Stairs Guard NW Out | 0x3B9 | 0x04 | 0x01E, 0x048 |
| Prison Towers | Used Prison Hole Going Up | 0x3B9 | 0x10 | 0x047 |
| Prison Towers | Used Prison Hole Going Down | 0x3B9 | 0x20 | 0x047 |
| Prison Towers | Prison Torture Room Switch Hit | 0x3B9 | 0x80 | 0x047, 0x048 |
| Prison Towers | Prison Torture Decedent Battle | 0x3C0 | 0x01 | 0x01E, 0x047 |
| Prison Towers | Crono Taken Execution | 0x3C0 | 0x10 | 0x047 |
| Prison Towers | Prison Supervisor Guard Battle | 0x3C0 | 0x20 | 0x01D |
| Prison Towers | Prison Stairs Lucca Guards Out | 0x3C0 | 0x40 | 0x01E |
| Prison Towers | Prison Decedent Battle NW | 0x3C0 | 0x80 | 0x047 |
| Prison Towers | Prison Catwalks Guards Battle | 0x3C1 | 0x02 | 0x01C |
| Prison Towers | Has Defeated Dragon Tank | 0x3C1 | 0x04 | 0x01C |
| Prison Towers | Prison Decedent Battle SE | 0x3C1 | 0x10 | 0x047 |
| Prison Towers | Prison Jump Back from Guard | 0x3C1 | 0x20 | 0x01E, 0x048 |
| Prison Towers | Prison Storage Guard Battle | 0x3C1 | 0x40 | 0x047 |
| Prison Towers | Rescued Fritz | 0x3C3 | 0x40 | 0x048 |
| Prison Towers | Received Prison Cell Gift | 0x3C3 | 0x80 | 0x047 |
| Proto Dome | Proto Dome Portal Power tab | 0x372 | 0x01 | 0x0E3 |
| Proto Dome | Proto Dome Entrance Battle | 0x378 | 0x04 | 0x0E2 |
| Proto Dome | Proto Dome Enertron Battle | 0x378 | 0x08 | 0x0E2 |
| Site 32 | Has Met Rx Xr | 0x35E | 0x01 | 0x0DF, 0x0E1 |
| Site 32 | Obtained Johnny Race Power tab | 0x35E | 0x02 | 0x0DF, 0x0E1 |
| Site 32 | Has Met Johnny | 0x37C | 0x01 | 0x0DF |
| Site 32 | Johnny Race Just Finished | 0x37C | 0x02 | 0x0DF, 0x0E1 |
| Site 32 | Johnny Race from West | 0x37C | 0x04 | 0x0DF |
| Site 32 | Has Attempted Johnny Race | 0x37C | 0x08 | 0x0DF |
| Small Cave | Has Viewed Darkages Portal First Scene | 0x27F | 0x10 | 0x15F |
| Sun Shrine | Lucca Making Wondershot | 0x282 | 0x08 | 0x0FF |
| Sun Shrine | Found Moonstone Missing 1000 AD | 0x361 | 0x01 | 0x040 |
| Sun Shrine | Found Moonstone Missing 2300 AD | 0x361 | 0x02 | 0x0FF |
| Sun Shrine | Moonstone Placed Prehistory | 0x362 | 0x04 | 0x147 |
| Sun Shrine | Discovered Moonstone Missing | 0x362 | 0x08 | 0x040, 0x0FF |
| Sun Shrine | Moonstone Returned 1000 AD | 0x362 | 0x20 | 0x040 |
| Sun Shrine | Sun Keep 600 AD Power tab | 0x372 | 0x02 | 0x0BF |
| Sun Temple | Sun Palace Boss Defeated | 0x362 | 0x01 | 0x0FB |
| Sun Temple | Sun Palace Item Obtained | 0x362 | 0x02 | 0x0FB |
| Sunken Desert | Sunken Desert Boss Defeated | 0x3CB | 0x01 | 0x0A1 |
| Sunken Desert | Sunken Desert Power tab | 0x3CC | 0x01 | 0x0A0 |
| Sunken Desert | Sunken Desert Middle Battle | 0x3D5 | 0x01 | 0x0A0 |
| Surviving Village | Last Village Nu Shop Magic tab | 0x372 | 0x10 | 0x1AA |
| The End of Time | Has Gaspar Item | 0x2A4 | 0x01 | 0x1D0 |
| The End of Time | Robo Joins End of Time | 0x2D0 | 0x40 | 0x1D0 |
| The End of Time | Has Seen Gaspar Bucket Warning | 0x2D3 | 0x10 | 0x1D0 |
| The End of Time | Returned to End of Time After Death Peak | 0x2D3 | 0x20 | 0x1D0 |
| The End of Time | Asked Gaspar About Guru of Time | 0x300 | 0x01 | 0x1D0 |
| The End of Time | Received C Trigger from Gaspar | 0x300 | 0x02 | 0x1D0 |
| The End of Time | Gaspar Called Party Back | 0x300 | 0x04 | 0x1D0 |
| The End of Time | Gaspar Told Sidequests | 0x300 | 0x08 | 0x1D0 |
| The End of Time | Gaspar Wants to Talk Magic | 0x300 | 0x10 | 0x1D0 |
| The End of Time | Met Pink Nu Spekkio | 0x300 | 0x20 | 0x1D1 |
| Trann Dome | Trann Dome Sealed Magic tab | 0x2FD | 0x04 | 0x0D3 |
| Truce | Turned Keepers Nu Off | 0x298 | 0x08 | 0x074, 0x0F2 |
| Truce | Truce Inn Sealed 1000 AD Obtained | 0x372 | 0x20 | 0x00C, 0x075 |
| Truce | Truce Inn Sealed Chest Charged | 0x372 | 0x40 | 0x075 |
| Truce | Truce Inn Sealed 600 AD Obtained | 0x372 | 0x80 | 0x075 |
| Truce Canyon | First Truce Portal Trip | 0x36E | 0x01 | 0x071 |
| Tyranno Lair | Tyrano Lair Entrance Skull Open | 0x284 | 0x10 | 0x12B |
| Tyranno Lair | Kino at Tyrano Lair Entrance | 0x28B | 0x01 | 0x12E |
| Tyranno Lair | Tyrano Main Cell Right Battle | 0x28B | 0x02 | 0x1AE |
| Tyranno Lair | Tyrano Main Cell Left Battle | 0x28B | 0x04 | 0x1AE |
| Tyranno Lair | Tyrano Prisoners Freed | 0x28B | 0x08 | 0x1AE |
| Tyranno Lair | Tyrano Antechambers Right Trapdoor Open | 0x28B | 0x10 | 0x12E |
| Tyranno Lair | Tyrano Antechambers Left Trapdoor Open | 0x28B | 0x20 | 0x12E |
| Tyranno Lair | Melchior Treasury Free Item Given | 0x295 | 0x20 | 0x12E, 0x1B8 |
| Tyranno Lair | Black Tyrano Defeated | 0x314 | 0x80 | 0x12D |
| Tyranno Lair | Viewed Tyrano Throne Azala Monologue | 0x316 | 0x08 | 0x12C |
| Tyranno Lair Ruins | Has Viewed Lair Ruins First Scene | 0x27F | 0x08 | 0x133 |
| Tyranno Lair Ruins | Skyways Locked | 0x27F | 0x80 | 0x133 |
| Tyranno Lair Ruins | Has Dark Ages Portal | 0x2CE | 0x04 | 0x133, 0x18C, 0x18D |
| West Cape | Has Poured Tomas Pop | 0x3CB | 0x80 | 0x03F |
| West Cape | West Cape Speed tab | 0x3D4 | 0x10 | 0x03F |
| West Cape | Toma Showing Giants Claw | 0x3D4 | 0x40 | 0x03F |
| Zeal Palace | Has Viewed Schalas Room Scene | 0x31C | 0x10 | 0x0CF, 0x148 |
| Zeal Palace | Failed to Open Zeal Throne Door | 0x31C | 0x40 | 0x149 |
| Zeal Palace | Has Opened Zeal Throne Door | 0x31C | 0x80 | 0x149 |
| Zeal Palace | Discovered Nu Scratch Point | 0x31D | 0x08 | 0x191 |
| Zeal Palace | Plant Lady Saves Seed | 0x31F | 0x02 | 0x191, 0x1A8 |
| Zenan Bridge | Zenan Requested Food | 0x2D1 | 0x04 | 0x086, 0x087 |
| Zenan Bridge | Zenan Captain Item | 0x3C2 | 0x01 | 0x086, 0x087 |
| Zenan Bridge | Zenan Battle 1 | 0x3C2 | 0x02 | 0x086, 0x087 |
| Zenan Bridge | Zenan Battle 2 | 0x3C2 | 0x04 | 0x086, 0x087 |
