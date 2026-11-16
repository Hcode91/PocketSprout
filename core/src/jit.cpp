// PocketSprout - ARM926 (ARMv5) -> AArch64 dynamic recompiler
//
// Design:
//  * Guest registers live in the ARM926 object (r[0..15]); each translated instruction loads its
//    operands from and stores results to memory (X19 = cpu). Simple and always in sync, so any
//    instruction can fall back to the interpreter without state shuffling.
//  * Guest NZCV lives in W20 (bits 31..28), which has exactly the AArch64 NZCV layout and
//    semantics (including the inverted-borrow carry for subtraction).
//  * Loads/stores do an inline software-TLB lookup (X21 = tlb array) and fall back to C++ helpers.
//    Faults are precise: no guest register is modified before the access succeeds.
//  * Blocks are keyed by (virtual PC, physical PC, privilege), never cross a 4 KB page, and are
//    invalidated when their physical page is written.
#include "jit.h"
#include "a64emit.h"
#include <cstdio>
#include <cstddef>
#include <algorithm>

#if defined(__aarch64__)
#include <sys/mman.h>
#define DIDJ_JIT 1
#else
#define DIDJ_JIT 0
#endif

namespace didj {

using namespace a64;

// Register assignments
enum { R_CPU = 19, R_FLAGS = 20, R_TLB = 21, R_SAVE = 22, R_RAM = 23, R_CODEFLAGS = 24, R_BUDGET = 25, R_CALL = 16 };

// Memory access kinds passed to the helper
enum { K_SIZE_MASK = 3, K_SIGNED = 4, K_USER = 8, K_WRITE = 16 };

static const int MAX_INSNS = 48;

// ---------------------------------------------------------------------------
// C++ helpers called from generated code
// ---------------------------------------------------------------------------
struct JitHelpers {
    static u32 mem(ARM926* c, u32 va, u32 val, u32 kind) {
        bool user = kind & K_USER;
        try {
            if (kind & K_WRITE) {
                switch (kind & K_SIZE_MASK) {
                case 0: c->vwrite8(va, (u8)val, user); break;
                case 1: c->vwrite16(va, (u16)val, user); break;
                default: c->vwrite32(va, val, user); break;
                }
                c->jit_exit = 1;
                return 0;
            }
            switch (kind & (K_SIZE_MASK | K_SIGNED)) {
            case 0: return c->vread8(va, user);
            case 0 | K_SIGNED: return (u32)(s32)(int8_t)c->vread8(va, user);
            case 1: return c->vread16(va, user);
            case 1 | K_SIGNED: return (u32)(s32)(int16_t)c->vread16(va, user);
            default: return c->load_word_rot(va, user);
            }
        } catch (const MemFault& f) {
            c->jit_fault = 1;
            c->jit_fault_info = f;
            return 0;
        }
    }
    // Execute one ARM instruction with the interpreter. Returns nonzero if the block must end.
    static u32 interp(ARM926* c, u32 insn, u32 pc) {
        u32 old = c->cpsr;
        c->cur_pc = pc;
        c->pc_written = false;
        c->r[15] = pc + 8;
        u32 cnd = insn >> 28;
        try {
            if (cnd == 0xE || cnd == 0xF || c->cond_ext(cnd)) c->exec_arm(insn);
        } catch (const MemFault& f) {
            c->data_abort(f);
        }
        if (!c->pc_written) c->r[15] = pc + 4;
        bool coproc = ((insn >> 25) & 7) == 7 || ((insn >> 25) & 7) == 6;
        return (c->pc_written || ((c->cpsr ^ old) & 0xFF) || c->jit_exit || coproc || c->wfi) ? 1 : 0;
    }
};


// ---------------------------------------------------------------------------
// Translator
// ---------------------------------------------------------------------------
class Translator {
public:
    Emitter& e;
    ARM926* cpu;
    bool priv;
    Label exit_pc, exit_nopc;
    u32 block_pc = 0;
    struct Slot { int pos; u32 target; };
    std::vector<Slot> slots;
    u32 off_r, off_cpsr, off_fault, off_exit, off_tmp;
    u32 off_tlb_host, off_tlb_perm, off_tlb_valid;

    static_assert(sizeof(void*) != 8 || sizeof(ARM926::TLBEntry) == 24, "generated code assumes 24-byte TLB entries");
    Translator(Emitter& em, ARM926* c, bool p) : e(em), cpu(c), priv(p) {
        const u8* base = (const u8*)c;
        off_r = (u32)((const u8*)&c->r[0] - base);
        off_cpsr = (u32)((const u8*)&c->cpsr - base);
        off_fault = (u32)((const u8*)&c->jit_fault - base);
        off_exit = (u32)((const u8*)&c->jit_exit - base);
        off_tmp = (u32)((const u8*)&c->jit_tmp[0] - base);
        off_tlb_host = offsetof(ARM926::TLBEntry, host);
        off_tlb_perm = offsetof(ARM926::TLBEntry, perm);
        off_tlb_valid = offsetof(ARM926::TLBEntry, valid);
    }

