Egg of Time - graphics mods
===========================

Replace the game's sprites and backgrounds with your own art, at the original size or
in HD (2 to 8 times larger).

1. Dump the game's graphics
---------------------------
System Settings > Dump graphics saves them in dump\ in this folder, as PNG files. It asks
first; the dump reads your own ROM and takes a while (a progress bar shows the time left).
The game keeps running meanwhile, and the menu can be closed: a message says when the dump
is done.

    dump\sheets\     every tile sheet: sprites (.NCGR) and backgrounds (_ncg.bin), 16 tiles
                     (128 pixels) wide, in the palette's first 16 colours
    dump\frames\     every frame of every sprite, one folder per sprite (Chara\Chara_0000\000.png,
                     001.png, ...), put together from its sheet as the game draws it
    dump\screens\    the backgrounds stored as whole pictures (menus, title, minimaps...)
    dump\palettes.txt  the palette each image was drawn with

The PNGs are indexed (palette) images: each pixel keeps the game's own colour number.
Colour 0 of each 16-colour row is transparent, as on the DS. Running the dump again
overwrites the files it wrote; it shows the same-size mods below as they are applied.

Palettes: an image is drawn with the palette file of the same name, or the only palette
in its folder; a field character Chara\Chara_xxxx with Chara\ObjPlt_xxxx. Where there is
none, it is drawn in a grey ramp (the field maps' tile sheets, Map\Bg\, and the characters
without an ObjPlt). Their colour numbers are still exact. palettes.txt lists "-" for these.

2. Make a mod
-------------
Copy the images you change out of dump\ to the same place in this folder, without the
dump\ part, and edit them there:

    dump\frames\Chara\Chara_0000\005.png  ->  frames\Chara\Chara_0000\005.png

Only the images you put here are replaced; everything else stays the game's. Mods are read
when the game starts (restart it after changing them) while System Settings > Mods is On.
The log (eggoftime.log) lists what was loaded, and any image it could not use.

Edit frames, sheets or screens, whichever is easiest: they all go back into the same 8x8
tiles. A frame or screen that shares tiles with another image changes them there too;
where two of your images set the same tile, the one later in path order wins. A pixel
belongs to the tile on top in that spot; pixels of a tile hidden under another stay as
they were.

Same size
---------
An image at its original size changes the game's own tiles, so it shows everywhere,
menus included.
- Indexed PNG (recommended): keep the colours in the tile's own palette row. A pixel in
  another row's colour is changed to the nearest colour of its row.
- RGBA PNG: each colour is changed to the nearest of the tile's palette row (meaningless
  for the grey-ramp images: use indexed PNGs for those).

HD (2x to 8x)
-------------
An image 2 to 8 times larger (both sides by the same whole number: a 19x64 frame becomes
76x256 at 4x) replaces each of its tiles with your art wherever the game shows that tile.
It is drawn at the HD size, or the window's internal resolution when that is smaller.
- Indexed PNG: colours come from the game's palette as it is at that moment, so palette
  animations, fades and enemies in other colours keep working. The colour numbers are the
  dump's (the tile's palette row).
- RGBA PNG: your own colours, as painted. The game's blending and brightness (fades,
  translucent layers) still apply; palette animations of those tiles do not.
Where your HD art is transparent over a pixel the game draws, what lies under it shows.
Tiles that are fully transparent in the game cannot be told apart, so art can only
replace tiles that have some pixels.

HD is drawn in the field, battles, the world map and the screens shown as they are; the
one-screen menus (Items, Equip, Shop...) show the art at the original size for now.
HD is not drawn while a shader is chosen (System Settings > Shader).

Colours
-------
In an indexed PNG for an image with a palette file of its own (see palettes.txt), changed
palette colours change that palette file: every image using it changes too. For example,
frames\Chara\Chara_0000\*.png with other colours in the PNG palette recolour Crono. The
game reads the palette when it loads the character: a save state keeps the colours it holds
until the game loads that character again.

Sharing
-------
The dumped graphics are the game's, from your copy: for your own modding only. Do not
share them, nor edits of them; share art you drew yourself.
