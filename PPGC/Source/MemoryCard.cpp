#include "MemoryCard.h"

#include <ogc/lwp_watchdog.h>

#include <cstring>
#include <string>

#include "save/storage.h"

#include "Stream.h"
#include "System/System.h"

// Octave's GameCube card functions (System_Dolphin.cpp) that System.h
// doesn't declare.
const char* SYS_GetSaveCardState(const char* saveName, uint32_t dataBytes, int32_t& blocksNeeded, int32_t& blocksFree);
void SYS_SetSaveInfo(const char* title, const char* description, const uint8_t* icon, uint32_t iconFrames,
                     const uint8_t* bannerCI8);
void OctLog(const char* format, ...);

namespace card {

namespace {

const char* kSaveName = "PPGC";

}  // namespace

void init() {
    // The icon and banner come with the data (tools/make_art.py); without an
    // icon the save is written bare, without a banner it has none.
    char* icon = nullptr;
    char* banner = nullptr;
    uint32_t icon_size = 0, banner_size = 0;
    SYS_AcquireFileData("PPGC/Scripts/Data/save_icon.bin", true, 0, icon, icon_size);
    SYS_AcquireFileData("PPGC/Scripts/Data/save_banner.bin", true, 0, banner, banner_size);
    if (icon && icon_size == 2048) {
        SYS_SetSaveInfo("Painter's Playground", "Game progress", reinterpret_cast<const uint8_t*>(icon), 0,
                        banner && banner_size == 3584 ? reinterpret_cast<const uint8_t*>(banner) : nullptr);
    }
    if (icon) SYS_ReleaseFileData(icon);
    if (banner) SYS_ReleaseFileData(banner);
}

Status query() {
    Status s;
    int32_t needed = 0, free = 0;
    const char* state = SYS_GetSaveCardState(kSaveName, save::Storage::kSize, needed, free);
    s.blocks_needed = needed;
    s.blocks_free = free;
    std::string st = state ? state : "error";
    if (st == "exists") s.state = State::Exists;
    else if (st == "ready") s.state = State::Ready;
    else if (st == "full") s.state = State::Full;
    else if (st == "nocard") s.state = State::NoCard;
    else s.state = State::Unusable;
    OctLog("card: slot A %s, %d blocks needed, %d free", st.c_str(), needed, free);
    return s;
}

bool read(std::vector<uint8_t>& out) {
    Stream stream;
    if (!SYS_ReadSave(kSaveName, stream) || stream.GetSize() == 0) return false;
    const auto* data = reinterpret_cast<const uint8_t*>(stream.GetData());
    out.assign(data, data + stream.GetSize());
    return true;
}

bool write(const std::vector<uint8_t>& data) {
    Stream stream;
    stream.WriteBytes(data.data(), uint32_t(data.size()));
    uint64_t start = ticks_to_microsecs(gettime());
    bool ok = SYS_WriteSave(kSaveName, stream);
    OctLog("card: wrote %u bytes in %u ms: %s", unsigned(data.size()),
           unsigned((ticks_to_microsecs(gettime()) - start) / 1000), ok ? "ok" : "FAILED");
    return ok;
}

}  // namespace card
