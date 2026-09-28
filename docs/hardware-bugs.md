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

**Why it happened:** PPGC's list builder was written from scratch rather than
following Octave's. Octave's own display lists (`GxUtils.cpp`, meshes) take
`GX_EndDispList`'s size too, but can't meet this: they are given 64 bytes of
room after the list's exact size, so the flush never reaches the buffer's
end. It wasn't Octave's fault, nor libogc's alone (its gx.h warns of buffers
the exact size of their list). See [gamecube-code.md](gamecube-code.md):
check Octave's version of low-level GameCube code first.

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
