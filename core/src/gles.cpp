// DidjDroid - OpenGL ES 1.1 software implementation (HLE of libopengles_lite.so)
#include "gles.h"
#include "hle.h"
#include "didj.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace didj {

// ---------------------------------------------------------------------------
// Function table
// ---------------------------------------------------------------------------
enum Fn {
#define X(n) F_##n,
#include "gles_funcs.inc"
#undef X
    F_COUNT
};
static const char* const g_names[] = {
#define X(n) #n,
#include "gles_funcs.inc"
#undef X
};

// GL enums used below
enum {
    GL_NO_ERROR = 0, GL_INVALID_ENUM = 0x500, GL_INVALID_VALUE = 0x501, GL_INVALID_OPERATION = 0x502,
    GL_BYTE = 0x1400, GL_UNSIGNED_BYTE = 0x1401, GL_SHORT = 0x1402, GL_UNSIGNED_SHORT = 0x1403,
    GL_FLOAT = 0x1406, GL_FIXED = 0x140C,
    GL_POINTS = 0, GL_LINES = 1, GL_LINE_LOOP = 2, GL_LINE_STRIP = 3, GL_TRIANGLES = 4,
    GL_TRIANGLE_STRIP = 5, GL_TRIANGLE_FAN = 6,
    GL_MODELVIEW = 0x1700, GL_PROJECTION = 0x1701, GL_TEXTURE = 0x1702,
    GL_TEXTURE_2D = 0x0DE1, GL_CULL_FACE = 0x0B44, GL_BLEND = 0x0BE2, GL_ALPHA_TEST = 0x0BC0,
    GL_DEPTH_TEST = 0x0B71, GL_FOG = 0x0B60, GL_LIGHTING = 0x0B50, GL_LIGHT0 = 0x4000,
    GL_COLOR_MATERIAL = 0x0B57, GL_SCISSOR_TEST = 0x0C11, GL_NORMALIZE = 0x0BA1, GL_RESCALE_NORMAL = 0x803A,
    GL_POLYGON_OFFSET_FILL = 0x8037, GL_MATRIX_PALETTE_OES = 0x8840,
    GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY = 0x8075, GL_COLOR_ARRAY = 0x8076, GL_TEXTURE_COORD_ARRAY = 0x8078,
    GL_POINT_SIZE_ARRAY_OES = 0x8B9C, GL_MATRIX_INDEX_ARRAY_OES = 0x8844, GL_WEIGHT_ARRAY_OES = 0x86AD,
    GL_TEXTURE0 = 0x84C0,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_RGBA = 0x1908, GL_RGB = 0x1907, GL_ALPHA = 0x1906, GL_LUMINANCE = 0x1909, GL_LUMINANCE_ALPHA = 0x190A,
    GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034, GL_UNSIGNED_SHORT_5_6_5 = 0x8363,
    GL_NEAREST = 0x2600, GL_LINEAR = 0x2601, GL_REPEAT = 0x2901, GL_CLAMP_TO_EDGE = 0x812F,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803, GL_GENERATE_MIPMAP = 0x8191, GL_TEXTURE_CROP_RECT_OES = 0x8B9D,
    GL_TEXTURE_ENV = 0x2300, GL_TEXTURE_ENV_MODE = 0x2200, GL_TEXTURE_ENV_COLOR = 0x2201,
    GL_MODULATE = 0x2100, GL_DECAL = 0x2101, GL_ADD = 0x0104, GL_REPLACE = 0x1E01, GL_BLEND_ENV = 0x0BE2,
    GL_COMBINE = 0x8570, GL_COMBINE_RGB = 0x8571, GL_COMBINE_ALPHA = 0x8572,
    GL_FLAT = 0x1D00, GL_SMOOTH = 0x1D01, GL_FRONT = 0x404, GL_BACK = 0x405, GL_FRONT_AND_BACK = 0x408,
    GL_CW = 0x900, GL_CCW = 0x901,
    GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x100,
    GL_FOG_DENSITY = 0xB62, GL_FOG_START = 0xB63, GL_FOG_END = 0xB64, GL_FOG_MODE = 0xB65, GL_FOG_COLOR = 0xB66,
    GL_EXP = 0x800, GL_EXP2 = 0x801,
    GL_AMBIENT = 0x1200, GL_DIFFUSE = 0x1201, GL_SPECULAR = 0x1202, GL_POSITION = 0x1203,
    GL_SPOT_DIRECTION = 0x1204, GL_SPOT_EXPONENT = 0x1205, GL_SPOT_CUTOFF = 0x1206,
    GL_CONSTANT_ATTENUATION = 0x1207, GL_LINEAR_ATTENUATION = 0x1208, GL_QUADRATIC_ATTENUATION = 0x1209,
    GL_EMISSION = 0x1600, GL_SHININESS = 0x1601, GL_AMBIENT_AND_DIFFUSE = 0x1602,
    GL_LIGHT_MODEL_TWO_SIDE = 0xB52, GL_LIGHT_MODEL_AMBIENT = 0xB53,
    GL_UNPACK_ALIGNMENT = 0xCF5,
};

// Enable bits
enum {
    E_CULL = 1 << 0, E_BLEND = 1 << 1, E_ALPHA = 1 << 2, E_DEPTH = 1 << 3, E_FOG = 1 << 4, E_LIGHTING = 1 << 5,
    E_COLOR_MAT = 1 << 6, E_SCISSOR = 1 << 7, E_NORMALIZE = 1 << 8, E_RESCALE = 1 << 9, E_POLYOFF = 1 << 10,
    E_PALETTE = 1 << 11, E_LIGHT0 = 1 << 16
};
enum { C_VERTEX = 1, C_NORMAL = 2, C_COLOR = 4, C_TEX0 = 8, C_TEX1 = 16, C_PSIZE = 32, C_MIDX = 64, C_WEIGHT = 128 };

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

GLES::GLES(Hle* h) : hle(h) { reset(); }

void GLES::reset() {
    call_counts.assign(F_COUNT + 1, 0);
    err = 0;
    for (auto& s : stacks) { s.clear(); s.push_back(Mat4::identity()); }
    matrix_mode = 0; active_tex = client_tex = 0;
    for (auto& l : lights) l = Light();
    lights[0].diffuse[0] = lights[0].diffuse[1] = lights[0].diffuse[2] = 1;
    lights[0].specular[0] = lights[0].specular[1] = lights[0].specular[2] = 1;
    mat = Material();
    enables = 0; tex_enabled[0] = tex_enabled[1] = false; client_states = 0;
    textures.clear(); buffers.clear(); guest_strings.clear();
    bound_tex[0] = bound_tex[1] = 0; bound_array = bound_element = 0;
    next_tex = next_buf = 1;
    color.assign(W * H, 0xFF000000u);
    depth.assign(W * H, 1.0f);
    swaps = 0; last_swap_frame = ~0ull;
}

int GLES::find(const char* name) {
    for (int i = 0; i < F_COUNT; i++) if (!strcmp(g_names[i], name)) return i;
    return -1;
}

u32 GLES::a(int i) { return hle->arg(i); }
float GLES::af(int i) { return hle->argf(i); }
float GLES::ax(int i) { return (float)(s32)hle->arg(i) / 65536.0f; }
void GLES::ret(u32 v) { hle->cpu->hle_return(v); }

u32 GLES::guest_string(u32 key, const char* s) {
    auto it = guest_strings.find(key);
    if (it != guest_strings.end()) return it->second;
    u32 len = (u32)strlen(s) + 1;
    u32 addr = hle->scratch_alloc(len);
    if (addr) { hle->write_mem(addr, s, len); guest_strings[key] = addr; }
    return addr;
}

std::vector<Mat4>& GLES::cur_stack() {
    if (matrix_mode == 2) return stacks[2 + active_tex];
    return stacks[matrix_mode];
}
Mat4& GLES::cur_matrix() { return cur_stack().back(); }

static Mat4 mat_rotate(float ang, float x, float y, float z) {
    float l = sqrtf(x * x + y * y + z * z);
    Mat4 r = Mat4::identity();
    if (l == 0) return r;
    x /= l; y /= l; z /= l;
    float c = cosf(ang * (float)M_PI / 180.f), s = sinf(ang * (float)M_PI / 180.f), t = 1 - c;
    r.m[0] = x * x * t + c;     r.m[4] = x * y * t - z * s; r.m[8] = x * z * t + y * s;
    r.m[1] = y * x * t + z * s; r.m[5] = y * y * t + c;     r.m[9] = y * z * t - x * s;
    r.m[2] = x * z * t - y * s; r.m[6] = y * z * t + x * s; r.m[10] = z * z * t + c;
    return r;
}
static Mat4 mat_ortho(float l, float r, float b, float t, float n, float f) {
    Mat4 m = Mat4::identity();
    m.m[0] = 2 / (r - l); m.m[5] = 2 / (t - b); m.m[10] = -2 / (f - n);
    m.m[12] = -(r + l) / (r - l); m.m[13] = -(t + b) / (t - b); m.m[14] = -(f + n) / (f - n);
    return m;
}
static Mat4 mat_frustum(float l, float r, float b, float t, float n, float f) {
    Mat4 m; memset(m.m, 0, sizeof m.m);
    m.m[0] = 2 * n / (r - l); m.m[5] = 2 * n / (t - b);
    m.m[8] = (r + l) / (r - l); m.m[9] = (t + b) / (t - b); m.m[10] = -(f + n) / (f - n); m.m[11] = -1;
    m.m[14] = -2 * f * n / (f - n);
    return m;
}
static void mat_vec(const Mat4& m, const float* v, float* o) {
    for (int i = 0; i < 4; i++) o[i] = m.m[i] * v[0] + m.m[4 + i] * v[1] + m.m[8 + i] * v[2] + m.m[12 + i] * v[3];
}

// ---------------------------------------------------------------------------
// Enables & queries
// ---------------------------------------------------------------------------
void GLES::enable(u32 cap, bool on) {
    u32 bitv = 0;
    switch (cap) {
    case GL_TEXTURE_2D: tex_enabled[active_tex] = on; return;
    case GL_CULL_FACE: bitv = E_CULL; break;
    case GL_BLEND: bitv = E_BLEND; break;
    case GL_ALPHA_TEST: bitv = E_ALPHA; break;
    case GL_DEPTH_TEST: bitv = E_DEPTH; break;
    case GL_FOG: bitv = E_FOG; break;
    case GL_LIGHTING: bitv = E_LIGHTING; break;
    case GL_COLOR_MATERIAL: bitv = E_COLOR_MAT; break;
    case GL_SCISSOR_TEST: bitv = E_SCISSOR; break;
    case GL_NORMALIZE: bitv = E_NORMALIZE; break;
    case GL_RESCALE_NORMAL: bitv = E_RESCALE; break;
    case GL_POLYGON_OFFSET_FILL: bitv = E_POLYOFF; break;
    case GL_MATRIX_PALETTE_OES: bitv = E_PALETTE; break;
    default:
        if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8) bitv = E_LIGHT0 << (cap - GL_LIGHT0);
        else return;
    }
    if (on) enables |= bitv; else enables &= ~bitv;
}

