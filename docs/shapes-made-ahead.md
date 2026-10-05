# Shapes made ahead: building a room's shapes behind its loading screen

Castle Crashers' art is Flash vector shapes. The GameCube can't draw curves,
so each shape is cut into triangles (tessellated) and made into a display
list the first time it is drawn (`make_shape` in `renderer_gx.cpp`). A small
shape takes a millisecond or two; a big one 10-50 ms, and the biggest
(the world map's, some menu art) 100-540 ms. Once made, a list is kept, in
main memory or the shape cache in ARAM ([gamecube-code.md](gamecube-code.md)).

A frame has 33 ms (30 ticks a second), of which the game itself uses about
7-15 ms. A build that fits in what's left costs nothing visible. Many big
builds in the same frames don't fit, and the game drops ticks.

## The problem (2026-10-05)

Two places dropped ticks on the console, both the first time a lot of new
shapes appeared at once mid-play:

| Where | What appeared | On the console |
|---|---|---|
| The Painter's room (`level53`, in the boss hall `level44`) | The fight's shapes, as it got going | 4 builds a frame (26 ms of building), 22 ticks a second |
| Industrial Castle's second part (`level58`) | The boiling liquid, as it spawned | 16 builds of 30-38 ms, 20-23 ticks a second |

Why those two: the Painter's room file is by far the heaviest of the boss
rooms, 172 shapes over 1 KB (394 KB of shape data) where the others have
29-50 (80-173 KB). Most of them aren't on screen when the room opens; they
come in all at once when the fight starts. On a second visit there was no
slowdown, because the lists were still in ARAM from the first.

## How it works now

1. **A movie loads** (the engine's `swf::Movie::on_loaded` hook, called at the
   end of `Movie::load`; the PC leaves it unset). If it is a stage's
   (`level*`) or an enemy's (`e*`, not the endings), `queue_ahead` queues it.
2. **At the next frame's first `draw_shape`**, `make_ahead` makes its shapes
   whose records are 1 KB or more, the biggest first, and sends each list to
   ARAM. The game is still drawing its loading screen then: the file has been
   read, but the room isn't showing yet.
3. **When the room first draws them**, the lists come back from ARAM by DMA
   (a fraction of a millisecond) instead of being built.
4. **Anything not made ahead** is made when first drawn, as before. Nothing
   is ever left undrawn because of this.

Limits, so a load never runs away or crowds ARAM:

- **Time:** up to 4 s for one load (`kAheadBudget`), a safety stop only. In
  practice: about 0.5 s for most rooms (their first screen's shapes
  included), 1.7 s for Flowery Field, 2.6 s for the Painter's.
- **ARAM:** keeps 1 MB free (`kAheadMargin`). If there isn't room, it first
  drops lists not drawn for 30 s or more (`aram_drop_aged`, `kAheadAged`):
  copies of lists still in main memory first, then lists only in ARAM
  (forgotten: made again if ever drawn). Lists drawn in the last 30 s are
  never touched.
- **One exception, `level58`** (Industrial Castle's second part): it waits 20
  frames (`kAheadFirstScreen`, `made_after_first_screen`) so its first screen
  is made the normal way as it is drawn, then makes the rest.

## Why it is built this way: what was tried

| Version | When it made them | Result |
|---|---|---|
| In the game's tick, as the movie loaded | Inside `Movie::load` | **Dolphin: "GFX FIFO: Unknown Opcode"**, a crash |
| At `begin_frame` | Before the frame's drawing | The same crash |
| Inside `draw_shape` (kept) | In a frame being drawn | Works |
| After the loads settled (20 frames), 2 s for a stage and its enemies together | | Liquid fixed, seamless. **The Painter's room froze 1.9 s** once already showing |
| Straight after each load, 1 s a load | | No freeze, but the biggest shapes (some on the first screen) used the second up: `level58` made 12 of 112, **the liquid lagged again** |
| Straight after each load, all of it | | Both fixed; `level58` stood **2.5 s** on its loading screen |
| As above, but `level58` waits for its first screen (now) | | Both fixed; Industrial back to about 1 s |

The lessons:

- **Making a list sends GX commands.** `build_shape` writes the state the GPU
  still owes and a triangle of no area into the live command stream before
  recording the list. That is only valid in the middle of drawing a frame.
  In the tick, or at `begin_frame` before the frame's GX is set up, the
  commands land where the GPU doesn't expect them. So the queue is worked
  through at the top of `draw_shape`, where shapes are normally first made.
- **When decides what you see.** The same work, done while the loading screen
  is up, is invisible; done once the room shows, it is a freeze; done in
  play, it is lag.
- **Why waiting helps Industrial but not the Painter.** Waiting lets a room's
  first screen be made the normal way, spread over the frames the game draws
  under its black screen; only the rest is one block (1 s for `level58`).
  Made all at once, the same work is one still frame of 2.5 s. By the log the
  totals were about the same (the file read to the music: 4-5 s either way,
  3-4 s before any of this), but the one long still frame read as a longer
  load. Industrial's black screen is long (about 4 s) and one file loads;
  the Painter's room change is quick, its enemy's file loads a second after
  the room's, and the block (both files, 1.9 s) is bigger: there, waiting ran
  past the black screen and froze a room you could see. Hence the exception.
- **A cap per load has to cover the room's first screen too**, or it is used
  up before the shapes that matter (`level58` at 1 s).

## On the console

| Room | Made ahead | Time | In play |
|---|---|---|---|
| Industrial Castle `level58` | 44 of 44 (after its first screen) | 986 ms | The liquid at 30 ticks a second, was 20 |
| The Painter's room `level53` + `epainter` | 172 of 172 + 16 of 16 | 2.6 s + 160 ms | 30 ticks a second, was 22 |
| Industrial Castle's first part `level6` | 30 of 30 | 450 ms | |
| Flowery Field `level28` + `ebee` | 58 of 58 + 3 of 3 | 1.7 s + 33 ms | |
| The boss hall `level44` | 28 of 28 | 450 ms | |

Before room was taken back from lists not drawn for 30 s, late in a long
session ARAM was full: the room got 89-121 of its 172 and the Painter's own
16 none ("ARAM's room used"); those were made in the fight at 12-16 ms, one
at a time, without a slow second. With it, all of both (above).

## What to watch in the log

- `gx: made ahead for <files>: N of M shapes, K KB to ARAM, T ms (why it
  stopped)`, once per load.
- `gx: slow build: shape N in T ms (tessellating, list): ...` for any shape
  made in play that takes 4 ms or more. A run of these where a room's fight
  or set piece starts is the sign of a room that needs more made ahead.
- Signs it costs too much: `records in ARAM` falling on the perf line (shape
  records left in main memory for want of ARAM), free memory in rooms below
  what the full playthrough had (4.6-7.7 MB), `forgotten` above 0 in play,
  or `shapes not drawn` above 0 in a `SUMMARY`.

## Not covered

- **The world map** (`map`): its first screen, about 1.5 s of building with
  one 160 ms shape, is made again on every return, as the map's movie goes
  each time a stage loads. Keeping its lists in ARAM between visits would end
  that.
- **The players', effects' and menus' movies** (loaded once, kept all game):
  made the first time each shape is drawn, a few 12-50 ms builds per session.
- **A cleaner rule than the `level58` exception:** make a room's shapes after
  its first screen, but never later than the loading screen. That needs the
  engine to tell the renderer when its loading screen goes (it knows:
  `Game::loading_active_`), a hook like `on_loaded`.
