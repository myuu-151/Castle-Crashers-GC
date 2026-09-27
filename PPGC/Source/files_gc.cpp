// files::read on the GameCube: the assets are inside the disc image (read
// from the SD or the disc through Octave), under PPGC/Scripts/Data.
#include "common/files.h"

#include <malloc.h>
#include <ogc/system.h>

#include <cstdlib>

#include "System/System.h"

void OctLog(const char* format, ...);

namespace files {

namespace {

uint32_t free_kb() {
    const struct mallinfo info = mallinfo();
    return (uint32_t(info.fordblks) + uint32_t((char*)SYS_GetArena1Hi() - (char*)SYS_GetArena1Lo())) / 1024;
}

}  // namespace

bool read(const std::string& path, std::vector<uint8_t>& out) {
    char* data = nullptr;
    uint32_t size = 0;
    SYS_AcquireFileData(path.c_str(), true, 0, data, size);
    OctLog("files: %s %u KB, %u KB free", path.c_str(), unsigned(size / 1024), unsigned(free_kb()));
    if (data == nullptr) return false;
    out.assign(reinterpret_cast<uint8_t*>(data), reinterpret_cast<uint8_t*>(data) + size);
    SYS_ReleaseFileData(data);
    return true;
}

bool exists(const std::string& path) { return SYS_DoesFileExist(path.c_str(), true); }

}  // namespace files
