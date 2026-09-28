// render::Renderer on the GameCube's GX, in place of engine/render/renderer.cpp
// (OpenGL). The same drawing: vertex-coloured triangles for shapes, textured
// quads for bitmaps and text, each with a matrix and the colour transform
// as a multiply. The class's OpenGL members are unused here.
//
// A movie's display lists and textures are freed when it is destroyed
// (swf::Movie::on_destroy). Display lists live in list memory, a block of
// their own, so their comings and goings leave no holes among the game's
// memory; shapes are tessellated in scratch memory (memory_gc.h) for the same
// reason. Lists are a cache: those of the shapes not drawn lately move to
// ARAM, and come back from there by DMA when drawn again; only when ARAM is
// full too is a list thrown away (a shape can always be parsed and
// tessellated again from its record). Masks use the depth buffer (the
// GameCube has no stencil buffer): see "masks" below.
#include "render/renderer.h"

#include <gccore.h>
#include <malloc.h>
#include <ogc/aram.h>
#include <ogc/lwp.h>
#include <ogc/machine/processor.h>
#include <ogc/system.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <SDL3/SDL_log.h>

#include "aram_gc.h"

// Octave (Graphics/GX/GxUtils.h): waits until the GPU has drawn everything
// queued so far, and frees what Octave put off until then.
void GxWaitGpu();
#include "memory_gc.h"
#include "trace_gc.h"
#include "swf/movie.h"