    u32 roff(int n) { return off_r + 4 * n; }
    void ld(int w, int n, u32 pc) { if (n == 15) e.mov_imm32(w, pc + 8); else e.ldr_w(w, R_CPU, roff(n)); }
    void st(int w, int n) { e.str_w(w, R_CPU, roff(n)); }

    void sub_budget(u32 count) { if (count) e.sub_imm(R_BUDGET, R_BUDGET, count); }
    // linkable: a plain control-flow exit that may be patched into a direct jump to the next block.
    // Fault exits and forced exits must always return to the dispatcher.
    void exit_to(u32 count, u32 next_pc, bool linkable = false) {
        sub_budget(count);
        if (linkable && count && (next_pc >> 12) == (block_pc >> 12)) {
            // patchable slot: initially falls through to the normal exit
            slots.push_back({e.here(), next_pc});
            Label unl;
            e.b(unl);
            e.bind(unl);
        }
        e.mov_imm32(1, next_pc);
        e.b(exit_pc);
    }
    void exit_dyn(u32 count) { sub_budget(count); e.b(exit_pc); }     // W1 = next pc
    void exit_keep(u32 count) { sub_budget(count); e.b(exit_nopc); }  // r[15] already set
    void call(void* fn) { e.mov_imm64(R_CALL, (u64)(uintptr_t)fn); e.blr(R_CALL); }

    void flags_out() {   // merge W20 into cpu->cpsr
        e.ldr_w(2, R_CPU, off_cpsr);
        e.ubfx(2, 2, 0, 28);
        e.orr(2, 2, R_FLAGS);
        e.str_w(2, R_CPU, off_cpsr);
    }
    void flags_in() {
        e.ldr_w(R_FLAGS, R_CPU, off_cpsr);
        e.lsr_imm(R_FLAGS, R_FLAGS, 28);
        e.lsl_imm(R_FLAGS, R_FLAGS, 28);
    }
    // Set N,Z in W20 from W(src) (32-bit), keep C,V
    void set_nz(int src) {
        e.ands(ZR, src, src);
        e.mrs_nzcv(4);
        e.lsr_imm(4, 4, 30);
        e.bfi(R_FLAGS, 4, 30, 2);
    }
    void set_nz64(int src) {
        e.ands_x(ZR, src, src);
        e.mrs_nzcv(4);
        e.lsr_imm(4, 4, 30);
        e.bfi(R_FLAGS, 4, 30, 2);
    }

    // Interpreter fallback for one instruction
    void fallback(u32 insn, u32 pc, u32 idx) {
        flags_out();
        e.mov_x(0, R_CPU);
        e.mov_imm32(1, insn);
        e.mov_imm32(2, pc);
        call((void*)&JitHelpers::interp);
        flags_in();
        Label cont;
        e.cbz(0, cont);
        exit_keep(idx + 1);
        e.bind(cont);
    }

    // Memory access: address in W5, store value in W7, load result in W0.
    // On abort: exits with r15 = pc of this instruction and count = idx (instruction not executed).
    void access(u32 kind, u32 pc, u32 idx) {
        Label slow, done;
        int size = kind & K_SIZE_MASK;
        bool write = kind & K_WRITE;
        bool user = kind & K_USER;
        if (size == 1) { e.tst_low(5, 1); e.bcond(NE, slow); }
        else if (size == 2) { e.tst_low(5, 2); e.bcond(NE, slow); }
        e.lsr_imm(8, 5, 12);
        e.ubfx(9, 5, 12, 11);
        e.add_x(9, 9, 9, LSL, 1);
        e.add_x(9, R_TLB, 9, LSL, 3);
        e.ldr_w(10, 9, 0);
        e.cmp(10, 8);
        e.bcond(NE, slow);
        e.ldrb(10, 9, off_tlb_valid);
        e.cmp_imm(10, 1);
        e.bcond(NE, slow);
        e.ldrb(10, 9, off_tlb_perm);
        e.tst_bit(10, write ? (user ? 3 : 1) : (user ? 2 : 0));
        e.bcond(EQ, slow);
        e.ldr_x(10, 9, off_tlb_host);
        e.cbz_x(10, slow);
        e.ubfx(11, 5, 0, 12);
        e.add_x(10, 10, 11);
        if (write) {
            // code page? -> slow path (invalidates compiled code)
            e.sub_x(11, 10, R_RAM);
            e.lsr_x_imm(11, 11, 12);
            e.ldrb_r(11, R_CODEFLAGS, 11);
            e.cbnz(11, slow);
            if (size == 0) e.strb(7, 10, 0); else if (size == 1) e.strh(7, 10, 0); else e.str_w(7, 10, 0);
        } else {
            switch (kind & (K_SIZE_MASK | K_SIGNED)) {
            case 0: e.ldrb(0, 10, 0); break;
            case 0 | K_SIGNED: e.ldrsb(0, 10, 0); break;
            case 1: e.ldrh(0, 10, 0); break;
            case 1 | K_SIGNED: e.ldrsh(0, 10, 0); break;
            default: e.ldr_w(0, 10, 0); break;
            }
        }
        e.b(done);
        e.bind(slow);
        e.mov_x(0, R_CPU);
        e.mov(1, 5);
        e.mov(2, 7);
        e.mov_imm32(3, kind);
        call((void*)&JitHelpers::mem);
        e.ldrb(9, R_CPU, off_fault);
        Label ok;
        e.cbz(9, ok);
        exit_to(idx, pc);
        e.bind(ok);
        e.bind(done);
    }

