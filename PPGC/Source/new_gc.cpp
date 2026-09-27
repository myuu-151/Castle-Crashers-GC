// operator new for the GameCube. Small blocks come from pages of their own
// size; while a memory::Scratch is open, blocks come from the scratch region.
// When main memory has no piece big enough, display lists go to ARAM
// (render::gx_release_memory) until one is, rather than the game ending
// there; only when none are left does it throw.
#include "memory_gc.h"

#include <gccore.h>
#include <malloc.h>
#include <ogc/lwp.h>
#include <ogc/machine/processor.h>

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

// ---- small blocks (up to 256 bytes): pages of 4 KB, each of one size,
// taken from chunks of 64 KB. The game keeps tens of thousands of small
// blocks alive, freed at all times; among bigger ones in the heap they left
// it in pieces too small for anything. An empty page can take any size; an
// empty chunk goes back to the heap (but one is kept).

constexpr uint32_t kPage = 4096, kChunk = 64 * 1024, kPagesPerChunk = kChunk / kPage;
constexpr uintptr_t kMem1 = 0x80000000, kMem1Size = 24 * 1024 * 1024;
constexpr std::size_t kSmall = 256;
constexpr int kClasses = 24;  // 8..128 by 8, 144..256 by 16

struct Page {
    uint16_t cls;
    uint16_t used;
    uint16_t bump;   // slots never used start here
    uint16_t chunk;  // the page number of its chunk's first page
    void* free;
    Page* next;
    Page* prev;
};
constexpr uint32_t kPageHead = (sizeof(Page) + 7) & ~7u;

// By page number (4 KB of MEM1 each). Chunks are aligned only to a page:
// aligning them to their size left a gap behind each one.
constexpr uint32_t kPages = kMem1Size / kPage;
bool g_page_small[kPages];     // pages of small blocks
uint8_t g_chunk_free[kPages];  // at a chunk's first page: its free pages

Page* g_partial[kClasses];  // pages of each size with room
Page* g_free_pages = nullptr;
uint32_t g_free_page_count = 0, g_chunks = 0;

int class_of(std::size_t n) {
    if (n <= 8) return 0;
    if (n <= 128) return int((n + 7) / 8) - 1;
    return 16 + int((n - 128 + 15) / 16) - 1;
}

uint32_t class_size(int c) {
    return c < 16 ? uint32_t(c + 1) * 8 : 128 + uint32_t(c - 15) * 16;
}

uint32_t capacity(int c) {
    return (kPage - kPageHead) / class_size(c);
}

uint32_t page_number(const void* p) {
    return uint32_t((reinterpret_cast<uintptr_t>(p) - kMem1) / kPage);
}

void push(Page*& list, Page* pg) {
    pg->prev = nullptr;
    pg->next = list;
    if (list) list->prev = pg;
    list = pg;
}

void remove(Page*& list, Page* pg) {
    if (pg->prev) pg->prev->next = pg->next;
    else list = pg->next;
    if (pg->next) pg->next->prev = pg->prev;
}

Page* take_page() {
    if (!g_free_pages) {
        auto* chunk = static_cast<uint8_t*>(memalign(kPage, kChunk));
        if (!chunk) return nullptr;
        uint32_t first = page_number(chunk);
        g_chunk_free[first] = kPagesPerChunk;
        for (uint32_t i = 0; i < kPagesPerChunk; i++) {
            auto* pg = reinterpret_cast<Page*>(chunk + i * kPage);
            pg->chunk = uint16_t(first);
            g_page_small[first + i] = true;
            push(g_free_pages, pg);
        }
        g_free_page_count += kPagesPerChunk;
        g_chunks++;
    }
    Page* pg = g_free_pages;
    remove(g_free_pages, pg);
    g_free_page_count--;
    g_chunk_free[pg->chunk]--;
    return pg;
}

void give_page(Page* pg) {
    push(g_free_pages, pg);
    g_free_page_count++;
    uint32_t first = pg->chunk;
    if (++g_chunk_free[first] < kPagesPerChunk || g_free_page_count <= kPagesPerChunk) return;
    // A whole chunk free, and another's worth of pages to spare: back to the heap.
    auto* chunk = reinterpret_cast<uint8_t*>(kMem1 + uintptr_t(first) * kPage);
    for (uint32_t i = 0; i < kPagesPerChunk; i++) {
        remove(g_free_pages, reinterpret_cast<Page*>(chunk + i * kPage));
        g_page_small[first + i] = false;
    }
    g_free_page_count -= kPagesPerChunk;
    g_chunk_free[first] = 0;
    g_chunks--;
    std::free(chunk);
}

void* small_alloc(std::size_t size) {
    int c = class_of(size);
    uint32_t level;
    _CPU_ISR_Disable(level);
    Page* pg = g_partial[c];
    if (!pg) {
        pg = take_page();
        if (!pg) {
            _CPU_ISR_Restore(level);
            return nullptr;
        }
        pg->cls = uint16_t(c);
        pg->used = 0;
        pg->bump = 0;
        pg->free = nullptr;
        push(g_partial[c], pg);
    }
    void* p;
    if (pg->free) {
        p = pg->free;
        pg->free = *static_cast<void**>(p);
    } else {
        p = reinterpret_cast<uint8_t*>(pg) + kPageHead + uint32_t(pg->bump) * class_size(c);
        pg->bump++;
    }
    if (++pg->used == capacity(c)) remove(g_partial[c], pg);
    _CPU_ISR_Restore(level);
    return p;
}

bool small_free(void* p) {
    uintptr_t at = reinterpret_cast<uintptr_t>(p);
    if (at < kMem1 || at >= kMem1 + kMem1Size || !g_page_small[page_number(p)]) return false;
    uint32_t level;
    _CPU_ISR_Disable(level);
    auto* pg = reinterpret_cast<Page*>(at & ~uintptr_t(kPage - 1));
    int c = pg->cls;
    if (pg->used == capacity(c)) push(g_partial[c], pg);
    *static_cast<void**>(p) = pg->free;
    pg->free = p;
    if (--pg->used == 0) {
        remove(g_partial[c], pg);
        give_page(pg);
    }
    _CPU_ISR_Restore(level);
    return true;
}

void release(void* p) noexcept {
    if (scratch_free(p) || small_free(p)) return;
    std::free(p);
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

uint32_t small_kb() {
    return g_chunks * (kChunk / 1024);
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
    if (size <= kSmall)
        if (void* p = small_alloc(size)) return p;
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
