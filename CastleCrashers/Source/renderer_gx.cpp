// render::Renderer on the GameCube's GX, in place of engine/render/renderer.cpp
// (OpenGL). The same drawing: vertex-coloured triangles for shapes, textured
// quads for bitmaps and text, each with a matrix and the colour transform
// as a multiply. The class's OpenGL members are unused here.
//
// A movie's display lists and textures are freed when it is destroyed
// (swf::Movie::on_destroy). Not yet: masks (the GameCube has no stencil
// buffer; mask shapes are drawn nowhere and what they mask is drawn
// unmasked).
#include "render/renderer.h"

#include <gccore.h>
#include <malloc.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL_log.h>

#include "swf/movie.h"

namespace render {

namespace {

constexpr uint8_t kShapeFormat = GX_VTXFMT6;    // indexed position + colour
constexpr uint8_t kTexturedFormat = GX_VTXFMT7;  // direct position + uv

// A tessellated shape: its vertex arrays and a display list of indices into
// them, in one 32-byte-aligned block.
struct ShapeList {
    uint8_t* block = nullptr;
    uint32_t block_size = 0;
    const void* positions = nullptr;  // f32 x, y (twips)
    const void* colors = nullptr;     // RGBA8
    void* list = nullptr;
    uint32_t list_size = 0;
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

// Freed at the next begin_frame: the GPU may still be drawing the frame
// before from them (Octave waits for it before a frame begins).
std::vector<uint32_t> g_pending_shapes, g_pending_textures;

void free_shape(uint32_t handle) {
    ShapeList& s = g_shapes[handle - 1];
    if (s.block) {
        g_shape_bytes -= s.block_size;
        free(s.block);
    }
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
    for (auto& [id, ch] : movie.characters) {
        if (ch->type == swf::CharacterType::Shape) {
            auto& shape = static_cast<swf::ShapeCharacter&>(*ch).shape;
            if (shape.gpu_mesh) g_pending_shapes.push_back(shape.gpu_mesh);
            shape.gpu_mesh = 0;
        } else if (ch->type == swf::CharacterType::Bitmap) {
            auto& bitmap = static_cast<swf::BitmapCharacter&>(*ch);
            if (bitmap.texture) g_pending_textures.push_back(bitmap.texture);
            bitmap.texture = 0;
        }
    }
}

enum class Desc { None, Shape, Textured };
Desc g_desc = Desc::None;

uint32_t align32(uint32_t n) { return (n + 31) & ~31u; }

void use_desc(Desc d) {
    if (g_desc == d) return;
    g_desc = d;
    GX_ClearVtxDesc();
    if (d == Desc::Shape) {
        GX_SetVtxDesc(GX_VA_POS, GX_INDEX16);
        GX_SetVtxDesc(GX_VA_CLR0, GX_INDEX16);
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
    bool doubled = top > 1.0f;
    float k = doubled ? 0.5f : 1.0f;
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
    uint8_t scale = doubled ? GX_CS_SCALE_2 : GX_CS_SCALE_1;
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, scale, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, scale, GX_TRUE, GX_TEVPREV);
}

void load_matrix(const swf::Matrix& m) {
    Mtx mtx = {
        {m.a, m.c, 0, m.tx},
        {m.b, m.d, 0, m.ty},
        {0, 0, 1, 0},
    };
    GX_LoadPosMtxImm(mtx, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
}

ShapeList build_shape(const swf::Mesh& mesh) {
    ShapeList s;
    size_t count = mesh.indices.size() / 3 * 3;
    size_t vertices = mesh.vertices.size();
    if (count == 0) return s;
    if (vertices > 0xffff) {
        SDL_Log("gx: a shape of %u vertices is over the 16-bit index limit", unsigned(vertices));
        return s;
    }
    // Batches of up to 65535 indices (a multiple of 3), each a 3-byte header
    // then two 16-bit indices per vertex.
    const size_t per_batch = 65535;
    size_t batches = (count + per_batch - 1) / per_batch;
    uint32_t pos_size = align32(uint32_t(vertices * 8));
    uint32_t col_size = align32(uint32_t(vertices * 4));
    uint32_t list_size = align32(uint32_t(batches * 3 + count * 4)) + 32;
    s.block_size = pos_size + col_size + list_size;
    s.block = static_cast<uint8_t*>(memalign(32, s.block_size));
    if (!s.block) {
        SDL_Log("gx: out of memory for a shape (%u bytes)", unsigned(pos_size + col_size + list_size));
        return s;
    }
    float* pos = reinterpret_cast<float*>(s.block);
    uint8_t* col = s.block + pos_size;
    for (size_t i = 0; i < vertices; i++) {
        const swf::Vertex& v = mesh.vertices[i];
        pos[i * 2] = v.x;
        pos[i * 2 + 1] = v.y;
        col[i * 4] = v.color.r;
        col[i * 4 + 1] = v.color.g;
        col[i * 4 + 2] = v.color.b;
        col[i * 4 + 3] = v.color.a;
    }
    DCFlushRange(s.block, pos_size + col_size);
    s.positions = pos;
    s.colors = col;

    s.list = s.block + pos_size + col_size;
    DCInvalidateRange(s.list, list_size);
    GX_BeginDispList(s.list, list_size);
    for (size_t done = 0; done < count;) {
        size_t n = std::min(per_batch, count - done);
        GX_Begin(GX_TRIANGLES, kShapeFormat, uint16_t(n));
        for (size_t i = done; i < done + n; i++) {
            uint16_t index = uint16_t(mesh.indices[i]);
            GX_Position1x16(index);
            GX_Color1x16(index);
        }
        GX_End();
        done += n;
    }
    s.list_size = GX_EndDispList();
    if (s.list_size == 0) {
        SDL_Log("gx: a shape's display list overflowed");
        free(s.block);
        return ShapeList{};
    }
    // New arrays may sit where freed ones were.
    GX_InvVtxCache();
    g_shape_bytes += s.block_size;
    return s;
}

// RGBA (straight, rows top to bottom) -> a GX RGBA8 texture, halved until it
// fits GX's 1024x1024 limit and padded to whole 4x4 tiles.
uint32_t make_texture(const uint8_t* rgba, int width, int height) {
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
    // coordinates still span the full height), and an image that is all
    // greys (the font) is kept as intensity + alpha, half the size.
    int full_height = height;
    while (height > 1) {
        const uint8_t* row = rgba + size_t(height - 1) * size_t(width) * 4;
        bool empty = true;
        for (int x = 0; x < width && empty; x++) empty = row[x * 4 + 3] == 0;
        if (!empty) break;
        height--;
    }
    if (height < full_height) height++;  // one empty row, which clamping repeats
    bool grey = true;
    for (size_t i = 0, n = size_t(width) * size_t(height); i < n && grey; i++)
        grey = rgba[i * 4] == rgba[i * 4 + 1] && rgba[i * 4] == rgba[i * 4 + 2];
    int tw = (width + 3) & ~3, th = (height + 3) & ~3;
    uint32_t bytes = uint32_t(tw) * uint32_t(th) * (grey ? 2 : 4);
    uint8_t* texels = static_cast<uint8_t*>(memalign(32, bytes));
    if (!texels) {
        SDL_Log("gx: out of memory for a %dx%d texture", tw, th);
        return 0;
    }
    uint8_t* block = texels;
    const int tile_bytes = grey ? 32 : 64;
    for (int ty = 0; ty < th; ty += 4) {
        for (int tx = 0; tx < tw; tx += 4, block += tile_bytes) {
            for (int i = 0; i < 16; i++) {
                // The padding repeats the edge, so filtering at the edge
                // doesn't blend in anything else.
                int x = std::min(tx + (i & 3), width - 1), y = std::min(ty + (i >> 2), height - 1);
                const uint8_t* px = rgba + (size_t(y) * size_t(width) + size_t(x)) * 4;
                if (grey) {  // IA8: alpha, intensity
                    block[i * 2] = px[3];
                    block[i * 2 + 1] = px[0];
                    continue;
                }
                block[i * 2] = px[3];
                block[i * 2 + 1] = px[0];
                block[32 + i * 2] = px[1];
                block[32 + i * 2 + 1] = px[2];
            }
        }
    }
    DCFlushRange(texels, bytes);
    Texture t;
    t.texels = texels;
    t.bytes = bytes;
    t.u_max = float(width) / float(tw);
    t.v_max = float(full_height) / float(th);
    GX_InitTexObj(&t.obj, texels, uint16_t(tw), uint16_t(th), grey ? GX_TF_IA8 : GX_TF_RGBA8, GX_CLAMP, GX_CLAMP,
                  GX_FALSE);
    GX_InitTexObjFilterMode(&t.obj, GX_LINEAR, GX_LINEAR);
    g_texture_bytes += bytes;
    return add_slot(g_textures, g_free_textures, t);
}

}  // namespace

bool Renderer::init() {
    swf::Movie::on_destroy = release_movie;
    return true;
}

// For the GameCube's status line: what the display lists and textures take.
void gx_memory(uint32_t& shape_bytes, uint32_t& texture_bytes) {
    shape_bytes = g_shape_bytes;
    texture_bytes = g_texture_bytes;
}

void Renderer::begin_frame(int window_width, int window_height, const swf::Rect& stage, swf::Rgba background,
                           bool transparent) {
    (void)background;
    (void)transparent;
    for (uint32_t h : g_pending_shapes) free_shape(h);
    for (uint32_t h : g_pending_textures) free_texture(h);
    g_pending_shapes.clear();
    g_pending_textures.clear();
    float stage_w = float(stage.xmax - stage.xmin), stage_h = float(stage.ymax - stage.ymin);
    float scale = std::min(float(window_width) / stage_w, float(window_height) / stage_h);
    int w = int(stage_w * scale), h = int(stage_h * scale);
    int x = (window_width - w) / 2, y = (window_height - h) / 2;
    GX_SetViewport(float(x), float(y), float(w), float(h), 0, 1);
    GX_SetScissor(uint32_t(x), uint32_t(y), uint32_t(w), uint32_t(h));

    Mtx44 projection;
    guOrtho(projection, float(stage.ymin), float(stage.ymax), float(stage.xmin), float(stage.xmax), -1.0f, 1.0f);
    GX_LoadProjectionMtx(projection, GX_ORTHOGRAPHIC);

    GX_SetVtxAttrFmt(kShapeFormat, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(kShapeFormat, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(kTexturedFormat, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GX_SetVtxAttrFmt(kTexturedFormat, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    g_desc = Desc::None;

    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    mask_level_ = 0;
}

// ---- masks: not yet (see the top of the file)

void Renderer::set_stencil(Stencil mode) { GX_SetColorUpdate(mode == Stencil::Write ? GX_FALSE : GX_TRUE); }
void Renderer::clear_stencil() {}
void Renderer::mask_content() {}
void Renderer::mask_begin() { GX_SetColorUpdate(GX_FALSE); }
void Renderer::mask_apply() {
    mask_level_++;
    GX_SetColorUpdate(GX_TRUE);
}
void Renderer::mask_end_begin() { GX_SetColorUpdate(GX_FALSE); }
void Renderer::mask_end() {
    if (mask_level_ > 0) mask_level_--;
    GX_SetColorUpdate(GX_TRUE);
}

void Renderer::set_transform(const swf::Matrix& matrix, const swf::CXform& cxform, bool textured) {
    load_matrix(matrix);
    set_tev(textured, cxform);
}

// ---- drawing

void Renderer::draw_shape(swf::Shape& shape, const swf::Matrix& matrix, const swf::CXform& cxform) {
    if (shape.gpu_mesh == 0) {
        shape.tessellate();
        shape.gpu_mesh = add_slot(g_shapes, g_free_shapes, build_shape(shape.mesh));
        shape.mesh = {};  // the display list is all that's needed now
    }
    const ShapeList& s = g_shapes[shape.gpu_mesh - 1];
    if (s.list_size == 0) return;
    use_desc(Desc::Shape);
    GX_SetArray(GX_VA_POS, const_cast<void*>(s.positions), 8);
    GX_SetArray(GX_VA_CLR0, const_cast<void*>(s.colors), 4);
    set_transform(matrix, cxform, false);
    GX_CallDispList(s.list, s.list_size);
}

void Renderer::draw_bitmap(swf::BitmapCharacter& bitmap, const swf::Matrix& matrix, const swf::CXform& cxform) {
    if (bitmap.texture == 0) {
        bitmap.texture = make_texture(bitmap.rgba.data(), bitmap.width, bitmap.height);
        // The texture is all that's needed now (white_ is drawn again and again).
        if (bitmap.texture && &bitmap != &white_) std::vector<uint8_t>().swap(bitmap.rgba);
        if (bitmap.texture == 0) return;
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
