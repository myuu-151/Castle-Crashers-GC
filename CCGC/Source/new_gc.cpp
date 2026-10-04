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

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include <SDL3/SDL_log.h>

namespace render {
bool gx_release_memory();  // renderer_gx.cpp
}

// newlib's malloc: where its heap began, and its bins (the heap map walks them).
extern "C" char* __malloc_sbrk_base;
extern "C" void* __malloc_av_[];

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

// Pages of each size with room, in two lists: [1] half full or more, [0]
// less. New blocks go to the fuller pages first, so the sparse ones empty and
// go back. Taking whichever page came back last spread each size's blocks
// thinly over many pages, none of them ever empty: over ten levels the small
// pages grew by 1.5 MB for 0.5 MB more of blocks.
Page* g_partial[kClasses][2];
// Free pages by chunk (at its first page), and the chunks: a new page comes
// from the chunk with the fewest free, so the emptiest chunks empty and go
// back. Taking whichever page came back last spread the pages in use over
// every chunk, none of them all free again.
constexpr uint32_t kMaxChunks = kMem1Size / kChunk;
Page* g_chunk_pages[kPages];
uint16_t g_chunk_ids[kMaxChunks];
uint32_t g_chunk_count = 0;
uint32_t g_small_live = 0;  // bytes of the slots in use
uint32_t g_free_page_count = 0, g_chunks = 0;

// ---- where chunks come from: the top of main memory, going down. The heap
// grows up from the bottom (libogc's sbrk moves the arena's low end up to its
// high end, checking the high end each time), so a chunk taken by moving the
// high end down sits with the others at the top, out of the heap's way. From
// the heap, chunks landed wherever there was room, and, never all free again,
// cut its big free pieces up: over a session of ten levels the small pages
// grew from 2.6 to 3.6 MB and the heap's biggest piece shrank from 2.5 MB to
// 620 KB (level 20 then ran out of memory for 1 MB with 2.5 MB free). A chunk
// from the top that empties is kept for the next (the heap can't have it).
// When the top has no room left (the heap has grown up to it), chunks come
// from the heap as before.
bool g_chunk_top[kPages];      // at a chunk's first page: taken from the top
uint8_t* g_top_free = nullptr;  // chunks from the top, empty: a list through their first word
uint32_t g_top_chunks = 0;      // taken from the top, in use or not

// Interrupts off (as libogc's sbrk); null if the top has no room.
uint8_t* top_chunk() {
    if (uint8_t* p = g_top_free) {
        g_top_free = *reinterpret_cast<uint8_t**>(p);
        return p;
    }
    uintptr_t hi = reinterpret_cast<uintptr_t>(SYS_GetArena1Hi()), lo = reinterpret_cast<uintptr_t>(SYS_GetArena1Lo());
    uintptr_t at = (hi - kChunk) & ~uintptr_t(kPage - 1);
    if (hi < kMem1 + kChunk || at < lo) return nullptr;
    SYS_SetArena1Hi(reinterpret_cast<void*>(at));
    g_chunk_top[(at - kMem1) / kPage] = true;  // (its page number)
    g_top_chunks++;
    return reinterpret_cast<uint8_t*>(at);
}

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

int band(int c, uint32_t used) {
    return used * 2 >= capacity(c) ? 1 : 0;
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
        push(g_chunk_pages[first], pg);
    }
    g_chunk_ids[g_chunk_count++] = uint16_t(first);
    g_free_page_count += kPagesPerChunk;
    g_chunks++;
}

// Interrupts off; null if there's no free page.
Page* take_page() {
    uint32_t best = kPages;
    for (uint32_t i = 0; i < g_chunk_count; i++) {
        uint32_t id = g_chunk_ids[i];
        if (g_chunk_free[id] && (best == kPages || g_chunk_free[id] < g_chunk_free[best])) best = id;
    }
    if (best == kPages) return nullptr;
    Page* pg = g_chunk_pages[best];
    remove(g_chunk_pages[best], pg);
    g_free_page_count--;
    g_chunk_free[pg->chunk]--;
    return pg;
}

