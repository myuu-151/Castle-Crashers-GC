# Writing GameCube code here: check Octave first

PPGC has GameCube code of its own where Octave doesn't do what the game needs.
The main piece is the GX renderer. Octave draws 3D meshes; this game's engine
hands its `render::Renderer` 2D vector shapes (tessellated, coloured
triangles), colour transforms, masks and bitmaps, as the PC's OpenGL renderer
gets them. That code has to be written here.

But the low-level pieces under it (display lists, textures, ARAM, the pads,
the memory card, disc reads, threads, the retrace) Octave already does, and
has already been through on real hardware. **Before writing one of those,
read Octave's version and follow it**, unless there's a reason not to (then
say why in a comment).

**Why:** the menu's castle wall went missing on the console because PPGC's
display lists were written from scratch with 32 bytes of room after each
list. Octave's (`GxUtils.cpp`) have 64, with a comment saying why: the
flush at the end of a list. With 32, a list that's a whole number of 32-byte
blocks makes `GX_EndDispList` give 67108864 as its size on the console,
never in Dolphin ([hardware-bugs.md](hardware-bugs.md)). Following Octave
would have avoided it.

## Where Octave does each thing

Paths are in `octave-libogc/Engine/Source/`.

| What | Octave | PPGC |
|---|---|---|
| Display lists (sizing the buffer, the cache, the write-gather pipe) | `Graphics/GX/GxUtils.cpp` | `renderer_gx.cpp` (`build_shape`) |
| Frames: waiting for the GPU, the retrace, the copy to the framebuffer | `Graphics/GX/Graphics_GX.cpp` (`GFX_BeginFrame`, `GFX_EndFrame`) | draws inside Octave's frame (`StageWidget`) |
| Textures | `Graphics/GX/Graphics_GX.cpp`, `Engine/Assets/Texture.cpp` | `renderer_gx.cpp` (`make_texture`) |
| ARAM and sound | `Audio/Dolphin/Audio_Dolphin.cpp` | `aram_gc.cpp`, `audio_gc.cpp` |
| Disc reads | `System/Dolphin/System_Dolphin.cpp` (`SYS_ReadFileRange`), `Engine/Stream.cpp` | `files_gc.cpp`, `audio_gc.cpp` (use it) |
| Memory card | `System/Dolphin/System_Dolphin.cpp` | `MemoryCard.cpp` |
| Pads | `Input/Dolphin/Input_Dolphin.cpp` | `CastleGame.cpp` (`ReadPads`) |
| Threads | `System/Dolphin/System_Dolphin.cpp`, `Audio/Dolphin/Audio_Dolphin.cpp` | `trace_gc.cpp`, `audio_gc.cpp` |

## Also

- libogc's headers (`devkitPro/libogc/include/ogc/*.h`) have `\bug` and
  `\note` lines on edge cases; read the ones on any GX call used in a new way.
- Anything that differs from Octave's way is a place to look first when the
  console and Dolphin disagree.
- Test on the console as [hardware-testing.md](hardware-testing.md) says:
  Dolphin passing isn't enough for this kind of code.