bool GLES::is_enabled(u32 cap) {
    switch (cap) {
    case GL_TEXTURE_2D: return tex_enabled[active_tex];
    case GL_CULL_FACE: return enables & E_CULL;
    case GL_BLEND: return enables & E_BLEND;
    case GL_ALPHA_TEST: return enables & E_ALPHA;
    case GL_DEPTH_TEST: return enables & E_DEPTH;
    case GL_FOG: return enables & E_FOG;
    case GL_LIGHTING: return enables & E_LIGHTING;
    case GL_COLOR_MATERIAL: return enables & E_COLOR_MAT;
    case GL_SCISSOR_TEST: return enables & E_SCISSOR;
    case GL_NORMALIZE: return enables & E_NORMALIZE;
    case GL_RESCALE_NORMAL: return enables & E_RESCALE;
    case GL_POLYGON_OFFSET_FILL: return enables & E_POLYOFF;
    case GL_VERTEX_ARRAY: return client_states & C_VERTEX;
    case GL_NORMAL_ARRAY: return client_states & C_NORMAL;
    case GL_COLOR_ARRAY: return client_states & C_COLOR;
    case GL_TEXTURE_COORD_ARRAY: return client_states & (C_TEX0 << client_tex);
    default:
        if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8) return enables & (E_LIGHT0 << (cap - GL_LIGHT0));
        return false;
    }
}

void GLES::get_values(u32 p, std::vector<float>& o, bool* ints) {
    *ints = true;
    auto m16 = [&](const Mat4& m) { o.assign(m.m, m.m + 16); *ints = false; };
    switch (p) {
    case 0x0D33: o = {1024}; break;                      // MAX_TEXTURE_SIZE
    case 0x84E2: o = {2}; break;                         // MAX_TEXTURE_UNITS
    case 0x0D31: o = {8}; break;                         // MAX_LIGHTS
    case 0x0D32: o = {1}; break;                         // MAX_CLIP_PLANES
    case 0x0D36: o = {16}; break;                        // MAX_MODELVIEW_STACK_DEPTH
    case 0x0D38: case 0x0D39: o = {2}; break;
    case 0x0D3A: o = {1024, 1024}; break;
    case 0x0BA2: o = {(float)vp[0], (float)vp[1], (float)vp[2], (float)vp[3]}; break;
    case 0x0BA0: o = {(float)(GL_MODELVIEW + matrix_mode)}; break;
    case 0x0D52: o = {5}; break; case 0x0D53: o = {6}; break; case 0x0D54: o = {5}; break;
    case 0x0D55: o = {0}; break; case 0x0D56: o = {16}; break; case 0x0D57: o = {0}; break;
    case 0x0D50: o = {4}; break;
    case 0x8069: o = {(float)bound_tex[active_tex]}; break;
    case 0x8894: o = {(float)bound_array}; break;
    case 0x8895: o = {(float)bound_element}; break;
    case 0x84E0: o = {(float)(GL_TEXTURE0 + active_tex)}; break;
    case 0x84E1: o = {(float)(GL_TEXTURE0 + client_tex)}; break;
    case 0x0BA3: o = {(float)stacks[0].size()}; break;
    case 0x0BA4: o = {(float)stacks[1].size()}; break;
    case 0x0BA5: o = {(float)stacks[2 + active_tex].size()}; break;
    case 0x0BA6: m16(stacks[0].back()); break;
    case 0x0BA7: m16(stacks[1].back()); break;
    case 0x0BA8: m16(stacks[2 + active_tex].back()); break;
    case 0x898D: case 0x898E: case 0x898F: {            // *_MATRIX_FLOAT_AS_INT_BITS_OES
        const Mat4& m = p == 0x898D ? stacks[0].back() : p == 0x898E ? stacks[1].back() : stacks[2 + active_tex].back();
        o.resize(16);
        for (int i = 0; i < 16; i++) { u32 b; memcpy(&b, &m.m[i], 4); o[i] = (float)(s32)b; }
        break;
    }
    case 0x86A2: o = {10}; break;
    case 0x86A3: o.clear(); for (int i = 0; i < 10; i++) o.push_back((float)(0x8B90 + i)); break;
    case 0x0C22: o.assign(clear_color, clear_color + 4); *ints = false; break;
    case 0x0B73: o = {clear_depth}; *ints = false; break;
    case 0x0D05: case 0x0CF5: o = {(float)unpack_align}; break;
    case 0x8B9A: o = {(float)GL_UNSIGNED_BYTE}; break;
    case 0x8B9B: o = {(float)GL_RGBA}; break;
    case 0x846D: o = {1, 64}; break;
    case 0x846E: case 0x0B12: case 0x0B22: o = {1, 1}; break;
    case 0x0B21: o = {line_width}; *ints = false; break;
    case 0x0B11: o = {point_size}; *ints = false; break;
    case 0x0BC1: o = {alpha_ref}; *ints = false; break;
    case 0x0BC2: o = {(float)alpha_func}; break;
    case 0x0BE0: o = {(float)blend_dst}; break;
    case 0x0BE1: o = {(float)blend_src}; break;
    case 0x0B74: o = {(float)depth_func}; break;
    case 0x0B70: o = {depth_n, depth_f}; *ints = false; break;
    case 0x0B72: o = {depth_mask ? 1.f : 0.f}; break;
    case 0x0C23: o = {(float)color_mask[0], (float)color_mask[1], (float)color_mask[2], (float)color_mask[3]}; break;
    case 0x0B45: o = {(float)cull_mode}; break;
    case 0x0B46: o = {(float)front_face}; break;
    case 0x0B54: o = {(float)shade_model}; break;
    case 0x0C10: o = {(float)scissor[0], (float)scissor[1], (float)scissor[2], (float)scissor[3]}; break;
    case 0x0B00: o.assign(cur_color, cur_color + 4); *ints = false; break;
    case 0x0B02: o.assign(cur_normal, cur_normal + 3); *ints = false; break;
    case 0x0B03: o.assign(cur_tc[active_tex], cur_tc[active_tex] + 4); *ints = false; break;
    case 0x8842: o = {9}; break;                         // MAX_PALETTE_MATRICES_OES
    case 0x86A4: o = {3}; break;                         // MAX_VERTEX_UNITS_OES
    case 0x0D3B: o = {1}; break;
    default:
        o = {is_enabled(p) ? 1.f : 0.f};
        break;
    }
}

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------
GLES::Texture* GLES::cur_texture() {
    return &textures[bound_tex[active_tex]];
}

static inline u32 argb(u32 r, u32 g, u32 b, u32 a) { return (a << 24) | (r << 16) | (g << 8) | b; }