// Interrupts off. A chunk now all free (and another's worth of pages to
// spare) leaves the lists: the caller gives it back to the heap.
uint8_t* give_page(Page* pg) {
    push(g_chunk_pages[pg->chunk], pg);
    g_free_page_count++;
    uint32_t first = pg->chunk;
    if (++g_chunk_free[first] < kPagesPerChunk || g_free_page_count <= kPagesPerChunk) return nullptr;
    auto* chunk = reinterpret_cast<uint8_t*>(kMem1 + uintptr_t(first) * kPage);
    for (uint32_t i = 0; i < kPagesPerChunk; i++) g_page_small[first + i] = false;
    g_chunk_pages[first] = nullptr;
    for (uint32_t i = 0; i < g_chunk_count; i++)
        if (g_chunk_ids[i] == first) {
            g_chunk_ids[i] = g_chunk_ids[--g_chunk_count];
            break;
        }
    g_free_page_count -= kPagesPerChunk;
    g_chunk_free[first] = 0;
    g_chunks--;
    if (g_chunk_top[first]) {  // kept for the next chunk (the heap can't have it)
        *reinterpret_cast<uint8_t**>(chunk) = g_top_free;
        g_top_free = chunk;
        return nullptr;
    }
    return chunk;
}

void* small_alloc(std::size_t size) {
    int c = class_of(size);
    uint32_t level;
    _CPU_ISR_Disable(level);
    Page* pg = g_partial[c][1] ? g_partial[c][1] : g_partial[c][0];
    if (!pg) {
        pg = take_page();
        if (!pg) {
            uint8_t* chunk = top_chunk();
            if (!chunk) {
                _CPU_ISR_Restore(level);
                chunk = static_cast<uint8_t*>(memalign(kPage, kChunk));
                if (!chunk) return nullptr;
                _CPU_ISR_Disable(level);
            }
            add_chunk(chunk);
            // (another thread may have made one meanwhile)
            pg = g_partial[c][1] ? g_partial[c][1] : g_partial[c][0];
            if (!pg) pg = take_page();
        }
        if (pg != g_partial[c][0] && pg != g_partial[c][1]) {
            pg->cls = uint16_t(c);
            pg->used = 0;
            pg->bump = 0;
            pg->free = nullptr;
            push(g_partial[c][0], pg);
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
    g_small_live += class_size(c);
    int was = band(c, pg->used);
    if (++pg->used == capacity(c)) {
        remove(g_partial[c][was], pg);
    } else if (band(c, pg->used) != was) {
        remove(g_partial[c][was], pg);
        push(g_partial[c][1], pg);
    }
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
    bool was_full = pg->used == capacity(c);
    int was = band(c, pg->used);
    g_small_live -= class_size(c);
    *static_cast<void**>(p) = pg->free;
    pg->free = p;
    uint8_t* chunk = nullptr;
    if (--pg->used == 0) {
        if (!was_full) remove(g_partial[c][was], pg);
        chunk = give_page(pg);
    } else if (was_full) {
        push(g_partial[c][band(c, pg->used)], pg);
    } else if (band(c, pg->used) != was) {
        remove(g_partial[c][was], pg);
        push(g_partial[c][0], pg);
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
    uint32_t small_bytes, small_count;  // small blocks (diagnostic builds tag them too)
    uint32_t base;                      // bytes and small bytes at the first level's census
};
constexpr int kSites = 2048;
bool g_based = false;
Site g_sites[kSites];

// Live heap blocks by size (for the census): up to 1, 4, 16, 64, 256 KB, more.
constexpr int kBuckets = 6;
uint32_t g_bucket_bytes[kBuckets], g_bucket_count[kBuckets];
int bucket_of(uint32_t size) {
    int b = 0;
    for (uint32_t limit = 1024; b < kBuckets - 1 && size > limit; limit *= 4) b++;
    return b;
}

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
    g_bucket_bytes[bucket_of(uint32_t(size))] += uint32_t(size);
    g_bucket_count[bucket_of(uint32_t(size))]++;
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
    g_bucket_bytes[bucket_of(t->size)] -= t->size;
    g_bucket_count[bucket_of(t->size)]--;
    _CPU_ISR_Restore(level);
    std::free(t);
}

// ---- the heap map (diagnostic builds): newlib's chunks walked in address
// order, from its first to its top chunk (each is its size, low bit "the one
// before is in use", then its data). Logged with the census: the heap in
// 64 KB cells ('.' free, '#' used, 's' pages of small blocks), its biggest
// free pieces, and what lies between them, by size and, for a block from
// operator new, by caller.
void heap_map_log() {
    uint8_t* base = reinterpret_cast<uint8_t*>(__malloc_sbrk_base);
    uint8_t* top = static_cast<uint8_t*>(__malloc_av_[2]);  // (bin 0's fd: the top chunk)
    if (!base || base == reinterpret_cast<uint8_t*>(-1) || !top || top < base) {
        SDL_Log("heap map: no heap to walk");
        return;
    }
    // The first chunk: its data 8-aligned.
    uint8_t* p = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(base) + 8 + 7) / 8 * 8 - 8);
    constexpr uint32_t kCell = 64 * 1024;
    const uint32_t first_cell = uint32_t((reinterpret_cast<uintptr_t>(p) - kMem1) / kCell);
    const uint32_t cells = uint32_t((reinterpret_cast<uintptr_t>(top) - kMem1) / kCell) + 1 - first_cell;
    static uint32_t used_in[384], free_in[384], small_in[384];
    for (uint32_t i = 0; i < 384; i++) used_in[i] = free_in[i] = small_in[i] = 0;
    struct Piece {
        uint8_t* at;
        uint32_t size;
    };
    Piece big[6] = {};
    uint32_t walked = 0;
    while (p < top) {
        const uint32_t size = *reinterpret_cast<uint32_t*>(p + 4) & ~3u;
        if (size < 16 || p + size > top) {
            SDL_Log("heap map: lost at %p (size %u) after %u chunks", static_cast<void*>(p), unsigned(size),
                    unsigned(walked));
            return;
        }
        uint8_t* next = p + size;
        const bool used = next >= top ? true : (*reinterpret_cast<uint32_t*>(next + 4) & 1u);
        const bool small = used && g_page_small[page_number(p + 8)];
        for (uint32_t off = 0; off < size;) {  // its bytes into the cells
            uint32_t cell = uint32_t((reinterpret_cast<uintptr_t>(p) + off - kMem1) / kCell);
            uint32_t cell_end = (cell + 1) * kCell;
            uint32_t here = std::min<uint32_t>(size - off, uint32_t(kMem1 + cell_end - (reinterpret_cast<uintptr_t>(p) + off)));
            if (cell >= first_cell && cell - first_cell < 384)
                (small ? small_in : used ? used_in : free_in)[cell - first_cell] += here;
            off += here;
        }
        if (!used) {
            for (int i = 0; i < 6; i++) {
                if (size > big[i].size) {
                    for (int j = 5; j > i; j--) big[j] = big[j - 1];
                    big[i] = {p, size};
                    break;
                }
            }
        }
        walked++;
        p = next;
    }
    char row[97];
    SDL_Log("heap map: %u chunks, %p..%p ('.' free, '#' used, 's' small pages; a cell 64 KB):", unsigned(walked),
            static_cast<void*>(base), static_cast<void*>(top));
    for (uint32_t c = 0; c < cells && c < 384; c += 96) {
        uint32_t n = 0;
        for (; n < 96 && c + n < cells && c + n < 384; n++) {
            uint32_t u = used_in[c + n], f = free_in[c + n], s = small_in[c + n];
            row[n] = s >= u && s >= f && s ? 's' : f > u ? '.' : '#';
        }
        row[n] = 0;
        SDL_Log("heap map %08x %s", unsigned(kMem1 + (first_cell + c) * kCell), row);
    }
    // What lies between the biggest free pieces, in address order.
    for (int i = 0; i < 6 && big[i].size; i++)
        SDL_Log("heap map: free %u KB at %p", unsigned(big[i].size / 1024), static_cast<void*>(big[i].at));
    for (int i = 0; i < 6; i++)
        for (int j = i + 1; j < 6; j++)
            if (big[j].size && big[j].at < big[i].at) std::swap(big[i], big[j]);
    for (int i = 0; i + 1 < 6 && big[i + 1].size; i++) {
        if (!big[i].size) continue;
        uint8_t* q = big[i].at + big[i].size;
        uint8_t* end = big[i + 1].at;
        uint32_t between = uint32_t(end - q), shown = 0;
        SDL_Log("heap map: between %p and %p, %u KB:", static_cast<void*>(big[i].at), static_cast<void*>(end),
                unsigned(between / 1024));
        while (q < end && shown < 10) {
            const uint32_t size = *reinterpret_cast<uint32_t*>(q + 4) & ~3u;
            if (size < 16) break;
            uint8_t* next = q + size;
            const bool used = next >= top ? true : (*reinterpret_cast<uint32_t*>(next + 4) & 1u);
            if (used) {
                const Tag* t = reinterpret_cast<const Tag*>(q + 8);
                const bool tagged = t->site < kSites && g_sites[t->site].a && t->size + sizeof(Tag) <= size;
                if (g_page_small[page_number(q + 8)])
                    SDL_Log("heap map:   %6u bytes: pages of small blocks", unsigned(size));
                else if (tagged)
                    SDL_Log("heap map:   %6u bytes: new, for %08x %08x", unsigned(size), unsigned(g_sites[t->site].a),
                            unsigned(g_sites[t->site].b));
                else
                    SDL_Log("heap map:   %6u bytes: malloc/memalign", unsigned(size));
                shown++;
            }
            q = next;
        }
    }
}