    // After an instruction containing stores: exit if code was invalidated
    void check_exit(u32 idx, u32 pc) {
        Label ok;
        e.ldrb(9, R_CPU, off_exit);
        e.cbz(9, ok);
        exit_to(idx + 1, pc + 4);
        e.bind(ok);
    }

    // Shifter operand (register, immediate shift) -> W2; carry-out -> W3 if want_c.
    // Returns carry kind: 0 unchanged, 1 in W3.
    int shift_imm_operand(u32 insn, u32 pc, bool want_c) {
        int rm = insn & 0xF, type = (insn >> 5) & 3, amt = (insn >> 7) & 0x1F;
        ld(2, rm, pc);
        switch (type) {
        case 0:
            if (amt == 0) return 0;
            if (want_c) e.ubfx(3, 2, 32 - amt, 1);
            e.lsl_imm(2, 2, amt);
            return want_c ? 1 : 0;
        case 1:
            if (amt == 0) { if (want_c) e.lsr_imm(3, 2, 31); e.mov_imm32(2, 0); return want_c ? 1 : 0; }
            if (want_c) e.ubfx(3, 2, amt - 1, 1);
            e.lsr_imm(2, 2, amt);
            return want_c ? 1 : 0;
        case 2:
            if (amt == 0) { if (want_c) e.lsr_imm(3, 2, 31); e.asr_imm(2, 2, 31); return want_c ? 1 : 0; }
            if (want_c) e.ubfx(3, 2, amt - 1, 1);
            e.asr_imm(2, 2, amt);
            return want_c ? 1 : 0;
        default:
            if (amt == 0) { // RRX
                if (want_c) e.ubfx(3, 2, 0, 1);
                e.lsr_imm(2, 2, 1);
                e.ubfx(4, R_FLAGS, 29, 1);
                e.orr(2, 2, 4, LSL, 31);
                return want_c ? 1 : 0;
            }
            if (want_c) e.ubfx(3, 2, amt - 1, 1);
            e.ror_imm(2, 2, amt);
            return want_c ? 1 : 0;
        }
    }

    enum Result { CONT, ENDED };

    Result data_proc(u32 insn, u32 pc, u32 idx) {
        u32 op = (insn >> 21) & 0xF;
        bool S = (insn >> 20) & 1;
        int rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
        bool logical = (op == 0 || op == 1 || op == 8 || op == 9 || op >= 0xC);
        bool want_c = S && logical;
        int ckind = 0;           // 0 unchanged, 1 W3, 2 const 0, 3 const 1
        if ((insn >> 25) & 1) {
            u32 rot = ((insn >> 8) & 0xF) * 2;
            u32 imm = insn & 0xFF;
            imm = rot ? ((imm >> rot) | (imm << (32 - rot))) : imm;
            e.mov_imm32(2, imm);
            if (want_c && rot) ckind = (imm >> 31) ? 3 : 2;
        } else {
            ckind = shift_imm_operand(insn, pc, want_c) ? 1 : 0;
        }
        bool uses_rn = !(op == 0xD || op == 0xF);
        if (uses_rn) ld(1, rn, pc);
        bool needs_cin = (op == 5 || op == 6 || op == 7);
        if (needs_cin) e.msr_nzcv(R_FLAGS);
        switch (op) {
        case 0x0: e.and_(0, 1, 2); break;
        case 0x1: e.eor(0, 1, 2); break;
        case 0x2: if (S) e.subs(0, 1, 2); else e.sub(0, 1, 2); break;
        case 0x3: if (S) e.subs(0, 2, 1); else e.sub(0, 2, 1); break;
        case 0x4: if (S) e.adds(0, 1, 2); else e.add(0, 1, 2); break;
        case 0x5: if (S) e.adcs(0, 1, 2); else e.adc(0, 1, 2); break;
        case 0x6: if (S) e.sbcs(0, 1, 2); else e.sbc(0, 1, 2); break;
        case 0x7: if (S) e.sbcs(0, 2, 1); else e.sbc(0, 2, 1); break;
        case 0x8: e.and_(0, 1, 2); break;
        case 0x9: e.eor(0, 1, 2); break;
        case 0xA: e.subs(0, 1, 2); break;
        case 0xB: e.adds(0, 1, 2); break;
        case 0xC: e.orr(0, 1, 2); break;
        case 0xD: e.mov(0, 2); break;
        case 0xE: e.bic(0, 1, 2); break;
        case 0xF: e.mvn(0, 2); break;
        }
        if (S) {
            if (logical) {
                set_nz(0);
                if (ckind == 1) e.bfi(R_FLAGS, 3, 29, 1);
                else if (ckind == 2) e.bic_bit(R_FLAGS, R_FLAGS, 29);
                else if (ckind == 3) e.orr_bit(R_FLAGS, R_FLAGS, 29);
            } else {
                e.mrs_nzcv(R_FLAGS);
            }
        }
        bool writes = !(op >= 0x8 && op <= 0xB);
        if (!writes) return CONT;
        if (rd == 15) {
            e.bic_low(1, 0, 2);
            exit_dyn(idx + 1);
            return ENDED;
        }
        st(0, rd);
        return CONT;
    }

