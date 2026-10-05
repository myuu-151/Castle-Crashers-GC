# Memory roadmap: about 1.9 MB of main memory to take back

Three audits of the program (2026-10-04) looked for what a normal disc
carries but never uses. This plan comes from them. It changes nothing the
game does or shows: every item is code or data that a normal disc never
runs, reads or displays. The details are in
[streamlining.md](streamlining.md) as M1-M3. Here they're measured and
ordered.

**Why it matters:** the console has 24 MB of main memory and the program
takes 4.35 MB of it. Big levels leave only 3-5 MB free; the bride chase
ran out of memory before. 1.9 MB is about half again on top of that.

**How the sizes were measured:** from the link map
(`Intermediate/GCN/CCGC.elf.map`), counting only what loads (`.text`,
`.data`, `.rodata`, `.bss`, `.sdata`, `.sbss`, `.eh_frame`,
`.gcc_except_table`), not `.debug_*`; and `powerpc-eabi-nm --size-sort -S`
on `Build/GCN/CCGC.elf`. Unused functions are already removed by
`--gc-sections` (devkitPPC turns on `-ffunction-sections` and
`-fdata-sections` by default), so everything below is what's left after
that.

| Phase | What | Saves | Effort |
|---|---|---|---|
| 1 | Small, safe changes in CCGC (and a few in Octave) | about 830 KB; **done: 900 KB** | Small |
| 2 | More Octave feature switches | about 440 KB; **done: 631 KB** | Medium |
| 3 | Lua left out of Octave for CCGC | about 670 KB | Large |

Do them in order, and for each step: rebuild, check the map for the
saving, then play a session on the console (the Gecko log's `free` and
`in one piece` numbers, and nothing missing on screen).

## Phase 1: about 830 KB (done 2026-10-05: 900 KB)