namespace render {

namespace {

constexpr uint8_t kShapeFormat = GX_VTXFMT6;    // indexed position (u16 x, y) + colour
constexpr uint8_t kTexturedFormat = GX_VTXFMT7;  // direct position + uv

// A tessellated shape: its vertex arrays and a display list of indices into
// them, in one 32-byte-aligned block.
// Positions are u16 steps from the shape's corner (the matrix puts them
// back), and colours an index into the shape's own palette: 8-bit when it
// has 256 colours or fewer.
struct ShapeList {
    swf::Shape* owner = nullptr;  // null once its movie has gone
    uint32_t last_frame = 0;      // the frame it was last drawn in
    uint32_t retry_frame = 0;     // without a list (out of memory): when to try again
    uint8_t* block = nullptr;         // in main memory, or null (then in ARAM, or not made)
    uint32_t block_size = 0;
    uint32_t pos_size = 0, col_size = 0;  // the block: positions, palette, list
    uint32_t aram = 0;                // its copy in ARAM, or 0
    const void* positions = nullptr;  // u16 x, y
    const void* colors = nullptr;     // the palette, RGBA8
    bool wide_colors = false;         // 16-bit colour indices
    float origin_x = 0, origin_y = 0, step = 1;  // twips
    void* list = nullptr;
    uint32_t list_size = 0;
    bool overflowed = false;  // (while building)
    bool no_room = false;     // (while building: no list memory to be had)
    bool bad = false;         // (while building: not in memory as written)
    uint32_t vertices = 0;
};


struct Texture {
    GXTexObj obj;
    void* texels = nullptr;
    uint32_t bytes = 0;
    float u_max = 1, v_max = 1;  // the image's part of the (4-pixel-padded) texture
};

// Handles are index + 1; freed slots are reused.
std::vector<ShapeList> g_shapes;
std::vector<Texture> g_textures;
std::vector<uint32_t> g_free_shapes, g_free_textures;
uint32_t g_shape_bytes = 0, g_texture_bytes = 0;
uint32_t g_frame = 0;

// Only lists not drawn for this many frames are moved out to make room:
// those on screen stay, or making one shape would unmake others, frame after
// frame.
constexpr uint32_t kEvictAge = 30;

// ---- List memory: where display lists are in main memory
//
// One block, taken at the start. New lists go on top; between frames, when
// the GPU is done with them, the lists not drawn lately go to ARAM and the
// rest move down together, so that the top has room for the next frame's.
// (In the heap, lists coming and going among the game's own allocations
// left it in pieces too small for the game with 1.6 MB free.)
// Its size moves between these, a step at a time: up while lists don't fit
// (the world map's drew at 30-78 ms a frame, sent to ARAM and fetched back
// again and again) and the heap has room to spare, down when the heap runs
// short, and at once to the least when an allocation can't be had or a
// movie goes (the next movie loads with the most room).
// (Room to spare was 2 MB, and a shortage 1.5 MB: after a few levels the
// heap's biggest block was 2.6 MB, so on the world map the lists never grew
// and it drew at 20-54 ms a frame. An allocation that can't be had takes the
// list memory back to its least anyway.)
constexpr uint32_t kGrowSpare = 512 * 1024, kShrinkBelow = 512 * 1024;
bool g_lists_to_least = false;  // a movie went: the least, next frame
constexpr uint32_t kListMin = 1280 * 1024, kListMax = 2560 * 1024, kListStep = 512 * 1024;
uint32_t kListMemory = kListMin;  // (the size now)
constexpr uint32_t kListRoom = 256 * 1024;  // kept free at the top for a frame's new lists
uint8_t* g_lists = nullptr;
uint32_t g_lists_gone_frame = 0;  // list memory given back to the heap (gx_release_memory) then
uint32_t g_lists_top = 0;   // used below this
uint32_t g_lists_live = 0;  // of which by lists still there
uint32_t g_lists_waits = 0;  // times list memory filled in a frame, so far
uint32_t g_waits_seen = 0, g_resize_frame = 0;
std::vector<uint32_t> g_compact_order;

bool in_lists(const void* p) {
    return g_lists && p >= g_lists && p < g_lists + kListMemory;
}

// A block for a list: in list memory, or the heap for one too big for it.
// Null if list memory is full (until the next frame).
uint8_t* list_alloc(uint32_t size) {
    if (!g_lists || size > kListRoom) return static_cast<uint8_t*>(memalign(32, size));
    if (g_lists_top + size > kListMemory) return nullptr;
    uint8_t* p = g_lists + g_lists_top;
    g_lists_top += size;
    g_lists_live += size;
    return p;
}

void list_free(uint8_t* p, uint32_t size) {
    if (!in_lists(p)) {
        free(p);
        return;
    }
    g_lists_live -= size;
    if (p + size == g_lists + g_lists_top) g_lists_top -= size;  // the last one made: its room is back
}

// Tessellating: what a big shape needs, with its vectors doubling.
constexpr uint32_t kScratch = 384 * 1024;

// ---- ARAM: where display lists go when they leave main memory
//
// The top kAramCache bytes of ARAM (16 MB of audio memory the CPU reaches by
// DMA) hold copies of display lists, so a shape comes back by DMA instead of
// being tessellated again. First fit over that region; when it is full, the
// copies of the shapes drawn longest ago go. (The rest of ARAM is left for
// sound; Octave's own ARAM sounds, which this game doesn't use, allocate
// from the bottom.)
constexpr uint32_t kAramCache = aram::kShapeCache;
constexpr uint32_t kAramPiece = 8 * 1024;

struct AramBlock {
    uint32_t at, size;
    bool used;
};
std::vector<AramBlock> g_aram;  // address order, covering the region
uint32_t g_aram_used = 0;

void aram_init() {
    if (!aram::init()) return;
    g_aram.push_back({aram::top() - kAramCache, kAramCache, false});
}

void aram_dma(uint32_t dir, void* mem, uint32_t at, uint32_t len) {
    if (dir == AR_MRAMTOARAM) aram::to_aram(mem, at, len);
    else aram::from_aram(mem, at, len);
}

// A pattern there and back: if it doesn't come back the same, ARAM isn't used
// (shapes are tessellated again instead).
void aram_check() {
    if (g_aram.empty()) return;
    constexpr uint32_t n = 2 * kAramPiece + 96;  // more than one piece, and a part one
    uint32_t* out = static_cast<uint32_t*>(memalign(32, n));
    uint32_t* back = static_cast<uint32_t*>(memalign(32, n));
    bool ok = out && back;
    if (ok) {
        for (uint32_t i = 0; i < n / 4; i++) out[i] = i * 2654435761u;
        std::memset(back, 0, n);
        uint32_t at = g_aram[0].at + g_aram[0].size - n;
        aram_dma(AR_MRAMTOARAM, out, at, n);
        aram_dma(AR_ARAMTOMRAM, back, at, n);
        ok = std::memcmp(out, back, n) == 0;
    }
    free(out);
    free(back);
    if (!ok) {
        SDL_Log("gx: ARAM didn't read back; shapes stay in main memory");
        g_aram.clear();
        return;
    }
    SDL_Log("gx: ARAM cache of %u KB", unsigned(kAramCache / 1024));
}

// First fit; 0 if there is no room.
uint32_t aram_alloc(uint32_t len) {
    for (size_t i = 0; i < g_aram.size(); i++) {
        AramBlock& b = g_aram[i];
        if (b.used || b.size < len) continue;
        if (b.size > len) g_aram.insert(g_aram.begin() + ptrdiff_t(i) + 1, {b.at + len, b.size - len, false});
        g_aram[i].size = len;
        g_aram[i].used = true;
        g_aram_used += len;
        return g_aram[i].at;
    }
    return 0;
}

void aram_free(uint32_t at) {
    for (size_t i = 0; i < g_aram.size(); i++) {
        if (g_aram[i].at != at || !g_aram[i].used) continue;
        g_aram[i].used = false;
        g_aram_used -= g_aram[i].size;
        // Join free neighbours, so the space comes back whole.
        if (i + 1 < g_aram.size() && !g_aram[i + 1].used) {
            g_aram[i].size += g_aram[i + 1].size;
            g_aram.erase(g_aram.begin() + ptrdiff_t(i) + 1);
        }
        if (i > 0 && !g_aram[i - 1].used) {
            g_aram[i - 1].size += g_aram[i].size;
            g_aram.erase(g_aram.begin() + ptrdiff_t(i));
        }
        return;
    }
}

template <typename T>
uint32_t add_slot(std::vector<T>& slots, std::vector<uint32_t>& free, const T& item) {
    if (!free.empty()) {
        uint32_t handle = free.back();
        free.pop_back();
        slots[handle - 1] = item;
        return handle;
    }
    slots.push_back(item);
    return uint32_t(slots.size());
}

// Where the picture can differ from the PC's, counted for the log
// (gx_mismatch_log, every two seconds): what was left out of a frame, and
// what couldn't be drawn as the PC draws it.
struct Mismatches {
    uint32_t no_list_memory = 0;  // a shape not drawn: no list memory to be had
    uint32_t not_fetched = 0;     // a shape not drawn: its list couldn't come back from ARAM
    uint32_t retrying = 0;        // a shape not drawn: it couldn't be made, and waits to try again
    uint32_t bitmap_waiting = 0;  // a bitmap not drawn: its texture couldn't be made
    uint32_t tint_over = 0;       // a colour transform brightening past 4x (the PC's goes on)
};
Mismatches g_miss;
uint32_t g_tints_over_1 = 0, g_tints_over_2 = 0;  // draws brightened 1-2x, over 2x (on this screen)
// The first of each kind is logged with what it was.
void first_miss(uint32_t& counter, const char* what) {
    if (counter++ == 0) SDL_Log("gx: mismatch: %s", what);
}

// Bitmaps whose texture couldn't be made: when to try again.
std::unordered_map<const void*, uint32_t> g_texture_retry;

// Freed at the next begin_frame: the GPU may still be drawing the frame
// before from them (Octave waits for it before a frame begins).
std::vector<uint32_t> g_pending_shapes, g_pending_textures;

void free_shape(uint32_t handle) {
    ShapeList& s = g_shapes[handle - 1];
    if (s.block) {
        g_shape_bytes -= s.block_size;
        list_free(s.block, s.block_size);
    }
    if (s.aram) aram_free(s.aram);
    s = ShapeList{};
    g_free_shapes.push_back(handle);
}

void free_texture(uint32_t handle) {
    Texture& t = g_textures[handle - 1];
    if (t.texels) {
        g_texture_bytes -= t.bytes;
        free(t.texels);
    }
    t = Texture{};
    g_free_textures.push_back(handle);
}

// A movie going: everything made for its characters.
void release_movie(swf::Movie& movie) {
    SDL_Log("gx: movie %s goes", movie.name.c_str());
    g_lists_to_least = true;
    for (auto& [id, ch] : movie.characters) {
        if (ch->type == swf::CharacterType::Shape) {
            auto& shape = static_cast<swf::ShapeCharacter&>(*ch).shape;
            if (shape.gpu_mesh) {
                g_shapes[shape.gpu_mesh - 1].owner = nullptr;
                g_pending_shapes.push_back(shape.gpu_mesh);
            }
            shape.gpu_mesh = 0;
        } else if (ch->type == swf::CharacterType::Bitmap) {
            auto& bitmap = static_cast<swf::BitmapCharacter&>(*ch);
            g_texture_retry.erase(&bitmap);
            if (bitmap.texture) g_pending_textures.push_back(bitmap.texture);
            bitmap.texture = 0;
        }
    }
}

enum class Desc { None, Shape, WideShape, Textured };
Desc g_desc = Desc::None;

// A shape's list gone for good: tessellated again when next drawn.
void forget_shape(uint32_t handle) {
    ShapeList& s = g_shapes[handle - 1];
    s.owner->gpu_mesh = 0;
    s.owner->tessellated = false;
    free_shape(handle);
}

// Room in ARAM for `len`, dropping the ARAM copies of the shapes drawn
// longest ago (those only in ARAM are forgotten); 0 if there can't be.
uint32_t aram_room(uint32_t len, uint32_t keep) {
    for (;;) {
        if (uint32_t at = aram_alloc(len)) return at;
        uint32_t oldest = 0;
        for (uint32_t i = 0; i < g_shapes.size(); i++) {
            const ShapeList& s = g_shapes[i];
            // Not one drawn this frame: it may be the one being fetched.
            if (!s.aram || !s.owner || i + 1 == keep || s.last_frame >= g_frame) continue;
            if (!oldest || s.last_frame < g_shapes[oldest - 1].last_frame) oldest = i + 1;
        }
        if (!oldest) return 0;
        ShapeList& s = g_shapes[oldest - 1];
        if (s.block) {  // still in main memory: only its copy goes
            aram_free(s.aram);
            s.aram = 0;
        } else {
            forget_shape(oldest);
        }
    }
}

// Takes the display list of the shape drawn longest ago, not drawn for `age`
// frames, out of main memory: to ARAM, if it isn't there already and there's
// room, else it is forgotten. False if there is none to take. (The GPU has
// finished with every frame before the last one: Octave waits for it before
// a frame begins.) `heap` takes only one in the heap, not list memory.
bool evict_one(uint32_t age = kEvictAge, bool heap = false) {
    uint32_t oldest = 0;
    for (uint32_t i = 0; i < g_shapes.size(); i++) {
        const ShapeList& s = g_shapes[i];
        if (!s.block || !s.owner || s.last_frame + age > g_frame) continue;
        if (heap && in_lists(s.block)) continue;
        if (!oldest || s.last_frame < g_shapes[oldest - 1].last_frame) oldest = i + 1;
    }
    if (!oldest) return false;
    ShapeList& s = g_shapes[oldest - 1];
    if (!s.aram && !g_aram.empty()) {
        s.aram = aram_room(s.block_size, oldest);
        if (s.aram) aram_dma(AR_MRAMTOARAM, s.block, s.aram, s.block_size);
    }
    if (!s.aram) {
        forget_shape(oldest);
        return true;
    }
    g_shape_bytes -= s.block_size;
    list_free(s.block, s.block_size);
    s.block = nullptr;
    s.positions = s.colors = nullptr;
    s.list = nullptr;
    return true;
}

void place_block(ShapeList& s, uint8_t* block) {
    s.block = block;
    s.positions = block;
    s.colors = block + s.pos_size;
    s.list = block + s.pos_size + s.col_size;
}

// The lists in list memory, moved down together (between frames: the GPU
// must not be reading them).
void compact_lists() {
    g_compact_order.clear();
    for (uint32_t i = 0; i < g_shapes.size(); i++)
        if (in_lists(g_shapes[i].block)) g_compact_order.push_back(i);
    std::sort(g_compact_order.begin(), g_compact_order.end(),
              [](uint32_t a, uint32_t b) { return g_shapes[a].block < g_shapes[b].block; });
    uint32_t to = 0;
    for (uint32_t i : g_compact_order) {
        ShapeList& s = g_shapes[i];
        if (s.block != g_lists + to) {
            std::memmove(g_lists + to, s.block, s.block_size);
            place_block(s, g_lists + to);
        }
        to += s.block_size;
    }
    g_lists_top = to;
    g_lists_live = to;
    DCFlushRange(g_lists, to);
    GX_InvVtxCache();
}

// List memory at another size (the GPU must be done with it). Smaller: the
// lists drawn longest ago go to ARAM until the rest fit, they move down, and
// the block gives its end back to the heap where it is. Bigger: a new block,
// the lists copied into it, the old one freed. False if it can't be had.
bool resize_lists(uint32_t size) {
    if (!g_lists || size == kListMemory) return true;
    if (size < kListMemory) {
        while (g_lists_live + kListRoom > size && evict_one(0)) {
        }
        if (g_lists_live > size) return false;
        compact_lists();
        void* p = realloc(g_lists, size);  // (a block made smaller stays where it is)
        if (p != g_lists) return false;
    } else {
        auto* bigger = static_cast<uint8_t*>(memalign(32, size));
        if (!bigger) return false;
        compact_lists();
        std::memcpy(bigger, g_lists, g_lists_top);
        for (ShapeList& s : g_shapes)
            if (in_lists(s.block)) place_block(s, bigger + (s.block - g_lists));
        free(g_lists);
        g_lists = bigger;
        DCFlushRange(g_lists, g_lists_top);
        GX_InvVtxCache();
    }
    SDL_Log("gx: list memory %u KB", unsigned(size / 1024));
    kListMemory = size;
    return true;
}

// Between frames: more list memory while lists don't fit and the heap has
// room to spare; less when it is short.
void size_lists() {
    if (!g_lists || g_frame < g_resize_frame + 30) return;
    bool waited = g_lists_waits != g_waits_seen;
    g_waits_seen = g_lists_waits;
    bool can_grow = waited && kListMemory < kListMax;
    bool can_shrink = kListMemory > kListMin;
    if (!can_grow && !can_shrink) return;
    g_resize_frame = g_frame;
    uint32_t largest = memory::largest_free_kb() * 1024;
    if (can_grow && largest >= kListMemory + kListStep + kGrowSpare)
        resize_lists(kListMemory + kListStep);
    else if (can_shrink && largest < kShrinkBelow)
        resize_lists(kListMemory - kListStep);
}

// List memory full in the middle of a frame (more lists drawn in it than
// fit): once the GPU has drawn all it has been given, any list can go to
// ARAM, and the rest move down. Slow, but everything is drawn.
bool make_list_room(uint32_t size) {
    if (!g_lists || size > kListRoom) return false;
    g_lists_waits++;
    trace::at(trace::kMain, "list memory full: waiting for the GPU");
    GxWaitGpu();  // (Octave's: also frees what it put off until the GPU was done)
    trace::at(trace::kMain, "game render");
    // Half of it, so that the next ones this frame fit too.
    while (g_lists_live + size > kListMemory / 2 && evict_one(0)) {
    }
    if (g_lists_live + size > kListMemory) return false;
    compact_lists();
    return true;
}

uint8_t* list_block(uint32_t size) {
    uint8_t* block = list_alloc(size);
    if (!block && make_list_room(size)) block = list_alloc(size);
    return block;
}

// Brings a list back from ARAM; false if there's no room for it.
bool fetch_shape(ShapeList& s) {
    uint8_t* block = list_block(s.block_size);
    if (!block) return false;
    aram_dma(AR_ARAMTOMRAM, block, s.aram, s.block_size);
    place_block(s, block);
    g_shape_bytes += s.block_size;
    // The arrays may sit where others were.
    GX_InvVtxCache();
    return true;
}

uint32_t align32(uint32_t n) { return (n + 31) & ~31u; }

void use_desc(Desc d) {
    if (g_desc == d) return;
    g_desc = d;
    GX_ClearVtxDesc();
    if (d == Desc::Shape || d == Desc::WideShape) {
        GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
        GX_SetVtxDesc(GX_VA_CLR0, d == Desc::WideShape ? GX_INDEX16 : GX_INDEX8);
    } else {
        GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    }
}

// Vertex colour or texel, times the colour transform. TEV colours stop at
// 1.0, so a transform that brightens (up to 2x) is halved and doubled back.
void set_tev(bool textured, const swf::CXform& c) {
    float t[4], top = 0;
    for (int i = 0; i < 4; i++) {
        t[i] = std::max(0.0f, c.tint(i));
        top = std::max(top, t[i]);
    }
    // TEV colours stop at 1.0: a transform that brightens is scaled down,
    // and back up by 2 or 4 after (the PC's has no limit).
    uint8_t scale = top > 2.0f ? GX_CS_SCALE_4 : top > 1.0f ? GX_CS_SCALE_2 : GX_CS_SCALE_1;
    float k = top > 2.0f ? 0.25f : top > 1.0f ? 0.5f : 1.0f;
    if (top > 4.0f) first_miss(g_miss.tint_over, "a colour transform brightening past 4x");
    if (top > 2.0f) g_tints_over_2++;
    else if (top > 1.0f) g_tints_over_1++;
    auto byte = [&](float v) { return uint8_t(std::min(v * k, 1.0f) * 255.0f + 0.5f); };
    GX_SetTevColor(GX_TEVREG0, GXColor{byte(t[0]), byte(t[1]), byte(t[2]), byte(t[3])});

    GX_SetNumTevStages(1);
    if (textured) {
        GX_SetNumChans(0);
        GX_SetNumTexGens(1);
        GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_ZERO);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_A0, GX_CA_ZERO);
    } else {
        GX_SetNumChans(1);
        GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
        GX_SetNumTexGens(0);
        GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_RASC, GX_CC_C0, GX_CC_ZERO);
        GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_RASA, GX_CA_A0, GX_CA_ZERO);
    }
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, scale, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, scale, GX_TRUE, GX_TEVPREV);
}

