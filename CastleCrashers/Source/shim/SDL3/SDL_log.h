// The engine logs with SDL_Log; on the GameCube that goes to Octave's SD log
// (/octiso.log, when the local logger is enabled).
#pragma once

void OctLog(const char* format, ...);

#define SDL_Log OctLog
