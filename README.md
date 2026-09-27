![PPGC](art/readme.png)

# Painter's Playground GC

Painter's Playground ([Painters-Playground](https://github.com/myuu-151/Painters-Playground),
the native reimplementation) on the Nintendo GameCube, built on the
[Octave](https://github.com/myuu-151/Octave-libogc) engine.

The game logic is the Painters-Playground engine, compiled as is; this
repository adds what the GameCube needs: a GX renderer, file reads from the disc
image, the pads, memory card saves, and the Octave project around them.

## Building

Needs devkitPro with devkitPPC, Python 3 with Pillow, and next to this
repository:

- `PaintersPlayground/`: a checkout of Painters-Playground, built once for the
  PC (cmake fetches libtess2 and stb into its `build/_deps/`)
- `octave-libogc/`: [Octave-libogc](https://github.com/myuu-151/Octave-libogc)
  with its GameCube library and `Octave.exe` built

```sh
# Copy the game's data into the project, and make the disc banner and the
# memory card pictures from art/ (when the assets or the art change)
python tools/copy_data.py

# Build the disc image -> PPGC/Packaged/GameCube/PPGC.iso
Octave.exe -headless -project <path>/PPGC/PPGC/PPGC.octp -build GameCube
```

`make -f Makefile_GCN` in `PPGC/` compiles just the DOL.

## Layout

| Path | Contents |
|---|---|
| `PPGC/Source/` | The GameCube side: `CastleGame` (30 ticks a second, pads, the slot A check), `StageWidget` (draws the stage in Octave's UI pass), `renderer_gx.cpp`, `files_gc.cpp`, `MemoryCard.cpp` |
| `PPGC/Makefile_GCN` | Compiles the engine's sources from `../PaintersPlayground/engine` with these |
| `art/` | The disc and memory card banner (96 x 32), the memory card icon (32 x 32), and this page's banner |
| `tools/copy_data.py` | Copies `assets/` (SWFs, fonts, strings, collision) into `PPGC/Scripts/Data`, and runs `make_art.py` |

## Status

Boots, plays through the menus and character select into the first stage at
30 ticks a second, and saves to the memory card in slot A (checked at boot; the
title menu's Save / Load page). Not done yet:

- Masks (the GameCube has no stencil buffer; masked content draws unmasked)
- Sound (the game's xWMA needs converting)
- The later stages, tested; memory is tight
- Checking that it plays 1:1 with the PC version (replaying a recording on the
  console)