// The depth of what is drawn (masks, below): the view's z. guOrtho with
// near -1 and far 1 puts z 1 nearest and -1 farthest; a level of masking is
// 1/256 nearer than the one outside it.
float g_depth = 0;
constexpr float kFarthest = -0.99f;
float level_depth(int level) {
    return float(level) / 256.0f;
}

void load_matrix(const swf::Matrix& m) {
    Mtx mtx = {
        {m.a, m.c, 0, m.tx},
        {m.b, m.d, 0, m.ty},
        {0, 0, 1, g_depth},
    };
    GX_LoadPosMtxImm(mtx, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
}

// `slack`: room after the list's own bytes, rounded up to 32. GX_EndDispList
// flushes 32 bytes of no-ops after the list; with only 32 bytes of room, a
// list of a whole number of 32-byte blocks fills the buffer exactly, and the
// console's write pointer wraps to the start with its wrap flag set
// (0x04000000): GX_EndDispList then gives exactly 67108864 as the size
// (docs/hardware-bugs.md). 64 keeps the flush clear of the end.
ShapeList build_shape(const swf::Mesh& mesh, uint32_t slack = 64);

// GX calls a list overflowed when its commands come near the end of the
// buffer, not only past it: one that did is built again with more room. One
// not in memory as written is built again too.
ShapeList build_shape_list(const swf::Mesh& mesh) {
    ShapeList s = build_shape(mesh);
    if (s.list_size == 0 && s.overflowed) s = build_shape(mesh, 32 + 256);
    if (s.list_size == 0 && s.bad) s = build_shape(mesh, 32 + 256);
    return s;
}

// ---- Lists checked in memory
//
// Each list is read back as the GPU reads it (uncached) when built and
// compared with what was written; one that differs is logged and made
// again. (Written for the menu's castle wall, missing on the console: every
// list was as written there; the size GX gave for it wasn't.)
uint32_t g_checked = 0, g_bad_built = 0, g_gx_size_wrong = 0;

#ifdef PPGC_DIAG
// The block as the GPU sees it (words: it is uncached), into `out`. (Not a
// buffer kept between calls: made while tessellating, it would sit in
// scratch memory for good.)
void read_block(const uint8_t* block, uint32_t size, std::vector<uint8_t>& out) {
    out.resize(size);
    const volatile uint32_t* p = static_cast<const volatile uint32_t*>(MEM_K0_TO_K1(const_cast<uint8_t*>(block)));
    for (uint32_t i = 0; i < size / 4; i++) {
        uint32_t w = p[i];
        std::memcpy(&out[i * 4], &w, 4);
    }
}

// Bytes of `got` against `want` from `at`, logged with where the first
// difference is. `what` names the part.
bool same_bytes(const char* what, const ShapeList& s, const uint8_t* want, const uint8_t* got, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (want[i] == got[i]) continue;
        uint32_t differ = 0;
        for (uint32_t j = i; j < n; j++) differ += want[j] != got[j];
        char w[3 * 12 + 1] = {}, g[3 * 12 + 1] = {};
        for (uint32_t k = 0; k < 12 && i + k < n; k++) {
            std::snprintf(w + k * 3, 4, "%02x ", want[i + k]);
            std::snprintf(g + k * 3, 4, "%02x ", got[i + k]);
        }
        SDL_Log("gx: LIST CHECK: %s differs at %u of %u (%u bytes differ; block %p, %u vertices, list %u bytes): "
                "wrote %s got %s",
                what, unsigned(i), unsigned(n), unsigned(differ), static_cast<void*>(s.block),
                unsigned(s.pos_size / 4), unsigned(s.list_size), w, g);
        return false;
    }
    return true;
}
#endif

