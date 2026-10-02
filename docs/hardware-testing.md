# Testing on the console, and reading what it logs

First: is the bug the console's at all? See [triage.md](triage.md). A bug the
PC shows too is the game engine's, and far quicker to find there.

Dolphin doesn't show every bug (see [hardware-bugs.md](hardware-bugs.md)), so
the game logs what it does to the SD card as it runs. Take a build to the
console, play, bring the card back, and read the files.

## Diagnostic builds

The log, the watchdog, the perf and audio lines, the mismatch counts and
failed allocations are always on: they cost next to nothing. The log is
written to the SD card only in a build with the **card log**, made with
`SDLOG=1` in the environment (the builder's **SD card log**); without it the
lines stay in memory and the card isn't touched. What costs the game time is
only in a **diagnostic build**, made with `DIAG=1` in the environment
(Octave's build passes it on to `make`; test builds with `AUTOPRESS` have it
too; both have the card log):

- the flicker detector and the filmstrip (a copy of every frame off the GPU,
  and pictures written to the SD card);
- display lists read back and checked in memory as they're made;
- a `gx: made ...` line for every mesh;
- the heap census at each movie change and level read.

`make` doesn't notice the switch: delete
`CCGC/Intermediate/GCN/{renderer_gx,new_gc,CastleGame,Main,trace_gc}.o`
when switching between a diagnostic build and one for playing, or the card
log on or off (the builder does). A disc for playing is built without `DIAG`
or `SDLOG`.

## A test run

1. Build the disc image (README), with `SDLOG=1` for the log, or `DIAG=1`
   for the log and the flicker detector,
   filmstrip, list checks, mesh lines and census, and copy it to the SD card's root as
   `Painter's Playground.iso` (the name Swiss lists). Compare the copy with
   the original (`cmp`); a half-written image boots and fails in odd ways.
2. **Delete the old logs and pictures** from the card's root (`*.log`,
   `*.pgm`): `ppgc.log` is appended to, not replaced.
3. **Keep the build's `CCGC/Build/GCN/CCGC.elf`** beside the logs: failed
   allocations log code addresses, which only that build's ELF can name.
4. Boot it with Swiss and play to where the problem is. To film a menu, stay
   on it for 10 seconds (see the filmstrip below).
5. Switch off and bring the card back: its root has the files below.

Keep each run's files in a folder of their own, and compare a run's log with
the last good run's **as a whole** before building the next test: the blink
counts and the perf line's numbers show a new build's side effects (a build
that made the whole screen flicker showed it plainly in its blink counts).

## The files

| File | Written by | What |
|---|---|---|
| `ppgc.log` | `trace_gc.cpp` | Everything the game logs (`PpgcLog`, and `SDL_Log` from the engine). Lines start with the milliseconds since boot. |
| `ppgc_stall.log` | the watchdog | Only when the game stops: see below. |
| `octiso.log` | Octave | Octave's own log (`OctLog`, which `PpgcLog` lines go to as well) and its disc reads. |
| `ppgc_flicker_<screen>_a/b.pgm` | the flicker detector | Two frames with no tick between them that differ. |
| `ppgc_blink_<screen>_a/b/c.pgm` | the flicker detector | Three ticks where something went off and came back. |
| `ppgc_film_<screen>_NN.pgm` | the filmstrip | 48 consecutive ticks of a menu. |

The pictures are 160 x 120 greyscale PGM (any image viewer that reads PGM,
or Pillow); `<screen>` is `<movie>_page<N>`, as in the log's scene lines.

## How the log is kept

`PpgcLog(format, ...)` (trace_gc.h) puts a line into a ring of 256 lines in
memory and wakes a writer thread, which gathers for 200 ms and appends the
batch to `/ppgc.log`. The writer runs below every other thread, so writing
never holds up the game; it writes while the game waits on the GPU or the
retrace. With no SD card the ring is all there is.

Each thread marks where it is (`trace::at(thread, what)`): main, mixer
(audio), reader (disc reads). The main loop counts ticks and frames. The
**watchdog**, above every other thread, checks them four times a second; after
3 seconds with no tick and no frame it writes `/ppgc_stall.log` itself, then
again every 15 seconds: how long, where each thread was, and the last lines
of the ring. A black screen or a freeze leaves this file.

