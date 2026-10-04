// ARAM: the GameCube's 16 MB of audio memory, reached from the CPU only by
// DMA. Split between the renderer's cache of display lists (the top
// kShapeCache bytes, renderer_gx.cpp) and the sound effects (the rest,
// audio_gc.cpp).
#pragma once

#include <cstdint>

namespace aram {

// (5968 KB: the effects' bank, 10.4 MB, fits below it, and the music's ring
// (audio_gc.cpp) has the rest, 256 KB, 8 s. It was 5 MB, and level 30's boss,
// reached with it full of the level and the shapes' records, made shapes again
// every frame; at 5.5 MB a full run of level 30 peaked at 5436 KB.)
constexpr uint32_t kShapeCache = 5968 * 1024;

// AR_Init, once; false if there is no ARAM to speak of.
bool init();
// What is free to use: [base, top), past what the OS keeps.
uint32_t base();
uint32_t top();

// A transfer, a piece at a time with interrupts off (as Octave's sound code
// does, so transfers from different threads never meet). Addresses and
// length 32-byte aligned.
void to_aram(const void* mem, uint32_t at, uint32_t len);
void from_aram(void* mem, uint32_t at, uint32_t len);

}  // namespace aram