ShapeList build_shape(const swf::Mesh& mesh, uint32_t slack) {
    ShapeList s;
    size_t count = mesh.indices.size() / 3 * 3;
    size_t vertices = mesh.vertices.size();
    if (count == 0) return s;
    if (vertices > 0xffff) {
        SDL_Log("gx: a shape of %u vertices is over the 16-bit index limit", unsigned(vertices));
        return s;
    }

    // Positions: steps of a whole twip (or more, for a shape over 65535
    // twips across) from the corner.
    float x0 = mesh.vertices[0].x, y0 = mesh.vertices[0].y, x1 = x0, y1 = y0;
    for (const swf::Vertex& v : mesh.vertices) {
        x0 = std::min(x0, v.x);
        y0 = std::min(y0, v.y);
        x1 = std::max(x1, v.x);
        y1 = std::max(y1, v.y);
    }
    s.vertices = uint32_t(vertices);
#ifdef PPGC_DIAG
    // Each mesh logged with a hash, to set the console's beside Dolphin's
    // (the menu's castle wall comes out different on the console), and what
    // would be wrong in one: indices past the vertices, coordinates that
    // aren't numbers.
    {
        uint32_t h = 2166136261u, bad_index = 0, not_finite = 0;
        auto mix = [&](uint32_t w) { h = (h ^ w) * 16777619u; };
        for (const swf::Vertex& v : mesh.vertices) {
            uint32_t xb, yb;
            std::memcpy(&xb, &v.x, 4);
            std::memcpy(&yb, &v.y, 4);
            mix(xb);
            mix(yb);
            if (!std::isfinite(v.x) || !std::isfinite(v.y)) not_finite++;
        }
        for (size_t i = 0; i < count; i++) {
            mix(mesh.indices[i]);
            if (mesh.indices[i] >= vertices) bad_index++;
        }
        SDL_Log("gx: made %u vertices %u triangles, x %.1f..%.1f y %.1f..%.1f, hash %08x%s", unsigned(vertices),
                unsigned(count / 3), x0, x1, y0, y1, unsigned(h),
                bad_index || not_finite ? " BAD MESH" : "");
        if (bad_index || not_finite)
            SDL_Log("gx: BAD MESH: %u indices past the %u vertices, %u coordinates not numbers", unsigned(bad_index),
                    unsigned(vertices), unsigned(not_finite));
    }
#endif
    s.origin_x = std::floor(x0);
    s.origin_y = std::floor(y0);
    s.step = std::max(1.0f, std::ceil(std::max(x1 - s.origin_x, y1 - s.origin_y) / 65535.0f));

    // The palette, and each vertex's index into it.
    std::vector<uint32_t> palette;
    std::vector<uint16_t> color_of(vertices);
    {
        std::unordered_map<uint32_t, uint16_t> seen;
        for (size_t i = 0; i < vertices; i++) {
            const swf::Rgba& c = mesh.vertices[i].color;
            uint32_t rgba = uint32_t(c.r) << 24 | uint32_t(c.g) << 16 | uint32_t(c.b) << 8 | c.a;
            auto [it, added] = seen.emplace(rgba, uint16_t(palette.size()));
            if (added) palette.push_back(rgba);
            color_of[i] = it->second;
        }
    }
    s.wide_colors = palette.size() > 256;

    // Batches of up to 65535 indices (a multiple of 3), each a 3-byte header,
    // then per vertex a 16-bit position index and an 8- or 16-bit colour one.
    const size_t per_batch = 65535;
    size_t batches = (count + per_batch - 1) / per_batch;
    uint32_t pos_size = align32(uint32_t(vertices * 4));
    uint32_t col_size = align32(uint32_t(palette.size() * 4));
    uint32_t list_size = align32(uint32_t(batches * 3 + count * (s.wide_colors ? 4 : 3))) + slack;
    s.block_size = pos_size + col_size + list_size;
    s.pos_size = pos_size;
    s.col_size = col_size;
    s.block = list_block(s.block_size);
    if (!s.block) {
        ShapeList full;
        full.no_room = true;
        return full;
    }
    uint16_t* pos = reinterpret_cast<uint16_t*>(s.block);
    for (size_t i = 0; i < vertices; i++) {
        const swf::Vertex& v = mesh.vertices[i];
        pos[i * 2] = uint16_t(std::lround((v.x - s.origin_x) / s.step));
        pos[i * 2 + 1] = uint16_t(std::lround((v.y - s.origin_y) / s.step));
    }
    uint8_t* col = s.block + pos_size;
    for (size_t i = 0; i < palette.size(); i++) {
        col[i * 4] = uint8_t(palette[i] >> 24);
        col[i * 4 + 1] = uint8_t(palette[i] >> 16);
        col[i * 4 + 2] = uint8_t(palette[i] >> 8);
        col[i * 4 + 3] = uint8_t(palette[i]);
    }
    DCFlushRange(s.block, pos_size + col_size);
    s.positions = pos;
    s.colors = col;

    s.list = s.block + pos_size + col_size;
    // GX writes the state changes it still owes at the next GX_Begin: they
    // would land in the list, and set the state of whatever was drawn last
    // (a text's texture stage) each time the list is called. So they go out
    // now, with a triangle of no area in the list's own format.
    use_desc(s.wide_colors ? Desc::WideShape : Desc::Shape);
    set_tev(false, swf::CXform{});
    GX_SetArray(GX_VA_POS, pos, 4);
    GX_SetArray(GX_VA_CLR0, col, 4);
    GX_Begin(GX_TRIANGLES, kShapeFormat, 3);
    for (int k = 0; k < 3; k++) {
        GX_Position1x16(0);
        if (s.wide_colors) GX_Color1x16(0);
        else GX_Color1x8(0);
    }
    GX_End();
    DCInvalidateRange(s.list, list_size);
    GX_BeginDispList(s.list, list_size);
    for (size_t done = 0; done < count;) {
        size_t n = std::min(per_batch, count - done);
        GX_Begin(GX_TRIANGLES, kShapeFormat, uint16_t(n));
        for (size_t i = done; i < done + n; i++) {
            uint16_t index = uint16_t(mesh.indices[i]);
            GX_Position1x16(index);
            if (s.wide_colors) GX_Color1x16(color_of[index]);
            else GX_Color1x8(uint8_t(color_of[index]));
        }
        GX_End();
        done += n;
    }
    uint32_t gx_size = GX_EndDispList();
    if (gx_size == 0) {
        list_free(s.block, s.block_size);
        ShapeList overflowed;
        overflowed.overflowed = true;
        return overflowed;
    }
    // The list's size is never GX_EndDispList's alone: on the console it gave
    // 67108864 (the FIFO's wrap flag) for lists that filled their buffer
    // exactly, where Dolphin gave the right size, and the list called with
    // it drew almost nothing (the menu's castle wall popping out; see
    // `slack` above and docs/hardware-bugs.md).
#ifndef PPGC_DIAG
    // Its bytes, to a whole 32; GX's when it is that or a block more (no-ops:
    // its flush's, or any before the list).
    {
        uint32_t ours = align32(uint32_t(batches * 3 + count * (s.wide_colors ? 4 : 3)));
        s.list_size = gx_size >= ours && gx_size <= ours + 32 ? gx_size : ours;
        if (s.list_size != gx_size && ++g_gx_size_wrong <= 10)
            SDL_Log("gx: GX_EndDispList gave %u bytes for a list of %u; %u used", unsigned(gx_size), unsigned(ours),
                    unsigned(s.list_size));
    }
#else
    // Diagnostic builds: what was written, against memory as the GPU will
    // read it: the arrays exactly; the list after any no-ops before it, then
    // only no-ops. The size is from what was written, after those no-ops.
    {
        std::vector<uint8_t> want;
        want.reserve(size_t(s.pos_size) + s.col_size + batches * 3 + count * 4);
        for (size_t i = 0; i < vertices; i++) {
            const swf::Vertex& v = mesh.vertices[i];
            uint16_t xy[2] = {uint16_t(std::lround((v.x - s.origin_x) / s.step)),
                              uint16_t(std::lround((v.y - s.origin_y) / s.step))};
            for (uint16_t c : xy) {
                want.push_back(uint8_t(c >> 8));
                want.push_back(uint8_t(c));
            }
        }
        want.resize(pos_size, 0);
        for (uint32_t c : palette)
            for (int k = 3; k >= 0; k--) want.push_back(uint8_t(c >> (k * 8)));
        want.resize(size_t(pos_size) + col_size, 0);
        for (size_t done = 0; done < count;) {
            size_t n = std::min(per_batch, count - done);
            want.push_back(uint8_t(GX_TRIANGLES | kShapeFormat));
            want.push_back(uint8_t(n >> 8));
            want.push_back(uint8_t(n));
            for (size_t i = done; i < done + n; i++) {
                uint16_t index = uint16_t(mesh.indices[i]);
                want.push_back(uint8_t(index >> 8));
                want.push_back(uint8_t(index));
                if (s.wide_colors) want.push_back(uint8_t(color_of[index] >> 8));
                want.push_back(uint8_t(color_of[index]));
            }
            done += n;
        }
        std::vector<uint8_t> readback;
        read_block(s.block, s.block_size, readback);
        const uint8_t* got = readback.data();
        uint32_t arrays = pos_size + col_size;
        uint32_t lead = 0;
        while (lead < list_size && got[arrays + lead] == 0) lead++;
        uint32_t body = uint32_t(want.size()) - arrays;
        s.list_size = align32(lead + body);
        // (not the padding after each array: never written)
        bool ok = same_bytes("positions", s, want.data(), got, uint32_t(vertices * 4)) &&
                  same_bytes("colours", s, want.data() + pos_size, got + pos_size, uint32_t(palette.size() * 4));
        if (ok && s.list_size > list_size) {
            SDL_Log("gx: LIST CHECK: list of %u bytes (after %u no-ops) is longer than its room, %u", unsigned(body),
                    unsigned(lead), unsigned(list_size));
            ok = false;
        }
        // (GX's may be a block of no-ops more: its flush's)
        if (gx_size < s.list_size || gx_size > s.list_size + 32) {
            g_gx_size_wrong++;
            if (g_gx_size_wrong <= 10)
                SDL_Log("gx: GX_EndDispList gave %u bytes for a list of %u (after %u no-ops); %u used", unsigned(gx_size),
                        unsigned(body), unsigned(lead), unsigned(s.list_size));
        }
        ok = ok && same_bytes("list", s, want.data() + arrays, got + arrays + lead, body);
        if (ok) {
            std::vector<uint8_t> zeros(s.list_size - lead - body, 0);
            ok = same_bytes("list's end", s, zeros.data(), got + arrays + lead + body, uint32_t(zeros.size()));
        }
        if (lead) SDL_Log("gx: list check: %u no-ops before a list", unsigned(lead));
        g_checked++;
        if (!ok) {
            g_bad_built++;
            list_free(s.block, s.block_size);
            ShapeList bad;
            bad.bad = true;
            return bad;
        }
    }
#endif
    // New arrays may sit where freed ones were.
    GX_InvVtxCache();
    g_shape_bytes += s.block_size;
    return s;
}

