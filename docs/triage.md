# A bug on the console: whose is it?

Three layers can be at fault, and each has its own pipeline. Decide first:
weeks went into console-only theories (the SD card, thread stacks) for a bug
that was the game engine's all along, found in an afternoon once the PC
showed it too.

| Layer | What it is | Its pipeline |
|---|---|---|
| **The engine** | Castle-Crashers-Recomp's `engine/`: the game, its scripts, movie clips, menus, saves. Compiled into PPGC unchanged. | [Castle-Crashers-Recomp `docs/debugging.md`](https://github.com/myuu-151/Castle-Crashers-Recomp/blob/master/docs/debugging.md): reproduce on the PC, record, replay, dump, compare with the original game |
| **The port** | This repository's `CCGC/Source/`: the GX renderer, sound, files, memory, the card, pads, logging | [hardware-testing.md](hardware-testing.md): the console's logs, the flicker detector, filmstrips |
| **Octave / the hardware** | Octave-libogc, libogc, the GameCube itself (and Dolphin, which isn't it) | [gamecube-code.md](gamecube-code.md) (follow Octave's code), [hardware-bugs.md](hardware-bugs.md) |

## The questions, in order

1. **Does it happen on the PC?** (`castle.exe`, as far into the game as the
   console's save if that matters.) Play the same path.
   - **Yes:** it's the engine's. Follow the engine's pipeline, with the PC's
     session recording. Fix it there; PPGC's next build has it.
   - **No, or not yet seen:** go on. A bug that needs a long session (memory,
     pools, anything that fills up) may just not have been reached on the
     PC: play as long before deciding.
2. **Does it happen in Dolphin** (the same build, headless or not)?
   - **Yes:** the port's, most likely (the renderer, sound, memory):
     reproduce it headless and look at the log.
   - **No, only on the console:** the port's use of the hardware, or Octave's.
     Compare the code involved with Octave's own (gamecube-code.md); read the
     console's logs (hardware-testing.md); for the renderer, the filmstrip.
3. **Is it the game's logic** (a wrong screen, a missing character, a menu
   stuck, wrong numbers) or **the machine's** (a wrong picture, noise,
   lag, a crash, the logs stopping)? The first is almost always the engine's,
   whatever platform it's seen on; check the PC again, longer.

## Signs it's the engine's

- The screen is one the game draws itself (a menu, a HUD, a map) with wrong
  content or stuck.
- It follows a particular path through the game (a quit from the pause
  menu, a sub-level, a level after N levels) rather than a time or place on
  the screen.
- The console's log shows nothing wrong: no failed allocations, no list or
  mesh errors, the game ticking at 30 a second.
- The engine's own log lines: `DefineFunction ...: the pool of 1000 functions
  is full`, `attachMovie(...): no such export`, `loadMovie ...: dropped`.

## Signs it's the port's or the hardware's

- It shows in Dolphin but not the PC: the port's.
- Only on the console, and the log shows the machine: failed allocations,
  `LIST CHECK`, `GX_EndDispList gave`, a stall report, the logs stopping, draw
  time jumping.
- A picture wrong in part while the game's state is right (F3 on the PC, a
  filmstrip on the console).