    Result multiply(u32 insn, u32 pc, u32 idx) {
        (void)pc; (void)idx;
        u32 op = (insn >> 21) & 0xF;
        bool S = (insn >> 20) & 1;
        int rdhi = (insn >> 16) & 0xF, rdlo = (insn >> 12) & 0xF, rs = (insn >> 8) & 0xF, rm = insn & 0xF;
        e.ldr_w(1, R_CPU, roff(rm));
        e.ldr_w(2, R_CPU, roff(rs));
        if (op == 0 || op == 1) {
            if (op == 1) { e.ldr_w(3, R_CPU, roff(rdlo)); e.madd(0, 1, 2, 3); }
            else e.mul(0, 1, 2);
            st(0, rdhi);
            if (S) set_nz(0);
            return CONT;
        }
        bool sgn = op & 2, acc = op & 1;
        int ra = ZR;
        if (acc) {
            e.ldr_w(3, R_CPU, roff(rdlo));
            e.ldr_w(4, R_CPU, roff(rdhi));
            e.orr_x(3, 3, 4, LSL, 32);
            ra = 3;
        }
        if (sgn) e.smaddl(0, 1, 2, ra); else e.umaddl(0, 1, 2, ra);
        st(0, rdlo);
        e.lsr_x_imm(4, 0, 32);
        st(4, rdhi);
        if (S) set_nz64(0);
        return CONT;
    }

    // Load writeback/target helpers
    Result finish_load_to_pc(u32 idx) {
        // W0 = loaded value: interworking branch
        Label thumb;
        e.mov(1, 0);
        e.tbnz(1, 0, thumb);
        e.bic_low(1, 1, 2);
        exit_dyn(idx + 1);
        e.bind(thumb);
        e.ldr_w(2, R_CPU, off_cpsr);
        e.orr_bit(2, 2, 5);
        e.str_w(2, R_CPU, off_cpsr);
        e.bic_low(1, 1, 1);
        exit_dyn(idx + 1);
        return ENDED;
    }

    Result ldr_str(u32 insn, u32 pc, u32 idx) {
        bool I = (insn >> 25) & 1, P = (insn >> 24) & 1, U = (insn >> 23) & 1, B = (insn >> 22) & 1,
             W = (insn >> 21) & 1, L = (insn >> 20) & 1;
        int rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
        bool user = !priv || (!P && W);
        bool wb = (!P || W) && rn != 15;
        // offset -> W2
        if (I) shift_imm_operand(insn, pc, false);
        else e.mov_imm32(2, insn & 0xFFF);
        ld(1, rn, pc);
        if (U) e.add(6, 1, 2); else e.sub(6, 1, 2);       // W6 = base +/- offset
        e.mov(5, P ? 6 : 1);                              // address
        if (wb) e.str_w(6, R_CPU, off_tmp + 64);          // keep new base across helper calls
        u32 kind = (B ? 0 : 2) | (user ? K_USER : 0);
        if (L) {
            access(kind, pc, idx);
            if (wb) { e.ldr_w(6, R_CPU, off_tmp + 64); st(6, rn); }
            if (rd == 15) return finish_load_to_pc(idx);
            st(0, rd);
            return CONT;
        }
        if (rd == 15) e.mov_imm32(7, pc + 12); else e.ldr_w(7, R_CPU, roff(rd));
        access(kind | K_WRITE, pc, idx);
        if (wb) { e.ldr_w(6, R_CPU, off_tmp + 64); st(6, rn); }
        check_exit(idx, pc);
        return CONT;
    }