// RGBA (straight, rows top to bottom) -> a GX RGBA8 texture, halved until it
// fits GX's 1024x1024 limit and padded to whole 4x4 tiles.
uint32_t make_texture(const uint8_t* rgba, int width, int height, bool nearest = false) {
    if (width <= 0 || height <= 0) return 0;
    std::vector<uint8_t> scaled;
    while (width > 1024 || height > 1024) {
        int w = std::max(1, width / 2), h = std::max(1, height / 2);
        std::vector<uint8_t> half(size_t(w) * size_t(h) * 4);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                for (int c = 0; c < 4; c++) {
                    int sum = 0;
                    for (int dy = 0; dy < 2; dy++) {
                        for (int dx = 0; dx < 2; dx++) {
                            int sx = std::min(width - 1, x * 2 + dx), sy = std::min(height - 1, y * 2 + dy);
                            sum += rgba[(size_t(sy) * size_t(width) + size_t(sx)) * 4 + size_t(c)];
                        }
                    }
                    half[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)] = uint8_t(sum / 4);
                }
            }
        }
        scaled.swap(half);
        rgba = scaled.data();
        width = w;
        height = h;
    }
    // Rows at the bottom with nothing in them are left out (the texture
    // coordinates still span the full height).
    int full_height = height;
    while (height > 1) {
        const uint8_t* row = rgba + size_t(height - 1) * size_t(width) * 4;
        bool empty = true;
        for (int x = 0; x < width && empty; x++) empty = row[x * 4 + 3] == 0;
        if (!empty) break;
        height--;
    }
    if (height < full_height) height++;  // one empty row, which clamping repeats
    // The smallest format that keeps the picture: all greys (the font) as
    // IA4, 16 levels of intensity and alpha, a byte a texel in 8 x 4 tiles;
    // fully opaque (skies) as RGB565 in 4 x 4 tiles; the rest RGBA8, or, over
    // 128 KB (the keep's sky: 512 KB it couldn't have), RGB5A3 at half the
    // size: 5 bits a colour where opaque, 4 and 3 bits of alpha elsewhere.
    bool grey = true, opaque = true;
    for (size_t i = 0, n = size_t(width) * size_t(height); i < n && (grey || opaque); i++) {
        const uint8_t* px = rgba + i * 4;
        grey = grey && px[0] == px[1] && px[0] == px[2];
        opaque = opaque && px[3] == 255;
    }
    enum class Format { IA4, RGB565, RGBA8, RGB5A3 } format = grey ? Format::IA4 : opaque ? Format::RGB565 : Format::RGBA8;
    const int tile_w = format == Format::IA4 ? 8 : 4;
    int tw = (width + tile_w - 1) / tile_w * tile_w, th = (height + 3) & ~3;
    if (format == Format::RGBA8 && uint32_t(tw) * uint32_t(th) * 4 > 128 * 1024) format = Format::RGB5A3;
    uint32_t bytes = uint32_t(tw) * uint32_t(th) * (format == Format::IA4 ? 1 : format == Format::RGBA8 ? 4 : 2);
    uint8_t* texels = static_cast<uint8_t*>(memalign(32, bytes));
    if (!texels) return 0;
    uint8_t* block = texels;
    for (int ty = 0; ty < th; ty += 4) {
        for (int tx = 0; tx < tw; tx += tile_w, block += 32 * (format == Format::RGBA8 ? 2 : 1)) {
            for (int i = 0; i < tile_w * 4; i++) {
                // The padding repeats the edge, so filtering at the edge
                // doesn't blend in anything else.
                int x = std::min(tx + i % tile_w, width - 1), y = std::min(ty + i / tile_w, height - 1);
                const uint8_t* px = rgba + (size_t(y) * size_t(width) + size_t(x)) * 4;
                switch (format) {
                case Format::IA4:  // alpha in the high nibble, intensity in the low
                    block[i] = uint8_t((px[3] & 0xf0) | (px[0] >> 4));
                    break;
                case Format::RGB565: {
                    uint16_t v = uint16_t(((px[0] >> 3) << 11) | ((px[1] >> 2) << 5) | (px[2] >> 3));
                    block[i * 2] = uint8_t(v >> 8);
                    block[i * 2 + 1] = uint8_t(v);
                    break;
                }
                case Format::RGBA8:  // AR pairs, then GB pairs
                    block[i * 2] = px[3];
                    block[i * 2 + 1] = px[0];
                    block[32 + i * 2] = px[1];
                    block[32 + i * 2 + 1] = px[2];
                    break;
                case Format::RGB5A3: {  // 1 RRRRR GGGGG BBBBB, or 0 AAA RRRR GGGG BBBB
                    uint16_t v = px[3] >= 0xf0
                                     ? uint16_t(0x8000 | ((px[0] >> 3) << 10) | ((px[1] >> 3) << 5) | (px[2] >> 3))
                                     : uint16_t(((px[3] >> 5) << 12) | ((px[0] >> 4) << 8) | ((px[1] >> 4) << 4) |
                                                (px[2] >> 4));
                    block[i * 2] = uint8_t(v >> 8);
                    block[i * 2 + 1] = uint8_t(v);
                    break;
                }
                }
            }
        }
    }
    DCFlushRange(texels, bytes);
    // The texture may sit where a freed one was: none of it may come from the
    // texture cache.
    GX_InvalidateTexAll();
    Texture t;
    t.texels = texels;
    t.bytes = bytes;
    t.u_max = float(width) / float(tw);
    t.v_max = float(full_height) / float(th);
    GX_InitTexObj(&t.obj, texels, uint16_t(tw), uint16_t(th),
                  format == Format::IA4      ? GX_TF_IA4
                  : format == Format::RGB565 ? GX_TF_RGB565
                  : format == Format::RGB5A3 ? GX_TF_RGB5A3
                                             : GX_TF_RGBA8,
                  GX_CLAMP,
                  GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&t.obj, nearest ? GX_NEAR : GX_LINEAR, nearest ? GX_NEAR : GX_LINEAR);
    g_texture_bytes += bytes;
    return add_slot(g_textures, g_free_textures, t);
}

lwp_t g_main_thread = LWP_THREAD_NULL;

// A bitmap's texture made as its movie loads, from the pixels still in the
// file's data (swf::Movie::take_pixels): no copy of them is kept. If there's
// no memory for it now, they are copied, and it is made when first drawn.
bool take_pixels(swf::BitmapCharacter& bitmap, const uint8_t* rgba) {
    bitmap.texture = make_texture(rgba, bitmap.width, bitmap.height, bitmap.nearest);
    return bitmap.texture != 0;
}

}  // namespace

bool Renderer::init() {
    swf::Movie::on_destroy = release_movie;
    // Curves flattened to within 6 twips (0.3 pixel) rather than 2: a fifth
    // fewer triangles, where the GPU was the bottleneck (character select).
    swf::Shape::quality.curve_tolerance = 6.0f;
    swf::Movie::take_pixels = take_pixels;
    g_main_thread = LWP_GetSelf();
    // Taken first, while main memory is in one piece.
    g_lists = static_cast<uint8_t*>(memalign(32, kListMemory));
    if (!g_lists) SDL_Log("gx: no list memory; lists go in the heap");
    memory::scratch_init(kScratch);
    // So that making room never needs memory itself.
    g_free_shapes.reserve(8192);
    g_compact_order.reserve(8192);
    g_aram.reserve(8192);
    aram_init();
    aram_check();
    return true;
}

// Main memory ran out (operator new, new_gc.cpp): one more display list not
// drawn in the last frame leaves it, to ARAM. False if there is none (or
// this is another thread, or making room itself ran out).
bool gx_release_memory() {
    static bool busy = false;
    if (busy || g_main_thread == LWP_THREAD_NULL || LWP_GetSelf() != g_main_thread) return false;
    struct Busy {
        Busy() { busy = true; }
        ~Busy() { busy = false; }
    } guard;
    // List memory bigger than the least goes back to it first, its end to
    // the heap (waiting for the GPU to be done with it).
    if (g_lists && kListMemory > kListMin) {
        GxWaitGpu();  // (and Octave's frees put off until the GPU was done: memory too)
        if (resize_lists(kListMin)) return true;
    }
    if (evict_one(1, true)) return true;
    // Last: list memory itself goes, every list in it to ARAM (or forgotten,
    // made again when drawn), and the heap has its 1.25 MB block back. The
    // biggest level's file is 1.42 MB, read in one piece; after a few levels
    // the heap's biggest block was 13 KB short of it, and the game stopped
    // there (a black screen). Lists are made in the heap one by one until
    // list memory can be had again (begin_frame).
    if (g_lists) {
        GxWaitGpu();
        while (evict_one(0)) {
        }
        free(g_lists);
        g_lists = nullptr;
        g_lists_top = g_lists_live = 0;
        kListMemory = kListMin;
        g_lists_gone_frame = g_frame;
        SDL_Log("gx: list memory given back to the heap (an allocation needed it)");
        return true;
    }
    return false;
}

// How masks are drawn, switched on the pad for finding what differs on
// hardware (CastleGame: L + R + D-pad up): 0 by a range of depths (see
// "masks" below), 1 masked content where the depth equals the mask's (as
// before), 2 no masks at all.
int g_mask_mode = 0;

