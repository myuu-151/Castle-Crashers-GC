# Bugs only the console shows

Dolphin runs this game very closely to the GameCube, but not exactly. These
are the bugs that showed on real hardware and never in Dolphin: what they
looked like, how they were found, and what fixed them. How to test on the
console and read what it logs is in [hardware-testing.md](hardware-testing.md).

## GX_EndDispList gives 67108864 for a list that fills its buffer

**Seen:** on the title menu, part of the burning castle on the right (a dark
red wall with battlements, under the fire) was drawn for 12 ticks and missing
for 12, over and over. Dolphin and the PC drew it all the time.

**Cause:** the wall is a clip (menu.swf 537) that shows shape 535 for frames
1-12 and shape 536 for frames 13-24. On the console one of the two lists was
called with the wrong size.

`renderer_gx.cpp` builds each shape's display list into a buffer of the
list's own bytes rounded up to 32, plus 32 bytes of room. `GX_EndDispList`
flushes 32 bytes of no-ops after the list. When the list's bytes are a whole
number of 32-byte blocks (192, 3072, ...), list and flush fill the buffer
exactly, and on the console the CPU FIFO's write pointer wraps to the
buffer's start with its wrap flag set, bit 26 (0x04000000). `GX_EndDispList`
doesn't take that for an overflow; it gives back the pointer's distance,
flag and all: **exactly 67108864**. (libogc's gx.h has a `\bug` note on
buffers the exact size of their list.) Dolphin doesn't wrap there, and gives
the right size.

The list's bytes were right (checked by reading the list back uncached); only
the size was wrong. `GX_CallDispList` with it drew almost nothing of the list,
so the wall shape wasn't there. The menu's other shapes happen not to end on
a 32-byte block.

It wasn't only the wall: any shape whose list was a whole number of 32-byte
blocks was missing on the console. A shape that stays on screen was missing
for good, rather than popping. The fix also brought back the rope in the
weapon room, and the console logged two 192-byte lists with the wrong size
on the way into the attract screen.

**Found by**, in order (each a build on the SD card, see hardware-testing.md):

1. The filmstrip: 48 consecutive tick pictures of the menu showed which part
   popped, and its period (12 on, 12 off). A dump of the menu's display tree
   on the PC (`castle.exe menu --dump N`) matched that to clip 537's two shapes.
2. The wall's brightness logged per picture (`gx: film NN wall V`), on the
   console and in Dolphin: the same for frames 1-12, different for 13-24.
3. Nothing had gone to ARAM (`aram 0 KB`), and masks and tints don't touch it,
   which left the lists themselves.
4. Each list read back uncached and compared with what was written: all
   correct on the console.
5. The wall drawn three ways (as usual; vertex cache cleared first; its bytes
   written through the FIFO instead of called): missing all three ways, and
   only one of its two shapes ever got as far as being drawn.
6. The console logged `memory: no room for a 67105792-byte block ... for
   0x8001d32c 0x8001dab4`. `powerpc-eabi-addr2line` on that build's ELF put it
   in the list check, sizing a buffer from GX's size.
