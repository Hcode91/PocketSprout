// Differential test: JIT vs interpreter on random ARM instruction blocks.
// Build for aarch64 and run under qemu-aarch64 (or natively on an arm64 device).
#include "../core/src/arm926.h"
#include "../core/src/jit.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace didj;

struct FlatBus : Bus {
    std::vector<u8> ram;
    FlatBus() : ram(1 << 20) {}
    u8* page_ptr(u32 pa) override { return pa < ram.size() ? &ram[pa & ~0xFFFu] : nullptr; }
    u32 read32(u32 pa) override { u32 v = 0; if (pa <= ram.size() - 4) memcpy(&v, &ram[pa], 4); return v; }
    u16 read16(u32 pa) override { u16 v = 0; if (pa <= ram.size() - 2) memcpy(&v, &ram[pa], 2); return v; }
    u8 read8(u32 pa) override { return pa < ram.size() ? ram[pa] : 0; }
    void write32(u32 pa, u32 v) override { if (pa <= ram.size() - 4) memcpy(&ram[pa], &v, 4); }
    void write16(u32 pa, u16 v) override { if (pa <= ram.size() - 2) memcpy(&ram[pa], &v, 2); }
    void write8(u32 pa, u8 v) override { if (pa < ram.size()) ram[pa] = v; }
};

static std::mt19937 rng(12345);
static u32 rnd(u32 n) { return rng() % n; }
static const u32 CODE = 0x1000, DATA = 0x40000, TTB = 0x80000, L2 = 0x84000;

// destination registers allowed for random ops (bases r8-r11 are preserved)
static int rdst() { static const int d[] = {0, 1, 2, 3, 4, 5, 6, 7, 12, 14}; return d[rnd(10)]; }
static int rsrc() { return rnd(15); }
static int rbase() { return 8 + rnd(4); }

