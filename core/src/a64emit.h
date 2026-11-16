// PocketSprout - minimal AArch64 machine code emitter for the JIT
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace didj {
namespace a64 {

typedef uint32_t u32;
typedef uint64_t u64;

enum Cond { EQ = 0, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL };
enum Shift { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };
static const int ZR = 31, SP = 31;

struct Label {
    int pos = -1;
    std::vector<int> refs;   // instruction indices needing patch
};

class Emitter {
public:
    u32* buf = nullptr;
    int cap = 0, n = 0;
    bool overflow = false;

    void init(u32* b, int capacity) { buf = b; cap = capacity; n = 0; overflow = false; }
    void emit(u32 w) { if (n < cap) buf[n++] = w; else overflow = true; }
    int here() const { return n; }

    // ---- labels / branches ----
    void bind(Label& l) {
        l.pos = n;
        for (int r : l.refs) patch(r, n);
        l.refs.clear();
    }
    void patch(int at, int target) {
        if (at >= cap) return;
        u32 ins = buf[at];
        int32_t off = target - at;
        if ((ins & 0x7C000000) == 0x14000000) buf[at] = (ins & 0xFC000000) | ((u32)off & 0x03FFFFFF);       // B/BL
        else if ((ins & 0xFF000010) == 0x54000000) buf[at] = (ins & 0xFF00001F) | (((u32)off & 0x7FFFF) << 5); // B.cond
        else if ((ins & 0x7E000000) == 0x34000000) buf[at] = (ins & 0xFF00001F) | (((u32)off & 0x7FFFF) << 5); // CBZ/CBNZ
        else if ((ins & 0x7E000000) == 0x36000000) buf[at] = (ins & 0xFFF8001F) | (((u32)off & 0x3FFF) << 5);  // TBZ/TBNZ
    }
    void ref(Label& l) { if (l.pos >= 0) patch(n - 1, l.pos); else l.refs.push_back(n - 1); }

    void b(Label& l) { emit(0x14000000); ref(l); }
    void bcond(Cond c, Label& l) { emit(0x54000000 | c); ref(l); }
    void cbz(int rt, Label& l) { emit(0x34000000 | rt); ref(l); }
    void cbnz(int rt, Label& l) { emit(0x35000000 | rt); ref(l); }
    void cbz_x(int rt, Label& l) { emit(0xB4000000 | rt); ref(l); }
    void tbz(int rt, int bit, Label& l) { emit(0x36000000 | ((bit & 31) << 19) | rt); ref(l); }
    void tbnz(int rt, int bit, Label& l) { emit(0x37000000 | ((bit & 31) << 19) | rt); ref(l); }
    void blr(int rn) { emit(0xD63F0000 | (rn << 5)); }
    void ret() { emit(0xD65F03C0); }

    // ---- moves ----
    void mov_imm32(int rd, u32 v) {
        u32 lo = v & 0xFFFF, hi = v >> 16;
        if (hi == 0) { emit(0x52800000 | (lo << 5) | rd); return; }
        if (hi == 0xFFFF && lo != 0) { emit(0x12800000 | ((~lo & 0xFFFF) << 5) | rd); return; }   // MOVN
        if (lo == 0) { emit(0x52800000 | (1 << 21) | (hi << 5) | rd); return; }
        emit(0x52800000 | (lo << 5) | rd);
        emit(0x72800000 | (1 << 21) | (hi << 5) | rd);
    }
    void mov_imm64(int rd, u64 v) {
        emit(0xD2800000 | ((u32)(v & 0xFFFF) << 5) | rd);
        for (int i = 1; i < 4; i++) {
            u32 part = (u32)(v >> (16 * i)) & 0xFFFF;
            if (part) emit(0xF2800000 | (i << 21) | (part << 5) | rd);
        }
    }
    void mov(int rd, int rm) { emit(0x2A0003E0 | (rm << 16) | rd); }            // W
    void mov_x(int rd, int rm) { emit(0xAA0003E0 | (rm << 16) | rd); }          // X

    // ---- arithmetic (W, shifted register) ----
    void alu(u32 base, int rd, int rn, int rm, Shift sh = LSL, int amt = 0) {
        emit(base | ((u32)sh << 22) | (rm << 16) | ((amt & 63) << 10) | (rn << 5) | rd);
    }
    void add(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0x0B000000, rd, rn, rm, sh, a); }
    void adds(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0x2B000000, rd, rn, rm, sh, a); }
    void sub(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0x4B000000, rd, rn, rm, sh, a); }
    void subs(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0x6B000000, rd, rn, rm, sh, a); }
    void and_(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0x0A000000, rd, rn, rm, sh, a); }
    void ands(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0x6A000000, rd, rn, rm, sh, a); }
    void orr(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0x2A000000, rd, rn, rm, sh, a); }
    void eor(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0x4A000000, rd, rn, rm, sh, a); }
    void bic(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0x0A200000, rd, rn, rm, sh, a); }
    void orn(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0x2A200000, rd, rn, rm, sh, a); }
    void mvn(int rd, int rm) { orn(rd, ZR, rm); }
    // X forms
    void add_x(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0x8B000000, rd, rn, rm, sh, a); }
    void sub_x(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0xCB000000, rd, rn, rm, sh, a); }
    void add_x_uxtw(int rd, int rn, int rm) { emit(0x8B204000 | (rm << 16) | (rn << 5) | rd); }
    void orr_x(int rd, int rn, int rm, Shift sh = LSL, int a = 0) { alu(0xAA000000, rd, rn, rm, sh, a); }
    void ands_x(int rd, int rn, int rm) { alu(0xEA000000, rd, rn, rm); }

    void adc(int rd, int rn, int rm) { emit(0x1A000000 | (rm << 16) | (rn << 5) | rd); }
    void adcs(int rd, int rn, int rm) { emit(0x3A000000 | (rm << 16) | (rn << 5) | rd); }
    void sbc(int rd, int rn, int rm) { emit(0x5A000000 | (rm << 16) | (rn << 5) | rd); }
    void sbcs(int rd, int rn, int rm) { emit(0x7A000000 | (rm << 16) | (rn << 5) | rd); }

    // immediates (imm12, optionally shifted by 12)
    void add_imm(int rd, int rn, u32 imm) { emit(0x11000000 | ((imm & 0xFFF) << 10) | (rn << 5) | rd); }
    void sub_imm(int rd, int rn, u32 imm) { emit(0x51000000 | ((imm & 0xFFF) << 10) | (rn << 5) | rd); }
    void cmp_imm(int rn, u32 imm) { emit(0x71000000 | ((imm & 0xFFF) << 10) | (rn << 5) | ZR); }
    void add_imm_x(int rd, int rn, u32 imm) { emit(0x91000000 | ((imm & 0xFFF) << 10) | (rn << 5) | rd); }
    void cmp(int rn, int rm) { subs(ZR, rn, rm); }

    // bitfield
    void ubfm(int rd, int rn, int immr, int imms) { emit(0x53000000 | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
    void sbfm(int rd, int rn, int immr, int imms) { emit(0x13000000 | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
    void bfm(int rd, int rn, int immr, int imms) { emit(0x33000000 | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
    void ubfm_x(int rd, int rn, int immr, int imms) { emit(0xD3400000 | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
    void lsl_imm(int rd, int rn, int s) { ubfm(rd, rn, (32 - s) & 31, 31 - s); }
    void lsr_imm(int rd, int rn, int s) { ubfm(rd, rn, s, 31); }
    void asr_imm(int rd, int rn, int s) { sbfm(rd, rn, s, 31); }
    void ror_imm(int rd, int rn, int s) { emit(0x13800000 | (rn << 16) | ((s & 31) << 10) | (rn << 5) | rd); } // EXTR
    void ubfx(int rd, int rn, int lsb, int width) { ubfm(rd, rn, lsb, lsb + width - 1); }
    void bfi(int rd, int rn, int lsb, int width) { bfm(rd, rn, (32 - lsb) & 31, width - 1); }
    void sxtb(int rd, int rn) { sbfm(rd, rn, 0, 7); }
    void sxth(int rd, int rn) { sbfm(rd, rn, 0, 15); }
    void lsl_x_imm(int rd, int rn, int s) { ubfm_x(rd, rn, (64 - s) & 63, 63 - s); }
    void lsr_x_imm(int rd, int rn, int s) { ubfm_x(rd, rn, s, 63); }

    // variable shifts (W)
    void lslv(int rd, int rn, int rm) { emit(0x1AC02000 | (rm << 16) | (rn << 5) | rd); }
    void lsrv(int rd, int rn, int rm) { emit(0x1AC02400 | (rm << 16) | (rn << 5) | rd); }
    void asrv(int rd, int rn, int rm) { emit(0x1AC02800 | (rm << 16) | (rn << 5) | rd); }
    void rorv(int rd, int rn, int rm) { emit(0x1AC02C00 | (rm << 16) | (rn << 5) | rd); }

    // multiply
    void madd(int rd, int rn, int rm, int ra) { emit(0x1B000000 | (rm << 16) | (ra << 10) | (rn << 5) | rd); }
    void mul(int rd, int rn, int rm) { madd(rd, rn, rm, ZR); }
    void umaddl(int rd, int rn, int rm, int ra) { emit(0x9BA00000 | (rm << 16) | (ra << 10) | (rn << 5) | rd); }
    void smaddl(int rd, int rn, int rm, int ra) { emit(0x9B200000 | (rm << 16) | (ra << 10) | (rn << 5) | rd); }
    void clz(int rd, int rn) { emit(0x5AC01000 | (rn << 5) | rd); }

    // conditional select
    void csel(int rd, int rn, int rm, Cond c) { emit(0x1A800000 | (rm << 16) | (c << 12) | (rn << 5) | rd); }
    void cset(int rd, Cond c) { emit(0x1A9F07E0 | ((c ^ 1) << 12) | rd); }

    // flags
    void mrs_nzcv(int rt) { emit(0xD53B4200 | rt); }
    void msr_nzcv(int rt) { emit(0xD51B4200 | rt); }

    // ---- loads/stores ----
    void ldr_w(int rt, int rn, u32 off) { emit(0xB9400000 | ((off / 4) << 10) | (rn << 5) | rt); }
    void str_w(int rt, int rn, u32 off) { emit(0xB9000000 | ((off / 4) << 10) | (rn << 5) | rt); }
    void ldr_x(int rt, int rn, u32 off) { emit(0xF9400000 | ((off / 8) << 10) | (rn << 5) | rt); }
    void str_x(int rt, int rn, u32 off) { emit(0xF9000000 | ((off / 8) << 10) | (rn << 5) | rt); }
    void ldrb(int rt, int rn, u32 off) { emit(0x39400000 | (off << 10) | (rn << 5) | rt); }
    void strb(int rt, int rn, u32 off) { emit(0x39000000 | (off << 10) | (rn << 5) | rt); }
    void ldrh(int rt, int rn, u32 off) { emit(0x79400000 | ((off / 2) << 10) | (rn << 5) | rt); }
    void strh(int rt, int rn, u32 off) { emit(0x79000000 | ((off / 2) << 10) | (rn << 5) | rt); }
    void ldrsb(int rt, int rn, u32 off) { emit(0x39C00000 | (off << 10) | (rn << 5) | rt); }
    void ldrsh(int rt, int rn, u32 off) { emit(0x79C00000 | ((off / 2) << 10) | (rn << 5) | rt); }
    // register offset [Xn, Xm]
    void ldr_w_r(int rt, int rn, int rm) { emit(0xB8606800 | (rm << 16) | (rn << 5) | rt); }
    void ldrb_r(int rt, int rn, int rm) { emit(0x38606800 | (rm << 16) | (rn << 5) | rt); }

    void stp_pre(int rt, int rt2, int rn, int off) { emit(0xA9800000 | (((u32)(off / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }
    void stp(int rt, int rt2, int rn, int off) { emit(0xA9000000 | (((u32)(off / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }
    void ldp(int rt, int rt2, int rn, int off) { emit(0xA9400000 | (((u32)(off / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }
    void ldp_post(int rt, int rt2, int rn, int off) { emit(0xA8C00000 | (((u32)(off / 8) & 0x7F) << 15) | (rt2 << 10) | (rn << 5) | rt); }

    // TST with immediate single bit / low mask: use ANDS with logical immediate encodings for 2^k-1 masks and single bits
    // Logical immediate (W): N=0, immr, imms
    void ands_imm(int rd, int rn, u32 immr, u32 imms) { emit(0x72000000 | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
    void and_imm(int rd, int rn, u32 immr, u32 imms) { emit(0x12000000 | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
    void orr_imm(int rd, int rn, u32 immr, u32 imms) { emit(0x32000000 | (immr << 16) | (imms << 10) | (rn << 5) | rd); }
    // test low bits mask (2^k - 1), k in 1..31
    void tst_low(int rn, int k) { ands_imm(ZR, rn, 0, k - 1); }
    // test single bit b
    void tst_bit(int rn, int b) { ands_imm(ZR, rn, (32 - b) & 31, 0); }
    // clear low k bits: AND with ~(2^k-1) = ones in [31:k]: immr = 32-k? element 32 bits, ones count = 32-k, rotated left by k
    void bic_low(int rd, int rn, int k) { and_imm(rd, rn, (32 - k) & 31, 31 - k); }
    void orr_bit(int rd, int rn, int b) { orr_imm(rd, rn, (32 - b) & 31, 0); }
    void bic_bit(int rd, int rn, int b) {
        // AND with ~(1<<b): 31 ones rotated so that the zero lands at bit b
        and_imm(rd, rn, (32 - (b + 1)) & 31, 30);
    }
};

} // namespace a64
} // namespace didj
