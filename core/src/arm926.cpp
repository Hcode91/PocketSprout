// DidjDroid - ARM926EJ-S interpreter (ARMv5TE)
#include "arm926.h"

namespace didj {

static inline u32 ror32(u32 v, u32 s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
static inline u32 bit(u32 v, int n) { return (v >> n) & 1; }

// Fault status codes
enum { FS_ALIGN = 0x1, FS_TRANS_SEC = 0x5, FS_TRANS_PAGE = 0x7, FS_DOM_SEC = 0x9,
       FS_DOM_PAGE = 0xB, FS_PERM_SEC = 0xD, FS_PERM_PAGE = 0xF, FS_EXT_LINE = 0x8 };

enum { CR_M = 1 << 0, CR_A = 1 << 1, CR_S = 1 << 8, CR_R = 1 << 9, CR_V = 1 << 13 };

ARM926::ARM926(Bus* b) : bus(b) { reset(); }

void ARM926::reset() {
    memset(r, 0, sizeof(r));
    memset(bank_r13, 0, sizeof(bank_r13));
    memset(bank_r14, 0, sizeof(bank_r14));
    memset(bank_spsr, 0, sizeof(bank_spsr));
    memset(fiq_r8_12, 0, sizeof(fiq_r8_12));
    memset(usr_r8_12, 0, sizeof(usr_r8_12));
    cpsr = MODE_SVC | FLAG_I | FLAG_F;
    spsr = 0;
    cp15_control = 0x00050078;
    cp15_ttb = cp15_dacr = cp15_dfsr = cp15_ifsr = cp15_far = 0;
    cp15_fcse_pid = cp15_context_id = 0;
    cp15_cache_lockdown_d = cp15_cache_lockdown_i = 0xFFF0;
    cp15_tlb_lockdown = 0;
    wfi = false;
    total_cycles = 0;
    flush_tlb();
}

int ARM926::bank_index(u32 mode) const {
    switch (mode) {
    case MODE_FIQ: return 1;
    case MODE_IRQ: return 2;
    case MODE_SVC: return 3;
    case MODE_ABT: return 4;
    case MODE_UND: return 5;
    default: return 0; // USR/SYS
    }
}

void ARM926::switch_mode(u32 newmode) {
    u32 oldmode = cpsr & 0x1F;
    newmode &= 0x1F;
    if (oldmode == newmode) return;
    int ob = bank_index(oldmode), nb = bank_index(newmode);
    if (ob != nb) {
        bank_r13[ob] = r[13]; bank_r14[ob] = r[14];
        if (ob != 0) bank_spsr[ob] = spsr;
        // FIQ banking of r8-r12
        if (ob == 1 && nb != 1) {
            for (int i = 0; i < 5; i++) { fiq_r8_12[i] = r[8 + i]; r[8 + i] = usr_r8_12[i]; }
        } else if (nb == 1 && ob != 1) {
            for (int i = 0; i < 5; i++) { usr_r8_12[i] = r[8 + i]; r[8 + i] = fiq_r8_12[i]; }
        }
        r[13] = bank_r13[nb]; r[14] = bank_r14[nb];
        spsr = (nb != 0) ? bank_spsr[nb] : 0;
    }
    cpsr = (cpsr & ~0x1Fu) | newmode;
}

u32 ARM926::get_user_reg(int n) {
    u32 m = cpsr & 0x1F;
    if (m == MODE_USR || m == MODE_SYS) return r[n];
    if (n >= 8 && n <= 12) return (m == MODE_FIQ) ? usr_r8_12[n - 8] : r[n];
    if (n == 13) return bank_r13[0];
    if (n == 14) return bank_r14[0];
    return r[n];
}

void ARM926::set_user_reg(int n, u32 v) {
    u32 m = cpsr & 0x1F;
    if (m == MODE_USR || m == MODE_SYS) { r[n] = v; return; }
    if (n >= 8 && n <= 12) { if (m == MODE_FIQ) usr_r8_12[n - 8] = v; else r[n] = v; return; }
    if (n == 13) { bank_r13[0] = v; return; }
    if (n == 14) { bank_r14[0] = v; return; }
    r[n] = v;
}

// ---------------------------------------------------------------------------
// MMU
// ---------------------------------------------------------------------------
void ARM926::flush_tlb() {
    for (int i = 0; i < TLB_SIZE; i++) tlb[i].valid = 0;
    fetch_tag = 0xFFFFFFFF;
    fetch_host = nullptr;
}

static inline u8 ap_perm(u32 ap, u32 control) {
    // bit0 priv read, bit1 priv write, bit2 user read, bit3 user write
    switch (ap & 3) {
    case 0: {
        bool s = control & CR_S, rr = control & CR_R;
        if (s && !rr) return 1;
        if (!s && rr) return 1 | 4;
        return 0;
    }
    case 1: return 1 | 2;
    case 2: return 1 | 2 | 4;
    default: return 0xF;
    }
}

// Walk translation tables. Fills e (except tag). Returns false on translation fault with *fsr set.
bool ARM926::walk(u32 m, TLBEntry* e, u32* fsr, bool* is_section) {
    u32 l1addr = (cp15_ttb & 0xFFFFC000) | ((m >> 20) << 2);
    u32 d1 = bus->read32(l1addr);
    u32 dom = (d1 >> 5) & 0xF;
    u32 ap = 0;
    u32 pa = 0;
    bool cacheable = true;
    *is_section = false;
    switch (d1 & 3) {
    case 0:
        *fsr = FS_TRANS_SEC;
        return false;
    case 2: // section
        pa = (d1 & 0xFFF00000) | (m & 0x000FF000);
        ap = (d1 >> 10) & 3;
        *is_section = true;
        break;
    case 1: { // coarse
        u32 l2addr = (d1 & 0xFFFFFC00) | (((m >> 12) & 0xFF) << 2);
        u32 d2 = bus->read32(l2addr);
        switch (d2 & 3) {
        case 0: *fsr = FS_TRANS_PAGE | (dom << 4); return false;
        case 1: { // large 64K
            pa = (d2 & 0xFFFF0000) | (m & 0xF000);
            u32 sub = (m >> 14) & 3;
            ap = (d2 >> (4 + 2 * sub)) & 3;
            break;
        }
        case 2: { // small 4K
            pa = d2 & 0xFFFFF000;
            u32 a0 = (d2 >> 4) & 3, a1 = (d2 >> 6) & 3, a2 = (d2 >> 8) & 3, a3 = (d2 >> 10) & 3;
            ap = (a0 >> 0);
            if (!(a0 == a1 && a1 == a2 && a2 == a3)) {
                ap = (d2 >> (4 + 2 * ((m >> 10) & 3))) & 3;
                cacheable = false;
            }
            break;
        }
        default: *fsr = FS_TRANS_PAGE | (dom << 4); return false;
        }
        break;
    }
    case 3: { // fine
        u32 l2addr = (d1 & 0xFFFFF000) | (((m >> 10) & 0x3FF) << 2);
        u32 d2 = bus->read32(l2addr);
        switch (d2 & 3) {
        case 0: *fsr = FS_TRANS_PAGE | (dom << 4); return false;
        case 1: {
            pa = (d2 & 0xFFFF0000) | (m & 0xF000);
            ap = (d2 >> (4 + 2 * ((m >> 14) & 3))) & 3;
            break;
        }
        case 2: {
            pa = d2 & 0xFFFFF000;
            ap = (d2 >> (4 + 2 * ((m >> 10) & 3))) & 3;
            cacheable = false;
            break;
        }
        case 3: { // tiny 1K: page base chosen so that base + (va & 0xFFF) is correct
            pa = (d2 & 0xFFFFFC00) - (m & 0x00000C00);
            ap = (d2 >> 4) & 3;
            cacheable = false;
            break;
        }
        }
        break;
    }
    }
    u32 dacr = (cp15_dacr >> (dom * 2)) & 3;
    e->domain = (u8)dom;
    e->pa_page = pa;
    e->is_section = *is_section;
    if (dacr == 0 || dacr == 2) {
        e->perm = 0;
        *fsr = (*is_section ? FS_DOM_SEC : FS_DOM_PAGE) | (dom << 4);
    } else if (dacr == 3) {
        e->perm = 0xF;
        *fsr = 0;
    } else {
        e->perm = ap_perm(ap, cp15_control);
        *fsr = (*is_section ? FS_PERM_SEC : FS_PERM_PAGE) | (dom << 4);
    }
    e->valid = cacheable ? 1 : 2; // 2 = do not cache
    return true;
}

ARM926::TLBEntry* ARM926::translate(u32 va, bool write, bool user, bool fetch) {
    static thread_local TLBEntry tmp;
    u32 m = mva(va);
    u32 page = m >> 12;
    u8 need = user ? (write ? 8 : 4) : (write ? 2 : 1);
    if (!(cp15_control & CR_M)) {
        TLBEntry* e = &tlb[page & (TLB_SIZE - 1)];
        if (!(e->valid == 1 && e->tag == page)) {
            e->tag = page;
            e->pa_page = va & 0xFFFFF000;
            e->host = bus->page_ptr(e->pa_page);
            e->perm = 0xF;
            e->valid = 1;
        }
        return e;
    }
    TLBEntry* e = &tlb[page & (TLB_SIZE - 1)];
    if (e->valid == 1 && e->tag == page) {
        if (e->perm & need) return e;
        // permission or domain fault: recompute fsr via walk
    }
    u32 fsr = 0;
    bool sec = false;
    TLBEntry ne;
    if (!walk(m, &ne, &fsr, &sec)) {
        if (fetch) throw MemFault{fsr, va};
        throw MemFault{fsr, va};
    }
    ne.tag = page;
    ne.host = (ne.valid == 1) ? bus->page_ptr(ne.pa_page) : nullptr;
    if (ne.valid == 1) {
        *e = ne;
    } else {
        tmp = ne;
        e = &tmp;
    }
    if (!(e->perm & need)) throw MemFault{fsr, va};
    return e;
}

bool ARM926::debug_translate(u32 va, u32* pa) {
    if (!(cp15_control & CR_M)) { *pa = va; return true; }
    TLBEntry e; u32 fsr; bool sec;
    if (!walk(mva(va), &e, &fsr, &sec)) return false;
    *pa = e.pa_page + (va & 0xFFF);
    return true;
}

u32 ARM926::debug_read32(u32 va) {
    u32 pa;
    if (!debug_translate(va, &pa)) return 0;
    u8* p = bus->page_ptr(pa & 0xFFFFF000);
    if (p) { u32 v; memcpy(&v, p + (pa & 0xFFC), 4); return v; }
    return 0;
}

#define ALIGN_CHECK(va, mask) \
    if (((va) & (mask)) && (cp15_control & CR_A)) throw MemFault{FS_ALIGN, va};

u32 ARM926::vread32(u32 va, bool user) {
    {   // fast path: cached TLB hit on RAM
        u32 m = (va < 0x02000000) ? (va | (cp15_fcse_pid & 0xFE000000)) : va;
        const TLBEntry& e = tlb[(m >> 12) & (TLB_SIZE - 1)];
        if (!(va & 3) && e.valid == 1 && e.tag == (m >> 12) && e.host && (e.perm & (user ? 4 : 1))) {
            u32 v; memcpy(&v, e.host + (va & 0xFFF), 4); return v;
        }
    }
    ALIGN_CHECK(va, 3);
    va &= ~3u;
    TLBEntry* e = translate(va, false, user, false);
    if (e->host) { u32 v; memcpy(&v, e->host + (va & 0xFFF), 4); return v; }
    return bus->read32(e->pa_page + (va & 0xFFF));
}
u16 ARM926::vread16(u32 va, bool user) {
    ALIGN_CHECK(va, 1);
    va &= ~1u;
    TLBEntry* e = translate(va, false, user, false);
    if (e->host) { u16 v; memcpy(&v, e->host + (va & 0xFFF), 2); return v; }
    return bus->read16(e->pa_page + (va & 0xFFF));
}
u8 ARM926::vread8(u32 va, bool user) {
    TLBEntry* e = translate(va, false, user, false);
    if (e->host) return e->host[va & 0xFFF];
    return bus->read8(e->pa_page + (va & 0xFFF));
}
void ARM926::vwrite32(u32 va, u32 v, bool user) {
    {
        u32 m = (va < 0x02000000) ? (va | (cp15_fcse_pid & 0xFE000000)) : va;
        const TLBEntry& e = tlb[(m >> 12) & (TLB_SIZE - 1)];
        if (!(va & 3) && e.valid == 1 && e.tag == (m >> 12) && e.host && (e.perm & (user ? 8 : 2))) {
            memcpy(e.host + (va & 0xFFF), &v, 4); return;
        }
    }
    ALIGN_CHECK(va, 3);
    va &= ~3u;
    TLBEntry* e = translate(va, true, user, false);
    if (e->host) { memcpy(e->host + (va & 0xFFF), &v, 4); return; }
    bus->write32(e->pa_page + (va & 0xFFF), v);
}
void ARM926::vwrite16(u32 va, u16 v, bool user) {
    ALIGN_CHECK(va, 1);
    va &= ~1u;
    TLBEntry* e = translate(va, true, user, false);
    if (e->host) { memcpy(e->host + (va & 0xFFF), &v, 2); return; }
    bus->write16(e->pa_page + (va & 0xFFF), v);
}
void ARM926::vwrite8(u32 va, u8 v, bool user) {
    TLBEntry* e = translate(va, true, user, false);
    if (e->host) { e->host[va & 0xFFF] = v; return; }
    bus->write8(e->pa_page + (va & 0xFFF), v);
}

#define FORCE_INLINE inline __attribute__((always_inline))

u32 ARM926::fetch32(u32 va) {
    bool user = !privileged();
    u32 page = va >> 12;
    if (page == fetch_tag && user == fetch_user && fetch_host) {
        u32 v; memcpy(&v, fetch_host + (va & 0xFFC), 4); return v;
    }
    TLBEntry* e = translate(va, false, user, true);
    if (e->host && e->valid == 1) {
        fetch_tag = page; fetch_host = e->host; fetch_user = user;
        u32 v; memcpy(&v, e->host + (va & 0xFFC), 4); return v;
    }
    fetch_tag = 0xFFFFFFFF;
    if (e->host) { u32 v; memcpy(&v, e->host + (va & 0xFFC), 4); return v; }
    return bus->read32(e->pa_page + (va & 0xFFC));
}

u16 ARM926::fetch16(u32 va) {
    bool user = !privileged();
    u32 page = va >> 12;
    if (page == fetch_tag && user == fetch_user && fetch_host) {
        u16 v; memcpy(&v, fetch_host + (va & 0xFFE), 2); return v;
    }
    TLBEntry* e = translate(va, false, user, true);
    if (e->host && e->valid == 1) {
        fetch_tag = page; fetch_host = e->host; fetch_user = user;
        u16 v; memcpy(&v, e->host + (va & 0xFFE), 2); return v;
    }
    fetch_tag = 0xFFFFFFFF;
    if (e->host) { u16 v; memcpy(&v, e->host + (va & 0xFFE), 2); return v; }
    return bus->read16(e->pa_page + (va & 0xFFE));
}

// ---------------------------------------------------------------------------
// Exceptions
// ---------------------------------------------------------------------------
void ARM926::exception(u32 vector, u32 mode, u32 lr, bool disable_fiq) {
    u32 old = cpsr;
    switch_mode(mode);
    spsr = old;
    r[14] = lr;
    cpsr &= ~FLAG_T;
    cpsr |= FLAG_I;
    if (disable_fiq) cpsr |= FLAG_F;
    u32 base = (cp15_control & CR_V) ? 0xFFFF0000 : 0;
    r[15] = base + vector;
    pc_written = true;
}

void ARM926::raise_undefined() {
    u32 next = cur_pc + (in_thumb() ? 2 : 4);
    exception(0x04, MODE_UND, next, false);
}

void ARM926::data_abort(const MemFault& f) {
    cp15_dfsr = f.fsr & 0xFF;
    cp15_far = f.addr;
    exception(0x10, MODE_ABT, cur_pc + 8, false);
}

void ARM926::prefetch_abort(const MemFault& f) {
    cp15_ifsr = f.fsr & 0xFF;
    exception(0x0C, MODE_ABT, cur_pc + 4, false);
}

void ARM926::check_interrupts() {
    if (fiq_line && !(cpsr & FLAG_F)) {
        wfi = false;
        exception(0x1C, MODE_FIQ, r[15] + 4, true);
    } else if (irq_line && !(cpsr & FLAG_I)) {
        wfi = false;
        exception(0x18, MODE_IRQ, r[15] + 4, false);
    }
}

FORCE_INLINE bool ARM926::cond(u32 c) const {
    bool n = cpsr & FLAG_N, z = cpsr & FLAG_Z, cy = cpsr & FLAG_C, v = cpsr & FLAG_V;
    switch (c) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return cy;
    case 0x3: return !cy;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return cy && !z;
    case 0x9: return !cy || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && (n == v);
    case 0xD: return z || (n != v);
    case 0xE: return true;
    default: return false;
    }
}

FORCE_INLINE u32 ARM926::shift_imm(u32 insn, bool* carry) {
    u32 rm = r[insn & 0xF];
    u32 amt = (insn >> 7) & 0x1F;
    u32 type = (insn >> 5) & 3;
    bool c = cpsr & FLAG_C;
    switch (type) {
    case 0: // LSL
        if (amt == 0) { *carry = c; return rm; }
        *carry = bit(rm, 32 - amt);
        return rm << amt;
    case 1: // LSR
        if (amt == 0) { *carry = bit(rm, 31); return 0; }
        *carry = bit(rm, amt - 1);
        return rm >> amt;
    case 2: // ASR
        if (amt == 0) { *carry = bit(rm, 31); return (u32)((s32)rm >> 31); }
        *carry = bit(rm, amt - 1);
        return (u32)((s32)rm >> amt);
    default: // ROR / RRX
        if (amt == 0) { *carry = rm & 1; return (rm >> 1) | ((u32)c << 31); }
        *carry = bit(rm, amt - 1);
        return ror32(rm, amt);
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
s64 ARM926::run(s64 cycles) {
    s64 done = 0;
    while (done < cycles) {
        if (wfi) {
            if (irq_line || fiq_line) wfi = false;
            else { done = cycles; break; }
        }
        if ((irq_line && !(cpsr & FLAG_I)) || (fiq_line && !(cpsr & FLAG_F))) {
            check_interrupts();
        }
        cur_pc = r[15];
        pc_written = false;
        if (((u32)(cur_pc - hle_base) < hle_size || cur_pc == hle_watch_pc) && hle_hook &&
            hle_hook(this, cur_pc, hle_user)) {
            done++;
            continue;
        }
        try {
            if (cpsr & FLAG_T) {
                u16 insn;
                try { insn = fetch16(cur_pc); }
                catch (const MemFault& f) { prefetch_abort(f); done++; continue; }
                if (trace_hook) trace_hook(this, cur_pc, trace_user);
                r[15] = cur_pc + 4;
                exec_thumb(insn);
                if (!pc_written) r[15] = cur_pc + 2;
            } else {
                u32 insn;
                if ((cur_pc >> 12) == fetch_tag && fetch_host && fetch_user == !privileged()) {
                    memcpy(&insn, fetch_host + (cur_pc & 0xFFC), 4);
                } else {
                    try { insn = fetch32(cur_pc); }
                    catch (const MemFault& f) { prefetch_abort(f); done++; continue; }
                }
                if (trace_hook) trace_hook(this, cur_pc, trace_user);
                r[15] = cur_pc + 8;
                u32 c = insn >> 28;
                if (c == 0xE || (c != 0xF && cond(c)) || c == 0xF) exec_arm(insn);
                if (!pc_written) r[15] = cur_pc + 4;
            }
        } catch (const MemFault& f) {
            data_abort(f);
        }
        done++;
        insn_count++;
    }
    total_cycles += done;
    return done;
}


void ARM926::set_pc(u32 v) {
    if (cpsr & FLAG_T) r[15] = v & ~1u;
    else r[15] = v & ~3u;
    pc_written = true;
}

void ARM926::set_pc_interwork(u32 v) {
    if (v & 1) { cpsr |= FLAG_T; r[15] = v & ~1u; }
    else { cpsr &= ~FLAG_T; r[15] = v & ~3u; }
    pc_written = true;
}

void ARM926::write_cpsr(u32 val, u32 mask) {
    if (!privileged()) mask &= 0xFF000000;
    u32 nv = (cpsr & ~mask) | (val & mask);
    if (mask & 0x1F) switch_mode(nv & 0x1F);
    cpsr = (cpsr & 0x1F) | (nv & ~0x1Fu);
    // T bit writes via MSR are unpredictable; preserve behaviour of hardware (allowed)
}

// ---------------------------------------------------------------------------
// ARM instruction execution
// ---------------------------------------------------------------------------

u32 ARM926::shift_reg(u32 insn, bool* carry) {
    u32 rmn = insn & 0xF;
    u32 rm = r[rmn] + (rmn == 15 ? 4 : 0);
    u32 amt = r[(insn >> 8) & 0xF] & 0xFF;
    u32 type = (insn >> 5) & 3;
    bool c = cpsr & FLAG_C;
    if (amt == 0) { *carry = c; return rm; }
    switch (type) {
    case 0:
        if (amt < 32) { *carry = bit(rm, 32 - amt); return rm << amt; }
        if (amt == 32) { *carry = rm & 1; return 0; }
        *carry = 0; return 0;
    case 1:
        if (amt < 32) { *carry = bit(rm, amt - 1); return rm >> amt; }
        if (amt == 32) { *carry = bit(rm, 31); return 0; }
        *carry = 0; return 0;
    case 2:
        if (amt < 32) { *carry = bit(rm, amt - 1); return (u32)((s32)rm >> amt); }
        *carry = bit(rm, 31); return (u32)((s32)rm >> 31);
    default: {
        u32 a = amt & 31;
        if (a == 0) { *carry = bit(rm, 31); return rm; }
        *carry = bit(rm, a - 1);
        return ror32(rm, a);
    }
    }
}

static inline u32 set_nz(u32 cpsr, u32 res) {
    cpsr &= ~(FLAG_N | FLAG_Z);
    if (res & 0x80000000) cpsr |= FLAG_N;
    if (res == 0) cpsr |= FLAG_Z;
    return cpsr;
}

void ARM926::data_proc(u32 insn, u32 op2, bool shc) {
    u32 op = (insn >> 21) & 0xF;
    bool s = bit(insn, 20);
    u32 rnn = (insn >> 16) & 0xF;
    u32 rdn = (insn >> 12) & 0xF;
    u32 rn = r[rnn];
    if (rnn == 15 && !bit(insn, 25) && bit(insn, 4)) rn += 4; // reg-shift-reg: PC+12
    u32 res = 0;
    bool c = shc, v = cpsr & FLAG_V;
    bool logical = false, write = true;
    u64 t;
    u32 cin = (cpsr & FLAG_C) ? 1 : 0;
    switch (op) {
    case 0x0: res = rn & op2; logical = true; break;
    case 0x1: res = rn ^ op2; logical = true; break;
    case 0x2: res = rn - op2; c = rn >= op2; v = ((rn ^ op2) & (rn ^ res)) >> 31; break;
    case 0x3: res = op2 - rn; c = op2 >= rn; v = ((op2 ^ rn) & (op2 ^ res)) >> 31; break;
    case 0x4: t = (u64)rn + op2; res = (u32)t; c = t >> 32; v = (~(rn ^ op2) & (rn ^ res)) >> 31; break;
    case 0x5: t = (u64)rn + op2 + cin; res = (u32)t; c = t >> 32; v = (~(rn ^ op2) & (rn ^ res)) >> 31; break;
    case 0x6: t = (u64)rn - op2 - (1 - cin); res = (u32)t; c = !((t >> 32) & 1); v = ((rn ^ op2) & (rn ^ res)) >> 31; break;
    case 0x7: t = (u64)op2 - rn - (1 - cin); res = (u32)t; c = !((t >> 32) & 1); v = ((op2 ^ rn) & (op2 ^ res)) >> 31; break;
    case 0x8: res = rn & op2; logical = true; write = false; break;
    case 0x9: res = rn ^ op2; logical = true; write = false; break;
    case 0xA: res = rn - op2; c = rn >= op2; v = ((rn ^ op2) & (rn ^ res)) >> 31; write = false; break;
    case 0xB: t = (u64)rn + op2; res = (u32)t; c = t >> 32; v = (~(rn ^ op2) & (rn ^ res)) >> 31; write = false; break;
    case 0xC: res = rn | op2; logical = true; break;
    case 0xD: res = op2; logical = true; break;
    case 0xE: res = rn & ~op2; logical = true; break;
    case 0xF: res = ~op2; logical = true; break;
    }
    (void)logical;
    if (s) {
        if (rdn == 15 && write) {
            // restore CPSR from SPSR
            u32 m = cpsr & 0x1F;
            if (m != MODE_USR && m != MODE_SYS) {
                u32 sp = spsr;
                switch_mode(sp & 0x1F);
                cpsr = sp;
            }
            if (cpsr & FLAG_T) r[15] = res & ~1u; else r[15] = res & ~3u;
            pc_written = true;
            return;
        }
        u32 f = set_nz(cpsr, res);
        f &= ~(FLAG_C | FLAG_V);
        if (c) f |= FLAG_C;
        if (v) f |= FLAG_V;
        cpsr = f;
    }
    if (write) {
        if (rdn == 15) set_pc(res);
        else r[rdn] = res;
    }
}

u32 ARM926::load_word_rot(u32 addr, bool user) {
    u32 v = vread32(addr, user);
    return ror32(v, (addr & 3) * 8);
}

void ARM926::ldr_str(u32 insn) {
    bool I = bit(insn, 25), P = bit(insn, 24), U = bit(insn, 23), B = bit(insn, 22),
         W = bit(insn, 21), L = bit(insn, 20);
    u32 rnn = (insn >> 16) & 0xF, rdn = (insn >> 12) & 0xF;
    u32 off;
    if (I) {
        if (bit(insn, 4)) { raise_undefined(); return; }
        bool dummy; off = shift_imm(insn, &dummy);
    } else off = insn & 0xFFF;
    u32 base = r[rnn];
    u32 addr = P ? (U ? base + off : base - off) : base;
    bool user = !privileged() || (!P && W);
    u32 newbase = U ? base + off : base - off;
    if (L) {
        u32 val = B ? vread8(addr, user) : load_word_rot(addr, user);
        if (!P || W) { if (rnn != 15) r[rnn] = newbase; }
        if (rdn == 15) set_pc_interwork(val);
        else r[rdn] = val;
    } else {
        u32 val = r[rdn];
        if (rdn == 15) val = cur_pc + 12;
        if (B) vwrite8(addr, (u8)val, user); else vwrite32(addr, val, user);
        if (!P || W) { if (rnn != 15) r[rnn] = newbase; }
    }
}

void ARM926::ldrh_strh(u32 insn) {
    bool P = bit(insn, 24), U = bit(insn, 23), I = bit(insn, 22), W = bit(insn, 21), L = bit(insn, 20);
    u32 rnn = (insn >> 16) & 0xF, rdn = (insn >> 12) & 0xF;
    u32 sh = (insn >> 5) & 3;
    u32 off = I ? (((insn >> 4) & 0xF0) | (insn & 0xF)) : r[insn & 0xF];
    u32 base = r[rnn];
    u32 addr = P ? (U ? base + off : base - off) : base;
    u32 newbase = U ? base + off : base - off;
    bool user = !privileged();
    bool wb = !P || W;
    if (L) {
        u32 val;
        switch (sh) {
        case 1: val = vread16(addr, user); break;
        case 2: val = (u32)(s32)(int8_t)vread8(addr, user); break;
        default: val = (u32)(s32)(int16_t)vread16(addr, user); break;
        }
        if (wb && rnn != 15) r[rnn] = newbase;
        if (rdn == 15) set_pc(val); else r[rdn] = val;
    } else {
        if (sh == 1) {
            u32 val = (rdn == 15) ? cur_pc + 12 : r[rdn];
            vwrite16(addr, (u16)val, user);
            if (wb && rnn != 15) r[rnn] = newbase;
        } else if (sh == 2) { // LDRD
            if (rdn & 1) { raise_undefined(); return; }
            if ((addr & 7) && (cp15_control & CR_A) && (addr & 3)) throw MemFault{FS_ALIGN, addr};
            u32 a = vread32(addr, user);
            u32 b = vread32(addr + 4, user);
            if (wb && rnn != 15) r[rnn] = newbase;
            r[rdn] = a;
            if (rdn + 1 == 15) set_pc(b); else r[rdn + 1] = b;
        } else { // STRD
            if (rdn & 1) { raise_undefined(); return; }
            vwrite32(addr, r[rdn], user);
            vwrite32(addr + 4, (rdn + 1 == 15) ? cur_pc + 12 : r[rdn + 1], user);
            if (wb && rnn != 15) r[rnn] = newbase;
        }
    }
}

void ARM926::ldm_stm(u32 insn) {
    bool P = bit(insn, 24), U = bit(insn, 23), S = bit(insn, 22), W = bit(insn, 21), L = bit(insn, 20);
    u32 rnn = (insn >> 16) & 0xF;
    u32 list = insn & 0xFFFF;
    u32 base = r[rnn];
    int n = __builtin_popcount(list);
    bool empty = (n == 0);
    if (empty) { list = 0x8000; n = 16; }
    u32 start;
    if (U) start = P ? base + 4 : base;
    else start = P ? base - n * 4 : base - n * 4 + 4;
    u32 newbase = U ? base + n * 4 : base - n * 4;
    if (empty) list = 0x8000; // ARMv5 behaviour: transfer PC only, but writeback by 0x40
    bool user = !privileged();
    u32 addr = start & ~3u;
    if (L) {
        u32 vals[16];
        u32 a = addr;
        for (int i = 0; i < 16; i++) if (list & (1u << i)) { vals[i] = vread32(a, user); a += 4; }
        bool pc_in = list & 0x8000;
        if (W) r[rnn] = newbase;
        if (S && !pc_in) {
            for (int i = 0; i < 15; i++) if (list & (1u << i)) set_user_reg(i, vals[i]);
        } else {
            for (int i = 0; i < 15; i++) if (list & (1u << i)) r[i] = vals[i];
        }
        if (pc_in) {
            if (S) {
                u32 m = cpsr & 0x1F;
                if (m != MODE_USR && m != MODE_SYS) {
                    u32 sp = spsr;
                    switch_mode(sp & 0x1F);
                    cpsr = sp;
                }
                if (cpsr & FLAG_T) r[15] = vals[15] & ~1u; else r[15] = vals[15] & ~3u;
                pc_written = true;
            } else {
                set_pc_interwork(vals[15]);
            }
        }
    } else {
        u32 a = addr;
        bool first = true;
        for (int i = 0; i < 16; i++) {
            if (!(list & (1u << i))) continue;
            u32 v;
            if (i == 15) v = cur_pc + 12;
            else if (S) v = get_user_reg(i);
            else v = r[i];
            if (i == (int)rnn && !first && W) v = newbase;
            vwrite32(a, v, user);
            a += 4;
            first = false;
        }
        if (W) r[rnn] = newbase;
    }
}

void ARM926::multiply(u32 insn) {
    u32 op = (insn >> 21) & 0xF;
    bool s = bit(insn, 20);
    u32 rdhi = (insn >> 16) & 0xF, rdlo = (insn >> 12) & 0xF;
    u32 rs = r[(insn >> 8) & 0xF], rm = r[insn & 0xF];
    switch (op) {
    case 0: { // MUL
        u32 res = rm * rs;
        r[rdhi] = res;
        if (s) cpsr = set_nz(cpsr, res);
        break;
    }
    case 1: { // MLA
        u32 res = rm * rs + r[rdlo];
        r[rdhi] = res;
        if (s) cpsr = set_nz(cpsr, res);
        break;
    }
    case 4: case 5: case 6: case 7: {
        u64 res;
        if (op == 4) res = (u64)rm * rs;
        else if (op == 5) res = (u64)rm * rs + (((u64)r[rdhi] << 32) | r[rdlo]);
        else if (op == 6) res = (u64)((s64)(s32)rm * (s64)(s32)rs);
        else res = (u64)((s64)(s32)rm * (s64)(s32)rs + (s64)(((u64)r[rdhi] << 32) | r[rdlo]));
        r[rdlo] = (u32)res;
        r[rdhi] = (u32)(res >> 32);
        if (s) {
            cpsr &= ~(FLAG_N | FLAG_Z);
            if (res >> 63) cpsr |= FLAG_N;
            if (res == 0) cpsr |= FLAG_Z;
        }
        break;
    }
    default: raise_undefined(); break;
    }
}

static inline s32 sat_add(s32 a, s32 b, bool* q) {
    s64 r = (s64)a + b;
    if (r > 0x7FFFFFFF) { *q = true; return 0x7FFFFFFF; }
    if (r < -(s64)0x80000000) { *q = true; return (s32)0x80000000; }
    return (s32)r;
}
static inline s32 sat_sub(s32 a, s32 b, bool* q) {
    s64 r = (s64)a - b;
    if (r > 0x7FFFFFFF) { *q = true; return 0x7FFFFFFF; }
    if (r < -(s64)0x80000000) { *q = true; return (s32)0x80000000; }
    return (s32)r;
}

void ARM926::misc(u32 insn) {
    u32 op = (insn >> 21) & 3;
    u32 low = (insn >> 4) & 0xF;
    if (low == 0) {
        if (op & 1) { // MSR reg
            msr(insn, r[insn & 0xF]);
        } else { // MRS
            u32 rd = (insn >> 12) & 0xF;
            u32 v;
            if (bit(insn, 22)) {
                u32 m = cpsr & 0x1F;
                v = (m == MODE_USR || m == MODE_SYS) ? cpsr : spsr;
            } else v = cpsr;
            r[rd] = v;
        }
        return;
    }
    if (low == 1 && op == 1) { set_pc_interwork(r[insn & 0xF]); return; }   // BX
    if (low == 2 && op == 1) { set_pc_interwork(r[insn & 0xF]); return; }   // BXJ (no Jazelle)
    if (low == 3 && op == 1) {                                             // BLX reg
        u32 t = r[insn & 0xF];
        r[14] = cur_pc + 4;
        set_pc_interwork(t);
        return;
    }
    if (low == 1 && op == 3) {                                             // CLZ
        u32 v = r[insn & 0xF];
        r[(insn >> 12) & 0xF] = v ? __builtin_clz(v) : 32;
        return;
    }
    if (low == 5) {                                                        // QADD etc
        s32 rm = r[insn & 0xF], rn = r[(insn >> 16) & 0xF];
        u32 rd = (insn >> 12) & 0xF;
        bool q = false;
        s32 res;
        switch (op) {
        case 0: res = sat_add(rm, rn, &q); break;
        case 1: res = sat_sub(rm, rn, &q); break;
        case 2: { s32 d = sat_add(rn, rn, &q); res = sat_add(rm, d, &q); break; }
        default: { s32 d = sat_add(rn, rn, &q); res = sat_sub(rm, d, &q); break; }
        }
        r[rd] = (u32)res;
        if (q) cpsr |= FLAG_Q;
        return;
    }
    if (low == 7 && op == 1) {                                             // BKPT
        MemFault f{0x2, cur_pc};
        cp15_ifsr = 0x2;
        exception(0x0C, MODE_ABT, cur_pc + 4, false);
        (void)f;
        return;
    }
    if ((low & 9) == 8) {                                                  // DSP multiplies
        u32 rd = (insn >> 16) & 0xF, rn = (insn >> 12) & 0xF;
        u32 rs = r[(insn >> 8) & 0xF], rm = r[insn & 0xF];
        bool x = bit(insn, 5), y = bit(insn, 6);
        s32 a = x ? (s32)rm >> 16 : (s32)(int16_t)rm;
        s32 b = y ? (s32)rs >> 16 : (s32)(int16_t)rs;
        switch (op) {
        case 0: { // SMLAxy
            s32 p = a * b;
            bool q = false;
            s64 sum = (s64)p + (s32)r[rn];
            if (sum > 0x7FFFFFFF || sum < -(s64)0x80000000) q = true;
            r[rd] = (u32)(s32)sum;
            if (q) cpsr |= FLAG_Q;
            break;
        }
        case 1: { // SMLAWy / SMULWy
            s32 bb = y ? (s32)rs >> 16 : (s32)(int16_t)rs;
            s64 p = ((s64)(s32)rm * bb) >> 16;
            if (!x) { // SMLAW
                s64 sum = p + (s32)r[rn];
                if ((s32)sum != sum) cpsr |= FLAG_Q;
                r[rd] = (u32)(s32)sum;
            } else {
                r[rd] = (u32)(s32)p;
            }
            break;
        }
        case 2: { // SMLALxy
            s64 p = (s64)a * b;
            u64 acc = ((u64)r[rd] << 32) | r[rn];
            acc += (u64)p;
            r[rn] = (u32)acc;
            r[rd] = (u32)(acc >> 32);
            break;
        }
        default: // SMULxy
            r[rd] = (u32)(a * b);
            break;
        }
        return;
    }
    raise_undefined();
}

void ARM926::msr(u32 insn, u32 val) {
    u32 mask = 0;
    if (bit(insn, 16)) mask |= 0x000000FF;
    if (bit(insn, 17)) mask |= 0x0000FF00;
    if (bit(insn, 18)) mask |= 0x00FF0000;
    if (bit(insn, 19)) mask |= 0xFF000000;
    if (bit(insn, 22)) {
        u32 m = cpsr & 0x1F;
        if (m != MODE_USR && m != MODE_SYS) spsr = (spsr & ~mask) | (val & mask);
    } else {
        write_cpsr(val, mask);
    }
}

u32 ARM926::cp15_read(u32 crn, u32 crm, u32 op1, u32 op2) {
    (void)op1;
    switch (crn) {
    case 0:
        if (op2 == 1) return 0x1D152152;   // cache type: 16K I / 16K D
        if (op2 == 2) return 0;            // TCM status
        return 0x41069265;                 // ARM926EJ-S r0p5
    case 1: return cp15_control;
    case 2: return cp15_ttb;
    case 3: return cp15_dacr;
    case 5: return op2 == 1 ? cp15_ifsr : cp15_dfsr;
    case 6: return cp15_far;
    case 7:
        if (crm == 10 && op2 == 3) return 0x40000000; // test & clean: done (Z set)
        if (crm == 14 && op2 == 3) return 0x40000000;
        return 0;
    case 9: return crm == 0 ? (op2 ? cp15_cache_lockdown_i : cp15_cache_lockdown_d) : 0;
    case 10: return cp15_tlb_lockdown;
    case 13: return op2 == 1 ? cp15_context_id : cp15_fcse_pid;
    default: return 0;
    }
}

void ARM926::cp15_write(u32 crn, u32 crm, u32 op1, u32 op2, u32 v) {
    (void)op1;
    switch (crn) {
    case 1: {
        u32 old = cp15_control;
        cp15_control = (v & 0x0000F387) | 0x00050078;
        if ((old ^ cp15_control) & (CR_M | CR_S | CR_R)) flush_tlb();
        break;
    }
    case 2: cp15_ttb = v & 0xFFFFC000; flush_tlb(); if (ttb_hook) ttb_hook(this, hle_user); break;
    case 3: cp15_dacr = v; flush_tlb(); break;
    case 5: if (op2 == 1) cp15_ifsr = v; else cp15_dfsr = v; break;
    case 6: cp15_far = v; break;
    case 7:
        if ((crm == 0 && op2 == 4) || (crm == 8 && op2 == 2)) wfi = true;  // wait for interrupt
        if (crm == 5 || crm == 7) { fetch_tag = 0xFFFFFFFF; }               // icache invalidate
        break;
    case 8: flush_tlb(); break;                                             // TLB ops
    case 9: if (crm == 0) { if (op2) cp15_cache_lockdown_i = v; else cp15_cache_lockdown_d = v; } break;
    case 10: cp15_tlb_lockdown = v; break;
    case 13:
        if (op2 == 1) cp15_context_id = v;
        else { cp15_fcse_pid = v & 0xFE000000; fetch_tag = 0xFFFFFFFF; }
        break;
    default: break;
    }
}

void ARM926::coproc(u32 insn) {
    u32 cp = (insn >> 8) & 0xF;
    if (bit(insn, 25) && bit(insn, 24) == 0 && bit(insn, 4)) {
        // MRC / MCR
        bool L = bit(insn, 20);
        u32 crn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF, crm = insn & 0xF;
        u32 op1 = (insn >> 21) & 7, op2 = (insn >> 5) & 7;
        if (cp == 15) {
            if (!privileged()) { raise_undefined(); return; }
            if (L) {
                u32 v = cp15_read(crn, crm, op1, op2);
                if (rd == 15) cpsr = (cpsr & 0x0FFFFFFF) | (v & 0xF0000000);
                else r[rd] = v;
            } else {
                cp15_write(crn, crm, op1, op2, rd == 15 ? cur_pc + 12 : r[rd]);
            }
            return;
        }
        if (cp == 14) { // debug coprocessor: reads as zero
            if (L) { if (rd == 15) cpsr &= 0x0FFFFFFF; else r[rd] = 0; }
            return;
        }
    }
    if (undef_hook) undef_hook(this, cur_pc, insn, trace_user);
    raise_undefined();
}

void ARM926::exec_arm(u32 insn) {
    u32 c = insn >> 28;
    if (c == 0xF) {
        // Unconditional space
        if (((insn >> 25) & 7) == 5) { // BLX imm
            s32 off = ((s32)(insn << 8) >> 6) | (bit(insn, 24) << 1);
            r[14] = cur_pc + 4;
            cpsr |= FLAG_T;
            r[15] = (cur_pc + 8 + off) & ~1u;
            pc_written = true;
            return;
        }
        if ((insn & 0x0D70F000) == 0x0550F000) return; // PLD
        if (undef_hook) undef_hook(this, cur_pc, insn, trace_user);
        raise_undefined();
        return;
    }
    switch ((insn >> 25) & 7) {
    case 0: {
        if ((insn & 0x90) == 0x90) {
            // multiplies / extra load-store / swap
            if ((insn & 0x60) == 0) {
                if (bit(insn, 24)) {
                    // SWP / SWPB
                    if ((insn & 0x0FB00FF0) == 0x01000090) {
                        u32 rn = r[(insn >> 16) & 0xF];
                        u32 rd = (insn >> 12) & 0xF, rm = r[insn & 0xF];
                        bool user = !privileged();
                        if (bit(insn, 22)) {
                            u32 t = vread8(rn, user);
                            vwrite8(rn, (u8)rm, user);
                            r[rd] = t;
                        } else {
                            u32 t = load_word_rot(rn, user);
                            vwrite32(rn, rm, user);
                            r[rd] = t;
                        }
                        return;
                    }
                    raise_undefined();
                    return;
                }
                multiply(insn);
                return;
            }
            ldrh_strh(insn);
            return;
        }
        if ((insn & 0x01900000) == 0x01000000) {
            // misc (TST/TEQ/CMP/CMN without S)
            if (bit(insn, 7) && !bit(insn, 4)) { misc(insn); return; }
            if (!bit(insn, 7)) { misc(insn); return; }
            raise_undefined();
            return;
        }
        bool carry;
        u32 op2 = bit(insn, 4) ? shift_reg(insn, &carry) : shift_imm(insn, &carry);
        data_proc(insn, op2, carry);
        return;
    }
    case 1: {
        if ((insn & 0x01900000) == 0x01000000) {
            if (bit(insn, 21)) { // MSR imm
                u32 imm = ror32(insn & 0xFF, ((insn >> 8) & 0xF) * 2);
                msr(insn, imm);
                return;
            }
            raise_undefined();
            return;
        }
        u32 rot = ((insn >> 8) & 0xF) * 2;
        u32 imm = ror32(insn & 0xFF, rot);
        bool carry = rot ? (imm >> 31) : (bool)(cpsr & FLAG_C);
        data_proc(insn, imm, carry);
        return;
    }
    case 2: case 3:
        ldr_str(insn);
        return;
    case 4:
        ldm_stm(insn);
        return;
    case 5: {
        s32 off = (s32)(insn << 8) >> 6;
        if (bit(insn, 24)) r[14] = cur_pc + 4;
        r[15] = cur_pc + 8 + off;
        pc_written = true;
        return;
    }
    case 6:
        coproc(insn);
        return;
    case 7:
        if (bit(insn, 24)) {
            if (swi_hook) swi_hook(this, cur_pc, insn & 0xFFFFFF, trace_user);
            exception(0x08, MODE_SVC, cur_pc + 4, false);
            return;
        }
        coproc(insn);
        return;
    }
}

// ---------------------------------------------------------------------------
// Thumb
// ---------------------------------------------------------------------------
void ARM926::exec_thumb(u16 insn) {
    u32 pc4 = cur_pc + 4;
    bool user = !privileged();
    switch (insn >> 11) {
    case 0x00: case 0x01: case 0x02: { // shift by immediate
        u32 rd = insn & 7, rm = r[(insn >> 3) & 7];
        u32 amt = (insn >> 6) & 0x1F;
        u32 type = insn >> 11;
        bool c = cpsr & FLAG_C;
        u32 res;
        if (type == 0) {
            if (amt == 0) res = rm;
            else { c = bit(rm, 32 - amt); res = rm << amt; }
        } else if (type == 1) {
            if (amt == 0) { c = bit(rm, 31); res = 0; }
            else { c = bit(rm, amt - 1); res = rm >> amt; }
        } else {
            if (amt == 0) { c = bit(rm, 31); res = (u32)((s32)rm >> 31); }
            else { c = bit(rm, amt - 1); res = (u32)((s32)rm >> amt); }
        }
        r[rd] = res;
        cpsr = set_nz(cpsr, res);
        cpsr = c ? (cpsr | FLAG_C) : (cpsr & ~FLAG_C);
        return;
    }
    case 0x03: { // add/sub
        u32 rd = insn & 7, rn = r[(insn >> 3) & 7];
        u32 op2 = bit(insn, 10) ? ((insn >> 6) & 7) : r[(insn >> 6) & 7];
        u32 res; bool c, v;
        if (bit(insn, 9)) { res = rn - op2; c = rn >= op2; v = ((rn ^ op2) & (rn ^ res)) >> 31; }
        else { u64 t = (u64)rn + op2; res = (u32)t; c = t >> 32; v = (~(rn ^ op2) & (rn ^ res)) >> 31; }
        r[rd] = res;
        u32 f = set_nz(cpsr, res) & ~(FLAG_C | FLAG_V);
        if (c) f |= FLAG_C; if (v) f |= FLAG_V;
        cpsr = f;
        return;
    }
    case 0x04: case 0x05: case 0x06: case 0x07: { // mov/cmp/add/sub imm8
        u32 rd = (insn >> 8) & 7, imm = insn & 0xFF;
        u32 op = (insn >> 11) & 3;
        u32 a = r[rd], res; bool c = cpsr & FLAG_C, v = cpsr & FLAG_V;
        switch (op) {
        case 0: res = imm; break;
        case 1: case 3: res = a - imm; c = a >= imm; v = ((a ^ imm) & (a ^ res)) >> 31; break;
        default: { u64 t = (u64)a + imm; res = (u32)t; c = t >> 32; v = (~(a ^ imm) & (a ^ res)) >> 31; break; }
        }
        if (op != 1) r[rd] = res;
        u32 f = set_nz(cpsr, res);
        if (op != 0) { f &= ~(FLAG_C | FLAG_V); if (c) f |= FLAG_C; if (v) f |= FLAG_V; }
        cpsr = f;
        return;
    }
    case 0x08: {
        if (!bit(insn, 10)) { // ALU ops
            u32 rd = insn & 7, rs = r[(insn >> 3) & 7];
            u32 a = r[rd], res = 0;
            u32 op = (insn >> 6) & 0xF;
            bool c = cpsr & FLAG_C, v = cpsr & FLAG_V;
            bool wr = true, setcv = false;
            u32 cin = c ? 1 : 0;
            u64 t;
            switch (op) {
            case 0x0: res = a & rs; break;
            case 0x1: res = a ^ rs; break;
            case 0x2: { u32 s = rs & 0xFF; if (s == 0) res = a; else if (s < 32) { c = bit(a, 32 - s); res = a << s; } else if (s == 32) { c = a & 1; res = 0; } else { c = 0; res = 0; } break; }
            case 0x3: { u32 s = rs & 0xFF; if (s == 0) res = a; else if (s < 32) { c = bit(a, s - 1); res = a >> s; } else if (s == 32) { c = bit(a, 31); res = 0; } else { c = 0; res = 0; } break; }
            case 0x4: { u32 s = rs & 0xFF; if (s == 0) res = a; else if (s < 32) { c = bit(a, s - 1); res = (u32)((s32)a >> s); } else { c = bit(a, 31); res = (u32)((s32)a >> 31); } break; }
            case 0x5: t = (u64)a + rs + cin; res = (u32)t; c = t >> 32; v = (~(a ^ rs) & (a ^ res)) >> 31; setcv = true; break;
            case 0x6: t = (u64)a - rs - (1 - cin); res = (u32)t; c = !((t >> 32) & 1); v = ((a ^ rs) & (a ^ res)) >> 31; setcv = true; break;
            case 0x7: { u32 s = rs & 0xFF; if (s == 0) res = a; else { u32 s5 = s & 31; if (s5 == 0) { c = bit(a, 31); res = a; } else { c = bit(a, s5 - 1); res = ror32(a, s5); } } break; }
            case 0x8: res = a & rs; wr = false; break;
            case 0x9: res = 0 - rs; c = 0 >= rs; v = ((0 ^ rs) & (0 ^ res)) >> 31; setcv = true; break;
            case 0xA: res = a - rs; c = a >= rs; v = ((a ^ rs) & (a ^ res)) >> 31; setcv = true; wr = false; break;
            case 0xB: t = (u64)a + rs; res = (u32)t; c = t >> 32; v = (~(a ^ rs) & (a ^ res)) >> 31; setcv = true; wr = false; break;
            case 0xC: res = a | rs; break;
            case 0xD: res = a * rs; break;
            case 0xE: res = a & ~rs; break;
            case 0xF: res = ~rs; break;
            }
            if (wr) r[rd] = res;
            u32 f = set_nz(cpsr, res);
            if (setcv) { f &= ~(FLAG_C | FLAG_V); if (c) f |= FLAG_C; if (v) f |= FLAG_V; }
            else if (op >= 2 && op <= 7 && op != 5 && op != 6) { f = c ? (f | FLAG_C) : (f & ~FLAG_C); }
            cpsr = f;
            return;
        }
        if (!bit(insn, 9) || !bit(insn, 8) || true) {
            u32 op = (insn >> 8) & 3;
            u32 rd = (insn & 7) | (bit(insn, 7) << 3);
            u32 rmn = (insn >> 3) & 0xF;
            u32 rm = r[rmn];
            switch (op) {
            case 0: { // ADD hi
                u32 res = r[rd] + rm;
                if (rd == 15) set_pc(res); else r[rd] = res;
                return;
            }
            case 1: { // CMP hi
                u32 a = r[rd], res = a - rm;
                bool c = a >= rm, v = ((a ^ rm) & (a ^ res)) >> 31;
                u32 f = set_nz(cpsr, res) & ~(FLAG_C | FLAG_V);
                if (c) f |= FLAG_C; if (v) f |= FLAG_V;
                cpsr = f;
                return;
            }
            case 2: // MOV hi
                if (rd == 15) set_pc(rm); else r[rd] = rm;
                return;
            default: // BX / BLX
                if (bit(insn, 7)) r[14] = (cur_pc + 2) | 1;
                set_pc_interwork(rm);
                return;
            }
        }
        return;
    }
    case 0x09: { // LDR pc-relative
        u32 addr = (pc4 & ~3u) + ((insn & 0xFF) << 2);
        r[(insn >> 8) & 7] = vread32(addr, user);
        return;
    }
    case 0x0A: case 0x0B: { // load/store register offset
        u32 rd = insn & 7, rb = r[(insn >> 3) & 7], ro = r[(insn >> 6) & 7];
        u32 addr = rb + ro;
        switch ((insn >> 9) & 7) {
        case 0: vwrite32(addr, r[rd], user); break;
        case 1: vwrite16(addr, (u16)r[rd], user); break;
        case 2: vwrite8(addr, (u8)r[rd], user); break;
        case 3: r[rd] = (u32)(s32)(int8_t)vread8(addr, user); break;
        case 4: r[rd] = load_word_rot(addr, user); break;
        case 5: r[rd] = vread16(addr, user); break;
        case 6: r[rd] = vread8(addr, user); break;
        default: r[rd] = (u32)(s32)(int16_t)vread16(addr, user); break;
        }
        return;
    }
    case 0x0C: { u32 a = r[(insn >> 3) & 7] + (((insn >> 6) & 0x1F) << 2); vwrite32(a, r[insn & 7], user); return; }
    case 0x0D: { u32 a = r[(insn >> 3) & 7] + (((insn >> 6) & 0x1F) << 2); r[insn & 7] = load_word_rot(a, user); return; }
    case 0x0E: { u32 a = r[(insn >> 3) & 7] + ((insn >> 6) & 0x1F); vwrite8(a, (u8)r[insn & 7], user); return; }
    case 0x0F: { u32 a = r[(insn >> 3) & 7] + ((insn >> 6) & 0x1F); r[insn & 7] = vread8(a, user); return; }
    case 0x10: { u32 a = r[(insn >> 3) & 7] + (((insn >> 6) & 0x1F) << 1); vwrite16(a, (u16)r[insn & 7], user); return; }
    case 0x11: { u32 a = r[(insn >> 3) & 7] + (((insn >> 6) & 0x1F) << 1); r[insn & 7] = vread16(a, user); return; }
    case 0x12: { u32 a = r[13] + ((insn & 0xFF) << 2); vwrite32(a, r[(insn >> 8) & 7], user); return; }
    case 0x13: { u32 a = r[13] + ((insn & 0xFF) << 2); r[(insn >> 8) & 7] = load_word_rot(a, user); return; }
    case 0x14: r[(insn >> 8) & 7] = (pc4 & ~3u) + ((insn & 0xFF) << 2); return;
    case 0x15: r[(insn >> 8) & 7] = r[13] + ((insn & 0xFF) << 2); return;
    case 0x16: case 0x17: {
        u32 sub = (insn >> 8) & 0xF;
        if (sub == 0) { // add/sub sp
            u32 imm = (insn & 0x7F) << 2;
            r[13] = bit(insn, 7) ? r[13] - imm : r[13] + imm;
            return;
        }
        if (sub == 0x4 || sub == 0x5) { // PUSH
            u32 list = insn & 0xFF;
            bool lr = bit(insn, 8);
            int n = __builtin_popcount(list) + (lr ? 1 : 0);
            u32 a = r[13] - n * 4;
            u32 addr = a;
            for (int i = 0; i < 8; i++) if (list & (1u << i)) { vwrite32(addr, r[i], user); addr += 4; }
            if (lr) vwrite32(addr, r[14], user);
            r[13] = a;
            return;
        }
        if (sub == 0xC || sub == 0xD) { // POP
            u32 list = insn & 0xFF;
            bool pc = bit(insn, 8);
            u32 addr = r[13];
            u32 vals[8]; u32 pcv = 0;
            for (int i = 0; i < 8; i++) if (list & (1u << i)) { vals[i] = vread32(addr, user); addr += 4; }
            if (pc) { pcv = vread32(addr, user); addr += 4; }
            for (int i = 0; i < 8; i++) if (list & (1u << i)) r[i] = vals[i];
            r[13] = addr;
            if (pc) set_pc_interwork(pcv);
            return;
        }
        if (sub == 0xE) { // BKPT
            cp15_ifsr = 0x2;
            exception(0x0C, MODE_ABT, cur_pc + 4, false);
            return;
        }
        raise_undefined();
        return;
    }
    case 0x18: { // STMIA
        u32 rb = (insn >> 8) & 7, list = insn & 0xFF;
        u32 addr = r[rb];
        int n = __builtin_popcount(list);
        bool first = true;
        for (int i = 0; i < 8; i++) if (list & (1u << i)) {
            u32 v = r[i];
            if (i == (int)rb && !first) v = r[rb] + n * 4;
            vwrite32(addr, v, user); addr += 4; first = false;
        }
        if (list == 0) { vwrite32(addr, pc4 + 2, user); r[rb] += 0x40; return; }
        r[rb] = addr;
        return;
    }
    case 0x19: { // LDMIA
        u32 rb = (insn >> 8) & 7, list = insn & 0xFF;
        u32 addr = r[rb];
        if (list == 0) { u32 v = vread32(addr, user); r[rb] += 0x40; set_pc(v); return; }
        u32 vals[8];
        for (int i = 0; i < 8; i++) if (list & (1u << i)) { vals[i] = vread32(addr, user); addr += 4; }
        r[rb] = addr;
        for (int i = 0; i < 8; i++) if (list & (1u << i)) r[i] = vals[i];
        return;
    }
    case 0x1A: case 0x1B: {
        u32 c = (insn >> 8) & 0xF;
        if (c == 0xF) {
            if (swi_hook) swi_hook(this, cur_pc, insn & 0xFF, trace_user);
            exception(0x08, MODE_SVC, cur_pc + 2, false);
            return;
        }
        if (c == 0xE) { raise_undefined(); return; }
        if (cond(c)) {
            s32 off = (s32)(int8_t)(insn & 0xFF) << 1;
            r[15] = pc4 + off;
            pc_written = true;
        }
        return;
    }
    case 0x1C: {
        s32 off = ((s32)(insn << 21)) >> 20;
        r[15] = pc4 + off;
        pc_written = true;
        return;
    }
    case 0x1D: { // BLX suffix
        if (insn & 1) { raise_undefined(); return; }
        u32 target = (r[14] + ((insn & 0x7FF) << 1)) & ~3u;
        r[14] = (cur_pc + 2) | 1;
        cpsr &= ~FLAG_T;
        r[15] = target;
        pc_written = true;
        return;
    }
    case 0x1E: { // BL prefix
        s32 off = ((s32)(insn << 21)) >> 9;
        r[14] = pc4 + off;
        return;
    }
    case 0x1F: { // BL suffix
        u32 target = r[14] + ((insn & 0x7FF) << 1);
        r[14] = (cur_pc + 2) | 1;
        r[15] = target & ~1u;
        pc_written = true;
        return;
    }
    }
}

} // namespace didj
