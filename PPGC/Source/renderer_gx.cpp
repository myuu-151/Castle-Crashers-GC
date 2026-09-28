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
#include <cstring>

#include <SDL3/SDL_log.h>

#include "aram_gc.h"
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
// short, and at once to the least when an allocation can't be had.
constexpr uint32_t kListMin = 1280 * 1024, kListMax = 2560 * 1024, kListStep = 512 * 1024;
uint32_t kListMemory = kListMin;  // (the size now)
constexpr uint32_t kListRoom = 256 * 1024;  // kept free at the top for a frame's new lists
uint8_t* g_lists = nullptr;
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
    if (can_grow && largest >= kListMemory + kListStep + 2048 * 1024)
        resize_lists(kListMemory + kListStep);
    else if (can_shrink && largest < 1536 * 1024)
        resize_lists(kListMemory - kListStep);
}

// List memory full in the middle of a frame (more lists drawn in it than
// fit): once the GPU has drawn all it has been given, any list can go to
// ARAM, and the rest move down. Slow, but everything is drawn.
bool make_list_room(uint32_t size) {
    if (!g_lists || size > kListRoom) return false;
    g_lists_waits++;
    trace::at(trace::kMain, "list memory full: waiting for the GPU");
    GX_DrawDone();
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

ShapeList build_shape(const swf::Mesh& mesh, uint32_t slack = 32);

// GX calls a list overflowed when its commands come near the end of the
// buffer, not only past it: one that did is built again with more room.
ShapeList build_shape_list(const swf::Mesh& mesh) {
    ShapeList s = build_shape(mesh);
    if (s.list_size == 0 && s.overflowed) s = build_shape(mesh, 32 + 256);
    return s;
}

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
    s.list_size = GX_EndDispList();
    if (s.list_size == 0) {
        list_free(s.block, s.block_size);
        ShapeList overflowed;
        overflowed.overflowed = true;
        return overflowed;
    }
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
        GX_DrawDone();
        if (resize_lists(kListMin)) return true;
    }
    return evict_one(1, true);
}

// What differed from the PC's picture since the last call, into the log
// (nothing if nothing did).
void gx_mismatch_log() {
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
    for (uint32_t h : g_pending_shapes) free_shape(h);
    for (uint32_t h : g_pending_textures) free_texture(h);
    g_pending_shapes.clear();
    g_pending_textures.clear();
    // Room at the top of list memory for this frame's new lists: lists not
    // drawn lately go to ARAM (and if that isn't enough, those drawn before
    // this frame, drawn longest ago first), and what's left moves down.
    if (g_lists) {
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
    if (mask_level_ == 0) {
        g_depth = level_depth(0);
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
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
    GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
}

void Renderer::mask_apply() {
    mask_level_++;
    mask_content();
}

void Renderer::mask_end_begin() {
    mask_writes();
    g_depth = level_depth(mask_level_ - 1);
    GX_SetZMode(GX_TRUE, GX_GREATER, GX_TRUE);  // farther than the inner level: only its pixels
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
