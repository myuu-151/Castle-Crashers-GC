// Tracing for the GameCube build: a log of what the game does, and a
// watchdog for when it stops (trace_gc.cpp).
//
// PpgcLog lines go to Octave's OctLog (Dolphin's log window, or the SD
// card's /octiso.log) and into a ring in memory, which a thread writes to
// /ppgc.log on the SD card in batches. Each thread marks where it is
// (trace::at); when the game stops ticking or drawing for a few seconds,
// the watchdog, above every other thread, writes /ppgc_stall.log itself:
// where each thread was, and the last lines of the log.
#pragma once

#include <cstdint>

extern "C" void PpgcLog(const char* format, ...) __attribute__((format(printf, 1, 2)));

// Octave's lock on the SD card (System_Dolphin.cpp): its reads of the disc
// image (every SYS_ReadFileRange: the game's files and music) and its log's
// writes take it. Anything else that touches the card must too: the SD
// driver keeps state per card, and two threads using it at once hang it or
// corrupt what's read (the logs stopping, then a level broken or never
// loading; docs/hardware-bugs.md).
void OctLockFileIo();
void OctUnlockFileIo();

namespace trace {

// Holds the SD card's lock for a scope.
struct SdLock {
    SdLock() { OctLockFileIo(); }
    ~SdLock() { OctUnlockFileIo(); }
    SdLock(const SdLock&) = delete;
    SdLock& operator=(const SdLock&) = delete;
};

enum Thread { kMain, kMixer, kReader, kThreads };

// Where a thread is now; `what` must stay valid (a literal), `detail` is
// copied (a path, a name).
void at(Thread thread, const char* what, const char* detail = nullptr);

// The main loop's progress, for the watchdog.
void ticked();
void drew();

// For the heartbeat line.
uint32_t ticks();
uint32_t frames();

// Starts the writer and the watchdog (once).
void start();

// Milliseconds since start.
uint32_t now_ms();

// A line into the ring (PpgcLog's; it also goes to OctLog).
void log_line(const char* text);

}  // namespace trace
