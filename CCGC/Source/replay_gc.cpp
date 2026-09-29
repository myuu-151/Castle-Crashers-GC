#include "replay_gc.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

#include "System/System.h"
#include "player/game.h"
#include "player/player.h"
#include "trace_gc.h"

namespace replay {

namespace {

const char* const kPath = "CCGC/Scripts/Data/replay.bin";
constexpr uint32_t kRecordSize = 68;
constexpr uint32_t kBatch = 64;  // records read at a time

enum Kind : uint8_t { kPad = 1, kKeys = 2, kFocus = 3, kSuspend = 4, kFocusSuspend = 5, kMouse = 6 };

struct Change {
    int32_t total = 0, frame = 0, occurrence = 0;
    uint8_t kind = 0, pad = 0;
    input::PadReading reading;
    int8_t value = 0;
    uint32_t mouse = 0;
    float mouse_x = 0, mouse_y = 0;
    uint8_t keys[32] = {};
};

bool g_open = false;
uint32_t g_count = 0, g_next = 0, g_fast_forward = 0, g_records_at = 0, g_updates = 0;
std::vector<uint8_t> g_storage;
std::string g_tag;
std::map<std::pair<int, int>, int> g_seen;

// The records g_batch_first.. in memory.
Change g_batch[kBatch];
uint32_t g_batch_first = 0, g_batch_count = 0;

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

bool read(uint32_t offset, uint32_t size, void* out) {
    return SYS_ReadFileRange(kPath, true, offset, size, static_cast<char*>(out));  // (under Octave's SD lock)
}

// Change `index`, reading its batch when needed; null past the end.
const Change* change(uint32_t index) {
    if (index >= g_count) return nullptr;
    if (index < g_batch_first || index >= g_batch_first + g_batch_count) {
        g_batch_first = index;
        g_batch_count = std::min(kBatch, g_count - index);
        static uint8_t raw[kBatch * kRecordSize] __attribute__((aligned(32)));
        if (!read(g_records_at + index * kRecordSize, g_batch_count * kRecordSize, raw)) {
            PpgcLog("replay: couldn't read changes %u-%u; stopping", unsigned(index), unsigned(index + g_batch_count));
            g_count = index;
            g_batch_count = 0;
            return nullptr;
        }
        for (uint32_t i = 0; i < g_batch_count; i++) {
            const uint8_t* r = raw + i * kRecordSize;
            Change& c = g_batch[i];
            c.total = int32_t(be32(r));
            c.frame = int32_t(be32(r + 4));
            c.occurrence = int32_t(be32(r + 8));
            c.kind = r[12];
            c.pad = r[13];
            c.reading.buttons = be16(r + 14);
            c.reading.left_trigger = r[16];
            c.reading.right_trigger = r[17];
            c.reading.thumb_lx = int16_t(be16(r + 18));
            c.reading.thumb_ly = int16_t(be16(r + 20));
            c.reading.connected = r[22] != 0;
            c.value = int8_t(r[23]);
            c.mouse = be32(r + 24);
            uint32_t xb = be32(r + 28), yb = be32(r + 32);
            std::memcpy(&c.mouse_x, &xb, 4);
            std::memcpy(&c.mouse_y, &yb, 4);
            std::memcpy(c.keys, r + 36, 32);
        }
    }
    return &g_batch[index - g_batch_first];
}

// Every change tied to this update (total 0: before any), in order.
void apply(player::Game& game, int total, int frame, int occurrence) {
    while (const Change* c = change(g_next)) {
        if (c->total != 0 && !(c->total == total && c->frame == frame && c->occurrence == occurrence)) break;
        switch (c->kind) {
        case kPad:
            if (c->pad < 4) game.input.pads[c->pad] = c->reading;
            break;
        case kKeys:
            for (int vk = 0; vk < 256; vk++) game.input.keyboard.keys[size_t(vk)] = (c->keys[vk / 8] >> (vk % 8)) & 1;
            break;
        case kFocus: game.window_active = c->value != 0; break;
        case kSuspend:
        case kFocusSuspend: game.suspend_request = c->value; break;
        case kMouse: game.mouse_event(int(c->mouse), c->mouse_x, c->mouse_y); break;
        default: break;
        }
        g_next++;
    }
    if (g_next >= g_count && g_count) {
        PpgcLog("replay: all %u changes applied at update %u; the pads are yours", unsigned(g_count),
                unsigned(g_updates));
        g_count = 0;  // (done: inactive)
    }
}

}  // namespace

bool open() {
    uint8_t head[16] __attribute__((aligned(32)));
    if (!read(0, sizeof(head), head) || std::memcmp(head, "CCRP", 4) != 0 || be32(head + 4) != 1) {
        PpgcLog("replay: no %s", kPath);
        return false;
    }
    uint32_t count = be32(head + 8);
    g_fast_forward = be32(head + 12);
    uint8_t n[4] __attribute__((aligned(32)));
    uint32_t at = 16;
    if (!read(at, 4, n)) return false;
    g_storage.resize(be32(n));
    if (!g_storage.empty() && !read(at + 4, uint32_t(g_storage.size()), g_storage.data())) return false;
    at += 4 + uint32_t(g_storage.size());
    if (!read(at, 4, n)) return false;
    g_tag.resize(be32(n));
    if (!g_tag.empty() && !read(at + 4, uint32_t(g_tag.size()), g_tag.data())) return false;
    g_records_at = at + 4 + uint32_t(g_tag.size());
    g_count = count;
    g_open = true;
    PpgcLog("replay: %u changes, a %u-byte save, fast forward to update %u", unsigned(count),
            unsigned(g_storage.size()), unsigned(g_fast_forward));
    return true;
}

bool active() { return g_open && g_next < g_count; }
uint32_t fast_forward() { return g_fast_forward; }
uint32_t updates() { return g_updates; }
const std::vector<uint8_t>& storage() { return g_storage; }
const std::string& gamer_tag() { return g_tag; }

void start(player::Game& game) { apply(game, 0, 0, 0); }

void on_update(player::Game& game, player::Player& p) {
    g_updates++;
    if (g_updates % 3000 == 0 && active())
        PpgcLog("replay: update %u, change %u of %u, %s", unsigned(g_updates), unsigned(g_next), unsigned(g_count),
                p.name().c_str());
    if (!active()) return;
    if (p.name() == "loading" || p.name() == "pause") return;
    int total = p.root()->total_frames, frame = p.root()->frame;
    apply(game, total, frame, ++g_seen[{total, frame}]);
}

}  // namespace replay
