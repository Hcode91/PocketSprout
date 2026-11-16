// PocketSprout - LeapFrog Didj emulator
// ARM926EJ-S CPU core (ARMv5TE, ARM + Thumb, CP15 MMU) - interpreter.
#pragma once
#include <cstdint>
#include <cstring>

namespace didj {

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

// Physical bus interface implemented by the system.
class Bus {
public:
    virtual ~Bus() {}
    // Returns a host pointer to the 4 KB physical page containing 'pa' if it is plain RAM,
    // or nullptr if the page is I/O or unmapped (slow path is used).
    virtual u8* page_ptr(u32 pa) = 0;
    virtual u32 read32(u32 pa) = 0;
    virtual u16 read16(u32 pa) = 0;
    virtual u8 read8(u32 pa) = 0;
    virtual void write32(u32 pa, u32 v) = 0;
    virtual void write16(u32 pa, u16 v) = 0;
    virtual void write8(u32 pa, u8 v) = 0;
};

enum Mode : u32 {
    MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13,
    MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F
};

enum { FLAG_N = 1u << 31, FLAG_Z = 1u << 30, FLAG_C = 1u << 29, FLAG_V = 1u << 28,
       FLAG_Q = 1u << 27, FLAG_I = 1u << 7, FLAG_F = 1u << 6, FLAG_T = 1u << 5 };

// Thrown by memory accessors on MMU fault.
struct MemFault {
    u32 fsr;   // fault status (domain<<4 | status)
    u32 addr;  // faulting virtual address
};

class Jit;
struct JitHelpers;
class Translator;

class ARM926 {
    friend class Jit;
    friend struct JitHelpers;
    friend class Translator;
public:
    explicit ARM926(Bus* bus);
    void reset();

    // Executes until at least 'cycles' cycles elapsed (or halted). Returns cycles executed.
    s64 run(s64 cycles);

    void set_irq(bool level) { irq_line = level; }
    void set_fiq(bool level) { fiq_line = level; }
    bool halted() const { return wfi; }
    void halt() { wfi = true; }        // sleep until the next interrupt

    // Register access (current mode view). r[15] holds address of next instruction.
    u32 r[16];
    u32 cpsr;
    u32 spsr;
    // JIT communication (kept near the start for short offsets)
    u8 jit_fault = 0;          // set by memory helpers when an access aborted
    u8 jit_exit = 0;           // set when the current block must stop (e.g. its code was overwritten)
    u32 jit_tmp[20];           // scratch for multi-register loads etc.
    s32 jit_budget = 0;
    MemFault jit_fault_info{0, 0};
    // Self-modifying-code tracking (installed by the JIT)
    u8* smc_flags = nullptr;   // one byte per 4 KB RAM page: 1 = holds compiled code
    u8* smc_ram = nullptr;
    u32 smc_pages = 0;
    void (*smc_cb)(void* user, u32 page) = nullptr;
    void* smc_user = nullptr;
    inline void smc_check(const u8* host) {
        if (smc_flags) {
            size_t pg = (size_t)(host - smc_ram) >> 12;
            if (pg < smc_pages && smc_flags[pg]) smc_cb(smc_user, (u32)pg);
        }
    }

    // Banked registers (index by bank)
    u32 bank_r13[6], bank_r14[6], bank_spsr[6];
    u32 fiq_r8_12[5], usr_r8_12[5];

    // CP15
    u32 cp15_control;
    u32 cp15_ttb;
    u32 cp15_dacr;
    u32 cp15_dfsr, cp15_ifsr, cp15_far;
    u32 cp15_fcse_pid, cp15_context_id;
    u32 cp15_cache_lockdown_d, cp15_cache_lockdown_i, cp15_tlb_lockdown;

    u64 total_cycles = 0;
    u64 insn_count = 0;
    u64 stat_walks = 0, stat_aborts = 0, stat_flushes = 0, stat_irqs = 0, stat_thumb = 0, stat_slowfetch = 0;

    // Debug hooks
    void (*trace_hook)(ARM926* cpu, u32 pc, void* user) = nullptr;
    void* trace_user = nullptr;
    void (*undef_hook)(ARM926* cpu, u32 pc, u32 insn, void* user) = nullptr;
    void (*swi_hook)(ARM926* cpu, u32 pc, u32 imm, void* user) = nullptr;

