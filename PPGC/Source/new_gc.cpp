// operator new for the GameCube. When main memory has no piece big enough,
// display lists go to ARAM (render::gx_release_memory) until one is, rather
// than the game ending there; only when none are left does it throw. And
// while a memory::Scratch is open, it takes from the scratch region.
#include "memory_gc.h"

#include <gccore.h>
#include <malloc.h>
#include <ogc/lwp.h>

#include <cstdlib>
#include <new>

#include <SDL3/SDL_log.h>

namespace render {
bool gx_release_memory();  // renderer_gx.cpp
}

namespace {

// ---- scratch: first fit over a list of free blocks, each block after a
// header with its size and the size of the one before (so a freed block
// joins free neighbours). Made one free block again when all are freed.

struct alignas(16) Block {
    uint32_t size;       // with the header; a multiple of 16
    uint32_t prev_size;  // of the block before, 0 for the first
    uint32_t used;
    uint32_t pad;
};

struct Links {  // in a free block, after its header
    Block* prev;
    Block* next;
};

uint8_t* g_scratch = nullptr;
uint32_t g_scratch_size = 0, g_scratch_live = 0, g_scratch_overflows = 0;
Block* g_free = nullptr;
bool g_scratch_on = false;
lwp_t g_scratch_thread = LWP_THREAD_NULL;

Links* links(Block* b) {
    return reinterpret_cast<Links*>(b + 1);
}

Block* after(Block* b) {
    uint8_t* n = reinterpret_cast<uint8_t*>(b) + b->size;
    return n < g_scratch + g_scratch_size ? reinterpret_cast<Block*>(n) : nullptr;
}

Block* before(Block* b) {
    return b->prev_size ? reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(b) - b->prev_size) : nullptr;
}

void link(Block* b) {
    links(b)->prev = nullptr;
    links(b)->next = g_free;
    if (g_free) links(g_free)->prev = b;
    g_free = b;
}

void unlink(Block* b) {
    Links* l = links(b);
    if (l->prev) links(l->prev)->next = l->next;
    else g_free = l->next;
    if (l->next) links(l->next)->prev = l->prev;
}

void scratch_reset() {
    auto* b = reinterpret_cast<Block*>(g_scratch);
    b->size = g_scratch_size;
    b->prev_size = 0;
    b->used = 0;
    g_free = nullptr;
    link(b);
}

void* scratch_alloc(std::size_t size) {
    if (!g_scratch_on || LWP_GetSelf() != g_scratch_thread) return nullptr;
    uint32_t need = uint32_t((sizeof(Block) + size + 15) & ~std::size_t(15));
    if (need < sizeof(Block) + sizeof(Links)) need = 32;
    Block* b = g_free;
    while (b && b->size < need) b = links(b)->next;
    if (!b) {
        g_scratch_overflows++;
        return nullptr;
    }
    unlink(b);
    if (b->size - need >= 32) {  // the rest stays free
        auto* rest = reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(b) + need);
        rest->size = b->size - need;
        rest->prev_size = need;
        rest->used = 0;
        if (Block* n = after(rest)) n->prev_size = rest->size;
        b->size = need;
        link(rest);
    }
    b->used = 1;
    g_scratch_live++;
    return b + 1;
}

bool scratch_free(void* p) {
    auto* at = static_cast<uint8_t*>(p);
    if (!g_scratch || at < g_scratch || at >= g_scratch + g_scratch_size) return false;
    Block* b = static_cast<Block*>(p) - 1;
    b->used = 0;
    if (--g_scratch_live == 0) {
        scratch_reset();
        return true;
    }
    if (Block* n = after(b); n && !n->used) {
        unlink(n);
        b->size += n->size;
    }
    if (Block* v = before(b); v && !v->used) {
        unlink(v);
        v->size += b->size;
        b = v;
    }
    if (Block* n = after(b)) n->prev_size = b->size;
    link(b);
    return true;
}

// Logging may need memory itself (and not from scratch: a log may keep it).
bool g_logging = false;

void log(const char* what, std::size_t size, unsigned lists) {
    if (g_logging) return;
    g_logging = true;
    bool on = g_scratch_on;
    g_scratch_on = false;
    const struct mallinfo info = mallinfo();
    SDL_Log("memory: %s a %u-byte block (%u display lists to ARAM, %u KB free in pieces, %u KB in one)", what,
            unsigned(size), lists, unsigned(info.fordblks / 1024), unsigned(memory::largest_free_kb()));
    g_scratch_on = on;
    g_logging = false;
}

void release(void* p) noexcept {
    if (!scratch_free(p)) std::free(p);
}

}  // namespace

namespace memory {

void scratch_init(std::size_t bytes) {
    if (g_scratch) return;
    bytes &= ~std::size_t(15);
    g_scratch = static_cast<uint8_t*>(memalign(32, bytes));
    if (!g_scratch) return;
    g_scratch_size = uint32_t(bytes);
    g_scratch_thread = LWP_GetSelf();
    scratch_reset();
}

Scratch::Scratch(bool on) : was_(g_scratch_on) {
    if (on && g_scratch) g_scratch_on = true;
}

Scratch::~Scratch() {
    g_scratch_on = was_;
}

uint32_t scratch_overflows() {
    return g_scratch_overflows;
}

uint32_t largest_free_kb() {
    // The biggest size malloc gives, to 4 KB.
    // Through a volatile pointer: the compiler would take a malloc freed at
    // once for one that always works, and leave both out.
    void* (*volatile get)(std::size_t) = std::malloc;
    uint32_t lo = 0, hi = 24 * 1024 / 4;
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (void* p = get(std::size_t(mid) * 4096)) {
            std::free(p);
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    return lo * 4;
}

}  // namespace memory

void* operator new(std::size_t size) {
    if (size == 0) size = 1;
    if (void* p = scratch_alloc(size)) return p;
    unsigned lists = 0;
    for (;;) {
        if (void* p = std::malloc(size)) {
            if (lists) log("made room for", size, lists);
            return p;
        }
        if (!render::gx_release_memory()) break;
        lists++;
    }
    log("no room for", size, lists);
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* p) noexcept {
    release(p);
}

void operator delete[](void* p) noexcept {
    release(p);
}

void operator delete(void* p, std::size_t) noexcept {
    release(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    release(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    release(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    release(p);
}
