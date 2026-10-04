#include "MemoryCard.h"

#include <ogc/lwp_watchdog.h>

#include <cstdio>
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
#include "trace_gc.h"

namespace card {

namespace {

// The save's file name: what Swiss's file list shows (the console's memory
// card screen shows the title and comment instead).
const char* kSaveName = "Castle Crashers";
// Its name before (the project's first name, PPGC): a save still under it is
// moved to kSaveName the first time the card is looked at, so progress carries
// on; the old file goes only once the new one is written.
const char* kOldSaveName = "PPGC";
// The data written: the save padded with zeros to 8 KB, which with the
// comment, banner and icon in front makes the file two blocks. (A test: Swiss
// showed no banner for the one-block file, but does for Sonic Pipe Dream's.)
const uint32_t kDataBytes = 8192;

bool write_padded(const char* name, const uint8_t* data, uint32_t size) {
    std::vector<uint8_t> padded(data, data + size);
    if (padded.size() < kDataBytes) padded.resize(kDataBytes, 0);
    Stream stream;
    stream.WriteBytes(padded.data(), uint32_t(padded.size()));
    return SYS_WriteSave(name, stream);
}

// What a save holds, on one line for the log: its progress bytes (header
// 0x08-0x1c) and each played character's level and level unlocks (records of
// 0x30 bytes from 0x40: +1 level - 1, +12 three bytes of unlocks). To see
// whether progress reaches the card and comes back from it.
void log_save(const char* what, const std::vector<uint8_t>& d) {
    char line[512];
    int n = snprintf(line, sizeof(line), "card: %s %u bytes; header", what, unsigned(d.size()));
    for (size_t i = 0x08; i < 0x1d && i < d.size() && n < 400; i++) n += snprintf(line + n, sizeof(line) - size_t(n), " %02x", d[i]);
    for (uint32_t c = 0; c < 30 && n < 480; c++) {
        const size_t r = 0x40 + c * 0x30;
        if (r + 15 > d.size()) break;
        if (d[r + 1] == 0 && d[r + 2] == 0 && d[r + 3] == 0 && d[r + 4] == 0 && d[r + 5] == 0 && !d[r + 12]) continue;
        n += snprintf(line + n, sizeof(line) - size_t(n), "; char %u level %u unlocks %02x%02x%02x", unsigned(c + 1),
                      unsigned(d[r + 1] + 1), d[r + 12], d[r + 13], d[r + 14]);
    }
    PpgcLog("%s", line);
}

}  // namespace

void init() {
    // The icon and banner come with the data (tools/make_art.py); without an
    // icon the save is written bare, without a banner it has none.
    char* icon = nullptr;
    char* banner = nullptr;
    uint32_t icon_size = 0, banner_size = 0;
    SYS_AcquireFileData("CCGC/Scripts/Data/save_icon.bin", true, 0, icon, icon_size);
    SYS_AcquireFileData("CCGC/Scripts/Data/save_banner.bin", true, 0, banner, banner_size);
    // The icon is told by its size: one still RGB5A3 picture (2048 bytes), or 1 to 8 CI8 frames and their
    // palette (n x 1024 + 512), which the card's menu plays in a loop.
    uint32_t icon_frames = 9;
    if (icon_size == 2048) icon_frames = 0;
    else if (icon_size > 512 && (icon_size - 512) % 1024 == 0) icon_frames = (icon_size - 512) / 1024;
    if (icon && icon_frames <= 8) {
        SYS_SetSaveInfo("Castle Crashers", "Game progress", reinterpret_cast<const uint8_t*>(icon), icon_frames,
                        banner && banner_size == 3584 ? reinterpret_cast<const uint8_t*>(banner) : nullptr);
    }
    if (icon) SYS_ReleaseFileData(icon);
    if (banner) SYS_ReleaseFileData(banner);
}

void move_old_save() {
    static bool done = false;
    if (done) return;
    int32_t needed = 0, free = 0;
    const char* now = SYS_GetSaveCardState(kSaveName, kDataBytes, needed, free);
    const std::string state = now ? now : "";
    if (state == "exists") {
        done = true;
        return;
    }
    if (state != "ready" && state != "full") return;  // (no card yet: asked again next time)
    const char* old = SYS_GetSaveCardState(kOldSaveName, kDataBytes, needed, free);
    if (!old || std::string(old) != "exists") {
        done = true;
        return;
    }
    Stream stream;
    if (!SYS_ReadSave(kOldSaveName, stream) || stream.GetSize() < save::Storage::kSize) {
        PpgcLog("card: the save under its old name (%s) couldn't be read; left as it is", kOldSaveName);
        done = true;
        return;
    }
    const bool written = write_padded(kSaveName, reinterpret_cast<const uint8_t*>(stream.GetData()),
                                      save::Storage::kSize);
    const bool deleted = written && SYS_DeleteSave(kOldSaveName);
    PpgcLog("card: the save moved from \"%s\" to \"%s\": %s", kOldSaveName, kSaveName,
            !written ? "FAILED (the old one kept)" : deleted ? "ok" : "ok (the old one not deleted)");
    done = true;
}

Status query() {
    move_old_save();
    Status s;
    int32_t needed = 0, free = 0;
    const char* state = SYS_GetSaveCardState(kSaveName, kDataBytes, needed, free);
    s.blocks_needed = needed;
    s.blocks_free = free;
    std::string st = state ? state : "error";
    if (st == "exists") s.state = State::Exists;
    else if (st == "ready") s.state = State::Ready;
    else if (st == "full") s.state = State::Full;
    else if (st == "nocard") s.state = State::NoCard;
    else s.state = State::Unusable;
    PpgcLog("card: slot A %s, %d blocks needed, %d free", st.c_str(), needed, free);
    return s;
}

bool read(std::vector<uint8_t>& out) {
    Stream stream;
    if (!SYS_ReadSave(kSaveName, stream) || stream.GetSize() == 0) return false;
    const auto* data = reinterpret_cast<const uint8_t*>(stream.GetData());
    out.assign(data, data + stream.GetSize());
    log_save("read", out);
    if (out.size() > save::Storage::kSize) out.resize(save::Storage::kSize);  // (the padding)
    return true;
}

bool write(const std::vector<uint8_t>& data) {
    uint64_t start = ticks_to_microsecs(gettime());
    log_save("writing", data);
    bool ok = write_padded(kSaveName, data.data(), uint32_t(data.size()));
    PpgcLog("card: wrote %u bytes in %u ms: %s", unsigned(data.size()),
           unsigned((ticks_to_microsecs(gettime()) - start) / 1000), ok ? "ok" : "FAILED");
    return ok;
}

}  // namespace card