static u32 gen(u32 idx, u32 n) {
    u32 cond = rnd(3) ? 0xE : rnd(15);
    u32 k = rnd(100);
    if (k < 40) { // data processing
        u32 op = rnd(16), s = rnd(2);
        if (op >= 8 && op <= 11) s = 1;
        u32 rd = rdst(), rn = rsrc();
        u32 op2;
        u32 form = rnd(10);
        if (form < 4) op2 = (1u << 25) | rnd(4096);
        else if (form < 9) op2 = (rnd(32) << 7) | (rnd(4) << 5) | rsrc();
        else op2 = (rdst() << 8) | (rnd(4) << 5) | 0x10 | rsrc();       // reg shift by reg (fallback)
        return (cond << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | op2;
    }
    if (k < 47) { // multiply
        u32 op = (u32[]){0, 1, 4, 5, 6, 7}[rnd(6)];
        int a = rdst(), b = rdst();
        while (b == a) b = rdst();
        return (cond << 28) | (op << 21) | (rnd(2) << 20) | (a << 16) | (b << 12) | (rsrc() << 8) | 0x90 | rsrc();
    }
    if (k < 65) { // ldr/str
        u32 I = rnd(2), P = rnd(2), U = rnd(2), B = rnd(2), W = P ? rnd(2) : 0, L = rnd(2);
        u32 rn = rbase(), rd = rnd(3) ? rdst() : rbase();
        u32 off = I ? ((rnd(3) << 7) | (rnd(4) << 5) | rdst()) : rnd(256);
        if (I && ((off >> 5) & 3) == 3 && !((off >> 7) & 31)) off |= 1 << 7;  // avoid RRX huge offsets
        if (I && ((off >> 5) & 3)) off = (off & ~0x60u);                     // LSL only for reg offsets
        if (I) { off = (off & ~0xFu) | (u32)(8 + rnd(4)); off &= ~(0x1Fu << 7); off |= rnd(3) << 7; }
        if (rd == rn && (W || !P)) rd = 0;
        return (cond << 28) | (1u << 26) | (I << 25) | (P << 24) | (U << 23) | (B << 22) | (W << 21) | (L << 20) | (rn << 16) | (rd << 12) | off;
    }
    if (k < 75) { // halfword / signed
        u32 P = rnd(2), U = rnd(2), W = P ? rnd(2) : 0, L = rnd(2);
        u32 sh = L ? 1 + rnd(3) : 1;
        u32 rn = rbase(), rd = rdst();
        u32 imm = rnd(64);
        return (cond << 28) | (P << 24) | (U << 23) | (1u << 22) | (W << 21) | (L << 20) | (rn << 16) | (rd << 12) | ((imm >> 4) << 8) | 0x90 | (sh << 5) | (imm & 0xF);
    }
    if (k < 83) { // ldm/stm
        u32 P = rnd(2), U = rnd(2), W = rnd(2), L = rnd(2);
        u32 rn = rbase();
        u32 list = 0;
        while (!list) { static const int d[] = {0, 1, 2, 3, 4, 5, 6, 7, 12, 14}; for (int i = 0; i < 10; i++) if (rnd(3) == 0) list |= 1u << d[i]; }
        if (rnd(4) == 0) list |= 1u << rbase();
        if (W) list &= ~(1u << rn);
        if (!list) list = 1;
        return (cond << 28) | (4u << 25) | (P << 24) | (U << 23) | (W << 21) | (L << 20) | (rn << 16) | list;
    }
    if (k < 88) { // forward branch within block
        u32 remain = n - idx;
        u32 tgt = rnd(remain + 1);                 // 0..remain (remain = terminator)
        s32 off = (s32)tgt - 2;                    // relative to pc+8 in words
        if (off < -1) off = -1;
        return (cond << 28) | (0xAu << 24) | (rnd(4) == 0 ? (1u << 24) : 0) | ((u32)off & 0xFFFFFF);
    }
    if (k < 91) return (cond << 28) | 0x016F0F10 | (rdst() << 12) | rsrc();                     // CLZ
    if (k < 94) return (cond << 28) | 0x010F0000 | (rdst() << 12);                               // MRS
    if (k < 96) return (cond << 28) | 0x0328F000 | rnd(256);                                     // MSR flags imm
    if (k < 98) return (cond << 28) | 0x01000050 | (rnd(4) << 21) | (rsrc() << 16) | (rdst() << 12) | rsrc(); // QADD
    return (cond << 28) | 0x01000080 | ((u32[]){0, 1, 3}[rnd(3)] << 21) | (rdst() << 16) | (rdst() << 12) | (rsrc() << 8) | (rnd(4) << 5) | rsrc();
}

struct Sys {
    FlatBus bus;
    ARM926 cpu{&bus};
};

static void setup(Sys& s, const std::vector<u32>& prog, const u32* regs, u32 cpsr, bool mmu, u32 unmapped_page) {
    std::fill(s.bus.ram.begin(), s.bus.ram.end(), 0);
    for (size_t i = 0; i < prog.size(); i++) memcpy(&s.bus.ram[CODE + i * 4], &prog[i], 4);
    // abort/undef vectors: branch to self
    u32 self = 0xEAFFFFFE;
    for (u32 v = 0; v < 0x20; v += 4) memcpy(&s.bus.ram[v], &self, 4);
    std::mt19937 drng(777);
    for (u32 a = DATA; a < DATA + 0x10000; a++) s.bus.ram[a] = (u8)drng();
    s.cpu.reset();
    s.cpu.switch_mode(cpsr & 0x1F);
    s.cpu.cpsr = cpsr;
    for (int i = 0; i < 15; i++) s.cpu.r[i] = regs[i];
    s.cpu.r[15] = CODE;
    if (mmu) {
        // first MB through a coarse table, identity mapped, AP=3; one page left unmapped
        u32 l1 = L2 | 1;
        memcpy(&s.bus.ram[TTB], &l1, 4);
        for (u32 p = 0; p < 256; p++) {
            u32 d = (p << 12) | (0xFF << 4) | 2;
            if (p == unmapped_page) d = 0;
            memcpy(&s.bus.ram[L2 + p * 4], &d, 4);
        }
        s.cpu.cp15_ttb = TTB;
        s.cpu.cp15_dacr = 1;    // domain 0 client
        s.cpu.cp15_control |= 1;
        s.cpu.flush_tlb();
    }
}

int main(int argc, char** argv) {
    int iters = argc > 1 ? atoi(argv[1]) : 2000;
    if (!Jit::available()) { printf("JIT not available on this host\n"); return 2; }
    int fails = 0, aborts = 0;
    Sys* A = new Sys();
    Sys* B = new Sys();
    Jit jit(&B->cpu, B->bus.ram.data(), (u32)B->bus.ram.size());
    if (!jit.ok()) { printf("no exec memory\n"); return 2; }
    jit.stop_on_exception = true;
    for (int it = 0; it < iters; it++) {
        u32 n = 4 + rnd(40);
        std::vector<u32> prog;
        for (u32 i = 0; i < n; i++) prog.push_back(gen(i, n));
        bool loop = rnd(3) == 0;
        if (loop) {
            // subs r13, r13, #1 ; bne CODE  (r13 is never written by random ops)
            prog.push_back(0xE25DD001);
            s32 off = (s32)(0 - (n + 1) * 4 - 8) / 4;
            prog.push_back(0x1A000000 | ((u32)off & 0xFFFFFF));
            n += 2;
        }
        prog.push_back(0xEAFFFFFE);  // terminator: b .
        u32 term = CODE + n * 4;
        u32 regs[15];
        for (int i = 0; i < 15; i++) regs[i] = rnd(4) == 0 ? rnd(256) : (u32)rng();
        bool mmu = rnd(2);
        u32 unmapped = 0x40 + rnd(16);   // a page inside the data area
        for (int i = 8; i < 12; i++) regs[i] = DATA + 0x800 + rnd(0xE000) / 4 * 4 + (rnd(8) == 0 ? rnd(4) : 0);
        regs[13] = 1 + rnd(40);
        u32 cpsr = (rnd(2) ? 0x1F : 0x10) | (rnd(16) << 28);
        setup(*A, prog, regs, cpsr, mmu, unmapped);
        setup(*B, prog, regs, cpsr, mmu, unmapped);
        jit.flush();
        for (int s = 0; s < 20000 && A->cpu.r[15] != term && A->cpu.r[15] >= 0x20; s++) A->cpu.run(1);
        u32 bud = rnd(3) == 0 ? 1 : 1 + rnd(3000);
        bool dbg = getenv("DBG") && atoi(getenv("DBG")) == it;
        if (dbg) {
            // lock-step: one JIT dispatch at a time vs the same number of interpreter steps
            Sys* T = new Sys(); setup(*T, prog, regs, cpsr, mmu, unmapped);
            Sys* U = new Sys(); setup(*U, prog, regs, cpsr, mmu, unmapped);
            Jit j2(&U->cpu, U->bus.ram.data(), (u32)U->bus.ram.size());
            for (int s = 0; s < 5000; s++) {
                u32 pc = U->cpu.r[15];
                s64 nn = j2.run(1);
                for (s64 k = 0; k < nn; k++) T->cpu.run(1);
                bool d = memcmp(T->cpu.r, U->cpu.r, 64) || T->cpu.cpsr != U->cpu.cpsr || memcmp(&T->bus.ram[DATA], &U->bus.ram[DATA], 0x10000);
                if (d) {
                    printf("DIVERGE after block at %08X (n=%lld)\n", pc, (long long)nn);
                    for (int i = 0; i < 16; i++) printf("  r%-2d I=%08X J=%08X%s\n", i, T->cpu.r[i], U->cpu.r[i], T->cpu.r[i] != U->cpu.r[i] ? "  <--" : "");
                    printf("  cpsr I=%08X J=%08X\n", T->cpu.cpsr, U->cpu.cpsr);
                    for (u32 a = DATA; a < DATA + 0x10000; a++) if (T->bus.ram[a] != U->bus.ram[a]) { printf("  mem[%X] I=%02X J=%02X\n", a, T->bus.ram[a], U->bus.ram[a]); break; }
                    break;
                }
                if (U->cpu.r[15] == term || U->cpu.r[15] < 0x20) { printf("no divergence in lock-step\n"); break; }
            }
            delete T; delete U;
            printf("budget=%u r13=%u\n", bud, regs[13]);
            for (int variant = 0; variant < 2; variant++) {
                Sys* V = new Sys(); setup(*V, prog, regs, cpsr, mmu, unmapped);
                Jit j3(&V->cpu, V->bus.ram.data(), (u32)V->bus.ram.size());
                for (int s = 0; s < 20000 && V->cpu.r[15] != term && V->cpu.r[15] >= 0x20; s++) j3.run(variant ? bud : 1);
                printf("fresh jit, budget %u: r15=%08X far=%08X r14=%08X\n", variant ? bud : 1, V->cpu.r[15], V->cpu.cp15_far, V->cpu.r[14]);
                delete V;
            }
        }
        for (int s = 0; s < 20000 && B->cpu.r[15] != term && B->cpu.r[15] >= 0x20; s++) {
            u32 pc = B->cpu.r[15];
            jit.run(bud);
            if (dbg && s < 3000) printf("J %08X -> %08X cpsr=%08X far=%08X\n", pc, B->cpu.r[15], B->cpu.cpsr, B->cpu.cp15_far);
        }
        bool same = true;
        for (int i = 0; i < 16; i++) if (A->cpu.r[i] != B->cpu.r[i]) same = false;
        if (A->cpu.cpsr != B->cpu.cpsr) same = false;
        if (A->cpu.cp15_dfsr != B->cpu.cp15_dfsr || A->cpu.cp15_far != B->cpu.cp15_far) same = false;
        if (A->cpu.spsr != B->cpu.spsr) same = false;
        if (memcmp(&A->bus.ram[DATA], &B->bus.ram[DATA], 0x10000)) same = false;
        if (A->cpu.r[15] < 0x20) aborts++;
        if (!same) {
            fails++;
            if (fails <= 8) {
                printf("MISMATCH iter %d mmu=%d cpsr_in=%08X n=%u\n", it, mmu, cpsr, n);
                for (u32 i = 0; i < n; i++) printf("  %08X: %08X\n", CODE + i * 4, prog[i]);
                for (int i = 0; i < 16; i++) if (A->cpu.r[i] != B->cpu.r[i]) printf("  r%d interp=%08X jit=%08X\n", i, A->cpu.r[i], B->cpu.r[i]);
                if (A->cpu.cpsr != B->cpu.cpsr) printf("  cpsr interp=%08X jit=%08X\n", A->cpu.cpsr, B->cpu.cpsr);
                if (A->cpu.cp15_far != B->cpu.cp15_far || A->cpu.cp15_dfsr != B->cpu.cp15_dfsr)
                    printf("  far %08X/%08X fsr %X/%X\n", A->cpu.cp15_far, B->cpu.cp15_far, A->cpu.cp15_dfsr, B->cpu.cp15_dfsr);
                for (u32 a = DATA; a < DATA + 0x10000; a++) if (A->bus.ram[a] != B->bus.ram[a]) { printf("  mem[%X] %02X/%02X\n", a, A->bus.ram[a], B->bus.ram[a]); break; }
            }
        }
    }
    printf("iterations=%d fails=%d (aborting cases=%d) blocks=%llu fallbacks=%llu interp=%llu\n", iters, fails, aborts,
           (unsigned long long)jit.blocks_compiled, (unsigned long long)jit.fallbacks, (unsigned long long)jit.interp_steps);
    return fails ? 1 : 0;
}