**Result:** the program went from 4,448 KB to 3,526 KB (libstdc++ from 477
KB to 39 KB: the removed locale code's exception tables went with it). On
the console, free memory at boot went from 17,684 KB to 18,552 KB, and the
same 868 KB in game (a little less than the program shrank: with a Gecko
plugged in, its queue now comes from the heap). Checked in Dolphin first:
both parsers read what the old ones did (276 files, 9 with holes; 420
effects, a 10,129 KB bank), and an out-of-memory thrown on purpose deep in
the drawing was caught as before. The C files' unwind flag (7 KB) was left
out: with this Makefile it would reach the C++ code too.

### 1a. No C++ string streams (380-410 KB)

About 412 KB of libstdc++ (86% of all of it that's linked) is locale and
stream machinery, pulled in by three `std::istringstream` parsers:

| Where | Parses | Replace with |
|---|---|---|
| `CCGC/Source/audio_gc.cpp:173-179` | The effects bank's index: a name, then 7 `uint32_t` and an `int` | A pointer walk: skip spaces, copy the name, `strtoul` each number. **Parse before `SYS_ReleaseFileData`** (line 174 releases the buffer first now, after copying it to a string) |
| `CCGC/Source/files_gc.cpp:75-89` | `files.txt`: a path, a size, then `off:size` holes (already `strtoul`) | Lines by `memchr('\n')`, then the same pointer walk; carry on from `strtoul`'s `end` |
| `engine/player/mods.cpp` | The PC's replacement graphics (`mod.txt`) | Leave it out of the GameCube build (1b) |

How the locale code gets in (from the map's "archive member included"
section): `audio_gc.o` → `istream-string.o` → `locale.o` →
`cow-shim_facets.o` / `cxx11-shim_facets.o` → the four `*locale-inst.o`
members (288 KB on their own). `files_gc.o` and `mods.o` pull `ctype.o`
through `getline`.

- **Open question:** `std::filesystem::path` (used all over) still refers
  to `locale.o`, so the members are still pulled from the archive. The
  saving depends on `--gc-sections` throwing them out. The code paths
  that keep them run only from the stream constructors, so it should.
  Check the map after the change.
- **1:1 with the PC:** no float is parsed in what changes (the only floats
  are in `mods.cpp`, which never runs here). `std::stoi` and
  `std::to_string(int)` don't use locale and stay.
- **Overflow:** `>>` fails on an overflowing number and `strtoul` doesn't;
  the files are made by tools, so this can't happen, but check `errno` to
  match.

### 1b. The PC's replacement graphics, left out (about 55 KB)

`player/mods.cpp` runs only if `Game::mod_dir` is set (`game.cpp:47`), and
only the PC's `engine/main.cpp:640` sets it (not compiled here). It also
brings `stb_image` and `std::filesystem::directory_iterator`.

- **How:** add `mods.cpp` to `EXCLUDE` in `Makefile_GCN` and give
  `player::apply_mod` a stub returning 0 (as `octave_unused.cpp` does for
  Octave's).

### 1c. Diagnostics a normal disc never reads (about 180 KB)

| What | KB | Why a normal disc doesn't need it | How |
|---|---|---|---|
| `trace_gc.cpp` `g_ring` | 60 | Written by `put()` (`:47`), read only by `append()`, which runs only with the SD log (`stall_report`, `:115-117`) | `#ifdef PPGC_SD_LOG` |
| `trace_gc.cpp` `watchdog_stack` | 64 | Without the SD log the watchdog only sends a stall report to the Gecko (`OctLog`; Octave's file log is off) | Only with `PPGC_SD_LOG`, or keep it with an 8 KB stack (saves 56) |
| `new_gc.cpp` `g_sites` | 56 | Read only by `census_log`, which returns at once without `PPGC_DIAG` (`:733-736`) | `#ifdef PPGC_DIAG`; keep the 8-byte tag so the heap's layout stays the same |
| `replay_gc.o` | 2 | Every `replay::` call is under `CASTLE_REPLAY` | Wrap the file in `#ifdef CASTLE_REPLAY` |

### 1d. Buffers made only when they're first needed (128 KB)

| What | KB | Only used for | How |
|---|---|---|---|
| `IsoDvd_Dolphin.cpp` `sBounce` | 64 | Reading a real disc (`OctDvdRead`, `:122-146`), only when no image is on the SD card | `memalign` it in `IsoOpenDVD`, kept from then on. Not left out: a burned disc, or Dolphin with no SD card, still need it |
| `System_Dolphin.cpp` `sGeckoQueue` | 32 | Only when a Gecko is found (`:1240-1245`, `:1272`) | Allocate it in `OctGeckoLogEnable` when one is found |
| `System_Dolphin.cpp` `sStaging` | 32 | Background reads into ARAM (`:977-979`), which only Lua starts (`Texture::StashFrom`) | Allocate it on the first one |

These three are Octave's: an Octave change, for every game.

### 1e. Link and compile flags (about 72 KB)

| What | KB | Why | How |
|---|---|---|---|
| `.eh_frame_hdr` (the map charges it to `crtmain.o`) | 65 | A lookup table for exception frames that nothing reads here: the unwinder searches the frames registered at start (`__register_frame_info`), and there's no `dl_iterate_phdr` | `-Wl,--no-eh-frame-hdr` in `LDFLAGS` |
| Unwind tables of C files (libtess2 4.4, SdGeckoDma 2.8) | 7 | No exception passes through C: libtess2 runs out of memory by `longjmp` | `-fno-asynchronous-unwind-tables` for `.c` files only |

- **Check:** an out-of-memory catch still works (the game catches
  `bad_alloc` in `CastleGame.cpp:727`, `:754`, `:835`, `movie.cpp:291` and
  `new_gc.cpp:894-905`). The unwinder sorts its frame list with `malloc`
  on the first exception; if that fails when memory is short, lookups fall
  back to a slower search, which still works.

## Phase 2: about 440 KB of Octave feature switches (done 2026-10-05: 631 KB)

**Result:** the program went from 3,526 KB to 2,895 KB. Octave got ten new
switches in `EngineFeatures.h` (`OCT_NETWORK`, `OCT_VIDEO`, `OCT_SPLINES`,
`OCT_SKELETAL`, `OCT_PARTICLES`, `OCT_INSTANCING`, `OCT_TEXT3D`,
`OCT_UI_EXTRAS`, `OCT_CONSOLE`, `OCT_STATS`), each on unless a game turns
it off, so every other Octave game builds as before. `Engine/OctFeatures.mk`
reads them from the environment and names the library after what's off
(`Build/GCN_nophysics_..._nostats`); CCGC's `Makefile_GCN` turns all
thirteen off and links that library.

- **Every reference is gated, not just the `FORCE_LINK` line:** a class is
  linked if anything names it (its `DEFINE_NODE` registers it, which pulls
  in everything it uses). So the Lua `Bind()` calls, `As<T>()` checks, the
  `GFX_*` functions in `Graphics_GX.cpp` and `GxUtils.cpp`, and the bone
  and particle paths in `Renderer`, `Node3d`, `Scene`, `World` and
  `AssetManager` are all inside the switches.
- **Networking** isn't removed but replaced: with `OCT_NETWORK` off,
  `NetworkManager.cpp` builds a 2.7 KB stand-in that is always local and
  always the authority, with no clients, and sends nothing. That's what the
  real one is in a game that never connects.
- **More than measured:** the `*_Lua.o` bindings and `GFX_*` code went with
  each class, and `StageColorsFrom`'s 32 KB buffer is now allocated on first
  use (CCGC never calls it).
- **Left in (12 KB):** Bullet leftovers pulled by `Primitive3d.o` and
  CCGC's `Main.o` (9 KB), and `NetFunc`/`NetDatum` (1.5 KB) pulled by
  `Script.o`. They go with Lua in phase 3, or aren't worth it.
- **Checked in Dolphin:** boots, menus, character select, a level with
  fights and a cutscene.
- **On the console:** free memory at boot went from 18,552 KB to 19,210 KB
  (658 KB). Levels played, each with every shape and bitmap drawn: level 6
  left at least 6,614 KB free (3,156 KB in one piece), against 4,085-4,399
  KB (1,276-1,700) in the runs before phase 1; level 28 5,245 KB (2,556),
  against 3,329 KB (1,500). Saves, music and the SD card as before.
- **Needs Octave with `Engine/OctFeatures.mk`** (the feature switches
  commit).

More switches beside `OCT_PHYSICS`, `OCT_NAVIGATION` and `OCT_VORBIS` in
`EngineFeatures.h`, the same pattern (they freed about 0.9 MB). These are
linked because `Engine.cpp`'s `ForceLinkage()` (`:160-210`) and a few
direct calls reference them; CCGC never creates any of them.

| Switch | KB | What it must guard |
|---|---|---|
| Networking (`NetworkManager`, `NetMsg`, `NetDatum`, `NetFunc`) | 82 | `Engine.cpp:388`, `:503`, `:737`, `:781`, `:820`, `:840`; `Node.cpp:41-55`, `:454`, `:495`, `:1180`; `Node3d.cpp:281`; `Datum.cpp:402`; `World.cpp:686`; `Scene.cpp:557`; `StatsOverlay.cpp:7` (`NET_*` is already stubbed in CCGC's `octave_unused.cpp`) |
| Splines (`Spline3D`) | 65 | `Engine.cpp:178`; `Renderer.cpp:19`, `:813-814` |
| Video (`VideoStream`, `VideoQuad`, `Video3d`, `VideoClip`, `JpegYuvDecoder`, `VideoPlayer`) | 56 | `Engine.cpp:180`, `:195`, `:209`; `LuaBindings.cpp:111`, `:132` |
| Skeletal meshes | 55 | `Engine.cpp:171`, `:190`; `Renderer.cpp:17`, `:1232-1250`; `Node3d.cpp:152-158`, `:484`; `Scene.cpp:500`; `Script.cpp:4-6`; `Graphics_GX.cpp:1073-1366`; `GxUtils.cpp:207`, `:816`; `Mesh3d.cpp:58` |
| Particles | 32 | `Engine.cpp:169`, `:188`, `:189`; `Renderer.cpp:16`, `:1253`; `World.cpp:18`, `:1807-1820`; `Graphics_GX.cpp:1553-1568` |
| `Button` | 26 | `Engine.cpp:20`, `:208`, `:772` |
| Instanced meshes | 16 | `Engine.cpp:177`; `Mesh3d.cpp:63`; `Graphics_GX.cpp:1451` |
| Collision shapes (`Box3D`, `Sphere3D`, `Capsule3D`), already inert without physics | 15 | `Engine.cpp:166`, `:172`, `:174` (`Box3D` is also used by `Renderer.cpp` and `NavMesh3d`: check) |
| `ArrayWidget`, `Poly`, `PolyRect` | 12 | `Engine.cpp:198`, `:202`, `:203` |
| `TextMesh3D` | 11 | `Engine.cpp:176`; `Font.cpp:444`; `Graphics_GX.cpp:1457-1472` |
| `NavMesh3D` (into `OCT_NAVIGATION`) | 11 | `Engine.cpp:179`; `World.cpp:1258`, `:1298` |
| The hidden console (`CONSOLE_ENABLED`, made overridable) | 11 | `Constants.h:59`; `Engine.cpp:200` (`EnableConsole` already checks for none) |
| Bullet leftovers (into `OCT_PHYSICS`) | 8 | `Primitive3d.cpp:19`, `:914`; `CCGC/Source/Main.cpp` shouldn't include `World.h`'s Bullet headers |
| `StatsOverlay` (never shown) | 7 | `Renderer.cpp:11`, `:117`, `:137`, `:846`, `:1424`; `Engine.cpp:204`; `VideoQuad` |
| `StaticMesh::StageColorsFrom` `sPiece` | 32 | Called only from Lua (`StaticMesh.cpp:782`): heap or guarded |

With Lua still in (until phase 3), each switch also drops its `*_Lua.o`
bindings (splines 6 KB, particles 22 KB...). The `GFX_*` code those classes
use in `Graphics_GX.cpp` and `GxUtils.cpp` should go with them too (not
measured).

## Phase 3: about 670 KB, Lua left out

Octave starts Lua at every boot (`Engine.cpp:528-586`) and runs
`EngineStartup.lua`, which logs one line and looks for a `SaveInfo` script.
CCGC has no Lua at all: it sets the save's info in C++
(`MemoryCard.cpp:75`).

| Part | KB |
|---|---|
| The Lua VM | 209 |
| The bindings | 402 |
| `Script`, `ScriptUtils`, `ScriptFunc`, auto-registration | 57 |

- **How:** make `LUA_ENABLED` (`Constants.h:85`) overridable and let the
  variant library set it to 0. Today it can't be: `Engine.cpp:700-701` calls
  `lua_settop` unguarded, and `ScriptAutoReg.h:6-42`'s macros are used in
  about 170 places (`Node.cpp` 42, `Utilities.cpp` 52, `SmartPointer.cpp`
  34, `Datum.cpp` 20, ...). Drop `Source/LuaBindings` and `../External/Lua`
  from `Engine/Makefile_GCN:46-47` for that variant.
- **Effort:** large. An Octave change of its own (and release), worth it
  for any Octave game that doesn't use Lua.
- **Also saved at run time (not measured):** Lua's state and every bound
  class's table on the heap.

## Must stay

The audits checked these; they look droppable but aren't:

| What | KB | Why |
|---|---|---|
| C++ exception tables (`.eh_frame`, `.gcc_except_table`) | 409 + 44 | The game catches out-of-memory (`bad_alloc`). `-fno-asynchronous-unwind-tables` does nothing for C++ with this compiler (tested) |
| libogc's default stacks | 144 | The main thread's 128 KB stack (the game, the script interpreter's recursion, SD reads) and the exception stack. Don't shrink without measuring how deep it gets |
| `audio_gc` thread stacks and buffers | about 104 | The reader reads the SD card (64 KB of stack); the mixer's stack and buffers; `asnd`'s |
| `g_record_bounce` | 8 | Copies shape records to and from ARAM on every shape made |
| Save icon and banner | 12 | Written to the memory card |
| libogc's own SD driver | about 17 | The fallback if Octave's can't mount the card (`System_Dolphin.cpp:93`) |
| libogc's console and font | 13 | The crash screen |
| Octave's engine, renderer, world, nodes, widgets, `Quad`, `Texture`, `Text`, `Canvas`, `Font`, the loading screen, materials, the asset manager, `System_Dolphin`, `IsoDvd_Dolphin`, `SdGeckoDma`, `Graphics_GX`, `GxUtils`, audio and input | | Boot, the splash, the loading screen, the stage widget, every file and save |
| Cameras, lights, `Audio3d`, static meshes, materials | about 31+ | Unused, but `World.cpp` and `Renderer.cpp`'s 3D path refer to them throughout: later, if ever |

## Optional, later (about 60 KB)

| What | KB | Note |
|---|---|---|
| Logging-only code (`loading_state`, `input_state`, `LogPerformance`, `TraceChanges`, `gx_costs` and the rest) | about 25 | A `PPGC_TRACE` flag; the Gecko log would lose those lines, which have found every bug this month |
| `new_gc.cpp`'s small-block tables | about 38 | Indexed by chunk instead of page; hot allocator code, medium risk |
