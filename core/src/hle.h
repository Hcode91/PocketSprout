// DidjDroid - high-level emulation of LeapFrog's OpenGL ES library (libopengles_lite.so)
//
// The Didj's 3D engine (MagicEyes GA3D: programmable geometry engine + triangle setup +
// rasterizer) is driven entirely from userspace by libopengles_lite.so. Instead of emulating
// that undocumented hardware, we trap calls to the library's exported gl*/egl* functions and
// run our own OpenGL ES 1.1 implementation (gles.cpp), rendering into guest RAM.
#pragma once
#include "arm926.h"
#include <map>
#include <string>
#include <vector>
#include <functional>

namespace didj {

class Didj;
class GLES;

class Hle {
public:
    Hle(Didj* sys, ARM926* cpu);
    ~Hle();
    void reset();

    void on_swi(u32 pc, u32 imm);
    bool on_hook(u32 pc);
    void on_ttb();

    bool log_calls = false;
    std::function<void(const std::string&)> log;

    GLES* gl;

    // ---- guest memory helpers (current process) ----
    bool read_mem(u32 va, void* dst, u32 len);
    bool write_mem(u32 va, const void* src, u32 len);
    u32 rd32(u32 va) { u32 v = 0; read_mem(va, &v, 4); return v; }
    void wr32(u32 va, u32 v) { write_mem(va, &v, 4); }
    std::string read_str(u32 va, u32 max = 256);
    u32 arg(int i);   // i-th 32-bit argument (AAPCS)
    float argf(int i) { u32 v = arg(i); float f; memcpy(&f, &v, 4); return f; }

    // Scratch memory inside the library's own data segment (unused once we HLE it)
    u32 scratch_alloc(u32 len);

    ARM926* cpu;
    Didj* sys;

private:
    struct Proc {
        int fd = -1;
        u32 base = 0, size = 0;
        bool parsed = false;
        int parse_tries = 0;
        std::map<u32, int> funcs;   // address -> function id
        u32 data_lo = 0, data_hi = 0, scratch_next = 0;
    };
    std::map<u32, Proc> procs;      // keyed by TTB
    Proc* cur = nullptr;
    u32 watch_ttb = 0;
    enum { W_NONE, W_OPEN, W_MMAP } watch_kind = W_NONE;
    u32 mmap_len = 0;

    bool parse_lib(Proc& p);
    void apply_range();
};

} // namespace didj
