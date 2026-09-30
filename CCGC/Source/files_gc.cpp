// files::read on the GameCube: the assets are inside the disc image (read
// from the SD or the disc through Octave), under CCGC/Scripts/Data.
//
// A file whose size files.txt gives (tools/copy_data.py writes it) is read
// straight into the vector the game keeps. Octave's whole-file read gives a
// buffer of its own, and copying that out needed as much memory again, in
// one piece: loading the keep's sky (562 KB) ran out of memory that way.
// A SWF's large bitmaps' pixels are read apart from the rest (read_holed).
#include "common/files.h"

#include <malloc.h>
#include <ogc/system.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>
#include <string>

#include "System/System.h"
#include "memory_gc.h"
#include "trace_gc.h"

namespace files {

namespace {

const char* const kRoot = "CCGC/Scripts/Data/";

uint32_t free_kb() {
    const struct mallinfo info = mallinfo();
    return (uint32_t(info.fordblks) + uint32_t((char*)SYS_GetArena1Hi() - (char*)SYS_GetArena1Lo())) / 1024;
}

// files.txt, once: path (from the data root) -> size, and after a SWF's
// size its large bitmaps' pixels as offset:size (tools/copy_data.py).
std::map<std::string, std::vector<Hole>> g_holes;

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
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string path, hole;
        uint32_t n;
        if (!(fields >> path >> n)) continue;
        map[kRoot + path] = n;
        while (fields >> hole) {
            size_t colon = hole.find(':');
            if (colon == std::string::npos) continue;
            g_holes[kRoot + path].push_back(
                {uint32_t(std::strtoul(hole.c_str(), nullptr, 10)), uint32_t(std::strtoul(hole.c_str() + colon + 1, nullptr, 10))});
        }
    }
    PpgcLog("files: sizes of %u files, %u with bitmaps read apart", unsigned(map.size()), unsigned(g_holes.size()));
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

std::vector<Hole> holes(const std::string& path) {
    sizes();
    auto it = g_holes.find(path);
    return it != g_holes.end() ? it->second : std::vector<Hole>{};
}

// The file around its holes, piece by piece into `out`, which is made its
// final size first; each hole's pixels handed on as a stream, read a part at
// a time. The most in one piece is then the larger of the rest of the file
// and one bitmap's texture (for level 9's sky, 246 KB and 512 KB, not
// 2.35 MB, nor 1 MB of raw pixels).
bool read_holed(const std::string& path, const std::vector<Hole>& holes, std::vector<uint8_t>& out, TakeHole take,
                void* context) {
    trace::at(trace::kMain, "reading a file", path.c_str());
    if (path.find("/levels/") != std::string::npos) memory::census_log(path.c_str() + path.rfind('/') + 1);
    auto it = sizes().find(path);
    if (it == sizes().end()) return false;
    uint32_t start = trace::now_ms();
    uint32_t left_out = 0;
    for (const Hole& h : holes) left_out += h.size;
    if (left_out > it->second) return false;
    PpgcLog("files: reading %s, %u KB of it bitmaps read apart", path.c_str(), unsigned(left_out / 1024));
    out.resize(it->second - left_out);
    char* at = reinterpret_cast<char*>(out.data());
    uint32_t from = 0;
    bool ok = true;
    for (size_t i = 0; ok && i < holes.size(); i++) {
        const Hole& h = holes[i];
        if (h.offset < from) {
            ok = false;
            break;
        }
        uint32_t before = h.offset - from;
        if (before) ok = SYS_ReadFileRange(path.c_str(), true, from, before, at);
        at += before;
        if (!ok) break;
        // The hole's pixels as a stream, read a part at a time into the
        // texture (renderer_gx.cpp): never whole in memory. Read whole, a
        // sky's 1 MB in one piece wasn't to be had after 53 minutes and 15
        // levels (level 20 ran out of memory: 2.5 MB free, 512 KB in one).
        struct Source {
            const std::string* path;
            uint32_t base, size;
        } source{&path, h.offset, h.size};
        HoleStream stream;
        stream.self = &source;
        stream.read = [](void* self, uint32_t offset, uint32_t size, uint8_t* dst) {
            const Source& s = *static_cast<const Source*>(self);
            if (uint64_t(offset) + size > s.size) return false;
            return SYS_ReadFileRange(s.path->c_str(), true, s.base + offset, size, reinterpret_cast<char*>(dst));
        };
        ok = take(context, i, size_t(at - reinterpret_cast<char*>(out.data())), nullptr, &stream);
        from = h.offset + h.size;
    }
    if (ok && from < it->second) ok = SYS_ReadFileRange(path.c_str(), true, from, it->second - from, at);
    PpgcLog("files: %s %u KB (and %u KB of pixels) in %u ms, %u KB free%s", path.c_str(), unsigned(out.size() / 1024),
            unsigned(left_out / 1024), unsigned(trace::now_ms() - start), unsigned(free_kb()), ok ? "" : " (FAILED)");
    if (!ok) std::vector<uint8_t>().swap(out);
    return ok;
}

// From files.txt when there is one: SYS_DoesFileExist falls back to stat()
// on the SD card for a file not on the disc (a level is looked for in game/
// before levels/), without Octave's lock on the card, which disc reads may
// hold (docs/hardware-bugs.md). Without files.txt, under the lock.
bool exists(const std::string& path) {
    const auto& known = sizes();
    if (!known.empty()) return known.count(path) != 0;
    trace::SdLock lock;
    return SYS_DoesFileExist(path.c_str(), true);
}

}  // namespace files
