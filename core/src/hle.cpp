// DidjDroid - OpenGL ES library HLE: library tracking and call dispatch
#include "hle.h"
#include "gles.h"
#include "didj.h"
#include <cstdio>
#include <chrono>

namespace didj {

static bool hook_tramp(ARM926*, u32 pc, void* u) { return static_cast<Hle*>(u)->on_hook(pc); }
static void ttb_tramp(ARM926*, void* u) { static_cast<Hle*>(u)->on_ttb(); }

Hle::Hle(Didj* s, ARM926* c) : cpu(c), sys(s) {
    gl = new GLES(this);
    cpu->hle_hook = hook_tramp;
    cpu->ttb_hook = ttb_tramp;
    cpu->hle_user = this;
}

Hle::~Hle() { delete gl; }

void Hle::reset() {
    procs.clear();
    cur = nullptr;
    watch_kind = W_NONE;
    cpu->hle_base = cpu->hle_size = 0;
    cpu->hle_watch_pc = 0xFFFFFFFF;
    gl->reset();
}

// ---------------------------------------------------------------------------
// Guest memory (virtual, current address space)
// ---------------------------------------------------------------------------
bool Hle::read_mem(u32 va, void* dst, u32 len) {
    u8* d = (u8*)dst;
    bool ok = true;
    while (len) {
        u32 n = 0x1000 - (va & 0xFFF);
        if (n > len) n = len;
        u32 pa;
        u8* page = nullptr;
        if (cpu->debug_translate(va, &pa)) page = sys->page_ptr(pa & ~0xFFFu);
        if (page) memcpy(d, page + (pa & 0xFFF), n);
        else { memset(d, 0, n); ok = false; }
        d += n; va += n; len -= n;
    }
    return ok;
}

bool Hle::write_mem(u32 va, const void* src, u32 len) {
    const u8* s = (const u8*)src;
    bool ok = true;
    while (len) {
        u32 n = 0x1000 - (va & 0xFFF);
        if (n > len) n = len;
        u32 pa;
        u8* page = nullptr;
        if (cpu->debug_translate(va, &pa)) page = sys->page_ptr(pa & ~0xFFFu);
        if (page) memcpy(page + (pa & 0xFFF), s, n);
        else ok = false;
        s += n; va += n; len -= n;
    }
    return ok;
}

std::string Hle::read_str(u32 va, u32 max) {
    std::string s;
    for (u32 i = 0; i < max; i++) {
        u8 c = 0;
        read_mem(va + i, &c, 1);
        if (!c) break;
        s += (char)c;
    }
    return s;
}

u32 Hle::arg(int i) {
    if (i < 4) return cpu->r[i];
    return rd32(cpu->r[13] + (u32)(i - 4) * 4);
}

u32 Hle::scratch_alloc(u32 len) {
    if (!cur || !cur->data_hi) return 0;
    len = (len + 7) & ~7u;
    if (cur->scratch_next + len > cur->data_hi) return 0;
    u32 a = cur->scratch_next;
    cur->scratch_next += len;
    return a;
}

// ---------------------------------------------------------------------------
// Process / library tracking via system calls
// ---------------------------------------------------------------------------
void Hle::on_swi(u32 pc, u32 imm) {
    if ((cpu->cpsr & 0x1F) != MODE_USR) return;
    u32 nr = imm >= 0x900000 ? imm - 0x900000 : cpu->r[7];
    u32 ttb = cpu->cp15_ttb;
    u32 ret = pc + ((cpu->cpsr & FLAG_T) ? 2 : 4);
    switch (nr) {
    case 5: { // open
        std::string path = read_str(cpu->r[0]);
        if (path.size() >= 19 && path.compare(path.size() - 19, 19, "libopengles_lite.so") == 0) {
            watch_kind = W_OPEN; watch_ttb = ttb; cpu->hle_watch_pc = ret;
        }
        break;
    }
    case 192: { // mmap2
        auto it = procs.find(ttb);
        if (it != procs.end() && it->second.fd >= 0 && cpu->r[4] == (u32)it->second.fd &&
            cpu->r[5] == 0 && (cpu->r[2] & 4) && it->second.base == 0) {
            watch_kind = W_MMAP; watch_ttb = ttb; mmap_len = cpu->r[1]; cpu->hle_watch_pc = ret;
        }
        break;
    }
    case 6: { // close
        auto it = procs.find(ttb);
        if (it != procs.end() && cpu->r[0] == (u32)it->second.fd) it->second.fd = -1;
        break;
    }
    case 11: case 248: { // execve / exit_group: forget this process
        if (procs.erase(ttb)) {
            cur = nullptr; apply_range();
            if (log) { char b[80]; snprintf(b, sizeof b, "[hle] process ttb=%08X ended (syscall %u)", ttb, nr); log(b); }
        }
        break;
    }
    }
}

static int ttb_dbg = 0;
void Hle::on_ttb() {
    if (log_calls && log && ttb_dbg < 30 && !procs.empty()) { ttb_dbg++; char b[80]; snprintf(b, sizeof b, "[hle] ttb -> %08X", cpu->cp15_ttb); log(b); }
    auto it = procs.find(cpu->cp15_ttb);
    cur = (it != procs.end()) ? &it->second : nullptr;
    apply_range();
}

void Hle::apply_range() {
    if (cur && cur->base) { cpu->hle_base = cur->base; cpu->hle_size = cur->size; }
    else { cpu->hle_base = 0; cpu->hle_size = 0; }
}

static long hook_calls = 0;
bool Hle::on_hook(u32 pc) {
    u32 ttb = cpu->cp15_ttb;
    if (log_calls && log && (++hook_calls == 1 || hook_calls == 1000 || hook_calls == 1000000)) {
        char b[120]; snprintf(b, sizeof b, "[hle] hook #%ld pc=%08X cur=%p parsed=%d mode=%X", hook_calls, pc, (void*)cur, cur ? cur->parsed : -1, cpu->cpsr & 0x1F); log(b);
    }
    if (pc == cpu->hle_watch_pc) {
        if (ttb == watch_ttb && (cpu->cpsr & 0x1F) == MODE_USR) {
            u32 r0 = cpu->r[0];
            if (watch_kind == W_OPEN && (s32)r0 >= 0) {
                Proc& p = procs[ttb];
                if (p.base == 0) { p = Proc(); p.fd = (int)r0; }
                cur = &p;
            } else if (watch_kind == W_MMAP && r0 < 0xFFFFF000u) {
                Proc& p = procs[ttb];
                p.base = r0;
                p.size = (mmap_len + 0xFFF) & ~0xFFFu;
                cur = &p;
                apply_range();
                if (log) { char b[96]; snprintf(b, sizeof b, "[hle] libopengles_lite.so mapped at %08X (+%X) ttb=%08X", p.base, p.size, ttb); log(b); }
            }
            watch_kind = W_NONE;
            cpu->hle_watch_pc = 0xFFFFFFFF;
        }
        if ((u32)(pc - cpu->hle_base) >= cpu->hle_size) return false;
    }
    if (!cur || cur->base != cpu->hle_base) on_ttb();
    if (!cur) return false;
    if (!cur->parsed) {
        if (cur->parse_tries > 2000) return false;
        cur->parse_tries++;
        if (!parse_lib(*cur)) {
            if (log && (cur->parse_tries == 1 || cur->parse_tries % 500 == 0)) {
                char b[96]; snprintf(b, sizeof b, "[hle] parse attempt %d failed at pc=%08X", cur->parse_tries, pc); log(b);
            }
            return false;
        }
    }
    auto f = cur->funcs.find(pc);
    if (f == cur->funcs.end()) return false;
    auto t0 = std::chrono::steady_clock::now();
    bool r = gl->call(f->second);
    sys->prof_gl += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

// Parse ELF dynamic symbol table from guest memory; register gl*/egl* entry points.
bool Hle::parse_lib(Proc& p) {
    u32 b = p.base;
    if (rd32(b) != 0x464C457F) return false;
    u32 phoff = rd32(b + 0x1C);
    u32 phnum = rd32(b + 0x2C) & 0xFFFF;
    u32 dyn = 0, data_lo = 0, data_hi = 0;
    for (u32 i = 0; i < phnum; i++) {
        u32 ph = b + phoff + i * 32;
        u32 type = rd32(ph), vaddr = rd32(ph + 8), memsz = rd32(ph + 20), flags = rd32(ph + 24);
        if (type == 2) dyn = vaddr;
        if (type == 1 && (flags & 2)) { data_lo = vaddr; data_hi = vaddr + memsz; }
    }
    if (!dyn) return false;
    u32 symtab = 0, strtab = 0, hash = 0;
    for (u32 d = b + dyn;; d += 8) {
        u32 tag = rd32(d), val = rd32(d + 4);
        if (tag == 0) break;
        if (tag == 4) hash = val;
        else if (tag == 5) strtab = val;
        else if (tag == 6) symtab = val;
        if (d > b + dyn + 0x800) break;
    }
    if (!symtab || !strtab || !hash) return false;
    u32 nchain;
    if (!read_mem(b + hash + 4, &nchain, 4) || nchain == 0 || nchain > 20000) return false;
    std::map<u32, int> funcs;
    for (u32 i = 1; i < nchain; i++) {
        u8 sym[16];
        if (!read_mem(b + symtab + i * 16, sym, 16)) return false;
        u32 name, value; memcpy(&name, sym, 4); memcpy(&value, sym + 4, 4);
        u16 shndx; memcpy(&shndx, sym + 14, 2);
        if (!value || !shndx || (sym[12] & 0xF) != 2) continue; // defined functions only
        char nm[64] = {0};
        if (!read_mem(b + strtab + name, nm, 63)) return false;
        if (strncmp(nm, "gl", 2) && strncmp(nm, "egl", 3)) continue;
        int id = gl->find(nm);
        if (id < 0 && log_calls && log) log(std::string("[hle] unhandled export: ") + nm);
        funcs[b + (value & ~1u)] = id;
    }
    p.funcs = funcs;
    p.parsed = true;
    if (data_hi) {
        // Use the upper part of the library's own data/bss as scratch space for strings etc.
        p.data_lo = b + data_lo; p.data_hi = b + data_hi;
        u32 sz = data_hi - data_lo;
        p.scratch_next = p.data_hi - (sz > 0x4000 ? 0x2000 : sz / 2);
        p.scratch_next &= ~7u;
    }
    if (log) { char m[96]; snprintf(m, sizeof m, "[hle] hooked %u GL/EGL entry points", (unsigned)funcs.size()); log(m); }
    return true;
}

} // namespace didj
