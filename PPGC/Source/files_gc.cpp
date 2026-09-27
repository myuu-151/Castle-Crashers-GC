// files::read on the GameCube: the assets are inside the disc image (read
// from the SD or the disc through Octave), under PPGC/Scripts/Data.
//
// A file whose size files.txt gives (tools/copy_data.py writes it) is read
// straight into the vector the game keeps. Octave's whole-file read gives a
// buffer of its own, and copying that out needed as much memory again, in
// one piece: loading the keep's sky (562 KB) ran out of memory that way.
#include "common/files.h"

#include <malloc.h>
#include <ogc/system.h>

#include <cstdlib>
#include <map>
#include <sstream>
#include <string>

#include "System/System.h"
#include "memory_gc.h"
#include "trace_gc.h"

namespace files {

namespace {

const char* const kRoot = "PPGC/Scripts/Data/";

uint32_t free_kb() {
    const struct mallinfo info = mallinfo();
    return (uint32_t(info.fordblks) + uint32_t((char*)SYS_GetArena1Hi() - (char*)SYS_GetArena1Lo())) / 1024;
}

// files.txt, once: path (from the data root) -> size.
const std::map<std::string, uint32_t>& sizes() {
    static std::map<std::string, uint32_t> map;
    static bool loaded = false;
    if (loaded) return map;
    loaded = true;
    char* data = nullptr;
    uint32_t size = 0;
    SYS_AcquireFileData((std::string(kRoot) + "files.txt").c_str(), true, 0, data, size);
    if (!data) {
        PpgcLog("files: no files.txt; every file is read twice over");
        return map;
    }
    std::istringstream in(std::string(data, size));
    SYS_ReleaseFileData(data);
    std::string path;
    uint32_t n;
    while (in >> path >> n) map[kRoot + path] = n;
    PpgcLog("files: sizes of %u files", unsigned(map.size()));
    return map;
}

}  // namespace

bool read(const std::string& path, std::vector<uint8_t>& out) {
    trace::at(trace::kMain, "reading a file", path.c_str());
    // Before a level: what the heap holds, by caller (memory a level before
    // left behind shows as a caller that grows level after level).
    if (path.find("/levels/") != std::string::npos) memory::census_log(path.c_str() + path.rfind('/') + 1);
    PpgcLog("files: reading %s", path.c_str());
    uint32_t start = trace::now_ms();
    auto it = sizes().find(path);
    if (it != sizes().end()) {
        out.resize(it->second);
        bool ok = SYS_ReadFileRange(path.c_str(), true, 0, it->second, reinterpret_cast<char*>(out.data()));
        PpgcLog("files: %s %u KB in %u ms, %u KB free%s", path.c_str(), unsigned(it->second / 1024),
                unsigned(trace::now_ms() - start), unsigned(free_kb()), ok ? "" : " (FAILED)");
        if (!ok) std::vector<uint8_t>().swap(out);
        return ok;
    }
    char* data = nullptr;
    uint32_t size = 0;
    SYS_AcquireFileData(path.c_str(), true, 0, data, size);
    PpgcLog("files: %s %u KB in %u ms, %u KB free (not in files.txt)", path.c_str(), unsigned(size / 1024),
            unsigned(trace::now_ms() - start), unsigned(free_kb()));
    if (data == nullptr) return false;
    out.assign(reinterpret_cast<uint8_t*>(data), reinterpret_cast<uint8_t*>(data) + size);
    SYS_ReleaseFileData(data);
    return true;
}

bool exists(const std::string& path) { return SYS_DoesFileExist(path.c_str(), true); }

}  // namespace files