Log from new code with `PpgcLog` (or `SDL_Log`, which the GameCube build
routes to it). Keep lines under 240 characters, and don't log every frame
unless it's for a short, known stretch (a few hundred lines is fine; the ring
is 256 lines between writes). Anything that goes into the draw path on every
frame belongs in a test that's taken out again.

## The lines

### Where the game is

- `castle: tick N: <movie>, page P, X KB free (Y in one piece)`: each time
  the movie or the menu page changes.
- `census <movie>: N KB in blocks from the heap`, then ten `census  KB
  blocks  A B` lines: at each movie change, the heap's biggest users, by the
  two return addresses above `operator new` (name them with addr2line, below).
- `files: <path> N KB in M ms, X KB free`: each file read from the disc.
- `gx: movie <name> goes`: a movie's shapes and textures freed.

### Every two seconds

- `castle: perf <movie> page P  30.0 ticks/s  tick T ms (max M)  draw D ms  X
  KB free (Y in one piece)  small S KB  shapes L KB  textures X KB  aram A
  KB  list waits W  scratch over O  clips C  roots R  ticks N frames F`:
  - `tick`/`draw`: the game's update and drawing time per tick and frame
    (a frame is a sixtieth of a second, a tick a thirtieth);
  - `free`: heap free in total, and the biggest block (what a big allocation
    can get);
  - `small`: the pages of blocks of 256 bytes or less;
  - `shapes`: display lists in main memory; `aram`: display lists moved to
    ARAM (0 unless memory is short); `list waits`: frames that filled list
    memory and waited for the GPU;
  - `scratch over`: tessellations that didn't fit the scratch region (they
    use the heap instead; a jump means something is holding scratch memory);
  - `clips`, `roots`: movie clips and movie roots alive (they should level
    off, not climb).
  - `[masks: equal]` / `[masks: off]` after the page: the mask mode switched
    on the pad (L + R + D-pad up).
  - `frame F` after the page: the movie's timeline frame. One that stays put
    while nothing is loading means the movie's scripts are waiting on
    something.
- `castle: loading: state S (then T), changing to X, incoming after N
  updates, flags loading A ready B no screen C; sub-movies NAME
  ready|loading xREFS (now) ...; queued Q NAME (target gone) ...`: only while
  a movie change or loadMovie is under way. `state` is the game's (1 loading
  a movie, 2 running, 3 waiting for the loading screen, 4 loading a
  sub-movie); the flags are the scripts' `g_bLoading`, `g_bReadyToLoad` and
  `g_bNoLoadingScreen`; `sub-movies` are the loadMovie slots; `queued` are
  loadMovie calls waiting for their movie. A black screen that never ends
  shows here as the same line, every two seconds.
- `castle: audio N buffers mixed, V voices, effects K KB, music B blocks
  ahead`: the mixer's progress; `music ... ahead` falling to 0 means the disc
  isn't keeping up.
- `gx: <screen>: FLICKER n frames: (x,y)xk ...; BLINK n ticks: (x,y)xk ...
  (masks mode m; draws brightened 1-2x a, over 2x b)`: the flicker detector's
  counts on this screen, when they change (below).
- `gx: lists checked in memory: N made (B not as written; GX's size wrong for
  G)`: display lists built so far, when that changes.
- `gx: mismatches so far: ...`: when any of these counts changed: shapes not
  drawn (no list memory / not back from ARAM / waiting to be made again),
  bitmaps without a texture yet, colour transforms brightening past 4x. The
  first of each kind is logged on its own as `gx: mismatch: ...`, and
  `audio: mismatch: ...` for effects played before the effects bank was
  loaded, or with every voice busy.

### Only when something is wrong

- `memory: no room for a N-byte block (L display lists to ARAM, X KB free in
  pieces, Y KB in one; for A B)`: `operator new` failed, after moving L
  display lists to ARAM to make room. `A B` are the two return addresses
  above it: who asked (below). `made room for` instead: it succeeded after
  moving them.
- `castle: OUT OF MEMORY drawing` / `in a tick`: that failure ended a frame's
  drawing or a tick. Every frame that throws loses the rest of its drawing,
  which looks like flicker.
