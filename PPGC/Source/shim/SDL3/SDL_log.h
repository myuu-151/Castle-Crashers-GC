// The engine logs with SDL_Log; on the GameCube that goes to PpgcLog
// (trace_gc.h): Octave's log, and the SD card's /ppgc.log.
#pragma once

extern "C" void PpgcLog(const char* format, ...);

#define SDL_Log PpgcLog