namespace {

// ---- the flicker detector
//
// The game's state changes only in a tick, and a frame is drawn two or
// three times a tick. After the stage is drawn, the frame is copied off the
// GPU, as intensities (I8, 8 x 4 tiles; whole: on hardware the copy's box
// filter to half took only the left half of the frame); the next frame, when
// the GPU is done with it, it is made a quarter-size picture (160 x 120) and
// compared, pixel by pixel (only while the heap has room for the copy, 300 KB:
// it is given back when a piece of 2 MB can't be had):
// - with the frame before, when no tick came between: any change is the
//   renderer's doing (FLICKER);
// - on the first frame after a tick, with the same frame two ticks before
//   and one tick before: a pixel back to what it was two ticks ago after
//   being different one tick ago is something going off and on (BLINK;
//   animation moves on rather than back).
// Changed pixels are counted in 10 x 10 blocks (16 x 12 pixels); the first
// few of each kind are saved to the SD card (/ppgc_flicker_N_*.pgm), the
// frames involved as pictures.

constexpr int kShotW = 640, kShotH = 480;  // the copy
constexpr int kPicW = 160, kPicH = 120;    // what is compared
constexpr int kBlocksX = 10, kBlocksY = 10;
constexpr int kDiff = 40, kSame = 12, kPixelsInBlock = 12;
uint8_t* g_shot = nullptr;
bool g_shot_pending = false, g_shot_ticked = false;
uint8_t g_pic[kPicH][kPicW], g_prev[kPicH][kPicW];  // this frame, the frame before
uint8_t g_tick1[kPicH][kPicW], g_tick2[kPicH][kPicW];  // first frames after the last two ticks
int g_have_prev = 0, g_have_ticks = 0;
uint32_t g_flicker_frames = 0, g_flicker_count[kBlocksX * kBlocksY];
uint32_t g_blink_frames = 0, g_blink_count[kBlocksX * kBlocksY];
int g_saved_flicker = 0, g_saved_blink = 0;  // on this screen
char g_scene[48] = "-";  // the movie and menu page (CastleGame: gx_set_scene)
int g_scenes_saved = 0;
// A filmstrip: after a screen has stayed for 150 ticks, its next 48 ticks'
// pictures saved (/ppgc_film_<screen>_NN.pgm), for what pops in and out
// slower than the blink test sees; a few screens only.
constexpr int kFilmTicks = 48, kFilmAfter = 150, kFilmScreens = 3;
int g_scene_ticks = 0, g_films = 0, g_film_frame = -1;

// The copy (8 x 4 tiles of I8) to a quarter-size picture: of each 4 x 4
// texels, the four on the diagonal averaged.
void make_pic() {
    for (int y = 0; y < kPicH; y++) {
        for (int x = 0; x < kPicW; x++) {
            int sum = 0;
            for (int d = 0; d < 4; d++) {
                int sx = x * 4 + d, sy = y * 4 + d;
                int tile = (sy / 4) * (kShotW / 8) + sx / 8;
                sum += g_shot[tile * 32 + (sy % 4) * 8 + sx % 8];
            }
            g_pic[y][x] = uint8_t(sum / 4);
        }
    }
}

void save_pgm(const char* name, const uint8_t (*pic)[kPicW]) {
    trace::SdLock lock;
    FILE* f = std::fopen(name, "wb");
    if (!f) return;
    std::fprintf(f, "P5 %d %d 255\n", kPicW, kPicH);
    std::fwrite(pic, 1, kPicW * kPicH, f);
    std::fclose(f);
}

// Pixels of `a` differing from `b` by kDiff, counted by block; with `c`,
// only those where `a` is within kSame of `c`. True if any block has enough.
bool compare(const uint8_t (*a)[kPicW], const uint8_t (*b)[kPicW], const uint8_t (*c)[kPicW], uint32_t* counts) {
    uint16_t in_block[kBlocksX * kBlocksY] = {};
    for (int y = 0; y < kPicH; y++) {
        for (int x = 0; x < kPicW; x++) {
            int d = int(a[y][x]) - int(b[y][x]);
            if (d < kDiff && d > -kDiff) continue;
            if (c) {
                int same = int(a[y][x]) - int(c[y][x]);
                if (same >= kSame || same <= -kSame) continue;
            }
            in_block[(y * kBlocksY / kPicH) * kBlocksX + x * kBlocksX / kPicW]++;
        }
    }
    bool any = false;
    for (int i = 0; i < kBlocksX * kBlocksY; i++) {
        if (in_block[i] >= kPixelsInBlock) {
            counts[i]++;
            any = true;
        }
    }
    return any;
}

// At the start of a frame: last frame's copy, compared.
void flicker_read() {
    if (!g_shot || !g_shot_pending) return;
    g_shot_pending = false;
    DCInvalidateRange(g_shot, kShotW * kShotH);
    make_pic();
    if (!g_shot_ticked && g_have_prev && compare(g_pic, g_prev, nullptr, g_flicker_count)) {
        g_flicker_frames++;
        if (g_saved_flicker < 1 && g_scenes_saved < 12) {
            char name[96];
            g_saved_flicker++;
            g_scenes_saved++;
            std::snprintf(name, sizeof(name), "/ppgc_flicker_%s_a.pgm", g_scene);
            save_pgm(name, g_prev);
            std::snprintf(name, sizeof(name), "/ppgc_flicker_%s_b.pgm", g_scene);
            save_pgm(name, g_pic);
            SDL_Log("gx: FLICKER pictures saved (%s)", name);
        }
    }
    if (g_shot_ticked) {
        g_scene_ticks++;
        bool filmed = std::strncmp(g_scene, "menu", 4) == 0 || std::strncmp(g_scene, "main", 4) == 0;
        if (filmed && g_film_frame < 0 && g_scene_ticks == kFilmAfter && g_films < kFilmScreens) {
            g_film_frame = 0;
            g_films++;
            SDL_Log("gx: filmstrip of %s: %d pictures to the SD card", g_scene, kFilmTicks);
        }
        if (g_film_frame >= 0) {
            char name[96];
            std::snprintf(name, sizeof(name), "/ppgc_film_%s_%02d.pgm", g_scene, g_film_frame);
            save_pgm(name, g_pic);
            // (the menu's castle wall, below the fire: dark when drawn)
            uint32_t wall = 0;
            for (int y = 75; y < 90; y++)
                for (int x = 115; x < 155; x++) wall += g_pic[y][x];
            SDL_Log("gx: film %02d wall %u", g_film_frame, unsigned(wall / (15 * 40)));
            if (++g_film_frame == kFilmTicks) g_film_frame = -1;
        }
        if (g_have_ticks >= 2 && compare(g_pic, g_tick1, g_tick2, g_blink_count)) {
            g_blink_frames++;
            // (on a screen's first blink after its first few seconds)
            if (g_saved_blink < 1 && g_blink_frames > 20 && g_scenes_saved < 12) {
                char name[96];
                g_saved_blink++;
                g_scenes_saved++;
                std::snprintf(name, sizeof(name), "/ppgc_blink_%s_a.pgm", g_scene);
                save_pgm(name, g_tick2);
                std::snprintf(name, sizeof(name), "/ppgc_blink_%s_b.pgm", g_scene);
                save_pgm(name, g_tick1);
                std::snprintf(name, sizeof(name), "/ppgc_blink_%s_c.pgm", g_scene);
                save_pgm(name, g_pic);
                SDL_Log("gx: BLINK pictures saved (%s)", name);
            }
        }
        std::memcpy(g_tick2, g_tick1, sizeof(g_tick1));
        std::memcpy(g_tick1, g_pic, sizeof(g_pic));
        if (g_have_ticks < 2) g_have_ticks++;
    }
    std::memcpy(g_prev, g_pic, sizeof(g_pic));
    g_have_prev = 1;
}

// At the end of the stage's drawing: this frame's copy (read next frame).
void flicker_copy(int efb_w, int efb_h, bool ticked) {
    if (efb_w != kShotW || efb_h != kShotH) return;
    // Only with room to spare (checked now and then): given back otherwise.
    static uint32_t checked = 0;
    if (g_frame >= checked + 120 || (!g_shot && g_frame >= checked + 30)) {
        checked = g_frame;
        bool room = memory::largest_free_kb() >= 2048 + (g_shot ? 0 : kShotW * kShotH / 1024);
        if (g_shot && !room && !g_shot_pending) {
            free(g_shot);
            g_shot = nullptr;
            g_have_prev = g_have_ticks = 0;
        } else if (!g_shot && room) {
            g_shot = static_cast<uint8_t*>(memalign(32, kShotW * kShotH));
            // The GPU writes it past the cache: lines the heap left dirty
            // mustn't be written back over the copy (as Octave's display
            // lists, GxUtils.cpp).
            if (g_shot) DCInvalidateRange(g_shot, kShotW * kShotH);
        }
    }
    if (!g_shot) return;
    GX_SetTexCopySrc(0, 0, uint16_t(efb_w), uint16_t(efb_h));
    GX_SetTexCopyDst(kShotW, kShotH, GX_TF_I8, GX_FALSE);
    GX_CopyTex(g_shot, GX_FALSE);
    GX_PixModeSync();
    g_shot_pending = true;
    g_shot_ticked = ticked;
}

// The blocks counted most, "(x,y)xN ..." into `text`.
int top_blocks(char* text, size_t size, const uint32_t* counts) {
    int n = 0;
    int shown[5];
    for (int k = 0; k < 5; k++) {
        int best = -1;
        for (int b = 0; b < kBlocksX * kBlocksY; b++) {
            bool taken = false;
            for (int j = 0; j < k; j++) taken = taken || shown[j] == b;
            if (!taken && counts[b] && (best < 0 || counts[b] > counts[best])) best = b;
        }
        if (best < 0) break;
        shown[k] = best;
        n += std::snprintf(text + n, size - size_t(n), " (%d,%d)x%u", best % kBlocksX, best / kBlocksX,
                           unsigned(counts[best]));
    }
    return n;
}

}  // namespace

