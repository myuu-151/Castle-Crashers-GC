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
| **Anything on the SD card** (the disc image is read from it) | `OctLockFileIo` / `OctUnlockFileIo` around every use, `System/Dolphin/System_Dolphin.cpp` | `trace::SdLock` (`trace_gc.h`) around every write |
| Memory card | `System/Dolphin/System_Dolphin.cpp` | `MemoryCard.cpp` |
| Pads | `Input/Dolphin/Input_Dolphin.cpp` | `CastleGame.cpp` (`ReadPads`) |
| Threads (**64 KB of stack for any that touches the SD card**; below the main thread's priority 64 if it busy-waits on the card) | `System/Dolphin/System_Dolphin.cpp` (`SYS_CreateThread`), `Audio/Dolphin/Audio_Dolphin.cpp` | `trace_gc.cpp`, `audio_gc.cpp` |
| Waiting on the GPU | `GxWaitGpu` (`Graphics/GX/GxUtils.h`) | `renderer_gx.cpp` |
| ARAM transfers (32-byte addresses and lengths, interrupts off, flush / invalidate) | `Audio/Dolphin/Audio_Dolphin.cpp` (`AramDma`) | `aram_gc.cpp` |

## Sound: what lives in ARAM

ARAM is the console's 16 MB of audio memory, and sound belongs there. The
official games kept their samples in it and streamed music through an ARAM
buffer (or through the drive's own audio streaming). `audio_gc.cpp` does
the same.

- **Effects:** one bank (`audio/sounds.bank`, 10 MB), read into ARAM by the
  reader thread at boot. A playing effect is copied back a block at a time.
- **Music:** streamed from the disc into a ring in ARAM. The ring is the room
  between the bank and the renderer's cache, 256 KB (8 s). The reader
  stays below the main thread (priority 50, as Octave's), and fills the ring
  in the main thread's spare time.

**Why the music is in ARAM:** it used to be a 2 s ring in main memory. In
level 30's boss fight (2026-10-04) frames took longer than a frame to draw.
The reader, below the main thread, got no time, the ring ran dry, and the
music cut in and out. A single failed disc read then ended the track for
good. 8 s of music in ARAM outlasts any such stretch. A failed
read is now tried again (up to 5 s of them) rather than ending the track.
Don't raise the reader above the main thread for good to fix starving:
disc reads busy-wait (`IsoDvd_Dolphin.cpp`'s `DiWait`), so that just moves
the stall onto the game. What is raised is each read from the SD card
itself, for its few milliseconds (below).

## The SD card under load

The disc image is read from the SD card (Octave's `SdGeckoDma.c`). Twice
in level 30's boss fight a slow stretch was followed, within a minute, by
every read failing at once ("549 KB in 2 ms (FAILED)") until a reboot.
What's known and what was done (2026-10-04):

- A read that fails partway leaves the card mid-transfer. The driver's
  restart then read a junk ID and decided there was no card, on every read
  after. It now resets a card it has seen before instead of giving up.
- The card's waits are timed (1.5 s for a block). A reader below a busy main
  thread could be starved mid-transfer. Threads that read the card note
  their priority (`OctSd_NoteThreadPriority`), and each of their reads runs
  just above the main thread (66), then back.
- The driver's own messages (a read failed and at what stage, a restart
  failed and at what step) now reach the log as `sd:` lines. Before, only
  video playback printed them, so neither failure said why.

## The shape cache in ARAM

Display lists leaving main memory are copied to the top of ARAM, 5968 KB
(`renderer_gx.cpp`, with the shapes' records). When it's full, room is
made by dropping the copies of lists that are still in main memory first,
and only then forgetting lists that are only in ARAM (which must be
tessellated again, 10-35 ms for a big one). It used to take the oldest of
either: in level 30's boss fight, reached with the cache full of the level,
that forgot 1-2 shapes a frame that were needed again at once, and frames
took 20-50 ms. The perf log's `castle: draw` line counts shapes built,
fetched, evicted and forgotten, to watch for it.

**Strips.** Each display list sends its triangles as triangle strips where
they join (`strip_mesh` in `renderer_gx.cpp`), and the rest as plain
triangles. Same triangles, 57% of the vertex references (2026-10-04), so
every list takes about half the room in main memory and in ARAM, and the
GPU's vertex work falls with it: the character select went from 14.3 ms of
transforming for 123K triangles to 10.3 ms for 147K, on the console. A strip
only joins triangles that share an edge, which only the triangles of one
fill do, and strips go out in the order of their first triangle, so later
fills and outlines still draw over earlier ones. The `gx: strips` log line
gives the running total.

**Why ARAM and not the other fixes:** the options for the music cutting out,
and what each would have cost:

| Option | Music | Cost to the game |
|---|---|---|
| The old 2 s ring in main memory | Cut out when the game got busy | None |
| Raise the reader above the main thread | Kept up | Stutter: its disc reads busy-wait, so heavy scenes lose that time |
| A bigger ring in main memory | Kept up | 600 KB-1 MB of main memory, the scarce part (3-4 MB free in big levels): out-of-memory risk |
| A big ring in ARAM (done) | Kept up | None: no CPU time, no main memory; the reader stays low and fills it in spare moments |

**The ring's size** only has to outlast the longest stretch the reader can
fall behind, and a failing read's 5 s of retries. It was 1 MB (32 s) at
first, then 592 KB (18 s); on the console, through all of level 30, it
stayed about 580 of its 592 blocks full. It is now 256 KB (8 s), and the
rest went to the shape cache (above). If the reader ever falls further
behind, the music goes silent where it is and carries on from the same
place when data comes; nothing else is held up.

**The music and the shape cache are separate fixes** that share ARAM's
space: the ring fixed the music cutting out; the bigger, smarter shape
cache fixed level 30's boss slowdown. ARAM is a fixed 16 MB, so growing one
means taking from the other (or from the effects bank).

**Not done: playing on the DSP.** Here the CPU still decodes and mixes
every voice (`mixer_main`). On the real games the DSP plays ADPCM straight
out of ARAM, in hardware, and the CPU does no audio work at all. Doing that
would mean re-encoding every sound and track from Microsoft ADPCM (what
`tools/convert_audio.py` makes, from the PC's files) to the GameCube's DSP
ADPCM, and writing a DSP voice player. That's a CPU saving only, and so far
the mixer hasn't been what slows the game down. Worth it only if it ever is.

## Also

- libogc's headers (`devkitPro/libogc/include/ogc/*.h`) have `\bug` and
  `\note` lines on edge cases; read the ones on any GX call used in a new way.
- Anything that differs from Octave's way is a place to look first when the
  console and Dolphin disagree.
- Test on the console as [hardware-testing.md](hardware-testing.md) says:
  Dolphin passing isn't enough for this kind of code.