void GLES::tex_image(int level, u32 ifmt, int w, int h, u32 fmt, u32 type, u32 pixels, bool sub, int xo, int yo) {
    (void)ifmt;
    if (level < 0 || level > 12 || w < 0 || h < 0) { set_error(GL_INVALID_VALUE); return; }
    Texture& t = *cur_texture();
    int bpp;
    switch (type) {
    case GL_UNSIGNED_BYTE:
        bpp = fmt == GL_RGBA ? 4 : fmt == GL_RGB ? 3 : fmt == GL_LUMINANCE_ALPHA ? 2 : 1;
        break;
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1:
        bpp = 2; break;
    default: set_error(GL_INVALID_ENUM); return;
    }
    if (!sub) {
        if ((int)t.levels.size() <= level) { t.levels.resize(level + 1); t.lw.resize(level + 1); t.lh.resize(level + 1); }
        t.levels[level].assign((size_t)w * h, 0xFF000000u);
        t.lw[level] = w; t.lh[level] = h;
        if (level == 0) { t.w = w; t.h = h; }
        t.has_alpha = (fmt == GL_RGBA || fmt == GL_ALPHA || fmt == GL_LUMINANCE_ALPHA);
        xo = yo = 0;
    } else if ((int)t.levels.size() <= level || t.levels[level].empty()) { set_error(GL_INVALID_OPERATION); return; }
    if (!pixels || w == 0 || h == 0) { if (level == 0 && t.gen_mipmap) gen_mips(t); return; }
    int row = w * bpp;
    int stride = (row + unpack_align - 1) / unpack_align * unpack_align;
    std::vector<u8> src((size_t)stride * h);
    hle->read_mem(pixels, src.data(), (u32)src.size());
    std::vector<u32>& dst = t.levels[level];
    int dw = t.lw[level], dh = t.lh[level];
    for (int y = 0; y < h; y++) {
        const u8* p = &src[(size_t)y * stride];
        int ty = y + yo;
        if (ty < 0 || ty >= dh) continue;
        for (int x = 0; x < w; x++) {
            int tx = x + xo;
            if (tx < 0 || tx >= dw) continue;
            u32 c;
            if (type == GL_UNSIGNED_BYTE) {
                const u8* q = p + x * bpp;
                switch (fmt) {
                case GL_RGBA: c = argb(q[0], q[1], q[2], q[3]); break;
                case GL_RGB: c = argb(q[0], q[1], q[2], 255); break;
                case GL_LUMINANCE: c = argb(q[0], q[0], q[0], 255); break;
                case GL_ALPHA: c = argb(255, 255, 255, q[0]); break;
                default: c = argb(q[0], q[0], q[0], q[1]); break;
                }
            } else {
                u16 v = (u16)(p[x * 2] | (p[x * 2 + 1] << 8));
                if (type == GL_UNSIGNED_SHORT_5_6_5)
                    c = argb(((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31, 255);
                else if (type == GL_UNSIGNED_SHORT_4_4_4_4)
                    c = argb(((v >> 12) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17);
                else
                    c = argb(((v >> 11) & 31) * 255 / 31, ((v >> 6) & 31) * 255 / 31, ((v >> 1) & 31) * 255 / 31, (v & 1) ? 255 : 0);
            }
            dst[(size_t)ty * dw + tx] = c;
        }
    }
    if (level == 0 && t.gen_mipmap) gen_mips(t);
}

void GLES::compressed_tex_image(int level, u32 ifmt, int w, int h, u32 size, u32 data) {
    if (ifmt < 0x8B90 || ifmt > 0x8B99) {
        if (hle->log) { char b[80]; snprintf(b, sizeof b, "[gl] unsupported compressed format %04X", ifmt); hle->log(b); }
        set_error(GL_INVALID_ENUM);
        return;
    }
    std::vector<u8> src(size);
    hle->read_mem(data, src.data(), size);
    bool pal4 = ifmt <= 0x8B94;
    u32 kind = (ifmt - 0x8B90) % 5;   // 0 RGB8, 1 RGBA8, 2 R5G6B5, 3 RGBA4, 4 RGB5A1
    int entries = pal4 ? 16 : 256;
    int esz = kind == 0 ? 3 : kind == 1 ? 4 : 2;
    u32 pal[256];
    for (int i = 0; i < entries; i++) {
        const u8* q = &src[(size_t)i * esz];
        if ((size_t)(i + 1) * esz > src.size()) { pal[i] = 0; continue; }
        u16 v = (u16)(q[0] | (q[1] << 8));
        switch (kind) {
        case 0: pal[i] = argb(q[0], q[1], q[2], 255); break;
        case 1: pal[i] = argb(q[0], q[1], q[2], q[3]); break;
        case 2: pal[i] = argb(((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31, 255); break;
        case 3: pal[i] = argb(((v >> 12) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17); break;
        default: pal[i] = argb(((v >> 11) & 31) * 255 / 31, ((v >> 6) & 31) * 255 / 31, ((v >> 1) & 31) * 255 / 31, (v & 1) ? 255 : 0); break;
        }
    }
    Texture& t = *cur_texture();
    int nlevels = level <= 0 ? 1 - level : 1;
    int base = level > 0 ? level : 0;
    size_t off = (size_t)entries * esz;
    int lw = w, lh = h;
    t.has_alpha = (kind == 1 || kind == 3 || kind == 4);
    for (int l = 0; l < nlevels; l++) {
        int L = base + l;
        if ((int)t.levels.size() <= L) { t.levels.resize(L + 1); t.lw.resize(L + 1); t.lh.resize(L + 1); }
        t.levels[L].assign((size_t)lw * lh, 0);
        t.lw[L] = lw; t.lh[L] = lh;
        if (L == 0) { t.w = lw; t.h = lh; }
        for (int i = 0; i < lw * lh; i++) {
            u32 idx;
            if (pal4) {
                size_t bi = off + i / 2;
                u8 byte = bi < src.size() ? src[bi] : 0;
                idx = (i & 1) ? (byte & 15) : (byte >> 4);
            } else {
                size_t bi = off + i;
                idx = bi < src.size() ? src[bi] : 0;
            }
            t.levels[L][i] = pal[idx];
        }
        off += pal4 ? (size_t)(lw * lh + 1) / 2 : (size_t)lw * lh;
        lw = std::max(1, lw / 2); lh = std::max(1, lh / 2);
    }
}

void GLES::gen_mips(Texture& t) {
    if (t.levels.empty() || t.levels[0].empty()) return;
    int w = t.lw[0], h = t.lh[0];
    int L = 0;
    while (w > 1 || h > 1) {
        int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<u32> n((size_t)nw * nh);
        const std::vector<u32>& s = t.levels[L];
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                u32 acc[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; k++) {
                    int sx = std::min(w - 1, x * 2 + (k & 1)), sy = std::min(h - 1, y * 2 + (k >> 1));
                    u32 c = s[(size_t)sy * w + sx];
                    for (int ch = 0; ch < 4; ch++) acc[ch] += (c >> (ch * 8)) & 0xFF;
                }
                n[(size_t)y * nw + x] = (acc[0] / 4) | ((acc[1] / 4) << 8) | ((acc[2] / 4) << 16) | ((acc[3] / 4) << 24);
            }
        L++;
        if ((int)t.levels.size() <= L) { t.levels.resize(L + 1); t.lw.resize(L + 1); t.lh.resize(L + 1); }
        t.levels[L] = n; t.lw[L] = nw; t.lh[L] = nh;
        w = nw; h = nh;
    }
}

static inline int wrap_coord(int c, int size, u32 mode) {
    if (mode == GL_REPEAT) { c %= size; if (c < 0) c += size; return c; }
    return c < 0 ? 0 : (c >= size ? size - 1 : c);
}

u32 GLES::sample(const Texture& t, float s, float tt, float lod) {
    bool minify = lod > 0;
    u32 filt = minify ? t.min_filter : t.mag_filter;
    int L = 0;
    if (minify && filt >= 0x2700 && filt <= 0x2703) {
        L = (int)(lod + 0.5f);
        int maxl = (int)t.levels.size() - 1;
        while (maxl > 0 && t.levels[maxl].empty()) maxl--;
        if (L > maxl) L = maxl;
        filt = (filt == 0x2700 || filt == 0x2702) ? GL_NEAREST : GL_LINEAR;
    }
    if (L >= (int)t.levels.size() || t.levels[L].empty()) L = 0;
    const std::vector<u32>& d = t.levels[L];
    int w = t.lw[L], h = t.lh[L];
    if (w <= 0 || h <= 0 || d.empty()) return 0xFFFFFFFF;
    float u = s * w, v = tt * h;
    if (filt == GL_NEAREST) {
        int x = wrap_coord((int)floorf(u), w, t.wrap_s), y = wrap_coord((int)floorf(v), h, t.wrap_t);
        return d[(size_t)y * w + x];
    }
    u -= 0.5f; v -= 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    float fx = u - x0, fy = v - y0;
    int xa = wrap_coord(x0, w, t.wrap_s), xb = wrap_coord(x0 + 1, w, t.wrap_s);
    int ya = wrap_coord(y0, h, t.wrap_t), yb = wrap_coord(y0 + 1, h, t.wrap_t);
    u32 c00 = d[(size_t)ya * w + xa], c10 = d[(size_t)ya * w + xb], c01 = d[(size_t)yb * w + xa], c11 = d[(size_t)yb * w + xb];
    u32 out = 0;
    for (int ch = 0; ch < 32; ch += 8) {
        float a0 = (float)((c00 >> ch) & 0xFF), a1 = (float)((c10 >> ch) & 0xFF);
        float b0 = (float)((c01 >> ch) & 0xFF), b1 = (float)((c11 >> ch) & 0xFF);
        float top = a0 + (a1 - a0) * fx, bot = b0 + (b1 - b0) * fx;
        u32 val = (u32)(top + (bot - top) * fy + 0.5f);
        out |= (val > 255 ? 255 : val) << ch;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Vertex processing
// ---------------------------------------------------------------------------
// Arrays are snapshotted per draw call into host memory for speed.
struct ArraySnap { std::vector<u8> data; u32 base = 0; bool valid = false; };
static ArraySnap g_snap[8];

static int type_size(u32 t) {
    switch (t) { case GL_BYTE: case GL_UNSIGNED_BYTE: return 1; case GL_SHORT: case GL_UNSIGNED_SHORT: return 2; default: return 4; }
}

void GLES::fetch_attr(const Pointer& p, u32 idx, int comps, float* out, bool norm_ubyte) {
    // locate snapshot by pointer identity
    int slot = (&p == &vptr) ? 0 : (&p == &cptr) ? 1 : (&p == &nptr) ? 2 : (&p == &tptr[0]) ? 3 : (&p == &tptr[1]) ? 4 : 5;
    ArraySnap& sn = g_snap[slot];
    int ts = type_size(p.type);
    int stride = p.stride ? p.stride : p.size * ts;
    size_t off = (size_t)idx * stride;
    const u8* q;
    if (p.buffer) {
        auto it = buffers.find(p.buffer);
        if (it == buffers.end() || off + p.ptr + p.size * ts > it->second.size()) { for (int i = 0; i < comps; i++) out[i] = 0; return; }
        q = it->second.data() + p.ptr + off;
    } else {
        if (!sn.valid || off + p.size * ts > sn.data.size()) { for (int i = 0; i < comps; i++) out[i] = 0; return; }
        q = sn.data.data() + off;
    }
    for (int i = 0; i < p.size && i < comps; i++) {
        float v;
        switch (p.type) {
        case GL_BYTE: v = (float)(int8_t)q[i]; break;
        case GL_UNSIGNED_BYTE: v = norm_ubyte ? q[i] / 255.f : (float)q[i]; break;
        case GL_SHORT: { int16_t s; memcpy(&s, q + i * 2, 2); v = s; break; }
        case GL_UNSIGNED_SHORT: { u16 s; memcpy(&s, q + i * 2, 2); v = s; break; }
        case GL_FIXED: { s32 s; memcpy(&s, q + i * 4, 4); v = s / 65536.f; break; }
        default: memcpy(&v, q + i * 4, 4); break;
        }
        out[i] = v;
    }
}

bool GLES::lighting_on() { return (enables & E_LIGHTING) != 0; }

void GLES::light_vertex(const float* eye, const float* n, const float* in_col, float* out) {
    const float* amb = mat.ambient;
    const float* dif = mat.diffuse;
    if (enables & E_COLOR_MAT) { amb = in_col; dif = in_col; }
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = mat.emission[i] + light_model_ambient[i] * amb[i];
    for (int li = 0; li < 8; li++) {
        if (!(enables & (E_LIGHT0 << li))) continue;
        const Light& L = lights[li];
        float ldir[3], att = 1;
        if (L.position[3] != 0) {
            float d[3] = {L.position[0] - eye[0], L.position[1] - eye[1], L.position[2] - eye[2]};
            float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (dist > 0) for (int i = 0; i < 3; i++) d[i] /= dist;
            memcpy(ldir, d, sizeof ldir);
            float den = L.att[0] + L.att[1] * dist + L.att[2] * dist * dist;
            att = den > 0 ? 1 / den : 1;
            if (L.spot_cutoff < 180) {
                float sd[3] = {L.spot_dir[0], L.spot_dir[1], L.spot_dir[2]};
                float sl = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
                if (sl > 0) for (int i = 0; i < 3; i++) sd[i] /= sl;
                float cosang = -(ldir[0] * sd[0] + ldir[1] * sd[1] + ldir[2] * sd[2]);
                if (cosang < cosf(L.spot_cutoff * (float)M_PI / 180.f)) att = 0;
                else att *= powf(std::max(cosang, 0.f), L.spot_exp);
            }
        } else {
            float l = sqrtf(L.position[0] * L.position[0] + L.position[1] * L.position[1] + L.position[2] * L.position[2]);
            for (int i = 0; i < 3; i++) ldir[i] = l > 0 ? L.position[i] / l : 0;
        }
        float ndotl = n[0] * ldir[0] + n[1] * ldir[1] + n[2] * ldir[2];
        float diffuse = std::max(ndotl, 0.f);
        float spec = 0;
        if (ndotl > 0 && (mat.specular[0] + mat.specular[1] + mat.specular[2]) > 0) {
            float h[3] = {ldir[0], ldir[1], ldir[2] + 1};
            float hl = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
            if (hl > 0) {
                float ndoth = (n[0] * h[0] + n[1] * h[1] + n[2] * h[2]) / hl;
                if (ndoth > 0) spec = mat.shininess > 0 ? powf(ndoth, mat.shininess) : 1;
            }
        }
        for (int i = 0; i < 3; i++)
            c[i] += att * (L.ambient[i] * amb[i] + diffuse * L.diffuse[i] * dif[i] + spec * L.specular[i] * mat.specular[i]);
    }
    for (int i = 0; i < 3; i++) out[i] = clampf(c[i], 0, 1);
    out[3] = clampf(dif[3], 0, 1);
}


void GLES::transform(u32 idx, Vtx& v) {
    float pos[4] = {0, 0, 0, 1};
    if (client_states & C_VERTEX) { fetch_attr(vptr, idx, 4, pos, false); if (vptr.size < 3) pos[2] = 0; if (vptr.size < 4) pos[3] = 1; }
    float eye[4];
    const Mat4& mv = stacks[0].back();
    mat_vec(mv, pos, eye);
    float col[4] = {cur_color[0], cur_color[1], cur_color[2], cur_color[3]};
    if (client_states & C_COLOR) { col[3] = 1; fetch_attr(cptr, idx, 4, col, true); }
    if (lighting_on()) {
        float nrm[3] = {cur_normal[0], cur_normal[1], cur_normal[2]};
        if (client_states & C_NORMAL) {
            fetch_attr(nptr, idx, 3, nrm, false);
            if (nptr.type == GL_BYTE) for (float& x : nrm) x /= 127.f;
            else if (nptr.type == GL_SHORT) for (float& x : nrm) x /= 32767.f;
        }
        // normal transform by MV (assumes no non-uniform scale); normalize
        float en[3];
        for (int i = 0; i < 3; i++) en[i] = mv.m[i] * nrm[0] + mv.m[4 + i] * nrm[1] + mv.m[8 + i] * nrm[2];
        float l = sqrtf(en[0] * en[0] + en[1] * en[1] + en[2] * en[2]);
        if (l > 0) for (float& x : en) x /= l;
        float lit[4];
        light_vertex(eye, en, col, lit);
        memcpy(col, lit, sizeof col);
    }
    memcpy(v.col, col, sizeof col);
    for (int u = 0; u < 2; u++) {
        float tc[4] = {cur_tc[u][0], cur_tc[u][1], cur_tc[u][2], cur_tc[u][3]};
        if (client_states & (C_TEX0 << u)) {
            float t4[4] = {0, 0, 0, 1};
            fetch_attr(tptr[u], idx, 4, t4, false);
            if (tptr[u].size < 3) t4[2] = 0;
            if (tptr[u].size < 4) t4[3] = 1;
            memcpy(tc, t4, sizeof tc);
        }
        const Mat4& tm = stacks[2 + u].back();
        float o[4];
        mat_vec(tm, tc, o);
        if (o[3] != 0 && o[3] != 1) { o[0] /= o[3]; o[1] /= o[3]; }
        memcpy(v.tc[u], o, sizeof o);
    }
    float c4[4];
    mat_vec(stacks[1].back(), eye, c4);
    v.clip = {c4[0], c4[1], c4[2], c4[3]};
    v.eyez = eye[2];
    v.fog = 1;
    if (enables & E_FOG) {
        float d = fabsf(eye[2]);
        float f;
        if (fog_mode == GL_EXP) f = expf(-fog_density * d);
        else if (fog_mode == GL_EXP2) f = expf(-(fog_density * d) * (fog_density * d));
        else f = (fog_end != fog_start) ? (fog_end - d) / (fog_end - fog_start) : 1;
        v.fog = clampf(f, 0, 1);
    }
}

void GLES::project(Vtx& v) {
    float w = v.clip.w;
    if (fabsf(w) < 1e-6f) w = 1e-6f;
    float iw = 1.f / w;
    float nx = v.clip.x * iw, ny = v.clip.y * iw, nz = v.clip.z * iw;
    v.sx = vp[0] + (nx + 1) * 0.5f * vp[2];
    float gy = vp[1] + (ny + 1) * 0.5f * vp[3];
    v.sy = (float)H - gy;
    v.sz = depth_n + (nz + 1) * 0.5f * (depth_f - depth_n);
    v.invw = iw;
}

static GLES::Vtx lerp_vtx(const GLES::Vtx& a, const GLES::Vtx& b, float t) {
    GLES::Vtx r;
    r.clip = {a.clip.x + (b.clip.x - a.clip.x) * t, a.clip.y + (b.clip.y - a.clip.y) * t,
              a.clip.z + (b.clip.z - a.clip.z) * t, a.clip.w + (b.clip.w - a.clip.w) * t};
    for (int i = 0; i < 4; i++) r.col[i] = a.col[i] + (b.col[i] - a.col[i]) * t;
    for (int u = 0; u < 2; u++) for (int i = 0; i < 4; i++) r.tc[u][i] = a.tc[u][i] + (b.tc[u][i] - a.tc[u][i]) * t;
    r.fog = a.fog + (b.fog - a.fog) * t;
    r.eyez = a.eyez + (b.eyez - a.eyez) * t;
    return r;
}

// Clip polygon against near (z >= -w) and far (z <= w) planes, then rasterize as a fan.
void GLES::clip_and_raster(Vtx* poly, int n) {
    Vtx buf1[12], buf2[12];
    Vtx* in = poly; Vtx* out = buf1;
    for (int plane = 0; plane < 2; plane++) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            const Vtx& A = in[i];
            const Vtx& B = in[(i + 1) % n];
            float da = plane == 0 ? A.clip.z + A.clip.w : A.clip.w - A.clip.z;
            float db = plane == 0 ? B.clip.z + B.clip.w : B.clip.w - B.clip.z;
            if (da >= 0) { if (m < 11) out[m++] = A; }
            if ((da >= 0) != (db >= 0)) { float t = da / (da - db); if (m < 11) out[m++] = lerp_vtx(A, B, t); }
        }
        n = m;
        if (n < 3) return;
        in = out;
        out = (out == buf1) ? buf2 : buf1;
    }
    for (int i = 0; i < n; i++) project(in[i]);
    // flat shading: provoking vertex colour = last original vertex (copied to all)
    for (int i = 1; i + 1 < n; i++) raster_triangle(in[0], in[i], in[i + 1]);
}

void GLES::draw_triangle(const Vtx& a, const Vtx& b, const Vtx& c) {
    // Culling (in window coords, using projected positions of unclipped verts if all w>0)
    Vtx tri[3] = {a, b, c};
    if (shade_model == GL_FLAT) for (int i = 0; i < 2; i++) memcpy(tri[i].col, c.col, sizeof c.col);
    if (enables & E_CULL) {
        if (cull_mode == GL_FRONT_AND_BACK) return;
        if (a.clip.w > 0 && b.clip.w > 0 && c.clip.w > 0) {
            float ax_ = a.clip.x / a.clip.w, ay = a.clip.y / a.clip.w;
            float bx = b.clip.x / b.clip.w, by = b.clip.y / b.clip.w;
            float cx = c.clip.x / c.clip.w, cy = c.clip.y / c.clip.w;
            float area = (bx - ax_) * (cy - ay) - (cx - ax_) * (by - ay);
            bool ccw = area > 0;
            bool front = (front_face == GL_CCW) ? ccw : !ccw;
            if ((cull_mode == GL_BACK && !front) || (cull_mode == GL_FRONT && front)) return;
        }
    }
    clip_and_raster(tri, 3);
}

static inline bool depth_cmp(u32 f, float a, float b) {
    switch (f) {
    case 0x200: return false;
    case 0x201: return a < b;
    case 0x202: return a == b;
    case 0x203: return a <= b;
    case 0x204: return a > b;
    case 0x205: return a != b;
    case 0x206: return a >= b;
    default: return true;
    }
}

static inline float blend_factor(u32 f, int ch, const float* s, const float* d) {
    switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 0x300: return s[ch];
    case 0x301: return 1 - s[ch];
    case 0x302: return s[3];
    case 0x303: return 1 - s[3];
    case 0x304: return d[3];
    case 0x305: return 1 - d[3];
    case 0x306: return d[ch];
    case 0x307: return 1 - d[ch];
    case 0x308: return ch == 3 ? 1 : std::min(s[3], 1 - d[3]);
    default: return 1;
    }
}

// Per-triangle texture unit setup for the fast rasteriser
struct TexUnitSel {
    const u32* d; int w, h; u32 wmask, hmask; bool pot, linear, has_alpha, repeat_s, repeat_t;
    u32 mode; const float* envc;
};

static inline u32 fetch_texel(const TexUnitSel& t, int x, int y) {
    if (t.pot) {
        x = t.repeat_s ? (x & (int)t.wmask) : (x < 0 ? 0 : (x >= t.w ? t.w - 1 : x));
        y = t.repeat_t ? (y & (int)t.hmask) : (y < 0 ? 0 : (y >= t.h ? t.h - 1 : y));
    } else {
        if (t.repeat_s) { x %= t.w; if (x < 0) x += t.w; } else x = x < 0 ? 0 : (x >= t.w ? t.w - 1 : x);
        if (t.repeat_t) { y %= t.h; if (y < 0) y += t.h; } else y = y < 0 ? 0 : (y >= t.h ? t.h - 1 : y);
    }
    return t.d[(size_t)y * t.w + x];
}

static inline u32 sample_fast(const TexUnitSel& t, float s, float tt) {
    float u = s * t.w, v = tt * t.h;
    if (!t.linear) return fetch_texel(t, (int)floorf(u), (int)floorf(v));
    u -= 0.5f; v -= 0.5f;
    int x0 = (int)floorf(u), y0 = (int)floorf(v);
    int fx = (int)((u - x0) * 256.f), fy = (int)((v - y0) * 256.f);
    u32 c00 = fetch_texel(t, x0, y0), c10 = fetch_texel(t, x0 + 1, y0);
    u32 c01 = fetch_texel(t, x0, y0 + 1), c11 = fetch_texel(t, x0 + 1, y0 + 1);
    if (c00 == c10 && c00 == c01 && c00 == c11) return c00;
    u32 out = 0;
    for (int ch = 0; ch < 32; ch += 8) {
        int a0 = (c00 >> ch) & 0xFF, a1 = (c10 >> ch) & 0xFF, b0 = (c01 >> ch) & 0xFF, b1 = (c11 >> ch) & 0xFF;
        int top = a0 * 256 + (a1 - a0) * fx, bot = b0 * 256 + (b1 - b0) * fx;
        int val = (top * 256 + (bot - top) * fy + 32768) >> 16;
        out |= (u32)(val < 0 ? 0 : val > 255 ? 255 : val) << ch;
    }
    return out;
}

void GLES::raster_triangle(const Vtx& A, const Vtx& B, const Vtx& C) {
    float area = (B.sx - A.sx) * (C.sy - A.sy) - (C.sx - A.sx) * (B.sy - A.sy);
    if (fabsf(area) < 1e-8f) return;
    int x0 = 0, y0 = 0, x1 = W - 1, y1 = H - 1;
    if (enables & E_SCISSOR) {
        x0 = std::max(x0, scissor[0]); x1 = std::min(x1, scissor[0] + scissor[2] - 1);
        y0 = std::max(y0, H - (scissor[1] + scissor[3])); y1 = std::min(y1, H - 1 - scissor[1]);
    }
    int bx0 = std::max(x0, (int)floorf(std::min({A.sx, B.sx, C.sx})));
    int bx1 = std::min(x1, (int)ceilf(std::max({A.sx, B.sx, C.sx})));
    int by0 = std::max(y0, (int)floorf(std::min({A.sy, B.sy, C.sy})));
    int by1 = std::min(y1, (int)ceilf(std::max({A.sy, B.sy, C.sy})));
    if (bx0 > bx1 || by0 > by1) return;
    float inv_area = 1.f / area;

    // ---- per-triangle texture setup ----
    TexUnitSel tu[2]; int ntex = 0; int tex_unit[2];
    for (int u = 0; u < 2; u++) {
        if (!tex_enabled[u]) continue;
        auto it = textures.find(bound_tex[u]);
        if (it == textures.end() || it->second.w == 0 || it->second.levels.empty()) continue;
        const Texture& t = it->second;
        float tw = (float)t.w, th = (float)t.h;
        float ta = ((B.tc[u][0] - A.tc[u][0]) * tw) * ((C.tc[u][1] - A.tc[u][1]) * th) -
                   ((C.tc[u][0] - A.tc[u][0]) * tw) * ((B.tc[u][1] - A.tc[u][1]) * th);
        float ratio = fabsf(ta) / fabsf(area);
        float lod = ratio > 0 ? 0.5f * log2f(ratio) : 0;
        bool minify = lod > 0;
        u32 filt = minify ? t.min_filter : t.mag_filter;
        int L = 0;
        if (minify && filt >= 0x2700 && filt <= 0x2703) {
            L = (int)(lod + 0.5f);
            int maxl = (int)t.levels.size() - 1;
            while (maxl > 0 && t.levels[maxl].empty()) maxl--;
            if (L > maxl) L = maxl;
            filt = (filt == 0x2700 || filt == 0x2702) ? GL_NEAREST : GL_LINEAR;
        }
        if (t.levels[L].empty()) L = 0;
        if (t.levels[L].empty()) continue;
        TexUnitSel& s = tu[ntex];
        s.d = t.levels[L].data(); s.w = t.lw[L]; s.h = t.lh[L];
        s.pot = !(s.w & (s.w - 1)) && !(s.h & (s.h - 1));
        s.wmask = s.w - 1; s.hmask = s.h - 1;
        s.linear = filt == GL_LINEAR;
        s.repeat_s = t.wrap_s == GL_REPEAT; s.repeat_t = t.wrap_t == GL_REPEAT;
        s.has_alpha = t.has_alpha; s.mode = texenv[u].mode; s.envc = texenv[u].color;
        tex_unit[ntex] = u;
        ntex++;
    }
    const bool depth_test = enables & E_DEPTH, depth_write = depth_test && depth_mask;
    const bool alpha_test = enables & E_ALPHA, blend = enables & E_BLEND, fog_on = enables & E_FOG;
    const bool std_blend = blend && blend_src == 0x302 && blend_dst == 0x303;
    const bool all_mask = color_mask[0] && color_mask[1] && color_mask[2];
    const float zoff = (enables & E_POLYOFF) ? poly_offset_units * (1.f / 65536.f) : 0.f;
    const bool persp = !(fabsf(A.invw - B.invw) < 1e-9f && fabsf(A.invw - C.invw) < 1e-9f);
    const bool flat_col = A.col[0] == B.col[0] && A.col[0] == C.col[0] && A.col[1] == B.col[1] && A.col[1] == C.col[1] &&
                          A.col[2] == B.col[2] && A.col[2] == C.col[2] && A.col[3] == B.col[3] && A.col[3] == C.col[3];

    // barycentric derivatives along x
    const float d0 = (B.sy - C.sy) * inv_area, d1 = (C.sy - A.sy) * inv_area, d2 = -d0 - d1;
    const float eps = -1e-6f;

    for (int y = by0; y <= by1; y++) {
        float py = y + 0.5f;
        float pxs = bx0 + 0.5f;
        float w0s = ((B.sx - pxs) * (C.sy - py) - (C.sx - pxs) * (B.sy - py)) * inv_area;
        float w1s = ((C.sx - pxs) * (A.sy - py) - (A.sx - pxs) * (C.sy - py)) * inv_area;
        float w2s = 1.f - w0s - w1s;
        // exact span: solve w_i(x) >= eps for x offset k from bx0
        float klo = 0, khi = (float)(bx1 - bx0);
        const float ws[3] = {w0s, w1s, w2s}, ds[3] = {d0, d1, d2};
        bool empty = false;
        for (int e = 0; e < 3; e++) {
            if (ds[e] > 0) { float k = (eps - ws[e]) / ds[e]; if (k > klo) klo = k; }
            else if (ds[e] < 0) { float k = (eps - ws[e]) / ds[e]; if (k < khi) khi = k; }
            else if (ws[e] < eps) { empty = true; break; }
        }
        if (empty || klo > khi) continue;
        int ka = (int)ceilf(klo), kb = (int)floorf(khi);
        if (ka > kb) continue;
        u32* crow = &color[(size_t)y * W];
        float* zrow = &depth[(size_t)y * W];
        for (int k = ka; k <= kb; k++) {
            float w0 = w0s + d0 * k, w1 = w1s + d1 * k, w2 = 1.f - w0 - w1;
            if (w0 < eps || w1 < eps || w2 < eps) continue;
            int x = bx0 + k;
            float z = A.sz * w0 + B.sz * w1 + C.sz * w2 + zoff;
            if (depth_test && !depth_cmp(depth_func, z, zrow[x])) continue;
            float p0 = w0, p1 = w1, p2 = w2;
            if (persp) {
                float iw = A.invw * w0 + B.invw * w1 + C.invw * w2;
                float r = 1.f / iw;
                p0 = A.invw * w0 * r; p1 = B.invw * w1 * r; p2 = 1.f - p0 - p1;
            }
            float c[4];
            if (flat_col) { c[0] = A.col[0]; c[1] = A.col[1]; c[2] = A.col[2]; c[3] = A.col[3]; }
            else for (int i = 0; i < 4; i++) c[i] = A.col[i] * p0 + B.col[i] * p1 + C.col[i] * p2;
            for (int n = 0; n < ntex; n++) {
                int u = tex_unit[n];
                const TexUnitSel& t = tu[n];
                float s_ = A.tc[u][0] * p0 + B.tc[u][0] * p1 + C.tc[u][0] * p2;
                float t_ = A.tc[u][1] * p0 + B.tc[u][1] * p1 + C.tc[u][1] * p2;
                u32 tx = sample_fast(t, s_, t_);
                const float k255 = 1.f / 255.f;
                float tr = ((tx >> 16) & 0xFF) * k255, tg = ((tx >> 8) & 0xFF) * k255, tb = (tx & 0xFF) * k255, ta = (tx >> 24) * k255;
                switch (t.mode) {
                case GL_REPLACE: c[0] = tr; c[1] = tg; c[2] = tb; if (t.has_alpha) c[3] = ta; break;
                case GL_DECAL: c[0] += (tr - c[0]) * ta; c[1] += (tg - c[1]) * ta; c[2] += (tb - c[2]) * ta; break;
                case GL_BLEND_ENV:
                    c[0] = c[0] * (1 - tr) + t.envc[0] * tr; c[1] = c[1] * (1 - tg) + t.envc[1] * tg;
                    c[2] = c[2] * (1 - tb) + t.envc[2] * tb; c[3] *= ta; break;
                case GL_ADD:
                    c[0] = std::min(1.f, c[0] + tr); c[1] = std::min(1.f, c[1] + tg); c[2] = std::min(1.f, c[2] + tb); c[3] *= ta; break;
                default: c[0] *= tr; c[1] *= tg; c[2] *= tb; c[3] *= ta; break;
                }
            }
            if (fog_on) {
                float f = A.fog * p0 + B.fog * p1 + C.fog * p2;
                for (int i = 0; i < 3; i++) c[i] = c[i] * f + fog_color[i] * (1 - f);
            }
            if (alpha_test) {
                float a = c[3], r = alpha_ref; bool pass;
                switch (alpha_func) {
                case 0x200: pass = false; break; case 0x201: pass = a < r; break;
                case 0x202: pass = fabsf(a - r) < 1e-4f; break; case 0x203: pass = a <= r; break;
                case 0x204: pass = a > r; break; case 0x205: pass = fabsf(a - r) >= 1e-4f; break;
                case 0x206: pass = a >= r; break; default: pass = true; break;
                }
                if (!pass) continue;
            }
            u32& dst = crow[x];
            if (std_blend) {
                float a = clampf(c[3], 0, 1);
                if (a <= (0.5f / 255.f)) { if (depth_write) zrow[x] = z; continue; }   // fully transparent
                if (a < 1.f) {
                    float ia = (1 - a) * (1.f / 255.f);
                    c[0] = c[0] * a + ((dst >> 16) & 0xFF) * ia;
                    c[1] = c[1] * a + ((dst >> 8) & 0xFF) * ia;
                    c[2] = c[2] * a + (dst & 0xFF) * ia;
                }
            } else if (blend) {
                float d[4] = {((dst >> 16) & 0xFF) / 255.f, ((dst >> 8) & 0xFF) / 255.f, (dst & 0xFF) / 255.f, 1.f};
                float o[4];
                for (int i = 0; i < 4; i++) o[i] = c[i] * blend_factor(blend_src, i, c, d) + d[i] * blend_factor(blend_dst, i, c, d);
                memcpy(c, o, sizeof c);
            }
            u32 r = (u32)(clampf(c[0], 0, 1) * 255 + 0.5f), g = (u32)(clampf(c[1], 0, 1) * 255 + 0.5f), b = (u32)(clampf(c[2], 0, 1) * 255 + 0.5f);
            if (all_mask) dst = 0xFF000000u | (r << 16) | (g << 8) | b;
            else {
                u32 nv = dst;
                if (color_mask[0]) nv = (nv & 0xFF00FFFF) | (r << 16);
                if (color_mask[1]) nv = (nv & 0xFFFF00FF) | (g << 8);
                if (color_mask[2]) nv = (nv & 0xFFFFFF00) | b;
                dst = nv | 0xFF000000u;
            }
            if (depth_write) zrow[x] = z;
        }
    }
}

void GLES::raster_line(const Vtx& a, const Vtx& b) {
    float dx = b.sx - a.sx, dy = b.sy - a.sy;
    int steps = (int)std::max(fabsf(dx), fabsf(dy));
    if (steps < 1) steps = 1;
    float lod[2] = {0, 0};
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / steps;
        int x = (int)(a.sx + dx * t), y = (int)(a.sy + dy * t);
        if (x < 0 || y < 0 || x >= W || y >= H) continue;
        float col[4], tc[2][4];
        for (int k = 0; k < 4; k++) col[k] = a.col[k] + (b.col[k] - a.col[k]) * t;
        for (int u = 0; u < 2; u++) for (int k = 0; k < 2; k++) tc[u][k] = a.tc[u][k] + (b.tc[u][k] - a.tc[u][k]) * t;
        shade_fragment(x, y, a.sz + (b.sz - a.sz) * t, col, tc, a.fog + (b.fog - a.fog) * t, lod);
    }
}

void GLES::raster_point(const Vtx& v) {
    int s = std::max(1, (int)(point_size + 0.5f));
    int x0 = (int)(v.sx - s * 0.5f + 0.5f), y0 = (int)(v.sy - s * 0.5f + 0.5f);
    float lod[2] = {0, 0};
    for (int y = y0; y < y0 + s; y++)
        for (int x = x0; x < x0 + s; x++) {
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            float tc[2][4];
            for (int u = 0; u < 2; u++) { tc[u][0] = v.tc[u][0]; tc[u][1] = v.tc[u][1]; }
            shade_fragment(x, y, v.sz, v.col, tc, v.fog, lod);
        }
}



void GLES::shade_fragment(int x, int y, float z, const float* in_col, const float tc[2][4], float fog, float lod[2]) {
    size_t pi = (size_t)y * W + x;
    if ((enables & E_DEPTH) && !depth_cmp(depth_func, z, depth[pi])) return;
    float c[4] = {in_col[0], in_col[1], in_col[2], in_col[3]};
    for (int u = 0; u < 2; u++) {
        if (!tex_enabled[u]) continue;
        auto it = textures.find(bound_tex[u]);
        if (it == textures.end() || it->second.w == 0) continue;
        const Texture& t = it->second;
        u32 tx = sample(t, tc[u][0], tc[u][1], lod[u]);
        float tr = ((tx >> 16) & 0xFF) / 255.f, tg = ((tx >> 8) & 0xFF) / 255.f, tb = (tx & 0xFF) / 255.f, ta = (tx >> 24) / 255.f;
        const TexEnv& e = texenv[u];
        switch (e.mode) {
        case GL_REPLACE:
            c[0] = tr; c[1] = tg; c[2] = tb; if (t.has_alpha) c[3] = ta;
            break;
        case GL_DECAL:
            c[0] = c[0] * (1 - ta) + tr * ta; c[1] = c[1] * (1 - ta) + tg * ta; c[2] = c[2] * (1 - ta) + tb * ta;
            break;
        case GL_BLEND_ENV:
            c[0] = c[0] * (1 - tr) + e.color[0] * tr; c[1] = c[1] * (1 - tg) + e.color[1] * tg;
            c[2] = c[2] * (1 - tb) + e.color[2] * tb; c[3] *= ta;
            break;
        case GL_ADD:
            c[0] = std::min(1.f, c[0] + tr); c[1] = std::min(1.f, c[1] + tg); c[2] = std::min(1.f, c[2] + tb); c[3] *= ta;
            break;
        default: // MODULATE / COMBINE (approximated)
            c[0] *= tr; c[1] *= tg; c[2] *= tb; c[3] *= ta;
            break;
        }
    }
    if (enables & E_FOG) for (int i = 0; i < 3; i++) c[i] = c[i] * fog + fog_color[i] * (1 - fog);
    if (enables & E_ALPHA) {
        float a = c[3], r = alpha_ref;
        bool pass;
        switch (alpha_func) {
        case 0x200: pass = false; break;
        case 0x201: pass = a < r; break;
        case 0x202: pass = fabsf(a - r) < 1e-4f; break;
        case 0x203: pass = a <= r; break;
        case 0x204: pass = a > r; break;
        case 0x205: pass = fabsf(a - r) >= 1e-4f; break;
        case 0x206: pass = a >= r; break;
        default: pass = true; break;
        }
        if (!pass) return;
    }
    u32& dst = color[pi];
    if (enables & E_BLEND) {
        float d[4] = {((dst >> 16) & 0xFF) / 255.f, ((dst >> 8) & 0xFF) / 255.f, (dst & 0xFF) / 255.f, 1.f};
        float o[4];
        for (int i = 0; i < 4; i++) o[i] = c[i] * blend_factor(blend_src, i, c, d) + d[i] * blend_factor(blend_dst, i, c, d);
        memcpy(c, o, sizeof c);
    }
    u32 r = (u32)(clampf(c[0], 0, 1) * 255 + 0.5f), g = (u32)(clampf(c[1], 0, 1) * 255 + 0.5f), b = (u32)(clampf(c[2], 0, 1) * 255 + 0.5f);
    u32 nv = dst;
    if (color_mask[0]) nv = (nv & 0xFF00FFFF) | (r << 16);
    if (color_mask[1]) nv = (nv & 0xFFFF00FF) | (g << 8);
    if (color_mask[2]) nv = (nv & 0xFFFFFF00) | b;
    dst = nv | 0xFF000000u;
    if ((enables & E_DEPTH) && depth_mask) depth[pi] = z;
}

void GLES::draw(u32 mode, const std::vector<u32>& idx) {
    if (idx.empty() || !(client_states & C_VERTEX)) return;
    u32 maxi = 0;
    for (u32 i : idx) maxi = std::max(maxi, i);
    // snapshot client arrays from guest memory
    const Pointer* ps[5] = {&vptr, &cptr, &nptr, &tptr[0], &tptr[1]};
    u32 flags[5] = {C_VERTEX, C_COLOR, C_NORMAL, C_TEX0, C_TEX1};
    for (int s = 0; s < 5; s++) {
        g_snap[s].valid = false;
        const Pointer& p = *ps[s];
        if (!(client_states & flags[s]) || p.buffer) continue;
        int ts = type_size(p.type);
        int stride = p.stride ? p.stride : p.size * ts;
        size_t len = (size_t)maxi * stride + p.size * ts;
        if (len > (8u << 20)) continue;
        g_snap[s].data.resize(len);
        hle->read_mem(p.ptr, g_snap[s].data.data(), (u32)len);
        g_snap[s].valid = true;
    }
    std::vector<Vtx> v(idx.size());
    // cache transforms per unique index (flat table over the index range)
    u32 mini = maxi;
    for (u32 i : idx) mini = std::min(mini, i);
    if (maxi - mini < 65536) {
        static std::vector<int> slot;
        slot.assign(maxi - mini + 1, -1);
        for (size_t i = 0; i < idx.size(); i++) {
            int& sl = slot[idx[i] - mini];
            if (sl >= 0) { v[i] = v[sl]; continue; }
            transform(idx[i], v[i]);
            sl = (int)i;
        }
    } else {
        for (size_t i = 0; i < idx.size(); i++) transform(idx[i], v[i]);
    }
    size_t n = v.size();
    switch (mode) {
    case GL_TRIANGLES:
        for (size_t i = 0; i + 2 < n; i += 3) draw_triangle(v[i], v[i + 1], v[i + 2]);
        break;
    case GL_TRIANGLE_STRIP:
        for (size_t i = 0; i + 2 < n; i++) {
            if (i & 1) draw_triangle(v[i + 1], v[i], v[i + 2]);
            else draw_triangle(v[i], v[i + 1], v[i + 2]);
        }
        break;
    case GL_TRIANGLE_FAN:
        for (size_t i = 1; i + 1 < n; i++) draw_triangle(v[0], v[i], v[i + 1]);
        break;
    case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP: {
        for (auto& x : v) project(x);
        size_t step = mode == GL_LINES ? 2 : 1;
        for (size_t i = 0; i + 1 < n; i += step) if (v[i].clip.w > 0 && v[i + 1].clip.w > 0) raster_line(v[i], v[i + 1]);
        if (mode == GL_LINE_LOOP && n > 2) raster_line(v[n - 1], v[0]);
        break;
    }
    case GL_POINTS:
        for (auto& x : v) { if (x.clip.w <= 0) continue; project(x); raster_point(x); }
        break;
    }
}

void GLES::clear(u32 mask) {
    int x0 = 0, y0 = 0, x1 = W, y1 = H;
    if (enables & E_SCISSOR) {
        x0 = std::max(0, scissor[0]); x1 = std::min(W, scissor[0] + scissor[2]);
        y0 = std::max(0, H - (scissor[1] + scissor[3])); y1 = std::min(H, H - scissor[1]);
    }
    if (mask & GL_COLOR_BUFFER_BIT) {
        u32 c = argb((u32)(clampf(clear_color[0], 0, 1) * 255), (u32)(clampf(clear_color[1], 0, 1) * 255),
                     (u32)(clampf(clear_color[2], 0, 1) * 255), 255);
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) color[(size_t)y * W + x] = c;
    }
    if (mask & GL_DEPTH_BUFFER_BIT)
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) depth[(size_t)y * W + x] = clear_depth;
}

void GLES::present() {
    swaps++;
    hle->sys->present_gl(color.data(), W, H);
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------
bool GLES::call(int id) {
    int idx = id < 0 ? F_COUNT : id;
    if (hle->log_calls && call_counts[idx] < 20 && hle->log) {
        char b[200];
        snprintf(b, sizeof b, "[gl] %s(%08X, %08X, %08X, %08X, %08X) lr=%08X", id >= 0 ? g_names[id] : "?",
                 hle->arg(0), hle->arg(1), hle->arg(2), hle->arg(3), hle->arg(4), hle->cpu->r[14]);
        hle->log(b);
    }
    call_counts[idx]++;
    if (passthrough || id < 0) return false;

    u32 r = 0;
    switch ((Fn)id) {
    // ----- EGL -----
    case F_eglGetDisplay: r = 1; break;
    case F_eglInitialize:
        if (a(1)) hle->wr32(a(1), 1);
        if (a(2)) hle->wr32(a(2), 1);
        r = 1; break;
    case F_eglTerminate: case F_eglMakeCurrent: case F_eglDestroyContext: case F_eglDestroySurface:
    case F_eglWaitGL: case F_eglWaitNative: case F_eglSwapInterval: case F_eglBindAPI:
    case F_eglSurfaceAttrib: case F_eglBindTexImage: case F_eglReleaseTexImage: case F_eglCopyBuffers:
        r = 1; break;
    case F_eglGetError: r = 0x3000; break;
    case F_eglQueryString: {
        u32 nm = a(1);
        const char* s = nm == 0x3053 ? "MagicEyes" : nm == 0x3054 ? "1.1" : nm == 0x3055 ? "" : nm == 0x308D ? "OpenGL_ES" : "";
        r = guest_string(0x10000 + nm, s);
        break;
    }
    case F_eglGetConfigs: case F_eglChooseConfig: {
        int cfg_arg = id == F_eglChooseConfig ? 2 : 1, n_arg = id == F_eglChooseConfig ? 4 : 3;
        int size_arg = cfg_arg + 1;
        if (a(cfg_arg) && (s32)a(size_arg) > 0) hle->wr32(a(cfg_arg), 1);
        if (a(n_arg)) hle->wr32(a(n_arg), 1);
        r = 1; break;
    }
    case F_eglGetConfigAttrib: {
        u32 at = a(2), val = 0;
        switch (at) {
        case 0x3020: val = 16; break; case 0x3024: val = 5; break; case 0x3023: val = 6; break; case 0x3022: val = 5; break;
        case 0x3021: val = 0; break; case 0x3025: val = 16; break; case 0x3026: val = 0; break;
        case 0x3028: val = 1; break;   // CONFIG_ID
        case 0x3033: val = 5; break;   // SURFACE_TYPE window|pbuffer
        case 0x302C: val = 320; break; case 0x302A: val = 240; break;
        case 0x3040: val = 1; break;   // RENDERABLE_TYPE ES
        default: val = 0; break;
        }
        if (a(3)) hle->wr32(a(3), val);
        r = 1; break;
    }
    case F_eglCreateWindowSurface: r = 1; break;
    case F_eglCreatePbufferSurface: case F_eglCreatePixmapSurface: r = 2; break;
    case F_eglCreateContext: r = 1; break;
    case F_eglGetCurrentContext: case F_eglGetCurrentDisplay: case F_eglGetCurrentSurface: r = 1; break;
    case F_eglQuerySurface: case F_eglQueryContext: {
        u32 at = a(2), val = 0;
        if (at == 0x3057) val = W; else if (at == 0x3056) val = H; else if (at == 0x3028) val = 1;
        if (a(3)) hle->wr32(a(3), val);
        r = 1; break;
    }
    case F_eglGetProcAddress: r = 0; break;
    case F_eglSwapBuffers: {
        u64 fc = hle->sys->frame_count;
        if (fc == last_swap_frame) {
            // Throttle to one swap per video frame: sleep until the next interrupt and retry.
            hle->cpu->halt();
            return true;
        }
        last_swap_frame = fc;
        present();
        r = 1; break;
    }

    // ----- errors / strings / queries -----
    case F_glGetError: r = (u32)err; err = 0; break;
    case F_glGetString: {
        u32 nm = a(0);
        const char* s = nm == 0x1F00 ? "MagicEyes" : nm == 0x1F01 ? "DidjDroid HLE" : nm == 0x1F02 ? "OpenGL ES-CM 1.1" :
            nm == 0x1F03 ? "GL_OES_byte_coordinates GL_OES_fixed_point GL_OES_single_precision GL_OES_read_format "
                           "GL_OES_compressed_paletted_texture GL_OES_point_size_array GL_OES_point_sprite "
                           "GL_OES_matrix_palette GL_OES_draw_texture GL_OES_query_matrix" : nullptr;
        r = s ? guest_string(nm, s) : 0;
        break;
    }
    case F_glGetIntegerv: case F_glGetFloatv: case F_glGetFixedv: case F_glGetBooleanv: {
        std::vector<float> vals; bool ints;
        get_values(a(0), vals, &ints);
        u32 p = a(1);
        for (size_t i = 0; i < vals.size(); i++) {
            if (id == F_glGetBooleanv) { u8 b = vals[i] != 0; hle->write_mem(p + (u32)i, &b, 1); continue; }
            u32 w;
            if (id == F_glGetIntegerv) w = (u32)(s32)(ints ? vals[i] : vals[i]);
            else if (id == F_glGetFixedv) w = (u32)(s32)(vals[i] * 65536.f);
            else memcpy(&w, &vals[i], 4);
            hle->wr32(p + (u32)i * 4, w);
        }
        break;
    }
    case F_glIsEnabled: r = is_enabled(a(0)) ? 1 : 0; break;
    case F_glIsTexture: r = textures.count(a(0)) ? 1 : 0; break;
    case F_glIsBuffer: r = buffers.count(a(0)) ? 1 : 0; break;
    case F_glGetPointerv: hle->wr32(a(1), 0); break;
    case F_glGetBufferParameteriv: {
        u32 b = a(0) == GL_ARRAY_BUFFER ? bound_array : bound_element;
        hle->wr32(a(2), a(1) == 0x8764 ? (u32)buffers[b].size() : 0);
        break;
    }
    case F_glGetTexParameteriv: case F_glGetTexParameterfv: case F_glGetTexParameterxv: {
        Texture* t = cur_texture();
        u32 v = a(1) == GL_TEXTURE_MIN_FILTER ? t->min_filter : a(1) == GL_TEXTURE_MAG_FILTER ? t->mag_filter :
                a(1) == GL_TEXTURE_WRAP_S ? t->wrap_s : a(1) == GL_TEXTURE_WRAP_T ? t->wrap_t : 0;
        if (id == F_glGetTexParameterfv) { float f = (float)v; memcpy(&v, &f, 4); }
        hle->wr32(a(2), v);
        break;
    }
    case F_glGetTexEnviv: case F_glGetTexEnvfv: case F_glGetTexEnvxv: {
        u32 v = texenv[active_tex].mode;
        if (id == F_glGetTexEnvfv) { float f = (float)v; memcpy(&v, &f, 4); }
        hle->wr32(a(2), v);
        break;
    }
    case F_glGetLightfv: case F_glGetLightxv: case F_glGetMaterialfv: case F_glGetMaterialxv:
    case F_glGetClipPlanef: case F_glGetClipPlanex:
        for (int i = 0; i < 4; i++) hle->wr32(a(2) + i * 4, 0);
        break;

    // ----- simple state -----
    case F_glEnable: enable(a(0), true); break;
    case F_glDisable: enable(a(0), false); break;
    case F_glEnableClientState: case F_glDisableClientState: {
        u32 bitv = 0;
        switch (a(0)) {
        case GL_VERTEX_ARRAY: bitv = C_VERTEX; break;
        case GL_NORMAL_ARRAY: bitv = C_NORMAL; break;
        case GL_COLOR_ARRAY: bitv = C_COLOR; break;
        case GL_TEXTURE_COORD_ARRAY: bitv = C_TEX0 << client_tex; break;
        case GL_POINT_SIZE_ARRAY_OES: bitv = C_PSIZE; break;
        case GL_MATRIX_INDEX_ARRAY_OES: bitv = C_MIDX; break;
        case GL_WEIGHT_ARRAY_OES: bitv = C_WEIGHT; break;
        }
        if (id == F_glEnableClientState) client_states |= bitv; else client_states &= ~bitv;
        break;
    }
    case F_glActiveTexture: active_tex = (a(0) - GL_TEXTURE0) & 1; break;
    case F_glClientActiveTexture: client_tex = (a(0) - GL_TEXTURE0) & 1; break;
    case F_glAlphaFunc: alpha_func = a(0); alpha_ref = clampf(af(1), 0, 1); break;
    case F_glAlphaFuncx: alpha_func = a(0); alpha_ref = clampf(ax(1), 0, 1); break;
    case F_glBlendFunc: blend_src = a(0); blend_dst = a(1); break;
    case F_glClearColor: clear_color[0] = af(0); clear_color[1] = af(1); clear_color[2] = af(2); clear_color[3] = af(3); break;
    case F_glClearColorx: clear_color[0] = ax(0); clear_color[1] = ax(1); clear_color[2] = ax(2); clear_color[3] = ax(3); break;
    case F_glClearDepthf: clear_depth = clampf(af(0), 0, 1); break;
    case F_glClearDepthx: clear_depth = clampf(ax(0), 0, 1); break;
    case F_glClear: clear(a(0)); break;
    case F_glColor4f: cur_color[0] = af(0); cur_color[1] = af(1); cur_color[2] = af(2); cur_color[3] = af(3); break;
    case F_glColor4x: cur_color[0] = ax(0); cur_color[1] = ax(1); cur_color[2] = ax(2); cur_color[3] = ax(3); break;
    case F_glColor4ub: for (int i = 0; i < 4; i++) cur_color[i] = (a(i) & 0xFF) / 255.f; break;
    case F_glColorMask: for (int i = 0; i < 4; i++) color_mask[i] = (a(i) & 0xFF) != 0; break;
    case F_glCullFace: cull_mode = a(0); break;
    case F_glFrontFace: front_face = a(0); break;
    case F_glDepthFunc: depth_func = a(0); break;
    case F_glDepthMask: depth_mask = (a(0) & 0xFF) != 0; break;
    case F_glDepthRangef: depth_n = clampf(af(0), 0, 1); depth_f = clampf(af(1), 0, 1); break;
    case F_glDepthRangex: depth_n = clampf(ax(0), 0, 1); depth_f = clampf(ax(1), 0, 1); break;
    case F_glShadeModel: shade_model = a(0); break;
    case F_glViewport: vp[0] = (s32)a(0); vp[1] = (s32)a(1); vp[2] = (s32)a(2); vp[3] = (s32)a(3); break;
    case F_glScissor: scissor[0] = (s32)a(0); scissor[1] = (s32)a(1); scissor[2] = (s32)a(2); scissor[3] = (s32)a(3); break;
    case F_glPointSize: point_size = af(0); break;
    case F_glPointSizex: point_size = ax(0); break;
    case F_glLineWidth: line_width = af(0); break;
    case F_glLineWidthx: line_width = ax(0); break;
    case F_glPixelStorei: if (a(0) == GL_UNPACK_ALIGNMENT) unpack_align = std::max(1, (int)a(1)); break;
    case F_glPolygonOffset: poly_offset_factor = af(0); poly_offset_units = af(1); break;
    case F_glPolygonOffsetx: poly_offset_factor = ax(0); poly_offset_units = ax(1); break;
    case F_glNormal3f: cur_normal[0] = af(0); cur_normal[1] = af(1); cur_normal[2] = af(2); break;
    case F_glNormal3x: cur_normal[0] = ax(0); cur_normal[1] = ax(1); cur_normal[2] = ax(2); break;
    case F_glMultiTexCoord4f: { int u = (a(0) - GL_TEXTURE0) & 1; for (int i = 0; i < 4; i++) cur_tc[u][i] = af(1 + i); break; }
    case F_glMultiTexCoord4x: { int u = (a(0) - GL_TEXTURE0) & 1; for (int i = 0; i < 4; i++) cur_tc[u][i] = ax(1 + i); break; }
    case F_glHint: case F_glLogicOp: case F_glSampleCoverage: case F_glSampleCoveragex: case F_glStencilFunc:
    case F_glStencilMask: case F_glStencilOp: case F_glClearStencil: case F_glClipPlanef: case F_glClipPlanex:
    case F_glPointParameterf: case F_glPointParameterfv: case F_glPointParameterx: case F_glPointParameterxv:
    case F_glFlush: case F_glFinish:
        break;

    // ----- fog -----
    case F_glFogf: case F_glFogx: case F_glFogfv: case F_glFogxv: {
        bool fixed = (id == F_glFogx || id == F_glFogxv), vec = (id == F_glFogfv || id == F_glFogxv);
        u32 pn = a(0);
        auto val = [&](int i) -> float {
            if (!vec) return fixed ? ax(1) : af(1);
            u32 w = hle->rd32(a(1) + i * 4);
            if (fixed) return (s32)w / 65536.f;
            float f; memcpy(&f, &w, 4); return f;
        };
        auto raw = [&]() -> u32 { return vec ? hle->rd32(a(1)) : a(1); };
        switch (pn) {
        case GL_FOG_MODE: { u32 m = raw(); if (!vec && !fixed) { float f; memcpy(&f, &m, 4); m = (u32)f; } else if (fixed && !vec) m = a(1); fog_mode = m; break; }
        case GL_FOG_DENSITY: fog_density = val(0); break;
        case GL_FOG_START: fog_start = val(0); break;
        case GL_FOG_END: fog_end = val(0); break;
        case GL_FOG_COLOR: if (vec) for (int i = 0; i < 4; i++) fog_color[i] = val(i); break;
        }
        break;
    }

    // ----- lighting / material -----
    case F_glLightf: case F_glLightx: case F_glLightfv: case F_glLightxv: {
        int li = (a(0) - GL_LIGHT0) & 7;
        bool fixed = (id == F_glLightx || id == F_glLightxv), vec = (id == F_glLightfv || id == F_glLightxv);
        float v[4] = {0, 0, 0, 0};
        if (vec) for (int i = 0; i < 4; i++) { u32 w = hle->rd32(a(2) + i * 4); if (fixed) v[i] = (s32)w / 65536.f; else memcpy(&v[i], &w, 4); }
        else v[0] = fixed ? ax(2) : af(2);
        Light& L = lights[li];
        switch (a(1)) {
        case GL_AMBIENT: memcpy(L.ambient, v, sizeof v); break;
        case GL_DIFFUSE: memcpy(L.diffuse, v, sizeof v); break;
        case GL_SPECULAR: memcpy(L.specular, v, sizeof v); break;
        case GL_POSITION: mat_vec(stacks[0].back(), v, L.position); break;
        case GL_SPOT_DIRECTION: {
            const Mat4& m = stacks[0].back();
            for (int i = 0; i < 3; i++) L.spot_dir[i] = m.m[i] * v[0] + m.m[4 + i] * v[1] + m.m[8 + i] * v[2];
            break;
        }
        case GL_SPOT_EXPONENT: L.spot_exp = v[0]; break;
        case GL_SPOT_CUTOFF: L.spot_cutoff = v[0]; break;
        case GL_CONSTANT_ATTENUATION: L.att[0] = v[0]; break;
        case GL_LINEAR_ATTENUATION: L.att[1] = v[0]; break;
        case GL_QUADRATIC_ATTENUATION: L.att[2] = v[0]; break;
        }
        break;
    }
    case F_glMaterialf: case F_glMaterialx: case F_glMaterialfv: case F_glMaterialxv: {
        bool fixed = (id == F_glMaterialx || id == F_glMaterialxv), vec = (id == F_glMaterialfv || id == F_glMaterialxv);
        float v[4] = {0, 0, 0, 0};
        if (vec) for (int i = 0; i < 4; i++) { u32 w = hle->rd32(a(2) + i * 4); if (fixed) v[i] = (s32)w / 65536.f; else memcpy(&v[i], &w, 4); }
        else v[0] = fixed ? ax(2) : af(2);
        switch (a(1)) {
        case GL_AMBIENT: memcpy(mat.ambient, v, sizeof v); break;
        case GL_DIFFUSE: memcpy(mat.diffuse, v, sizeof v); break;
        case GL_AMBIENT_AND_DIFFUSE: memcpy(mat.ambient, v, sizeof v); memcpy(mat.diffuse, v, sizeof v); break;
        case GL_SPECULAR: memcpy(mat.specular, v, sizeof v); break;
        case GL_EMISSION: memcpy(mat.emission, v, sizeof v); break;
        case GL_SHININESS: mat.shininess = v[0]; break;
        }
        break;
    }
    case F_glLightModelf: case F_glLightModelx: case F_glLightModelfv: case F_glLightModelxv: {
        bool fixed = (id == F_glLightModelx || id == F_glLightModelxv), vec = (id == F_glLightModelfv || id == F_glLightModelxv);
        float v[4] = {0, 0, 0, 0};
        if (vec) for (int i = 0; i < 4; i++) { u32 w = hle->rd32(a(1) + i * 4); if (fixed) v[i] = (s32)w / 65536.f; else memcpy(&v[i], &w, 4); }
        else v[0] = fixed ? ax(1) : af(1);
        if (a(0) == GL_LIGHT_MODEL_AMBIENT) memcpy(light_model_ambient, v, sizeof v);
        else if (a(0) == GL_LIGHT_MODEL_TWO_SIDE) two_side = v[0] != 0;
        break;
    }

    // ----- matrices -----
    case F_glMatrixMode: matrix_mode = a(0) == GL_PROJECTION ? 1 : a(0) == GL_TEXTURE ? 2 : 0; break;
    case F_glLoadIdentity: cur_matrix() = Mat4::identity(); break;
    case F_glLoadMatrixf: case F_glLoadMatrixx: case F_glMultMatrixf: case F_glMultMatrixx: {
        Mat4 m;
        for (int i = 0; i < 16; i++) {
            u32 w = hle->rd32(a(0) + i * 4);
            if (id == F_glLoadMatrixx || id == F_glMultMatrixx) m.m[i] = (s32)w / 65536.f; else memcpy(&m.m[i], &w, 4);
        }
        if (id == F_glLoadMatrixf || id == F_glLoadMatrixx) cur_matrix() = m; else cur_matrix() = cur_matrix() * m;
        break;
    }
    case F_glPushMatrix: { auto& s = cur_stack(); if (s.size() < 32) s.push_back(s.back()); break; }
    case F_glPopMatrix: { auto& s = cur_stack(); if (s.size() > 1) s.pop_back(); break; }
    case F_glTranslatef: case F_glTranslatex: {
        bool fx = id == F_glTranslatex;
        Mat4 t = Mat4::identity();
        t.m[12] = fx ? ax(0) : af(0); t.m[13] = fx ? ax(1) : af(1); t.m[14] = fx ? ax(2) : af(2);
        cur_matrix() = cur_matrix() * t;
        break;
    }
    case F_glScalef: case F_glScalex: {
        bool fx = id == F_glScalex;
        Mat4 t = Mat4::identity();
        t.m[0] = fx ? ax(0) : af(0); t.m[5] = fx ? ax(1) : af(1); t.m[10] = fx ? ax(2) : af(2);
        cur_matrix() = cur_matrix() * t;
        break;
    }
    case F_glRotatef: case F_glRotatex: {
        bool fx = id == F_glRotatex;
        cur_matrix() = cur_matrix() * mat_rotate(fx ? ax(0) : af(0), fx ? ax(1) : af(1), fx ? ax(2) : af(2), fx ? ax(3) : af(3));
        break;
    }
    case F_glOrthof: case F_glOrthox: case F_glFrustumf: case F_glFrustumx: {
        bool fx = (id == F_glOrthox || id == F_glFrustumx);
        float v[6];
        for (int i = 0; i < 6; i++) v[i] = fx ? ax(i) : af(i);
        Mat4 m = (id == F_glOrthof || id == F_glOrthox) ? mat_ortho(v[0], v[1], v[2], v[3], v[4], v[5])
                                                        : mat_frustum(v[0], v[1], v[2], v[3], v[4], v[5]);
        cur_matrix() = cur_matrix() * m;
        break;
    }
    case F_glCurrentPaletteMatrixOES: break;
    case F_glLoadPaletteFromModelViewMatrixOES: break;
    case F_glMatrixIndexPointerOES: case F_glWeightPointerOES: case F_glPointSizePointerOES: break;

    // ----- arrays -----
    case F_glVertexPointer: vptr = {(int)a(0), a(1), (int)a(2), a(3), bound_array}; break;
    case F_glColorPointer: cptr = {(int)a(0), a(1), (int)a(2), a(3), bound_array}; break;
    case F_glNormalPointer: nptr = {3, a(0), (int)a(1), a(2), bound_array}; break;
    case F_glTexCoordPointer: tptr[client_tex] = {(int)a(0), a(1), (int)a(2), a(3), bound_array}; break;
    case F_glDrawArrays: {
        std::vector<u32> ix(a(2));
        for (u32 i = 0; i < a(2); i++) ix[i] = a(1) + i;
        draw(a(0), ix);
        break;
    }
    case F_glDrawElements: {
        u32 mode = a(0), count = a(1), type = a(2), ptr = a(3);
        std::vector<u32> ix(count);
        int ts = type == GL_UNSIGNED_BYTE ? 1 : 2;
        std::vector<u8> raw((size_t)count * ts);
        if (bound_element) {
            auto& b = buffers[bound_element];
            for (size_t i = 0; i < raw.size(); i++) raw[i] = (ptr + i < b.size()) ? b[ptr + i] : 0;
        } else hle->read_mem(ptr, raw.data(), (u32)raw.size());
        for (u32 i = 0; i < count; i++) ix[i] = ts == 1 ? raw[i] : (u32)(raw[i * 2] | (raw[i * 2 + 1] << 8));
        draw(mode, ix);
        break;
    }

    // ----- buffers -----
    case F_glGenBuffers: for (u32 i = 0; i < a(0); i++) { while (buffers.count(next_buf)) next_buf++; buffers[next_buf]; hle->wr32(a(1) + i * 4, next_buf++); } break;
    case F_glDeleteBuffers: for (u32 i = 0; i < a(0); i++) { u32 b = hle->rd32(a(1) + i * 4); buffers.erase(b); if (bound_array == b) bound_array = 0; if (bound_element == b) bound_element = 0; } break;
    case F_glBindBuffer: if (a(0) == GL_ARRAY_BUFFER) bound_array = a(1); else bound_element = a(1); if (a(1)) buffers[a(1)]; break;
    case F_glBufferData: {
        auto& b = buffers[a(0) == GL_ARRAY_BUFFER ? bound_array : bound_element];
        b.assign(a(1), 0);
        if (a(2)) hle->read_mem(a(2), b.data(), a(1));
        break;
    }
    case F_glBufferSubData: {
        auto& b = buffers[a(0) == GL_ARRAY_BUFFER ? bound_array : bound_element];
        u32 off = a(1), sz = a(2);
        if (off + sz <= b.size()) hle->read_mem(a(3), b.data() + off, sz);
        break;
    }

    // ----- textures -----
    case F_glGenTextures:
        for (u32 i = 0; i < a(0); i++) { while (textures.count(next_tex)) next_tex++; textures[next_tex]; hle->wr32(a(1) + i * 4, next_tex++); }
        break;
    case F_glDeleteTextures:
        for (u32 i = 0; i < a(0); i++) {
            u32 t = hle->rd32(a(1) + i * 4);
            if (t) textures.erase(t);
            for (auto& b : bound_tex) if (b == t) b = 0;
        }
        break;
    case F_glBindTexture: bound_tex[active_tex] = a(1); textures[a(1)]; break;
    case F_glTexParameterf: case F_glTexParameteri: case F_glTexParameterx:
    case F_glTexParameterfv: case F_glTexParameteriv: case F_glTexParameterxv: {
        Texture* t = cur_texture();
        u32 v;
        if (id == F_glTexParameterf) v = (u32)af(2);
        else if (id == F_glTexParameterfv) { u32 w = hle->rd32(a(2)); float f; memcpy(&f, &w, 4); v = (u32)f; }
        else if (id == F_glTexParameteriv || id == F_glTexParameterxv) v = hle->rd32(a(2));
        else v = a(2);
        switch (a(1)) {
        case GL_TEXTURE_MIN_FILTER: t->min_filter = v; break;
        case GL_TEXTURE_MAG_FILTER: t->mag_filter = v; break;
        case GL_TEXTURE_WRAP_S: t->wrap_s = v; break;
        case GL_TEXTURE_WRAP_T: t->wrap_t = v; break;
        case GL_GENERATE_MIPMAP: t->gen_mipmap = v != 0; break;
        }
        break;
    }
    case F_glTexEnvf: case F_glTexEnvi: case F_glTexEnvx: case F_glTexEnvfv: case F_glTexEnviv: case F_glTexEnvxv: {
        TexEnv& e = texenv[active_tex];
        u32 pn = a(1);
        if (pn == GL_TEXTURE_ENV_COLOR) {
            for (int i = 0; i < 4; i++) {
                u32 w = hle->rd32(a(2) + i * 4);
                if (id == F_glTexEnvxv) e.color[i] = (s32)w / 65536.f;
                else if (id == F_glTexEnviv) e.color[i] = (s32)w / 2147483647.f;
                else memcpy(&e.color[i], &w, 4);
            }
            break;
        }
        u32 v;
        if (id == F_glTexEnvf) v = (u32)af(2);
        else if (id == F_glTexEnvfv) { u32 w = hle->rd32(a(2)); float f; memcpy(&f, &w, 4); v = (u32)f; }
        else if (id == F_glTexEnviv || id == F_glTexEnvxv) v = hle->rd32(a(2));
        else v = a(2);
        if (pn == GL_TEXTURE_ENV_MODE) e.mode = v;
        else if (pn == GL_COMBINE_RGB) e.combine_rgb = v;
        else if (pn == GL_COMBINE_ALPHA) e.combine_alpha = v;
        break;
    }
    case F_glTexImage2D:
        tex_image((s32)a(1), a(2), (s32)a(3), (s32)a(4), a(6), a(7), a(8), false, 0, 0);
        break;
    case F_glTexSubImage2D:
        tex_image((s32)a(1), 0, (s32)a(4), (s32)a(5), a(6), a(7), a(8), true, (s32)a(2), (s32)a(3));
        break;
    case F_glCompressedTexImage2D:
        compressed_tex_image((s32)a(1), a(2), (s32)a(3), (s32)a(4), a(6), a(7));
        break;
    case F_glCompressedTexSubImage2D: set_error(GL_INVALID_OPERATION); break;
    case F_glCopyTexImage2D: case F_glCopyTexSubImage2D: {
        bool sub = id == F_glCopyTexSubImage2D;
        int level = (s32)a(1);
        int xo = sub ? (s32)a(2) : 0, yo = sub ? (s32)a(3) : 0;
        int x = (s32)a(sub ? 4 : 3), y = (s32)a(sub ? 5 : 4), w = (s32)a(sub ? 6 : 5), h = (s32)a(sub ? 7 : 6);
        Texture& t = *cur_texture();
        if (!sub) {
            if ((int)t.levels.size() <= level) { t.levels.resize(level + 1); t.lw.resize(level + 1); t.lh.resize(level + 1); }
            t.levels[level].assign((size_t)w * h, 0xFF000000u); t.lw[level] = w; t.lh[level] = h;
            if (level == 0) { t.w = w; t.h = h; }
            t.has_alpha = false;
        }
        if ((int)t.levels.size() > level)
            for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
                int sx = x + i, sy = H - 1 - (y + j);
                int tx = xo + i, ty = yo + j;
                if (sx < 0 || sy < 0 || sx >= W || sy >= H || tx >= t.lw[level] || ty >= t.lh[level]) continue;
                t.levels[level][(size_t)ty * t.lw[level] + tx] = color[(size_t)sy * W + sx];
            }
        break;
    }
    case F_glReadPixels: {
        int x = (s32)a(0), y = (s32)a(1), w = (s32)a(2), h = (s32)a(3);
        u32 fmt = a(4), type = a(5), p = a(6);
        int bpp = (type == GL_UNSIGNED_BYTE) ? (fmt == GL_RGBA ? 4 : 3) : 2;
        int stride = (w * bpp + 3) & ~3;
        std::vector<u8> out((size_t)stride * h, 0);
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            int sx = x + i, sy = H - 1 - (y + j);
            if (sx < 0 || sy < 0 || sx >= W || sy >= H) continue;
            u32 c = color[(size_t)sy * W + sx];
            u8* q = &out[(size_t)j * stride + i * bpp];
            if (bpp == 2) { u16 v = (u16)((((c >> 19) & 31) << 11) | (((c >> 10) & 63) << 5) | ((c >> 3) & 31)); q[0] = (u8)v; q[1] = (u8)(v >> 8); }
            else { q[0] = (u8)(c >> 16); q[1] = (u8)(c >> 8); q[2] = (u8)c; if (bpp == 4) q[3] = 255; }
        }
        hle->write_mem(p, out.data(), (u32)out.size());
        break;
    }
    case F_glDrawTexsOES: case F_glDrawTexiOES: case F_glDrawTexxOES: case F_glDrawTexfOES: {
        float v[5];
        for (int i = 0; i < 5; i++) {
            if (id == F_glDrawTexfOES) v[i] = af(i);
            else if (id == F_glDrawTexxOES) v[i] = ax(i);
            else if (id == F_glDrawTexsOES) v[i] = (float)(int16_t)a(i);
            else v[i] = (float)(s32)a(i);
        }
        // Screen-aligned textured quad using the full texture (crop rect not tracked)
        float lod[2] = {0, 0};
        int x0 = (int)v[0], y0g = (int)v[1], w = (int)v[3], h = (int)v[4];
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
            int sx = x0 + i, sy = H - 1 - (y0g + j);
            if (sx < 0 || sy < 0 || sx >= W || sy >= H) continue;
            float tc[2][4];
            for (int u = 0; u < 2; u++) { tc[u][0] = (i + 0.5f) / w; tc[u][1] = (j + 0.5f) / h; }
            shade_fragment(sx, sy, v[2], cur_color, tc, 1, lod);
        }
        break;
    }

    default:
        if (hle->log && call_counts[idx] == 1) hle->log(std::string("[gl] unimplemented: ") + g_names[id]);
        break;
    }
    ret(r);
    return true;
}

} // namespace didj