int gx_mask_mode() {
    return g_mask_mode;
}

void gx_mismatch_log();

// A new screen: the flicker and blink counts start again (logged by screen).
void gx_set_scene(const char* name) {
    gx_mismatch_log();  // (the last screen's)
    std::snprintf(g_scene, sizeof(g_scene), "%s", name);
    for (char* c = g_scene; *c; c++)
        if (*c == ' ' || *c == '/') *c = '_';
    g_flicker_frames = g_blink_frames = 0;
    g_tints_over_1 = g_tints_over_2 = 0;
    std::memset(g_flicker_count, 0, sizeof(g_flicker_count));
    std::memset(g_blink_count, 0, sizeof(g_blink_count));
    g_saved_flicker = g_saved_blink = 0;
    g_have_ticks = 0;
    g_scene_ticks = 0;
    g_film_frame = -1;
}

void gx_set_mask_mode(int mode) {
    g_mask_mode = mode;
    SDL_Log("gx: masks %s", mode == 0 ? "by a range of depths" : mode == 1 ? "by an equal depth" : "off");
}

void gx_flicker_copy(int efb_w, int efb_h, bool ticked) {
#ifndef PPGC_DIAG
    // (diagnostic builds only: a copy of every frame off the GPU, and the
    // filmstrip's pictures written to the SD card, cost the game time)
    (void)efb_w;
    (void)efb_h;
    (void)ticked;
    return;
#endif
    flicker_copy(efb_w, efb_h, ticked);
}

// What differed from the PC's picture since the last call, into the log
// (nothing if nothing did).
void gx_mismatch_log() {
    // Flicker and blinks: blocks (of a 10 x 10 grid), the most first.
    static uint32_t flicker_seen = 0, blink_seen = 0;
    if (g_flicker_frames != flicker_seen || g_blink_frames != blink_seen) {
        char text[240];
        int n = std::snprintf(text, sizeof(text), "gx: %s: FLICKER %u frames:", g_scene, unsigned(g_flicker_frames));
        n += top_blocks(text + n, sizeof(text) - size_t(n), g_flicker_count);
        n += std::snprintf(text + n, sizeof(text) - size_t(n), "; BLINK %u ticks:", unsigned(g_blink_frames));
        n += top_blocks(text + n, sizeof(text) - size_t(n), g_blink_count);
        std::snprintf(text + n, sizeof(text) - size_t(n), " (masks mode %d; draws brightened 1-2x %u, over 2x %u)",
                      g_mask_mode, unsigned(g_tints_over_1), unsigned(g_tints_over_2));
        SDL_Log("%s", text);
        flicker_seen = g_flicker_frames;
        blink_seen = g_blink_frames;
    }
    static uint32_t checked_seen = 0;
    if (g_checked != checked_seen) {
        SDL_Log("gx: lists checked in memory: %u made (%u not as written; GX's size wrong for %u)",
                unsigned(g_checked), unsigned(g_bad_built), unsigned(g_gx_size_wrong));
        checked_seen = g_checked;
    }
    static Mismatches last;
    const Mismatches& m = g_miss;
    if (m.no_list_memory == last.no_list_memory && m.not_fetched == last.not_fetched &&
        m.retrying == last.retrying && m.bitmap_waiting == last.bitmap_waiting && m.tint_over == last.tint_over)
        return;
    SDL_Log("gx: mismatches so far: shapes not drawn %u (no list memory) %u (not back from ARAM) %u (waiting to be "
            "made); bitmaps not drawn %u; tints over 4x %u",
            unsigned(m.no_list_memory), unsigned(m.not_fetched), unsigned(m.retrying), unsigned(m.bitmap_waiting),
            unsigned(m.tint_over));
    last = m;
}

// For the GameCube's status line: what the display lists and textures take.
void gx_memory(uint32_t& shape_bytes, uint32_t& texture_bytes, uint32_t& aram_bytes, uint32_t& put_off) {
    shape_bytes = g_shape_bytes;
    texture_bytes = g_texture_bytes;
    aram_bytes = g_aram_used;
    put_off = g_lists_waits;
}

