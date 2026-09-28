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
#include <cstring>
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
#ifdef PPGC_POISON
    // Test builds: scratch memory as the console's is, never zero (Dolphin's
    // starts zeroed), so what reads memory it never wrote goes wrong there too.
    std::memset(g_scratch, 0xA5, g_scratch_size);
#endif
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

void log(const char* what, std::size_t size, unsigned lists, void* a, void* b) {
    if (g_logging) return;
    g_logging = true;
    bool on = g_scratch_on;
    g_scratch_on = false;
    const struct mallinfo info = mallinfo();
    SDL_Log("memory: %s a %u-byte block (%u display lists to ARAM, %u KB free in pieces, %u KB in one; for %p %p)",
            what, unsigned(size), lists, unsigned(info.fordblks / 1024), unsigned(memory::largest_free_kb()), a, b);
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

// The lists are changed with interrupts off (other threads allocate too),
// and a chunk is taken from or given back to the heap with them on: malloc
// takes a lock, and a thread that has to wait for it mustn't have them off
// (with them off, it hung the game once a second thread allocated).

// Interrupts off.
void add_chunk(uint8_t* chunk) {
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

// Interrupts off; null if there's no free page.
Page* take_page() {
    Page* pg = g_free_pages;
    if (!pg) return nullptr;
    remove(g_free_pages, pg);
    g_free_page_count--;
    g_chunk_free[pg->chunk]--;
    return pg;
}

// Interrupts off. A chunk now all free (and another's worth of pages to
// spare) leaves the lists: the caller gives it back to the heap.
uint8_t* give_page(Page* pg) {
    push(g_free_pages, pg);
    g_free_page_count++;
    uint32_t first = pg->chunk;
    if (++g_chunk_free[first] < kPagesPerChunk || g_free_page_count <= kPagesPerChunk) return nullptr;
    auto* chunk = reinterpret_cast<uint8_t*>(kMem1 + uintptr_t(first) * kPage);
    for (uint32_t i = 0; i < kPagesPerChunk; i++) {
        remove(g_free_pages, reinterpret_cast<Page*>(chunk + i * kPage));
        g_page_small[first + i] = false;
    }
    g_free_page_count -= kPagesPerChunk;
    g_chunk_free[first] = 0;
    g_chunks--;
    return chunk;
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
            auto* chunk = static_cast<uint8_t*>(memalign(kPage, kChunk));
            if (!chunk) return nullptr;
            _CPU_ISR_Disable(level);
            add_chunk(chunk);
            pg = g_partial[c];  // (another thread may have made one meanwhile)
            if (!pg) pg = take_page();
        }
        if (pg != g_partial[c]) {
            pg->cls = uint16_t(c);
            pg->used = 0;
            pg->bump = 0;
            pg->free = nullptr;
            push(g_partial[c], pg);
        }
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
    uint8_t* chunk = nullptr;
    if (--pg->used == 0) {
        remove(g_partial[c], pg);
        chunk = give_page(pg);
    }
    _CPU_ISR_Restore(level);
    if (chunk) std::free(chunk);
    return true;
}

// ---- the census: what holds the heap, by who asked. Each block from the
// heap has a tag before it: its caller and the one before (two return
// addresses; tools: addr2line on the ELF) and its size. Logged at each movie
// change (memory::census_log), so memory a level leaves behind shows as a
// caller that only grows.

struct Site {
    uint32_t a, b;
    uint32_t bytes, count;
};
constexpr int kSites = 1024;
Site g_sites[kSites];

struct alignas(8) Tag {
    uint16_t site;
    uint16_t pad;
    uint32_t size;
};

uint16_t site_of(uint32_t a, uint32_t b) {
    uint32_t h = (a * 2654435761u ^ b * 40503u) % kSites;
    for (int n = 0; n < kSites; n++, h = (h + 1) % kSites) {
        Site& s = g_sites[h];
        if (s.a == a && s.b == b) return uint16_t(h);
        if (s.a == 0) {
            s.a = a;
            s.b = b;
            return uint16_t(h);
        }
    }
    return 0;  // full: counted with whatever is at 0
}

void* tagged_alloc(std::size_t size, void* a, void* b) {
    auto* t = static_cast<Tag*>(std::malloc(size + sizeof(Tag)));
    if (!t) return nullptr;
    uint32_t level;
    _CPU_ISR_Disable(level);
    uint16_t site = site_of(uint32_t(reinterpret_cast<uintptr_t>(a)), uint32_t(reinterpret_cast<uintptr_t>(b)));
    g_sites[site].bytes += uint32_t(size);
    g_sites[site].count++;
    _CPU_ISR_Restore(level);
    t->site = site;
    t->size = uint32_t(size);
    return t + 1;
}

void tagged_free(void* p) {
    Tag* t = static_cast<Tag*>(p) - 1;
    uint32_t level;
    _CPU_ISR_Disable(level);
    g_sites[t->site].bytes -= t->size;
    g_sites[t->site].count--;
    _CPU_ISR_Restore(level);
    std::free(t);
}

void release(void* p) noexcept {
    if (!p || scratch_free(p) || small_free(p)) return;
    tagged_free(p);
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

void census_log(const char* when) {
    // The 16 callers holding the most, biggest first.
    int top[16];
    int n = 0;
    uint32_t total = 0;
    for (int i = 0; i < kSites; i++) total += g_sites[i].bytes;
    for (int k = 0; k < 16; k++) {
        int best = -1;
        for (int i = 0; i < kSites; i++) {
            bool taken = false;
            for (int j = 0; j < n; j++) taken = taken || top[j] == i;
            if (!taken && g_sites[i].bytes && (best < 0 || g_sites[i].bytes > g_sites[best].bytes)) best = i;
        }
        if (best < 0) break;
        top[n++] = best;
    }
    SDL_Log("census %s: %u KB in blocks from the heap", when, unsigned(total / 1024));
    for (int j = 0; j < n; j++) {
        const Site& s = g_sites[top[j]];
        SDL_Log("census   %6u KB %5u blocks  %08x %08x", unsigned(s.bytes / 1024), unsigned(s.count), unsigned(s.a),
                unsigned(s.b));
    }
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
    void* a = __builtin_return_address(0);
    void* b = __builtin_return_address(1);
    for (;;) {
        if (void* p = tagged_alloc(size, a, b)) {
            if (lists) log("made room for", size, lists, a, b);
            return p;
        }
        if (!render::gx_release_memory()) break;
        lists++;
    }
    log("no room for", size, lists, a, b);
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
