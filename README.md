![PPGC](art/readme.png)

# Painter's Playground GC

The Castle Crashers native reimplementation
([Castle-Crashers](https://github.com/myuu-151/Castle-Crashers)) on the Nintendo
GameCube, built on the [Octave](https://github.com/myuu-151/Octave-libogc) engine.

The game logic is the Castle-Crashers repository's engine, compiled as is; this
repository adds what the GameCube needs: a GX renderer, file reads from the disc
image, the pads, and the Octave project around them.

## Building

Needs devkitPro with devkitPPC, Python 3, and next to this repository:

- `CastleCrashers-GC/`: a checkout of Castle-Crashers, built once for the PC
  (cmake fetches libtess2 and stb into its `build/_deps/`)
- `octave-libogc/`: [Octave-libogc](https://github.com/myuu-151/Octave-libogc)
  with its GameCube library and `Octave.exe` built

```sh
# Copy the game's data into the project (when the assets change)
python tools/copy_data.py

# Build the disc image -> CastleCrashers/Packaged/GameCube/CastleCrashers.iso
Octave.exe -headless -project <path>/CCGC/CastleCrashers/CastleCrashers.octp -build GameCube
```

`make -f Makefile_GCN` in `CastleCrashers/` compiles just the DOL.

## Layout

| Path | Contents |
|---|---|
| `CastleCrashers/Source/` | The GameCube side: `CastleGame` (30 ticks a second, pads), `StageWidget` (draws the stage in Octave's UI pass), `renderer_gx.cpp`, `files_gc.cpp` |
| `CastleCrashers/Makefile_GCN` | Compiles the engine's sources from `../CastleCrashers-GC/engine` with these |
| `tools/copy_data.py` | Copies `assets/` (SWFs, fonts, strings, collision) into `CastleCrashers/Scripts/Data` |

## Status

Boots in Dolphin and runs the logo, legal, loading and attract screens at 30
ticks a second. Not done yet:

- Masks (the GameCube has no stencil buffer; masked content draws unmasked)
- Freeing a movie's display lists and textures when it unloads
- Sound (the game's xWMA needs converting)
- Memory card saves
- Checking that it plays 1:1 with the PC version (replaying a recording on the
  console)