void Renderer::begin_frame(int window_width, int window_height, const swf::Rect& stage, swf::Rgba background,
                           bool transparent) {
    (void)background;
    (void)transparent;
    g_frame++;
    flicker_read();
    for (uint32_t h : g_pending_shapes) free_shape(h);
    for (uint32_t h : g_pending_textures) free_texture(h);
    g_pending_shapes.clear();
    g_pending_textures.clear();
    // List memory given back to the heap (gx_release_memory): taken again,
    // a second at a time, once the heap has it in one piece with room to
    // spare (the lists made in the heap meanwhile stay there until they go).
    if (!g_lists && g_frame >= g_lists_gone_frame + 60) {
        g_lists_gone_frame = g_frame;
        if (memory::largest_free_kb() * 1024 >= kListMin + kGrowSpare) {
            g_lists = static_cast<uint8_t*>(memalign(32, kListMin));
            if (g_lists) {
                g_lists_top = g_lists_live = 0;
                kListMemory = kListMin;
                SDL_Log("gx: list memory taken again");
            }
        }
    }
    // Room at the top of list memory for this frame's new lists: lists not
    // drawn lately go to ARAM (and if that isn't enough, those drawn before
    // this frame, drawn longest ago first), and what's left moves down.
    if (g_lists) {
        // A movie went (its lists are freed above): the least again.
        if (g_lists_to_least) {
            g_lists_to_least = false;
            if (kListMemory > kListMin) resize_lists(kListMin);
        }
        while (g_lists_live + kListRoom > kListMemory && evict_one(kEvictAge)) {
        }
        while (g_lists_live + kListRoom > kListMemory && evict_one(1)) {
        }
        if (g_lists_top + kListRoom > kListMemory && g_lists_top > g_lists_live) compact_lists();
        size_lists();
    }
    // Anamorphic: the 16:9 stage fills the whole 4:3 picture, squeezed, for
    // a 16:9 TV (or Dolphin at 16:9) to widen again, as GameCube games with a
    // widescreen mode do. No lines are lost to bars.
    GX_SetViewport(0.0f, 0.0f, float(window_width), float(window_height), 0, 1);
    GX_SetScissor(0, 0, uint32_t(window_width), uint32_t(window_height));

    Mtx44 projection;
    guOrtho(projection, float(stage.ymin), float(stage.ymax), float(stage.xmin), float(stage.xmax), -1.0f, 1.0f);
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);

    GX_SetVtxAttrFmt(kShapeFormat, GX_VA_POS, GX_POS_XY, GX_U16, 0);
    GX_SetVtxAttrFmt(kShapeFormat, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(kTexturedFormat, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(kTexturedFormat, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    g_desc = Desc::None;

    GX_SetCullMode(GX_CULL_NONE);
    // What Octave's world or UI may leave: no fog on the stage (Octave's
    // frame sets the world's), and stage 0 without swapped colours.
    GX_SetFog(GX_FOG_NONE, 0.0f, 1.0f, 0.1f, 1.0f, GXColor{0, 0, 0, 0});
    GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_SetAlphaUpdate(GX_FALSE);

    // The depth buffer as cleared, for the masks: the stage at the farthest.
    GX_SetColorUpdate(GX_FALSE);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    g_depth = kFarthest;
    draw_rect(stage, swf::Rgba{255, 255, 255, 255}, swf::Matrix{}, swf::CXform{});
    mask_level_ = 0;
    mask_content();
}

// ---- masks
//
// On the depth buffer, as the PC's are on the stencil buffer: each level of
// masking is a depth, nearer for each level in. A mask shape sets the next
// level's depth wherever it covers; what it masks is drawn where the depth
// is its level's or nearer (drawn half a level farther, passing where the
// buffer is at least as near: a test for the same depth failed on real
// hardware, where the depth written and the depth drawn round differently,
// and masked parts of the intro went missing); and the shape drawn again at
// the level before, passing only where it is nearer (so only on the inner
// level's pixels), puts it back. Unlike the PC's stencil, a mask inside a
// mask isn't cut to the outer one (the depth test compares with the value
// it writes); nor is castle.exe's, whose inner mask replaces the outer.

void Renderer::set_stencil(Stencil mode) { GX_SetColorUpdate(mode == Stencil::Write ? GX_FALSE : GX_TRUE); }
void Renderer::clear_stencil() {}

void Renderer::mask_content() {
    GX_SetColorUpdate(GX_TRUE);
    GX_SetZCompLoc(GX_TRUE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    if (mask_level_ == 0 || g_mask_mode == 2) {
        g_depth = level_depth(0);
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    } else if (g_mask_mode == 1) {
        g_depth = level_depth(mask_level_);
        GX_SetZMode(GX_TRUE, GX_EQUAL, GX_FALSE);
    } else {
        // (Smaller depth is nearer: this passes where the buffer holds this
        // level or a nearer one.)
        g_depth = level_depth(mask_level_) - 0.5f / 256.0f;
        GX_SetZMode(GX_TRUE, GX_GEQUAL, GX_FALSE);
    }
}

// The mask's own pixels only where it isn't see-through (as the PC's alpha
// test): the depth test after the alpha one.
static void mask_writes() {
    GX_SetColorUpdate(GX_FALSE);
    GX_SetZCompLoc(GX_FALSE);
    GX_SetAlphaCompare(GX_GREATER, 127, GX_AOP_AND, GX_ALWAYS, 0);
}

void Renderer::mask_begin() {
    mask_writes();
    g_depth = level_depth(mask_level_ + 1);
    GX_SetZMode(g_mask_mode == 2 ? GX_FALSE : GX_TRUE, GX_ALWAYS, g_mask_mode == 2 ? GX_FALSE : GX_TRUE);
}

void Renderer::mask_apply() {
    mask_level_++;
    mask_content();
}

void Renderer::mask_end_begin() {
    mask_writes();
    g_depth = level_depth(mask_level_ - 1);
    if (g_mask_mode == 2) GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    else GX_SetZMode(GX_TRUE, GX_GREATER, GX_TRUE);  // farther than the inner level: only its pixels
}

void Renderer::mask_end() {
    if (mask_level_ > 0) mask_level_--;
    mask_content();
}

void Renderer::set_transform(const swf::Matrix& matrix, const swf::CXform& cxform, bool textured) {
    load_matrix(matrix);
    set_tev(textured, cxform);
}

// ---- drawing

void Renderer::draw_shape(swf::Shape& shape, const swf::Matrix& matrix, const swf::CXform& cxform) {
    // One that couldn't be made for want of memory is tried again later.
    if (shape.gpu_mesh && g_shapes[shape.gpu_mesh - 1].list_size == 0 &&
        g_frame >= g_shapes[shape.gpu_mesh - 1].retry_frame) {
        free_shape(shape.gpu_mesh);
        shape.gpu_mesh = 0;
        shape.tessellated = false;
    }
    if (shape.gpu_mesh == 0) {
        // Room for its slot first: a list built before its slot is had isn't
        // in g_shapes, and an allocation failing then (the slot's) would
        // compact list memory, or give it back to the heap, over it.
        if (g_free_shapes.empty() && g_shapes.size() == g_shapes.capacity())
            g_shapes.reserve(g_shapes.size() + g_shapes.size() / 2 + 64);
        ShapeList list;
        bool made = false;  // or it has no triangles at all
        {
            // A shape parsed again from its record leaves nothing behind but
            // its list: the rest can come from scratch.
            trace::at(trace::kMain, "tessellating a shape");
            memory::Scratch scratch(shape.record != nullptr);
            shape.tessellate();
            if (!shape.out_of_memory) {
                list = build_shape_list(shape.mesh);
                made = list.list_size != 0 || shape.mesh.indices.empty();
            }
            shape.mesh = {};  // the display list is all that's needed now
        }
        trace::at(trace::kMain, "game render");
        if (list.no_room) {  // (list memory can't be had at all)
            shape.tessellated = false;
            first_miss(g_miss.no_list_memory, "a shape not drawn: no list memory");
            return;
        }
        if (!made) {
            SDL_Log(list.overflowed ? "gx: a shape's display list overflowed; trying again in a second"
                    : list.bad      ? "gx: a shape's list wasn't in memory as written, twice; trying again in a second"
                                    : "gx: out of memory for a shape; trying again in a second");
            list = ShapeList{};
            list.retry_frame = g_frame + 60;
        } else if (list.list_size == 0) {
            list.retry_frame = UINT32_MAX;  // nothing to draw: nothing to try again
        }
        list.owner = &shape;
        shape.gpu_mesh = add_slot(g_shapes, g_free_shapes, list);
    }
    ShapeList& s = g_shapes[shape.gpu_mesh - 1];
    s.last_frame = g_frame;
    if (s.list_size == 0) {
        if (s.retry_frame != UINT32_MAX) first_miss(g_miss.retrying, "a shape not drawn: it couldn't be made yet");
        return;
    }
    if (!s.block && !fetch_shape(s)) {  // in ARAM, and no room now: next frame
        first_miss(g_miss.not_fetched, "a shape not drawn: its list couldn't come back from ARAM");
        return;
    }
    use_desc(s.wide_colors ? Desc::WideShape : Desc::Shape);
    GX_SetArray(GX_VA_POS, const_cast<void*>(s.positions), 4);
    GX_SetArray(GX_VA_CLR0, const_cast<void*>(s.colors), 4);
    // The positions are steps from the shape's corner.
    swf::Matrix local;
    local.a = local.d = s.step;
    local.tx = s.origin_x;
    local.ty = s.origin_y;
    set_transform(matrix * local, cxform, false);
    GX_CallDispList(s.list, s.list_size);
}

void Renderer::draw_bitmap(swf::BitmapCharacter& bitmap, const swf::Matrix& matrix, const swf::CXform& cxform) {
    if (bitmap.texture == 0) {
        // One that couldn't be made is tried again a second later, not every
        // frame (in the keep, a sky that didn't fit logged 12,000 lines).
        auto retry = g_texture_retry.find(&bitmap);
        if (retry != g_texture_retry.end() && g_frame < retry->second) {
            first_miss(g_miss.bitmap_waiting, "a bitmap not drawn: its texture couldn't be made yet");
            return;
        }
        bitmap.texture = make_texture(bitmap.rgba.data(), bitmap.width, bitmap.height, bitmap.nearest);
        if (bitmap.texture == 0) {
            if (retry == g_texture_retry.end())
                SDL_Log("gx: out of memory for a %dx%d texture; trying again each second", bitmap.width, bitmap.height);
            g_texture_retry[&bitmap] = g_frame + 60;
            return;
        }
        if (retry != g_texture_retry.end()) g_texture_retry.erase(retry);
        // The texture is all that's needed now (white_ is drawn again and again).
        if (&bitmap != &white_) std::vector<uint8_t>().swap(bitmap.rgba);
    }
    const Texture& t = g_textures[bitmap.texture - 1];
    const swf::Rect& b = bitmap.bounds;
    use_desc(Desc::Textured);
    set_transform(matrix, cxform, true);
    GX_LoadTexObj(const_cast<GXTexObj*>(&t.obj), GX_TEXMAP0);
    GX_Begin(GX_QUADS, kTexturedFormat, 4);
    GX_Position2f32(float(b.xmin), float(b.ymin));
    GX_TexCoord2f32(0, 0);
    GX_Position2f32(float(b.xmax), float(b.ymin));
    GX_TexCoord2f32(t.u_max, 0);
    GX_Position2f32(float(b.xmax), float(b.ymax));
    GX_TexCoord2f32(t.u_max, t.v_max);
    GX_Position2f32(float(b.xmin), float(b.ymax));
    GX_TexCoord2f32(0, t.v_max);
    GX_End();
}

void Renderer::draw_text(text::Font& font, const std::vector<text::GlyphQuad>& quads, swf::Rgba color,
                         const swf::Matrix& matrix, const swf::CXform& cxform) {
    if (quads.empty()) return;
    if (font.texture == 0) {
        if (font.rgba.empty()) return;
        font.texture = make_texture(font.rgba.data(), font.page_width, font.page_height);
        if (font.texture == 0) return;
        std::vector<uint8_t>().swap(font.rgba);
    }
    const Texture& t = g_textures[font.texture - 1];
    const uint8_t channels[4] = {color.r, color.g, color.b, color.a};
    swf::CXform tinted;
    for (int i = 0; i < 4; i++) tinted.mul[i] = cxform.tint(i) * channels[i] / 255.0f * 256;
    use_desc(Desc::Textured);
    set_transform(matrix, tinted, true);
    GX_LoadTexObj(const_cast<GXTexObj*>(&t.obj), GX_TEXMAP0);
    for (size_t done = 0; done < quads.size();) {
        size_t n = std::min<size_t>(quads.size() - done, 0xfffc / 4);
        GX_Begin(GX_QUADS, kTexturedFormat, uint16_t(n * 4));
        for (size_t i = done; i < done + n; i++) {
            const auto& q = quads[i];
            float u0 = q.u0 * t.u_max, u1 = q.u1 * t.u_max, v0 = q.v0 * t.v_max, v1 = q.v1 * t.v_max;
            GX_Position2f32(q.x0, q.y0);
            GX_TexCoord2f32(u0, v0);
            GX_Position2f32(q.x1, q.y0);
            GX_TexCoord2f32(u1, v0);
            GX_Position2f32(q.x1, q.y1);
            GX_TexCoord2f32(u1, v1);
            GX_Position2f32(q.x0, q.y1);
            GX_TexCoord2f32(u0, v1);
        }
        GX_End();
        done += n;
    }
}

void Renderer::draw_rect(const swf::Rect& rect, swf::Rgba color, const swf::Matrix& matrix,
                         const swf::CXform& cxform) {
    if (white_.rgba.empty() && white_.texture == 0) {
        white_.width = white_.height = 1;
        white_.rgba = {255, 255, 255, 255};
    }
    white_.bounds = rect;
    const uint8_t channels[4] = {color.r, color.g, color.b, color.a};
    swf::CXform tinted;
    for (int i = 0; i < 4; i++) tinted.mul[i] = cxform.tint(i) * channels[i] / 255.0f * 256;
    draw_bitmap(white_, matrix, tinted);
}

}  // namespace render