#ifdef PPGC_DIAG
// Diagnostic builds: small blocks carry a tag too (8 bytes before them), so the
// census counts them by caller.
bool is_small(const void* p) {
    uintptr_t at = reinterpret_cast<uintptr_t>(p);
    return at >= kMem1 && at < kMem1 + kMem1Size && g_page_small[page_number(p)];
}

void* small_tagged_alloc(std::size_t size, void* a, void* b) {
    auto* t = static_cast<Tag*>(small_alloc(size + sizeof(Tag)));
    if (!t) return nullptr;
    uint32_t level;
    _CPU_ISR_Disable(level);
    uint16_t site = site_of(uint32_t(reinterpret_cast<uintptr_t>(a)), uint32_t(reinterpret_cast<uintptr_t>(b)));
    g_sites[site].small_bytes += uint32_t(size);
    g_sites[site].small_count++;
    _CPU_ISR_Restore(level);
    t->site = site;
    t->size = uint32_t(size);
    return t + 1;
}
#endif

// ---- regions (memory::Region): a RegionScope routes operator new's big
// blocks on its thread into its region.
constexpr std::size_t kRouteMin = 64 * 1024;
memory::Region* g_route = nullptr;
lwp_t g_route_thread = LWP_THREAD_NULL;

void* region_alloc(std::size_t size) {
    if (!g_route || size < kRouteMin || LWP_GetSelf() != g_route_thread) return nullptr;
    return g_route->alloc(size);
}

