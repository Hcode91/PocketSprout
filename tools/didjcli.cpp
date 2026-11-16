// DidjDroid desktop test runner.
// Usage: didjcli <nand.bin> [-c cart] [-f frames] [-o screenshot.ppm] [-t tracefile] [-u max_unknown]
#include "../core/src/didj.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace didj;

static FILE* trace_f = nullptr;
static void trace(ARM926* cpu, u32 pc, void*) {
    fprintf(trace_f, "%08X %c\n", pc, cpu->in_thumb() ? 'T' : 'A');
}

static int sys_log_frame = -1, cur_frame = 0;
static u32 dump_at = 0;
static void swi_log(ARM926* cpu, u32 pc, u32 imm, void*) {
    if (sys_log_frame < 0 || cur_frame < sys_log_frame || cur_frame > sys_log_frame + 5) return;
    if ((cpu->cpsr & 0x1F) != 0x10) return;
    u32 nr = imm >= 0x900000 ? imm - 0x900000 : cpu->r[7];
    fprintf(stderr, "SYS f%d pc=%08X nr=%u r0=%08X r1=%08X r2=%08X\n", cur_frame, pc, nr, cpu->r[0], cpu->r[1], cpu->r[2]);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s nand.bin [-c cart] [-f frames] [-o out.ppm] [-t trace]\n", argv[0]); return 1; }
    std::string nand = argv[1], cart, out, tracefile, dumpfile;
    int frames = 120;
    int maxunk = 300;
    struct Press { int frame; u32 mask; };
    std::vector<Press> presses;   // -k frame:mask  (held for 8 frames)
    int shot_every = 0;           // -s N : screenshot every N frames to <out>_NNNN.ppm
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-c" && i + 1 < argc) cart = argv[++i];
        else if (a == "-f" && i + 1 < argc) frames = atoi(argv[++i]);
        else if (a == "-o" && i + 1 < argc) out = argv[++i];
        else if (a == "-t" && i + 1 < argc) tracefile = argv[++i];
        else if (a == "-u" && i + 1 < argc) maxunk = atoi(argv[++i]);
        else if (a == "-d" && i + 1 < argc) dumpfile = argv[++i];
        else if (a == "-k" && i + 1 < argc) { Press p; sscanf(argv[++i], "%d:%x", &p.frame, &p.mask); presses.push_back(p); }
        else if (a == "-s" && i + 1 < argc) shot_every = atoi(argv[++i]);
        else if (a == "-x" && i + 1 < argc) dump_at = strtoul(argv[++i], nullptr, 16);
        else if (a == "-y" && i + 1 < argc) sys_log_frame = atoi(argv[++i]);
    }
    Didj d;
    d.log_unknown = maxunk;
    std::string err;
    if (!d.load_nand(nand, &err)) { fprintf(stderr, "NAND: %s\n", err.c_str()); return 1; }
    if (!cart.empty() && !d.load_cart(cart, &err)) { fprintf(stderr, "cart: %s\n", err.c_str()); return 1; }
    d.on_syscall = [](ARM926* c, u32 pc, u32 imm) { swi_log(c, pc, imm, nullptr); };
    if (getenv("GL_LOG")) d.hle->log_calls = true;
    d.on_uart = [](char c) { fputc(c, stdout); fflush(stdout); };
    if (!tracefile.empty()) { trace_f = fopen(tracefile.c_str(), "w"); d.cpu->trace_hook = trace; }
    auto save = [&](const std::string& path) {
        FILE* o = fopen(path.c_str(), "wb");
        fprintf(o, "P6\n%d %d\n255\n", Didj::SCREEN_W, Didj::SCREEN_H);
        const u32* fb = d.framebuffer();
        for (int i = 0; i < Didj::SCREEN_W * Didj::SCREEN_H; i++) {
            u8 px[3] = {(u8)(fb[i] >> 16), (u8)(fb[i] >> 8), (u8)fb[i]};
            fwrite(px, 1, 3, o);
        }
        fclose(o);
    };
    for (int f = 0; f < frames; f++) {
        u32 mask = 0;
        for (auto& p : presses) if (f >= p.frame && f < p.frame + 8) mask |= p.mask;
        d.set_buttons(mask);
        cur_frame = f;
        if (sys_log_frame >= 0 && f == sys_log_frame) d.log_user_io = 3000;
        d.run_frame();
        if (shot_every && !out.empty() && (f + 1) % shot_every == 0) {
            char b[32]; snprintf(b, sizeof b, "_%05d.ppm", f + 1);
            save(out + b);
        }
        if (f % 60 == 59) fprintf(stderr, "[frame %d] pc=%08X mode=%02X insns=%llu\n", f + 1, d.cpu->r[15], d.cpu->cpsr & 0x1F,
                                  (unsigned long long)d.cpu->insn_count);
    }
    if (!out.empty()) {
        FILE* o = fopen(out.c_str(), "wb");
        fprintf(o, "P6\n%d %d\n255\n", Didj::SCREEN_W, Didj::SCREEN_H);
        const u32* fb = d.framebuffer();
        for (int i = 0; i < Didj::SCREEN_W * Didj::SCREEN_H; i++) {
            u8 px[3] = {(u8)(fb[i] >> 16), (u8)(fb[i] >> 8), (u8)fb[i]};
            fwrite(px, 1, 3, o);
        }
        fclose(o);
    }
    if (trace_f) fclose(trace_f);
    if (!dumpfile.empty()) { // dump physical RAM
        FILE* o = fopen(dumpfile.c_str(), "wb");
        for (u32 a = 0; a < Didj::RAM_SIZE; a += 4096) fwrite(d.page_ptr(a), 1, 4096, o);
        fclose(o);
    }
    { // dump code around final PC (virtual) for disassembly
        FILE* o = fopen("/tmp/pcdump.bin", "wb");
        u32 base = dump_at ? dump_at : (d.cpu->r[15] & ~0xFFu) - 0x100;
        for (u32 a = base; a < base + 0x300; a += 4) { u32 w = d.cpu->debug_read32(a); fwrite(&w, 4, 1, o); }
        fclose(o);
        fprintf(stderr, "pcdump base=%08X\n", base);
    }
    if (getenv("MLC_DUMP")) { // mlc_dump
        for (u32 a = 0xC0004000; a < 0xC00040A8; a += 4) fprintf(stderr, "MLC+%02X=%08X%s", a & 0xFF, d.read32(a), ((a >> 2) & 3) == 3 ? "\n" : "  ");
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "hle range %08X+%X ttb=%08X\n", d.cpu->hle_base, d.cpu->hle_size, d.cpu->cp15_ttb);
    fprintf(stderr, "final pc=%08X cpsr=%08X\n", d.cpu->r[15], d.cpu->cpsr);
    for (int i = 0; i < 16; i++) fprintf(stderr, "r%d=%08X%s", i, d.cpu->r[i], (i % 4 == 3) ? "\n" : " ");
    return 0;
}