    Result ldrh_strh(u32 insn, u32 pc, u32 idx) {
        bool P = (insn >> 24) & 1, U = (insn >> 23) & 1, I = (insn >> 22) & 1, W = (insn >> 21) & 1, L = (insn >> 20) & 1;
        int rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
        u32 sh = (insn >> 5) & 3;
        bool wb = (!P || W) && rn != 15;
        if (I) e.mov_imm32(2, ((insn >> 4) & 0xF0) | (insn & 0xF));
        else ld(2, insn & 0xF, pc);
        ld(1, rn, pc);
        if (U) e.add(6, 1, 2); else e.sub(6, 1, 2);
        e.mov(5, P ? 6 : 1);
        if (wb) e.str_w(6, R_CPU, off_tmp + 64);
        u32 kind = (!priv ? K_USER : 0);
        if (L) {
            kind |= (sh == 1) ? 1 : (sh == 2) ? (0 | K_SIGNED) : (1 | K_SIGNED);
            access(kind, pc, idx);
            if (wb) { e.ldr_w(6, R_CPU, off_tmp + 64); st(6, rn); }
            st(0, rd);
            return CONT;
        }
        e.ldr_w(7, R_CPU, roff(rd));
        access(kind | 1 | K_WRITE, pc, idx);
        if (wb) { e.ldr_w(6, R_CPU, off_tmp + 64); st(6, rn); }
        check_exit(idx, pc);
        return CONT;
    }

    Result ldm_stm(u32 insn, u32 pc, u32 idx) {
        bool P = (insn >> 24) & 1, U = (insn >> 23) & 1, W = (insn >> 21) & 1, L = (insn >> 20) & 1;
        int rn = (insn >> 16) & 0xF;
        u32 list = insn & 0xFFFF;
        int n = __builtin_popcount(list);
        int start_off = U ? (P ? 4 : 0) : (P ? -4 * n : -4 * n + 4);
        int wb_off = U ? 4 * n : -4 * n;
        // W22 (callee-saved) = start address, word aligned
        e.ldr_w(R_SAVE, R_CPU, roff(rn));
        if (start_off > 0) e.add_imm(R_SAVE, R_SAVE, (u32)start_off);
        else if (start_off < 0) e.sub_imm(R_SAVE, R_SAVE, (u32)-start_off);
        e.bic_low(R_SAVE, R_SAVE, 2);
        u32 user = !priv ? K_USER : 0;
        int k = 0;
        if (L) {
            for (int i = 0; i < 16; i++) {
                if (!(list & (1u << i))) continue;
                if (k) e.add_imm(5, R_SAVE, 4 * k); else e.mov(5, R_SAVE);
                access(2 | user, pc, idx);
                e.str_w(0, R_CPU, off_tmp + 4 * i);
                k++;
            }
            if (W) {
                e.ldr_w(1, R_CPU, roff(rn));
                if (wb_off > 0) e.add_imm(1, 1, (u32)wb_off); else e.sub_imm(1, 1, (u32)-wb_off);
                st(1, rn);
            }
            for (int i = 0; i < 15; i++) {
                if (!(list & (1u << i))) continue;
                e.ldr_w(0, R_CPU, off_tmp + 4 * i);
                st(0, i);
            }
            if (list & 0x8000) {
                e.ldr_w(0, R_CPU, off_tmp + 4 * 15);
                return finish_load_to_pc(idx);
            }
            return CONT;
        }
        bool first = true;
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i))) continue;
            if (i == 15) e.mov_imm32(7, pc + 12);
            else if (i == rn && !first && W) {
                e.ldr_w(7, R_CPU, roff(rn));
                if (wb_off > 0) e.add_imm(7, 7, (u32)wb_off); else e.sub_imm(7, 7, (u32)-wb_off);
            } else e.ldr_w(7, R_CPU, roff(i));
            if (k) e.add_imm(5, R_SAVE, 4 * k); else e.mov(5, R_SAVE);
            access(2 | user | K_WRITE, pc, idx);
            k++;
            first = false;
        }
        if (W) {
            e.ldr_w(1, R_CPU, roff(rn));
            if (wb_off > 0) e.add_imm(1, 1, (u32)wb_off); else e.sub_imm(1, 1, (u32)-wb_off);
            st(1, rn);
        }
        check_exit(idx, pc);
        return CONT;
    }

    // Translate one instruction. Returns ENDED if it ended the block (all paths exit).
    Result insn(u32 insn, u32 pc, u32 idx) {
        u32 cnd = insn >> 28;
        if (cnd == 0xF) {
            if ((insn & 0x0D70F000) == 0x0550F000) return CONT;   // PLD
            fallback(insn, pc, idx);
            return CONT;
        }
        // classify
        enum { C_DP, C_MUL, C_LS, C_LSH, C_LDM, C_B, C_BX, C_CLZ, C_FB } cls = C_FB;
        u32 top = (insn >> 25) & 7;
        if (top == 0) {
            if ((insn & 0x90) == 0x90) {
                if ((insn & 0x60) == 0) {
                    if (!((insn >> 24) & 1)) {
                        u32 op = (insn >> 21) & 0xF;
                        int rdhi = (insn >> 16) & 0xF, rdlo = (insn >> 12) & 0xF, rs = (insn >> 8) & 0xF, rm = insn & 0xF;
                        bool any15 = rdhi == 15 || rs == 15 || rm == 15 || ((op == 1 || op >= 4) && rdlo == 15);
                        if ((op == 0 || op == 1 || (op >= 4 && op <= 7)) && !any15) cls = C_MUL;
                    }
                } else {
                    u32 sh = (insn >> 5) & 3;
                    bool L = (insn >> 20) & 1;
                    int rd = (insn >> 12) & 0xF, rn = (insn >> 16) & 0xF;
                    bool I = (insn >> 22) & 1;
                    bool ok = (L || sh == 1) && rd != 15 && (I || (insn & 0xF) != 15) && !(rn == 15 && ((insn >> 21) & 1));
                    if (ok) cls = C_LSH;
                }
            } else if ((insn & 0x01900000) == 0x01000000) {
                if ((insn & 0x0FFFFFF0) == 0x012FFF10) cls = C_BX;
                else if ((insn & 0x0FFF0FF0) == 0x016F0F10 && (insn & 0xF) != 15 && ((insn >> 12) & 0xF) != 15) cls = C_CLZ;
            } else {
                bool S = (insn >> 20) & 1;
                int rd = (insn >> 12) & 0xF;
                if (!((insn >> 4) & 1) && !(S && rd == 15)) cls = C_DP;     // reg-shift-by-reg -> fallback
            }
        } else if (top == 1) {
            if ((insn & 0x01900000) != 0x01000000) {
                bool S = (insn >> 20) & 1;
                int rd = (insn >> 12) & 0xF;
                if (!(S && rd == 15)) cls = C_DP;
            }
        } else if (top == 2 || top == 3) {
            if (!(top == 3 && ((insn >> 4) & 1))) cls = C_LS;
        } else if (top == 4) {
            bool S = (insn >> 22) & 1;
            int rn = (insn >> 16) & 0xF;
            if (!S && rn != 15 && (insn & 0xFFFF)) cls = C_LDM;
        } else if (top == 5) {
            cls = C_B;
        }
        if (cls == C_FB) { fallback(insn, pc, idx); return CONT; }

        Label skip;
        bool conditional = cnd != 0xE;
        if (conditional) {
            e.msr_nzcv(R_FLAGS);
            e.bcond((Cond)(cnd ^ 1), skip);
        }
        Result r = CONT;
        switch (cls) {
        case C_DP: r = data_proc(insn, pc, idx); break;
        case C_MUL: r = multiply(insn, pc, idx); break;
        case C_LS: r = ldr_str(insn, pc, idx); break;
        case C_LSH: r = ldrh_strh(insn, pc, idx); break;
        case C_LDM: r = ldm_stm(insn, pc, idx); break;
        case C_CLZ:
            e.ldr_w(1, R_CPU, roff(insn & 0xF));
            e.clz(0, 1);
            st(0, (insn >> 12) & 0xF);
            break;
        case C_B: {
            s32 off = (s32)(insn << 8) >> 6;
            if ((insn >> 24) & 1) { e.mov_imm32(0, pc + 4); st(0, 14); }
            exit_to(idx + 1, pc + 8 + off, true);
            r = ENDED;
            break;
        }
        case C_BX:
            ld(0, insn & 0xF, pc);
            r = finish_load_to_pc(idx);
            break;
        default: break;
        }
        if (conditional) {
            e.bind(skip);
            if (r == ENDED) { exit_to(idx + 1, pc + 4, true); }
            r = r == ENDED ? ENDED : CONT;
        }
        return r;
    }
};

