// files::read on the GameCube: the assets are inside the disc image (read
// from the SD or the disc through Octave), under CastleCrashers/Scripts/Data.
#include "common/files.h"

#include <cstdlib>

#include "System/System.h"

namespace files {

bool read(const std::string& path, std::vector<uint8_t>& out) {
    char* data = nullptr;
    uint32_t size = 0;
    SYS_AcquireFileData(path.c_str(), true, 0, data, size);
    if (data == nullptr) return false;
    out.assign(reinterpret_cast<uint8_t*>(data), reinterpret_cast<uint8_t*>(data) + size);
    SYS_ReleaseFileData(data);
    return true;
}

bool exists(const std::string& path) { return SYS_DoesFileExist(path.c_str(), true); }

}  // namespace files