- `gx: LIST CHECK: <part> differs at ...`: a display list read back from
  memory (uncached, as the GPU reads it) wasn't what was written; it's built
  again.
- `gx: GX_EndDispList gave N bytes for a list of M`: GX's size for a list
  wasn't its real one (the size used is always the real one).
- `loadMovie NAME: its target is gone; dropped` / `no clip free in the pool;
  dropped`: a loaded movie (a level) that was never put on the stage (the
  engine, player.cpp).
- `gx: list memory N KB`: display-list memory grew or shrank (it grows
  while lists don't fit and the heap has 512 KB to spare, and goes back to
  its least when a movie goes or an allocation fails).
- `gx: BAD MESH: ...`: a tessellated shape with indices past its vertices,
  or coordinates that aren't numbers.
- `gx: a shape's display list overflowed` / `out of memory for a shape`:
  tried again a second later.

### Each shape made

`gx: made V vertices T triangles, x a..b y c..d, hash H`: every tessellated
shape, with a hash of its vertices and indices. The same shapes in the same
order should give the same lines on the console and in Dolphin; diff the two
logs to find a shape that tessellates differently on the console.

## The flicker detector

`renderer_gx.cpp`, "the flicker detector". After the stage is drawn, each
frame is copied off the GPU as greys and read back the next frame (only
while at least 2 MB of the heap is free in one piece; it takes 300 KB). Each
copy becomes a 160 x 120 picture, compared in 10 x 10 blocks (a block counts
when at least 12 of its pixels differ by 40 or more):

- **FLICKER**: two frames with no tick between them differ. The game only
  changes in a tick, so this is the renderer's doing. The first per screen is
  saved (`_a` before, `_b` after).
- **BLINK**: on the first frame after a tick, pixels that differ from the tick
  before but are back (within 12) to the tick before that: something went off
  and came back. Animation moves on rather than back, but some genuinely
  blinks, so compare the count with a good run's. The first blink after 20 on
  a screen is saved (`_a`, `_b`, `_c`: the three ticks).

Pictures from at most 12 screens are saved. In the log, `(x,y)xk` is a block
(x, y from 0 to 9, left to right, top to bottom) and how many times it
counted, the most first.

**The filmstrip:** on a screen whose movie is `menu` or `main`, once it has
stayed 150 ticks (5 seconds), the next 48 ticks' pictures are saved as
`ppgc_film_<screen>_00..47.pgm` (three screens at most). This catches what
pops in and out slower than a blink: look at them in order, or diff each with
the one before. `gx: film NN wall V` is each picture's mean grey over the
menu's castle wall (x 115-155, y 75-90), from finding that bug.

## Naming a code address

Failed allocations and the census give return addresses. With the ELF of
**the same build**:

```sh
powerpc-eabi-addr2line -f -C -i -e CCGC.elf 0x8001d32c 0x8001dab4
```

`-i` shows the inlined functions as well, down to the file and line.

## The same checks in Dolphin

Headless Dolphin (`DolphinNoGUI -u <user dir> -p headless -e <iso>`) runs a
build without the console, with the same log: with the logger's OSReport
channels on (`Config/Logger.ini`: `OSREPORT`, `OSREPORT_HLE`), its
`Logs/dolphin.log` has every `PpgcLog` line. Mute it (`Volume = 0` in its
`Dolphin.ini`). Pictures don't come back from Dolphin (no SD card), so the
filmstrip's `gx: film NN wall V` lines are how its pictures compare with the
console's.

Builds for testing take these from `make -f Makefile_GCN` (or the
environment, for Octave's build):

- `AUTOPRESS=N`: pad 0 presses A every N ticks between ticks 800 and 1700
  (Start with every other press), to get through the menus unattended, then
  walks right and attacks (400 stays on the title menu long enough for its
  filmstrip). It also fills scratch memory with junk each time
  the region is emptied (`PPGC_POISON`), as the console's memory is, where
  Dolphin's starts zeroed.
- `CARDTEST=1`: walks into the Save / Load page, saves, loads and leaves it.

Neither goes on a disc for playing.