// ---------------------------------------------------------------------------
// Jit
// ---------------------------------------------------------------------------
bool Jit::available() { return DIDJ_JIT != 0; }

Jit::Jit(ARM926* c, u8* r, u32 rs) : cpu(c), ram(r), ram_size(rs) {
    code_flags.assign(rs >> 12, 0);
    for (auto& f : fast) f = nullptr;
#if DIDJ_JIT
    code_size = 32u << 20;
    void* p = mmap(nullptr, code_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { code_base = nullptr; return; }
    code_base = (u8*)p;
#endif
    cpu->smc_flags = code_flags.data();
    cpu->smc_ram = ram;
    cpu->smc_pages = (u32)code_flags.size();
    cpu->smc_cb = smc_trampoline;
    cpu->smc_user = this;
}

Jit::~Jit() {
    cpu->smc_flags = nullptr;
    for (Block* b : all_blocks) delete b;
#if DIDJ_JIT
    if (code_base) munmap(code_base, code_size);
#endif
}

void Jit::smc_trampoline(void* user, u32 page) { static_cast<Jit*>(user)->invalidate_page(page); }

void Jit::note_bus_write(u32 pa) {
    u32 page = (pa & (ram_size - 1)) >> 12;
    if (page < code_flags.size() && code_flags[page]) invalidate_page(page);
}

void Jit::invalidate_page(u32 page) {
    invalidations++;
    code_flags[page] = 0;
    cpu->jit_exit = 1;
    auto it = page_blocks.find(page);
    if (it == page_blocks.end()) return;
    for (Block* b : it->second) {
        if (b->dead) continue;
        b->dead = true;
        unlink_incoming(b);
        // forget this block's own outgoing links
        for (Link& l : b->out) {
            Key k{l.target, (b->pa & ~0xFFFu) | (l.target & 0xFFF), b->priv};
            auto pit = pending.find(k);
            if (pit != pending.end()) {
                auto& v = pit->second;
                v.erase(std::remove(v.begin(), v.end(), &l), v.end());
            }
            auto bit = blocks.find(k);
            if (bit != blocks.end()) {
                auto& v = bit->second->in;
                v.erase(std::remove(v.begin(), v.end(), &l), v.end());
            }
        }
        blocks.erase(Key{b->va, b->pa, b->priv});
        Block*& f = fast[(b->va >> 2) & (FAST - 1)];
        if (f == b) f = nullptr;
    }
    page_blocks.erase(it);
}

void Jit::patch(u32* slot, const u32* target) {
    s32 off = (s32)(target - slot);
    *slot = 0x14000000u | ((u32)off & 0x03FFFFFF);
    __builtin___clear_cache((char*)slot, (char*)(slot + 1));
}

void Jit::unlink_incoming(Block* b) {
    for (Link* l : b->in) {
        *l->slot = l->orig;
        __builtin___clear_cache((char*)l->slot, (char*)(l->slot + 1));
        // re-queue so it can link again when the block is recompiled
        pending[Key{b->va, b->pa, b->priv}].push_back(l);
    }
    b->in.clear();
}

void Jit::link_block(Block* b) {
    // outgoing exits of b
    for (Link& l : b->out) {
        Key k{l.target, (b->pa & ~0xFFFu) | (l.target & 0xFFF), b->priv};
        auto it = blocks.find(k);
        if (it != blocks.end() && !it->second->dead) {
            patch(l.slot, it->second->chain_entry);
            it->second->in.push_back(&l);
        } else {
            pending[k].push_back(&l);
        }
    }
    // exits elsewhere that were waiting for b
    auto pit = pending.find(Key{b->va, b->pa, b->priv});
    if (pit != pending.end()) {
        for (Link* l : pit->second) {
            if (l->from->dead) continue;
            patch(l->slot, b->chain_entry);
            b->in.push_back(l);
        }
        pending.erase(pit);
    }
}

void Jit::flush() {
    flushes++;
    pending.clear();
    blocks.clear();
    page_blocks.clear();
    for (Block* b : all_blocks) delete b;
    all_blocks.clear();
    for (auto& f : fast) f = nullptr;
    std::fill(code_flags.begin(), code_flags.end(), 0);
    code_used = 0;
}

Jit::Block* Jit::lookup(u32 pc, bool priv, u32* pa_out, bool* can_compile) {
    *can_compile = false;
    bool user = !priv;
    const ARM926::TLBEntry* te = cpu->tlb_fast(pc, user);
    u32 pa;
    u8* host;
    if (te) { pa = te->pa_page + (pc & 0xFFF); host = te->host; }
    else {
        try {
            ARM926::TLBEntry* e = cpu->translate(pc, false, user, true);
            if (!e->host || e->valid != 1) return nullptr;
            pa = e->pa_page + (pc & 0xFFF);
            host = e->host;
        } catch (const MemFault&) { return nullptr; }
    }
    // Only RAM-backed code is compiled
    if (host < ram || host >= ram + ram_size) return nullptr;
    *pa_out = (u32)(host - ram) + (pc & 0xFFF);
    (void)pa;
    *can_compile = true;
    Block* f = fast[(pc >> 2) & (FAST - 1)];
    u32 rpa = *pa_out;
    if (f && f->va == pc && f->pa == rpa && f->priv == (u32)priv) return f;
    auto it = blocks.find(Key{pc, rpa, (u32)priv});
    if (it == blocks.end()) return nullptr;
    fast[(pc >> 2) & (FAST - 1)] = it->second;
    return it->second;
}

Jit::Block* Jit::compile(u32 pc, u32 rpa, bool priv, const u8* host_page) {
#if DIDJ_JIT
    for (int attempt = 0; attempt < 2; attempt++) {
        if (code_size - code_used < 64 * 1024) flush();
        u32* start = (u32*)(code_base + code_used);
        Emitter e;
        e.init(start, (int)((code_size - code_used) / 4));
        Translator t(e, cpu, priv);
        t.block_pc = pc;
        // prologue
        e.stp_pre(29, 30, SP, -80);
        e.stp(19, 20, SP, 16);
        e.stp(21, 22, SP, 32);
        e.stp(23, 24, SP, 48);
        e.stp(25, 26, SP, 64);
        e.mov_x(R_CPU, 0);
        e.mov(R_BUDGET, 1);
        t.flags_in();
        e.mov_imm64(R_TLB, (u64)(uintptr_t)&cpu->tlb[0]);
        e.mov_imm64(R_RAM, (u64)(uintptr_t)ram);
        e.mov_imm64(R_CODEFLAGS, (u64)(uintptr_t)code_flags.data());
        // chain entry: other blocks jump here directly; stop when the budget is used up
        int chain_pos = e.here();
        {
            Label go;
            e.cmp_imm(R_BUDGET, 0);
            e.bcond(GT, go);
            e.mov_imm32(1, pc);
            e.b(t.exit_pc);
            e.bind(go);
        }

        u32 n = 0;
        bool ended = false;
        while (n < (u32)MAX_INSNS) {
            u32 va = pc + n * 4;
            if (n > 0 && (va & 0xFFF) == 0) break;                         // page boundary
            if (n > 0 && ((u32)(va - cpu->hle_base) < cpu->hle_size || va == cpu->hle_watch_pc)) break;
            u32 insn;
            memcpy(&insn, host_page + (va & 0xFFF), 4);
            Translator::Result r = t.insn(insn, va, n);
            n++;
            if (r == Translator::ENDED) { ended = true; break; }
        }
        if (!ended) t.exit_to(n, pc + n * 4, true);
        // epilogue
        e.bind(t.exit_pc);
        e.str_w(1, R_CPU, t.roff(15));
        e.bind(t.exit_nopc);
        t.flags_out();
        e.mov(0, R_BUDGET);
        e.ldp(19, 20, SP, 16);
        e.ldp(21, 22, SP, 32);
        e.ldp(23, 24, SP, 48);
        e.ldp(25, 26, SP, 64);
        e.ldp_post(29, 30, SP, 80);
        e.ret();
        if (e.overflow) { flush(); continue; }
        size_t bytes = (size_t)e.n * 4;
        __builtin___clear_cache((char*)start, (char*)start + bytes);
        code_used += (bytes + 15) & ~(size_t)15;
        Block* b = new Block();
        b->va = pc; b->pa = rpa; b->priv = (u32)priv;
        b->fn = (s32 (*)(ARM926*, s32))(void*)start;
        b->chain_entry = start + chain_pos;
        b->ninsn = n; b->dead = false;
        for (auto& sl : t.slots) b->out.push_back(Link{start + sl.pos, sl.target, start[sl.pos], b});
        all_blocks.push_back(b);
        blocks[Key{pc, rpa, (u32)priv}] = b;
        u32 page = rpa >> 12;
        page_blocks[page].push_back(b);
        code_flags[page] = 1;
        fast[(pc >> 2) & (FAST - 1)] = b;
        blocks_compiled++;
        link_block(b);
        return b;
    }
#else
    (void)pc; (void)rpa; (void)priv; (void)host_page;
#endif
    return nullptr;
}

s64 Jit::run(s64 cycles) {
    s64 done = 0;
    while (done < cycles) {
        if (cpu->wfi) {
            if (cpu->irq_line || cpu->fiq_line) cpu->wfi = false;
            else { done = cycles; break; }
        }
        if ((cpu->irq_line && !(cpu->cpsr & FLAG_I)) || (cpu->fiq_line && !(cpu->cpsr & FLAG_F))) {
            cpu->check_interrupts();
        }
        u32 pc = cpu->r[15];
        bool interp = (cpu->cpsr & FLAG_T) || cpu->trace_hook || (cpu->cp15_fcse_pid != 0) ||
                      cpu->hle_watch_pc != 0xFFFFFFFF || (u32)(pc - cpu->hle_base) < cpu->hle_size || !code_base;
        Block* b = nullptr;
        if (!interp) {
            bool priv = cpu->privileged();
            u32 rpa = 0;
            bool can = false;
            b = lookup(pc, priv, &rpa, &can);
            if (!b && can) b = compile(pc, rpa, priv, ram + (rpa & ~0xFFFu));
        }
        if (!b) {
            done += cpu->run(1);
            cpu->total_cycles -= 1;   // run() adds; we account below
            interp_steps++;
            continue;
        }
        cpu->jit_exit = 0;
        s64 left = cycles - done;
        s32 budget = left > 0x40000000 ? 0x40000000 : (s32)left;
        s32 remaining = b->fn(cpu, budget);
        s64 n = (s64)budget - remaining;
        blocks_run++;
        if (cpu->jit_fault) {
            cpu->jit_fault = 0;
            cpu->cur_pc = cpu->r[15];
            cpu->data_abort(cpu->jit_fault_info);
            n++;
            if (stop_on_exception) { done += n; cpu->insn_count += n; break; }
        }
        if (n <= 0) n = 1;
        done += n;
        cpu->insn_count += n;
        if (stop_on_exception && cpu->r[15] < 0x20) break;
    }
    cpu->total_cycles += done;
    return done;
}

} // namespace didj
