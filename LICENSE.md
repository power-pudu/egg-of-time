# License

The source code of Egg of Time (the runtime in `runtime/`, the recompiler and tools in `tools/`, the scripts in
`scripts/` and the build files) is released under the MIT License:

```
MIT License

Copyright (c) 2026 Power Pudu

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## What this license does not cover

- **The game.** Chrono Trigger, its code, graphics, text, music and other data are the property of Square Enix and
  are not covered by this license. This repository and its releases contain none of them: players supply their own
  dump of the game, from which the game is built on their machine. Code generated from a ROM, extracted files, ROMs
  and saves are not part of this project and must not be distributed with it.
- **`config/`** describes the game (addresses, function bounds, symbol names) for the recompiler; it contains no
  game code or data.
- **Third-party components** keep their own licenses: the files in `runtime/third_party/` (minicoro and librashader's
  header, both MIT), and in releases the libraries and tools listed in `kit/licenses`
  (SDL3, FFmpeg, librashader, dsd, xdelta3, llvm-mingw, Python). The Japanese glyphs in `runtime/ui_font_ja.inc` come
  from PixelMplus10, an M+ font: "These fonts are free software. Unlimited permission is granted to use, copy, and
  distribute them, with or without modification, either commercially or noncommercially." (Copyright (C) 2002-2013
  M+ FONTS PROJECT; provided "as is", without warranty.)

Egg of Time is an unofficial fan project, not affiliated with or endorsed by Square Enix or Nintendo. Chrono Trigger
is a trademark of Square Enix.
