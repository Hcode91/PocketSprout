// PocketSprout - OpenGL ES 1.1 + EGL implementation used for HLE of libopengles_lite.so
#pragma once
#include "arm926.h"
#include <map>
#include <string>
#include <vector>

namespace didj {

class Hle;

struct Mat4 {
    float m[16];   // column-major
    static Mat4 identity() { Mat4 r; for (int i = 0; i < 16; i++) r.m[i] = (i % 5 == 0) ? 1.f : 0.f; return r; }
    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += m[k * 4 + rr] * b.m[c * 4 + k];
                r.m[c * 4 + rr] = s;
            }
        return r;
    }
};

struct V4 { float x, y, z, w; };

class GLES {
public:
    explicit GLES(Hle* h);
    void reset();
    int find(const char* name);          // function id for a symbol name (-1 = unknown)
    bool call(int id);                   // run HLE function; true if handled
    bool passthrough = false;
    void clear_counts() { call_counts.assign(call_counts.size(), 0); }            // debug: log calls but run original code
    Hle* hle;

    // Render target (host side), presented to the guest framebuffer on eglSwapBuffers
    int W = 320, H = 240;
    std::vector<u32> color;              // ARGB8888
    std::vector<float> depth;
    u64 swaps = 0;
    u64 last_swap_frame = ~0ull;

private:
    std::vector<int> call_counts;
    int err = 0;

    // ---- matrices ----
    std::vector<Mat4> stacks[4];         // modelview, projection, texture0, texture1
    int matrix_mode = 0;                 // 0 MV, 1 P, 2 tex
    int active_tex = 0, client_tex = 0;
    Mat4& cur_matrix();
    std::vector<Mat4>& cur_stack();

    // ---- state ----
    struct Light {
        float ambient[4] = {0, 0, 0, 1}, diffuse[4] = {0, 0, 0, 1}, specular[4] = {0, 0, 0, 1};
        float position[4] = {0, 0, 1, 0}, spot_dir[3] = {0, 0, -1};
        float spot_exp = 0, spot_cutoff = 180, att[3] = {1, 0, 0};
    } lights[8];
    struct Material {
        float ambient[4] = {0.2f, 0.2f, 0.2f, 1}, diffuse[4] = {0.8f, 0.8f, 0.8f, 1};
        float specular[4] = {0, 0, 0, 1}, emission[4] = {0, 0, 0, 1}, shininess = 0;
    } mat;
    float light_model_ambient[4] = {0.2f, 0.2f, 0.2f, 1};
    bool two_side = false;

    u32 enables = 0;                     // bit flags (see gles.cpp)
    bool tex_enabled[2] = {false, false};
    u32 client_states = 0;
    float cur_color[4] = {1, 1, 1, 1}, cur_normal[3] = {0, 0, 1}, cur_tc[2][4] = {{0, 0, 0, 1}, {0, 0, 0, 1}};
    float clear_color[4] = {0, 0, 0, 0}, clear_depth = 1;
    int vp[4] = {0, 0, 320, 240};
    float depth_n = 0, depth_f = 1;
    int scissor[4] = {0, 0, 320, 240};
    u32 blend_src = 1, blend_dst = 0;
    u32 alpha_func = 0x207; float alpha_ref = 0;
    u32 depth_func = 0x201; bool depth_mask = true;
    bool color_mask[4] = {true, true, true, true};
    u32 cull_mode = 0x405, front_face = 0x901;
    u32 shade_model = 0x1D01;
    u32 fog_mode = 0x800; float fog_density = 1, fog_start = 0, fog_end = 1, fog_color[4] = {0, 0, 0, 0};
    float point_size = 1, line_width = 1;
    int unpack_align = 4;
    float poly_offset_factor = 0, poly_offset_units = 0;

    struct TexEnv { u32 mode = 0x2100; float color[4] = {0, 0, 0, 0};
                    u32 combine_rgb = 0x2100, combine_alpha = 0x2100; } texenv[2];

    struct Pointer { int size = 4; u32 type = 0x1406; int stride = 0; u32 ptr = 0; u32 buffer = 0; };
    Pointer vptr, cptr, nptr, tptr[2], psptr;

    // ---- objects ----
    struct Texture {
        int w = 0, h = 0;
        std::vector<std::vector<u32>> levels;   // ARGB8888 per mip level
        std::vector<int> lw, lh;
        u32 min_filter = 0x2702, mag_filter = 0x2601, wrap_s = 0x2901, wrap_t = 0x2901;
        bool gen_mipmap = false;
        bool has_alpha = true;
    };
    std::map<u32, Texture> textures;
    u32 bound_tex[2] = {0, 0};
    u32 next_tex = 1;
    std::map<u32, std::vector<u8>> buffers;
    u32 bound_array = 0, bound_element = 0;
    u32 next_buf = 1;

    // strings in guest memory
    std::map<u32, u32> guest_strings;
    u32 guest_string(u32 key, const char* s);

    // ---- dispatch helpers ----
    u32 a(int i);
    float af(int i);
    float ax(int i);   // 16.16 fixed
    void set_error(int e) { if (!err) err = e; }
    void ret(u32 v);

    void do_gl(const char* name);
    void enable(u32 cap, bool on);
    bool is_enabled(u32 cap);
    void get_values(u32 pname, std::vector<float>& out, bool* ints);

    // ---- textures ----
    Texture* cur_texture();
    void tex_image(int level, u32 ifmt, int w, int h, u32 fmt, u32 type, u32 pixels, bool sub, int xo, int yo);
    void compressed_tex_image(int level, u32 ifmt, int w, int h, u32 size, u32 data);
    void gen_mips(Texture& t);

public:
    struct Vtx {
        V4 clip;            // clip-space position
        float col[4];
        float tc[2][4];
        float fog;          // fog factor (1 = no fog)
        float eyez;
        // after projection
        float sx, sy, sz, invw;
    };
private:
    void fetch_attr(const Pointer& p, u32 idx, int comps, float* out, bool normalize_ubyte);
    void transform(u32 idx, Vtx& v);
    void draw(u32 mode, const std::vector<u32>& idx);
    void draw_triangle(const Vtx& a, const Vtx& b, const Vtx& c);
    void clip_and_raster(Vtx* poly, int n);
    void raster_triangle(const Vtx& a, const Vtx& b, const Vtx& c);
    void raster_line(const Vtx& a, const Vtx& b);
    void raster_point(const Vtx& a);
    void project(Vtx& v);
    void shade_fragment(int x, int y, float z, const float* col, const float tc[2][4], float fog, float lod[2]);
    u32 sample(const Texture& t, float s, float tt, float lod);

    void clear(u32 mask);
    void present();

    void light_vertex(const float* eye, const float* n, const float* in_col, float* out);
    bool lighting_on();
};

} // namespace didj