    // High-level-emulation hook: called before executing an instruction whose PC lies in
    // [hle_base, hle_base+hle_size) or equals hle_watch_pc, while the active TTB is hle_ttb.
    // Return true if the hook redirected execution (changed r[15]).
    bool (*hle_hook)(ARM926* cpu, u32 pc, void* user) = nullptr;
    void* hle_user = nullptr;
    u32 hle_base = 0, hle_size = 0, hle_watch_pc = 0xFFFFFFFF;
    // Return from an HLE'd function: r0 = value, PC = LR (with interworking)
    void hle_return(u32 value) {
        r[0] = value;
        u32 lr = r[14];
        if (lr & 1) { cpsr |= FLAG_T; r[15] = lr & ~1u; } else { cpsr &= ~FLAG_T; r[15] = lr & ~3u; }
    }
    // Called when the TTB changes, so the owner can switch the active HLE range per process.
    void (*ttb_hook)(ARM926* cpu, void* user) = nullptr;

    // Memory access (virtual). Throw MemFault on abort.
    u32 vread32(u32 va, bool user);
    u16 vread16(u32 va, bool user);
    u8 vread8(u32 va, bool user);
    void vwrite32(u32 va, u32 v, bool user);
    void vwrite16(u32 va, u16 v, bool user);
    void vwrite8(u32 va, u8 v, bool user);

    // Debug-friendly translation (no faults thrown); returns false if unmapped.
    bool debug_translate(u32 va, u32* pa);
    u32 debug_read32(u32 va);

    void flush_tlb();
    void switch_mode(u32 newmode);

    bool in_thumb() const { return cpsr & FLAG_T; }
    bool privileged() const { return (cpsr & 0x1F) != MODE_USR; }

private:
    Bus* bus;
    bool irq_line = false, fiq_line = false;
    bool wfi = false;

    // ---- software TLB ----
    struct TLBEntry {
        u32 tag;        // va >> 12 | 1 (valid in bit 0 of 'valid')
        u32 pa_page;    // physical page base
        u8* host;       // host pointer to page or null
        u8 perm;        // bit0 priv read, bit1 priv write, bit2 user read, bit3 user write
        u8 valid;
        u8 domain;
        u8 is_section;
    };
    static const int TLB_SIZE = 2048;
    TLBEntry tlb[TLB_SIZE];
    // Fetch cache
    u32 fetch_tag = 0xFFFFFFFF;
    u8* fetch_host = nullptr;
    bool fetch_user = false;

    TLBEntry* translate(u32 va, bool write, bool user, bool fetch);
    // Cached RAM translation with read permission, or null (no side effects)
    inline const TLBEntry* tlb_fast(u32 va, bool user) const {
        u32 m = (va < 0x02000000) ? (va | (cp15_fcse_pid & 0xFE000000)) : va;
        const TLBEntry& e = tlb[(m >> 12) & (TLB_SIZE - 1)];
        if (e.valid == 1 && e.tag == (m >> 12) && e.host && (e.perm & (user ? 4 : 1))) return &e;
        return nullptr;
    }
    bool walk(u32 mva, TLBEntry* e, u32* fsr, bool* is_section);
    inline u32 mva(u32 va) const {
        return (va < 0x02000000) ? (va | (cp15_fcse_pid & 0xFE000000)) : va;
    }

    u32 fetch32(u32 va);
    u16 fetch16(u32 va);

    void exec_arm(u32 insn);
    void exec_thumb(u16 insn);
    void exception(u32 vector, u32 mode, u32 lr, bool disable_fiq);
    void raise_undefined();
    void data_abort(const MemFault& f);
    void prefetch_abort(const MemFault& f);
    void check_interrupts();

    bool cond(u32 c) const;
    bool cond_ext(u32 c) const;   // out-of-line version for other translation units
    u32 shift_imm(u32 insn, bool* carry);
    u32 shift_reg(u32 insn, bool* carry);
    void data_proc(u32 insn, u32 op2, bool carry);
    void ldr_str(u32 insn);
    void ldrh_strh(u32 insn);
    void ldm_stm(u32 insn);
    void multiply(u32 insn);
    void misc(u32 insn);
    void coproc(u32 insn);
    void msr(u32 insn, u32 val);
    void write_cpsr(u32 val, u32 mask);
    void set_pc(u32 v);                  // branch, keep state
    void set_pc_interwork(u32 v);        // BX semantics
    u32 cp15_read(u32 crn, u32 crm, u32 op1, u32 op2);
    void cp15_write(u32 crn, u32 crm, u32 op1, u32 op2, u32 v);

    u32 load_word_rot(u32 addr, bool user);
    int bank_index(u32 mode) const;

    // user-bank access for LDM/STM ^
    u32 get_user_reg(int n);
    void set_user_reg(int n, u32 v);

    u32 cur_pc;          // address of instruction being executed
    bool pc_written;     // set when r15 changed by instruction
};

} // namespace didj
