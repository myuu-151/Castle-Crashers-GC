#include "aram_gc.h"

#include <gccore.h>
#include <ogc/aram.h>
#include <ogc/machine/processor.h>

#include <algorithm>

namespace aram {

namespace {

constexpr uint32_t kPiece = 8 * 1024;
uint32_t g_base = 0, g_top = 0;

void dma(uint32_t dir, void* mem, uint32_t at, uint32_t len) {
    uint8_t* m = static_cast<uint8_t*>(mem);
    if (dir == AR_MRAMTOARAM) DCFlushRange(m, len);
    else DCInvalidateRange(m, len);
    for (uint32_t done = 0; done < len; done += kPiece) {
        uint32_t piece = std::min(kPiece, len - done);
        uint32_t level;
        _CPU_ISR_Disable(level);
        AR_StartDMA(dir, uint32_t(MEM_VIRTUAL_TO_PHYSICAL(m + done)), at + done, piece);
        while (AR_GetDMAStatus()) {
        }
        _CPU_ISR_Restore(level);
    }
}

}  // namespace

bool init() {
    if (g_top) return true;
    g_base = AR_Init(nullptr, 0);  // as Octave does (the OS keeps the first 16 KB)
    g_base = (g_base + 31) & ~31u;
    g_top = AR_GetSize();
    return g_top > g_base + kShapeCache;
}

uint32_t base() {
    return g_base;
}

uint32_t top() {
    return g_top;
}

void to_aram(const void* mem, uint32_t at, uint32_t len) {
    dma(AR_MRAMTOARAM, const_cast<void*>(mem), at, len);
}

void from_aram(void* mem, uint32_t at, uint32_t len) {
    dma(AR_ARAMTOMRAM, mem, at, len);
}

}  // namespace aram