7. GX's size logged beside the real one: `GX_EndDispList gave 67108864 bytes
   for a list of 3072`, three times, each a whole number of 32-byte blocks.

**Fix** (PPGC `ec24438`, and the room after it): the list's size is computed
from the bytes written, rounded up to 32, and checked in memory; GX's is only
logged when it differs by more than its flush. The room after a list is 64
bytes, so the flush never reaches the buffer's end.

**Side effect while finding it:** the list check first sized a buffer from
GX's number. On the console that asked for 64 MB, failed, and the `bad_alloc`
stopped the frame's drawing. The shape was never made, so it was tried again
every frame, and the whole menu flickered (builds `b091ecc` to `badf6d3`). A
shape that fails to build is retried on every draw, so anything that throws
while building one ends every frame at that shape.

**Why it showed up when it did:** the 32 bytes of room dated from `37e3fcc`,
but the popping started with the disc that fixed the intro's lightning. The
same disc had `0e7f037`, which flattened curves more coarsely (tolerance 2 to
6 twips) for fewer triangles. That changed the wall's shapes: at tolerance 2
they were 445 and 524 triangles, lists of 4008 and 4719 bytes; at 6, 341 and
419 triangles, and 341 triangles is a list of 3 + 1023 x 3 = **3072** bytes,
exactly 96 blocks of 32. (Checked in Dolphin with the tolerance put back to
2.) Any change to tessellation shuffles which lists land on a whole number of
blocks.

**Why it happened:** PPGC's list builder was written from scratch rather than
following Octave's. Octave's own display lists (`GxUtils.cpp`, meshes) take
`GX_EndDispList`'s size too, but can't meet this: they are given 64 bytes of
room after the list's exact size, so the flush never reaches the buffer's
end. It wasn't Octave's fault, nor libogc's alone (its gx.h warns of buffers
the exact size of their list). See [gamecube-code.md](gamecube-code.md):
check Octave's version of low-level GameCube code first.

## The SD card written while the disc is read from it (suspected)

**Seen:** late in a session, after a few levels and trips to the world map,
a level loaded broken (the player's character invisible, the health bar at
0), and leaving it hung on a black screen. Another time a level's file was
read and the level never started (black, the game still ticking). Neither
happens in Dolphin.

**What the logs showed:** both logs on the card, `ppgc.log` and Octave's
`octiso.log`, written by different threads, stopped at the same moment, on
the world map, while the game went on to the broken level. From then on
nothing could be written to the card.

**Cause (suspected, not yet confirmed):** the disc image is read from the
same SD card, and Octave serializes everything that touches the card with a
lock (`OctLockFileIo` / `OctUnlockFileIo` in `System_Dolphin.cpp`): its disc
reads and its log writes take it, and its comment says overlapping use from
two threads hangs the SD driver. PPGC's own writes didn't take it: the
`ppgc.log` writer (which runs whenever the game waits, often while a disc
read is under way), the watchdog's `ppgc_stall.log`, and the filmstrip's
pictures. A write on top of a read can leave the driver's state wrong: reads
after that give wrong data (a level broken) or never finish (black).

**Fix:** every SD write in PPGC holds the lock (`trace::SdLock`,
trace_gc.h), and so does the one check for a file (`files::exists` answers
from files.txt; a level is looked for in `game/` before `levels/`, and the
miss went to `stat()` on the card unlocked). With that, a whole session's
logs ran to the end.

## Thread stacks too small for the SD card (suspected)

**Seen:** after the SD lock, a session ran to the end, but going back to the
world map late in it showed a wrong picture that stayed on screen, with the
game still running.

**Cause (suspected):** the log writer and the watchdog had 16 KB stacks and
write to the card through libfat and the SD driver. Octave gives every thread
that touches the card 64 KB: "16 KB overflowed on hardware once a thread read
the SD card (fread -> libfat -> SD driver)" (System_Dolphin.cpp). A stack
overflow writes over whatever is below it, silently. In the build's memory
map the renderer's tables of shapes and textures (`g_shapes`, `g_textures`)
sit just below the two stacks: scrambled, they make a shape draw another's
display list, a wrong picture.

**Fix:** 64 KB stacks. Found by comparing PPGC's low-level code with Octave's
throughout (after the display lists and the SD lock), which also brought:
Octave's `GxWaitGpu` where PPGC waited on the GPU itself, a shape's slot had
before its list is built, fog and the TEV swap set for the stage, the flicker
copy's buffer invalidated before the GPU writes it, ARAM transfers rounded to
32 bytes as Octave's are, and log lines no longer also written to Octave's
`/octiso.log` once `/ppgc.log` works (each line was another file opened and
closed on the card).

## Masked content tested for the same depth

**Seen:** in the intro, the lightning was drawn half way, and masked parts of
the slides were missing. Dolphin drew them.

**Cause:** masks are drawn on the depth buffer (the GameCube has no stencil
buffer). A mask writes its level's depth, and what it masks was drawn where
the depth *equalled* that level. On the console the depth a mask writes and
the depth later tested round differently, so the test failed on some pixels.

**Fix** (`c01535c`): masked content is drawn half a level farther, and passes
where the buffer is at least as near (`GX_GEQUAL`). The way masks are drawn
can be switched on the pad to compare (L + R + D-pad up: by a range of depths,
by an equal depth, or off), and the status line says which.