void release(void* p) noexcept {
    if (!p || scratch_free(p) || memory::level_region().free(p)) return;
#ifdef PPGC_DIAG
    if (is_small(p)) {
        Tag* t = static_cast<Tag*>(p) - 1;
        uint32_t level;
        _CPU_ISR_Disable(level);
        g_sites[t->site].small_bytes -= t->size;
        g_sites[t->site].small_count--;
        _CPU_ISR_Restore(level);
        small_free(t);
        return;
    }
#else
    if (small_free(p)) return;
#endif
    tagged_free(p);
}

}  // namespace

namespace memory {

bool Region::init(std::size_t bytes) {
    if (base_) return true;
    bytes = (bytes + 31) & ~std::size_t(31);
    base_ = static_cast<uint8_t*>(memalign(32, bytes));
    size_ = base_ ? uint32_t(bytes) : 0;
    return base_ != nullptr;
}

void* Region::alloc(std::size_t size) {
    if (!base_ || size == 0 || size > size_) return nullptr;
    const uint32_t len = uint32_t((size + 31) & ~std::size_t(31));
    uint32_t level;
    _CPU_ISR_Disable(level);
    void* p = nullptr;
    if (count_ < kBlocks) {
        // First fit: the gap before each block, then after the last.
        uint32_t from = 0;
        for (int i = 0; i <= count_ && !p; i++) {
            const uint32_t to = i < count_ ? at_[i] : size_;
            if (to - from >= len) {
                for (int j = count_; j > i; j--) {
                    at_[j] = at_[j - 1];
                    len_[j] = len_[j - 1];
                }
                at_[i] = from;
                len_[i] = len;
                count_++;
                p = base_ + from;
            } else if (i < count_) {
                from = at_[i] + len_[i];
            }
        }
    }
    _CPU_ISR_Restore(level);
    return p;
}

bool Region::owns(const void* p) const {
    return base_ && p >= base_ && p < base_ + size_;
}

bool Region::free(void* p) {
    if (!owns(p)) return false;
    const uint32_t at = uint32_t(static_cast<uint8_t*>(p) - base_);
    uint32_t level;
    _CPU_ISR_Disable(level);
    for (int i = 0; i < count_; i++) {
        if (at_[i] != at) continue;
        for (int j = i; j + 1 < count_; j++) {
            at_[j] = at_[j + 1];
            len_[j] = len_[j + 1];
        }
        count_--;
        break;
    }
    _CPU_ISR_Restore(level);
    return true;
}

uint32_t Region::used_kb() const {
    uint32_t used = 0;
    for (int i = 0; i < count_; i++) used += len_[i];
    return used / 1024;
}

Region& level_region() {
    static Region region;
    return region;
}

RegionScope::RegionScope(Region* region) : was_(g_route) {
    g_route = region;
    g_route_thread = LWP_GetSelf();
}

RegionScope::~RegionScope() {
    g_route = was_;
}

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
#ifndef PPGC_DIAG
    (void)when;  // (diagnostic builds only)
    return;
#endif
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
    SDL_Log("census %s: %u KB in blocks from the heap; small pages %u KB (%u KB from the top); largest free %u KB",
            when, unsigned(total / 1024), unsigned(g_chunks * (kChunk / 1024)), unsigned(g_top_chunks * (kChunk / 1024)),
            unsigned(memory::largest_free_kb()));
    {
        uint32_t partial = 0, level;
        _CPU_ISR_Disable(level);
        for (int c = 0; c < kClasses; c++)
            for (int b = 0; b < 2; b++)
                for (Page* pg = g_partial[c][b]; pg; pg = pg->next) partial++;
        uint32_t pages = g_chunks * kPagesPerChunk, free_pages = g_free_page_count, live = g_small_live;
        _CPU_ISR_Restore(level);
        SDL_Log("census   small pages: %u in use (%u partly), %u free in their chunks; %u KB of blocks in them",
                unsigned(pages - free_pages), unsigned(partial), unsigned(free_pages), unsigned(live / 1024));
    }
    SDL_Log("census   by size: <=1K %u KB (%u), <=4K %u KB (%u), <=16K %u KB (%u), <=64K %u KB (%u), <=256K %u KB (%u), "
            "more %u KB (%u)",
            unsigned(g_bucket_bytes[0] / 1024), unsigned(g_bucket_count[0]), unsigned(g_bucket_bytes[1] / 1024),
            unsigned(g_bucket_count[1]), unsigned(g_bucket_bytes[2] / 1024), unsigned(g_bucket_count[2]),
            unsigned(g_bucket_bytes[3] / 1024), unsigned(g_bucket_count[3]), unsigned(g_bucket_bytes[4] / 1024),
            unsigned(g_bucket_count[4]), unsigned(g_bucket_bytes[5] / 1024), unsigned(g_bucket_count[5]));
    // (The lists only until the first level: Octave's log queue holds 32
    // lines, and a census longer than that loses its end.)
    for (int j = 0; j < n && !g_based; j++) {
        const Site& s = g_sites[top[j]];
        SDL_Log("census   %6u KB %5u blocks  %08x %08x", unsigned(s.bytes / 1024), unsigned(s.count), unsigned(s.a),
                unsigned(s.b));
    }
    // The 12 callers holding the most small blocks (diagnostic builds).
    int stop[12];
    int sn = 0;
    for (int k = 0; k < 12; k++) {
        int best = -1;
        for (int i = 0; i < kSites; i++) {
            bool taken = false;
            for (int j = 0; j < sn; j++) taken = taken || stop[j] == i;
            if (!taken && g_sites[i].small_bytes && (best < 0 || g_sites[i].small_bytes > g_sites[best].small_bytes))
                best = i;
        }
        if (best < 0) break;
        stop[sn++] = best;
    }
    for (int j = 0; j < sn && !g_based; j++) {
        const Site& s = g_sites[stop[j]];
        SDL_Log("census   small %5u KB %6u blocks  %08x %08x", unsigned(s.small_bytes / 1024), unsigned(s.small_count),
                unsigned(s.a), unsigned(s.b));
    }
    // What grew since the first level: the 16 callers holding the most more
    // than they did then (small blocks and heap blocks together).
    uint32_t used = 0;
    for (int i = 0; i < kSites; i++) used += g_sites[i].a != 0;
    if (!g_based) {
        if (!std::strstr(when, "level")) return heap_map_log();  // (from the first level on)
        for (int i = 0; i < kSites; i++) g_sites[i].base = g_sites[i].bytes + g_sites[i].small_bytes;
        g_based = true;
        SDL_Log("census   growth from here on (%u callers of %u)", unsigned(used), unsigned(kSites));
    } else {
        int grew[16];
        int gn = 0;
        int32_t sum = 0;
        auto delta = [](const Site& s) { return int32_t(s.bytes + s.small_bytes) - int32_t(s.base); };
        for (int i = 0; i < kSites; i++) sum += delta(g_sites[i]);
        for (int k = 0; k < 16; k++) {
            int best = -1;
            for (int i = 0; i < kSites; i++) {
                bool taken = false;
                for (int j = 0; j < gn; j++) taken = taken || grew[j] == i;
                if (!taken && delta(g_sites[i]) > 0 && (best < 0 || delta(g_sites[i]) > delta(g_sites[best]))) best = i;
            }
            if (best < 0) break;
            grew[gn++] = best;
        }
        SDL_Log("census   growth since the first level: %d KB (%u callers of %u)", int(sum / 1024), unsigned(used),
                unsigned(kSites));
        for (int j = 0; j < gn; j++) {
            const Site& s = g_sites[grew[j]];
            SDL_Log("census   grew %+6d KB  now %5u KB %6u blocks  %08x %08x", int(delta(s) / 1024),
                    unsigned((s.bytes + s.small_bytes) / 1024), unsigned(s.count + s.small_count), unsigned(s.a),
                    unsigned(s.b));
        }
    }
    if (!g_based) heap_map_log();
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
    if (void* p = region_alloc(size)) return p;
#ifdef PPGC_DIAG
    if (size + sizeof(Tag) <= kSmall)
        if (void* p = small_tagged_alloc(size, __builtin_return_address(0), __builtin_return_address(1))) return p;
#else
    if (size <= kSmall)
        if (void* p = small_alloc(size)) return p;
#endif
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
