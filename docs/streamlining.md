# Streamlining: what's left to make cheaper, with no visual change

Everything here keeps the picture exactly as it is. Each item gives what it
buys, measured where it could be, what it costs to do, and how to check it
on the console. The numbers are from 2026-10-04 (the console's live log and
the linker's map), after the work listed under **Done** at the end.

Where the game stands, to judge the items against:

| Resource | Where it's tight | Measured |
|---|---|---|
| Main memory (24 MB) | Big levels | 3-5 MB free in big levels (the Parade dipped to about 3 MB before); the program itself is 4.35 MB of it |
| ARAM shape cache (5968 KB) | Long levels, before their bosses | With strips: about 2.7 MB in use on the map, 2.2 MB of it shape records |
| GPU | Heavy screens | Character select: 31 ms clipping against 10 ms transforming, so clipping is now its biggest cost |
| CPU | Shapes made again | 10-35 ms to tessellate a big shape, only when the cache overflows (it no longer does) |

The order to work in: **memory first (M1, M2, M3), then G1, then the rest
as needed.** Check each one on the console before the next.

## Memory (main memory)

The program is 4.35 MB, from the link map (`Intermediate/GCN/CCGC.elf.map`;
count only the loaded sections, not `.debug_*`):

| Part | Size |
|---|---|
| Octave engine (`libEngine.a`) | 2044 KB |
| This port and the game's engine | 1132 KB |
| libstdc++ | 477 KB |
| libogc, libc, libfat and the rest | about 700 KB |

### M1. More Octave feature switches (1-1.5 MB, a guess)

Most of Octave is 3D and other features this game never creates:
networking (`NetworkManager`, 64 KB), splines, skeletal and static meshes,
particles, instanced meshes, 3D nodes, the Lua bindings. They're linked in
because Octave registers every node type at start (`FORCE_LINK_CALL` in
`Engine.cpp`), so `--gc-sections` can't drop them.

- **How:** more switches in `EngineFeatures.h` beside `OCT_PHYSICS`,
  `OCT_NAVIGATION` and `OCT_VORBIS` (which freed about 0.9 MB), each
  guarding a feature's registration and code; `Makefile_GCN`'s variant
  library turns them off.
- **Check:** what CCGC actually creates or calls, before switching anything
  off (the stage widget, text, the asset manager, the scene). The size from
  the map, before and after; a full play session on the console.
- **Effort:** medium. Same pattern as the switches already done.

### M2. No string streams (250-300 KB)

About 280 KB of libstdc++ is locale machinery (`locale-inst`,
`wlocale-inst` and their `cxx11` versions), pulled in by a few
`std::stringstream` / `istringstream` uses, one of them in `audio_gc.cpp`
(the effects bank's index).

- **How:** replace each with plain parsing (`strtol`, `sscanf`, a small
  line reader). Find every user first, Octave's included: the locale code
  goes only when none is left.
- **Check:** `locale-inst.o` gone from the map; the bank index and anything
  else parsed still reads the same.
- **Effort:** small, once the users are found.

### M3. Diagnostic leftovers on normal discs (about 230 KB)

Kept in every build, used only by some:

| What | Size | Used by |
|---|---|---|
| The stall watchdog's stack and the trace ring (`trace_gc.cpp`) | about 120 KB | The SD log (`SDLOG=1`), off on normal discs |
| The disc read's bounce buffer (`sBounce`, `IsoDvd_Dolphin.cpp`) | 64 KB | Reading a real disc; never used when the image is read from the SD card |
| The PC's replacement-graphics mods (`player/mods.cpp`) | 51 KB | The PC only |

- **How:** only with the SD log, or allocated when first needed, or left
  out of the GameCube build (`EXCLUDE` in `Makefile_GCN`).
- **Effort:** small.

## GPU

### G1. Cheaper clipping (GPU time on heavy screens)

On the character select the GPU spends 31 ms clipping against 10 ms
transforming. Clipping comes from shapes that cross the screen's edges:
level 30's backgrounds draw an 867x624-px rock nine times a frame. The
GameCube can draw without clipping and let pixels off the screen simply
not be drawn (`GX_SetClipMode`), which looks the same as long as no vertex
lies too far off the screen.

- **How:** first measure how much of a heavy frame is clipping, on the
  console. Then clipping off where every vertex of a shape is within the
  safe range (from its box, as culling already tests), on otherwise.
- **Check:** the GPU log line's `clip=` time before and after, on the
  character select and level 30's boss; filmstrips of the screen's edges.
- **Risk:** a shape too far off the screen draws wrongly, visibly. Console
  only (Dolphin isn't the real GPU).
- **Effort:** small change, careful console test.

### G2. Smaller indices for small shapes (list memory)

Shapes with under 256 points could use 1-byte position indices instead of
2, so a third less list. Effects, smoke and the characters are mostly small.

- **How:** a second vertex format with 8-bit position indices, chosen per
  shape in `build_shape`.
- **Check:** the diagnostic build's list check (every byte against what the
  GPU reads), as for the strips; `shapes` and `aram` in the perf line.
- **Effort:** small to medium. The same kind of change as the strips.

## ARAM

### A1. Compressed shape records (about 1 MB of ARAM)

The shapes' records take 2.2 MB of the ARAM shape cache. Compressed, maybe
half that, which is room for display lists. A record is only unpacked when
its shape is made, which costs little beside making it.

- **How:** compress each record when it's stashed (`Shape::stash`,
  `stash_record` in `renderer_gx.cpp`), unpack in `fetch_record`. A small,
  fast compressor (LZ-type).
- **Check:** `records in ARAM` in the perf line; shapes made the same
  (the PC's tests use the same records, uncompressed).
- **Effort:** medium. Safe: records only feed the shape builder.

## CPU

### C1. Faster shape making (the worst case)

When a shape is made again it takes 10-35 ms for a big one (tessellation,
then gradients split until they're smooth). It only happens when the shape
cache overflows, which it no longer does, so this only shrinks a worst case.

- **How:** a faster tessellator, or keeping the work gradients need.
- **Effort:** medium to large. Only if overflows come back.

### C2. Sound on the DSP

The CPU decodes and mixes every voice. The DSP could play ADPCM straight
out of ARAM instead, but every sound would need re-encoding (from Microsoft
ADPCM to the GameCube's), and the mixer isn't what slows the game down.
See [gamecube-code.md](gamecube-code.md#sound-what-lives-in-aram).

## Done

| What | Bought | Commit |
|---|---|---|
| Music streamed into ARAM (8 s) | No music dropouts under load; 64 KB of main memory | 296cd91, 8999377 |
| Shape cache: copies dropped before shapes, 5968 KB | Level 30's boss at 30 ticks/s instead of 16-26 | 8999377 |
| Triangle strips | 57% of the vertex references: lists half the size; about 40% less vertex work | 3afa09c |
| SD card: recovery, read priority, logged | A slow stretch no longer kills every read after | Octave 3597b40f |
| Octave physics/navigation/Vorbis switched off, English-only font, shape records in ARAM | About 3.5 MB of main memory (the bride chase) | daea0ed, f7a29de |
