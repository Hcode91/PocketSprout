// DidjDroid - LF1000 system emulation
#include "didj.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace didj {

// IRQ numbers
enum {
    IRQ_PDISPLAY = 0, IRQ_DMA = 3, IRQ_TIMER0 = 4, IRQ_SYSCTRL = 5, IRQ_UART0 = 10,
    IRQ_TIMER1 = 11, IRQ_GPIO = 13, IRQ_TIMER2 = 15, IRQ_TIMER3 = 21, IRQ_AUDIO = 24,
    IRQ_ADC = 25, IRQ_GRP3D = 27, IRQ_RTC = 31, IRQ_TIMER4 = 43
};
const int Didj::timer_irq[5] = {IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2, IRQ_TIMER3, IRQ_TIMER4};


// ===========================================================================
// NAND chip
// ===========================================================================
bool NandChip::load(const std::string& path, std::string* err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { if (err) *err = "cannot open " + path; return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    // Decide layout: with spare (2112-byte pages) or data-only (2048)
    bool with_spare = (sz % 2112) == 0 && (sz / 2112) % 64 == 0;
    page_size = 2048; spare_size = 64; pages_per_block = 64;
    if (with_spare) {
        num_pages = sz / 2112;
        data.resize((size_t)sz);
        if (fread(data.data(), 1, sz, f) != (size_t)sz) { fclose(f); if (err) *err = "read error"; return false; }
    } else if (sz % 2048 == 0) {
        num_pages = sz / 2048;
        data.assign((size_t)num_pages * 2112, 0xFF);
        std::vector<u8> pg(2048);
        for (u32 p = 0; p < num_pages; p++) {
            if (fread(pg.data(), 1, 2048, f) != 2048) break;
            memcpy(&data[(size_t)p * 2112], pg.data(), 2048);
        }
    } else {
        fclose(f);
        if (err) *err = "unrecognised NAND image size";
        return false;
    }
    fclose(f);
    // Chip ID by size
    u32 mb = (u32)((u64)num_pages * 2048 >> 20);
    id[0] = 0x2C;
    if (mb >= 256) id[1] = 0xDA;       // 2 Gbit
    else if (mb >= 128) id[1] = 0xF1;  // 1 Gbit
    else id[1] = 0xF0;                 // 512 Mbit (large page)
    reset_state();
    return true;
}

bool NandChip::save(const std::string& path) const {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

void NandChip::reset_state() {
    state = S_IDLE; cmd = 0xFF; naddr = 0; column = row = 0; id_pos = 0;
}

void NandChip::decode_addr() {
    if (naddr >= 1) column = addr_cycles[0];
    if (naddr >= 2) column |= (u32)addr_cycles[1] << 8;
    row = 0;
    for (int i = 2; i < naddr && i < 5; i++) row |= (u32)addr_cycles[i] << (8 * (i - 2));
}

void NandChip::command(u8 c) {
    switch (c) {
    case 0x00: // read setup (also "back to read mode")
        cmd = c; naddr = 0;
        if (state != S_READ) state = S_READ;
        break;
    case 0x30: // read confirm
        decode_addr();
        state = S_READ;
        naddr = 0;
        break;
    case 0x05: cmd = c; naddr = 0; break;     // random data output
    case 0xE0: decode_addr(); naddr = 0; state = S_READ; break;
    case 0x90: cmd = c; naddr = 0; id_pos = 0; state = S_ID; break;
    case 0x70: state = S_STATUS; break;
    case 0xFF: reset_state(); break;
    case 0x80: cmd = c; naddr = 0; state = S_PROGRAM; break;
    case 0x85: cmd = c; naddr = 0; break;
    case 0x10: { // program confirm: data already merged
        state = S_IDLE;
        break;
    }
    case 0x60: cmd = c; naddr = 0; break;     // erase setup (row only)
    case 0xD0: {
        if (cmd == 0x60) {
            u32 r = 0;
            for (int i = 0; i < naddr && i < 3; i++) r |= (u32)addr_cycles[i] << (8 * i);
            u32 blk = r / pages_per_block;
            size_t start = (size_t)blk * pages_per_block * page_bytes();
            size_t len = (size_t)pages_per_block * page_bytes();
            if (!write_protect && start + len <= data.size()) { memset(&data[start], 0xFF, len); dirty = true; }
        }
        state = S_IDLE;
        break;
    }
    default: cmd = c; break;
    }
}

void NandChip::address(u8 a) {
    if (naddr < 8) addr_cycles[naddr++] = a;
    if (cmd == 0x80 || cmd == 0x85) decode_addr();
    if (state == S_ID) id_pos = 0;
}

u8 NandChip::read_byte() {
    switch (state) {
    case S_ID: return id_pos < 5 ? id[id_pos++] : 0;
    case S_STATUS: return write_protect ? 0x40 : 0xC0;
    case S_READ: {
        if (row >= num_pages || column >= page_bytes()) { column++; return 0xFF; }
        u8 v = data[(size_t)row * page_bytes() + column];
        column++;
        return v;
    }
    default: return 0xFF;
    }
}

void NandChip::write_byte(u8 v) {
    if (state != S_PROGRAM || write_protect) return;
    if (row < num_pages && column < page_bytes()) {
        data[(size_t)row * page_bytes() + column] &= v; // NAND programming only clears bits
        dirty = true;
    }
    column++;
}

// ===========================================================================
// System
// ===========================================================================
Didj::Didj() {
    ram = new u8[RAM_SIZE];
    memset(ram, 0, RAM_SIZE);
    memset(fb, 0, sizeof fb);
    cpu = new ARM926(this);
    rtc_base_secs = (u64)time(nullptr);
    reset();
}

Didj::~Didj() {
    delete cpu;
    delete[] ram;
}

void Didj::log(const std::string& s) {
    if (on_log) on_log(s);
    else fprintf(stderr, "%s\n", s.c_str());
}

void Didj::unknown(const char* what, u32 pa, u32 v, bool write, int size) {
    if (log_unknown <= 0) return;
    log_unknown--;
    char b[160];
    if (write) snprintf(b, sizeof b, "[%s] W%d %08X <= %08X  (pc=%08X)", what, size * 8, pa, v, cpu->r[15]);
    else snprintf(b, sizeof b, "[%s] R%d %08X  (pc=%08X)", what, size * 8, pa, cpu->r[15]);
    log(b);
}

bool Didj::load_nand(const std::string& path, std::string* err) {
    if (!nand_base.load(path, err)) return false;
    reset();
    return true;
}

bool Didj::load_cart(const std::string& path, std::string* err) {
    return nand_cart.load(path, err);
}

void Didj::eject_cart() { nand_cart.data.clear(); nand_cart.num_pages = 0; }

void Didj::reset() {
    cpu->reset();
    shadow = true;
    ic_mode = 0; ic_mask = ~0ull; ic_pend = 0; ic_level = 0; ic_prior = 0;
    for (auto& t : timers) t = Timer();
    clk_mode = 0;
    pll[0] = (11u << 18) | (160u << 8);   // ~392.7 MHz
    pll[1] = (3u << 18) | (16u << 8);     // 144 MHz
    memset(pwr_regs, 0, sizeof pwr_regs);
    pwr_regs[0x5C / 4] = 1u << 11;         // power-on reset
    for (auto& g : gpio) g = GpioPort();
    alive_gpio = 0; alive_scratch = 0; alive_pwrgate = 0;
    memset(uart_regs, 0, sizeof uart_regs);
    memset(mcus_regs, 0, sizeof mcus_regs);
    nf_control = (1u << 5) | (1u << 9);    // NAND boot, ready
    memset(mcuy_regs, 0, sizeof mcuy_regs);
    mcuy_regs[0] = 2;                      // SDRCAP: 256 Mbit (32 MB)
    memset(rtc_regs, 0, sizeof rtc_regs);
    adc_con = adc_dat = adc_intenb = 0;
    for (auto& s : spi) s = Spi();
    for (auto& d : dma) d = DmaCh();
    memset(ga3d_regs, 0, sizeof ga3d_regs);
    audio_buf.clear();
    memset(mlc_regs, 0, sizeof mlc_regs);
    memset(dpc_regs, 0, sizeof dpc_regs);
    misc_regs.clear();
    nand_base.reset_state();
    nand_cart.reset_state();
    cycles = 0;
    gpio_update_inputs();
    boot_from_nand();
}

void Didj::boot_from_nand() {
    // LF1000 NAND boot: the controller copies the first page (2 KB) of the
    // on-board NAND into SDRAM at 0 and releases the CPU at address 0.
    if (nand_base.present()) {
        memcpy(ram, nand_base.data.data(), 2048);
    }
    cpu->r[15] = 0;
}

// ---------------------------------------------------------------------------
// Bus
// ---------------------------------------------------------------------------
u8* Didj::page_ptr(u32 pa) {
    if (shadow) {
        if (pa < 0x80000000u) return ram + (pa & (RAM_SIZE - 1) & ~0xFFFu);
    } else {
        if (pa >= 0x80000000u && pa < 0xC0000000u) return ram + (pa & (RAM_SIZE - 1) & ~0xFFFu);
    }
    return nullptr;
}

#define RAMPTR(pa) (ram + ((pa) & (RAM_SIZE - 1)))

u32 Didj::read32(u32 pa) {
    u8* p = page_ptr(pa & ~0xFFFu);
    if (p) { u32 v; memcpy(&v, p + (pa & 0xFFC), 4); return v; }
    return io_read(pa & ~3u, 4);
}
u16 Didj::read16(u32 pa) {
    u8* p = page_ptr(pa & ~0xFFFu);
    if (p) { u16 v; memcpy(&v, p + (pa & 0xFFE), 2); return v; }
    return (u16)io_read(pa & ~1u, 2);
}
u8 Didj::read8(u32 pa) {
    u8* p = page_ptr(pa & ~0xFFFu);
    if (p) return p[pa & 0xFFF];
    return (u8)io_read(pa, 1);
}
void Didj::write32(u32 pa, u32 v) {
    u8* p = page_ptr(pa & ~0xFFFu);
    if (p) { memcpy(p + (pa & 0xFFC), &v, 4); return; }
    io_write(pa & ~3u, v, 4);
}
void Didj::write16(u32 pa, u16 v) {
    u8* p = page_ptr(pa & ~0xFFFu);
    if (p) { memcpy(p + (pa & 0xFFE), &v, 2); return; }
    io_write(pa & ~1u, v, 2);
}
void Didj::write8(u32 pa, u8 v) {
    u8* p = page_ptr(pa & ~0xFFFu);
    if (p) { p[pa & 0xFFF] = v; return; }
    io_write(pa, v, 1);
}

// ---------------------------------------------------------------------------
// Interrupt controller
// ---------------------------------------------------------------------------
void Didj::irq_set(int n, bool level) {
    u64 b = 1ull << n;
    if (level) { ic_level |= b; ic_pend |= b; }
    else ic_level &= ~b;
    irq_update();
}

void Didj::irq_pulse(int n) {
    ic_pend |= 1ull << n;
    irq_update();
}

void Didj::irq_update() {
    u64 active = ic_pend & ~ic_mask;
    cpu->set_irq((active & ~ic_mode) != 0);
    cpu->set_fiq((active & ic_mode) != 0);
}

// ---------------------------------------------------------------------------
// Clocks / timers
// ---------------------------------------------------------------------------
double Didj::pll_hz(int n) {
    u32 v = pll[n & 1];
    u32 p = (v >> 18) & 0x3F, m = (v >> 8) & 0x3FF, s = v & 0xFF;
    if (p == 0) p = 1;
    if (s > 5) s = 5;
    return 27000000.0 * m / (p * (double)(1u << s));
}

double Didj::timer_rate(int i) {
    Timer& t = timers[i];
    u32 src = (t.clkgen >> 1) & 7;
    u32 div = ((t.clkgen >> 4) & 0xFF) + 1;
    double hz = pll_hz(src >= 1 ? 1 : 0) / div;
    u32 sel = t.control & 3;
    if (sel != 3) hz /= (double)(2u << sel);
    return hz;
}

void Didj::timers_advance(u64 dt) {
    for (int i = 0; i < 5; i++) {
        Timer& t = timers[i];
        if (!(t.control & (1u << 3))) continue; // RUN
        t.frac += timer_rate(i) * (double)dt / CPU_HZ;
        if (t.frac < 1.0) continue;
        u64 ticks = (u64)t.frac;
        t.frac -= (double)ticks;
        u64 match = t.match ? (u64)t.match : 0x100000000ull;
        u64 c = (u64)t.count + ticks;
        if (c >= match) {
            t.control |= 1u << 5; // INTPEND
            c = (c - match) % match;
            if (t.control & (1u << 2)) {   // watchdog
                log("watchdog reset");
            }
        }
        t.count = (u32)c;
        bool lvl = (t.control & (1u << 5)) && (t.control & (1u << 4));
        if (lvl) irq_set(timer_irq[i], true);
        else if (ic_level & (1ull << timer_irq[i])) irq_set(timer_irq[i], false);
    }
}

u32 Didj::timer_read(int i, u32 off) {
    Timer& t = timers[i];
    switch (off) {
    case 0x00: return t.count;
    case 0x04: return t.match;
    case 0x08: return t.control & ~(1u << 6);
    case 0x40: return t.clkenb;
    case 0x44: return t.clkgen;
    }
    return 0;
}

void Didj::timer_write(int i, u32 off, u32 v) {
    Timer& t = timers[i];
    switch (off) {
    case 0x00: t.count = v; break;
    case 0x04: t.match = v; break;
    case 0x08: {
        u32 pend = t.control & (1u << 5);
        if (v & (1u << 5)) pend = 0;   // write 1 clears
        t.control = (v & ~((1u << 5) | (1u << 6))) | pend;
        bool lvl = pend && (t.control & (1u << 4));
        if (!lvl) {
            ic_level &= ~(1ull << timer_irq[i]);
        } else irq_set(timer_irq[i], true);
        break;
    }
    case 0x40: t.clkenb = v; break;
    case 0x44: t.clkgen = v; break;
    }
}

// ---------------------------------------------------------------------------
// GPIO
// ---------------------------------------------------------------------------
void Didj::set_buttons(u32 mask) {
    buttons = mask;
    gpio_update_inputs();
}

void Didj::set_touch(bool, int, int) {}

void Didj::gpio_update_inputs() {
    // Port A: cartridge detect on A18 (low = inserted), battery pack A17
    u32 a = 0xFFFFFFFF;
    if (nand_cart.present()) a &= ~(1u << 18);
    gpio[0].in = a;
    // Port B: board id in B27..31 (Didj = 3), cart type in B2..5 (production 0111)
    u32 b = 0xFFFFFFFF;
    b &= ~(0x1Fu << 27);
    b |= 0x03u << 27;
    b &= ~(0xFu << 2);
    b |= 0x7u << 2;
    gpio[1].in = b;
    // Port C: buttons (active low)
    static const int cpin[BTN_COUNT] = {0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 12, 13};
    u32 c = 0xFFFFFFFF;
    for (int i = 0; i < BTN_COUNT; i++) if (buttons & (1u << i)) c &= ~(1u << cpin[i]);
    gpio[2].in = c;
    gpio_eval_irq();
}

u32 Didj::gpio_pad(int port) {
    GpioPort& g = gpio[port];
    return (g.out & g.outenb) | (g.in & ~g.outenb);
}

void Didj::gpio_eval_irq() {
    bool any = false;
    for (int p = 0; p < 3; p++) {
        GpioPort& g = gpio[p];
        u32 pad = gpio_pad(p);
        for (int pin = 0; pin < 32; pin++) {
            u32 m = 1u << pin;
            u32 mode = ((pin < 16 ? g.detmode0 : g.detmode1) >> ((pin & 15) * 2)) & 3;
            bool now = pad & m, before = g.last_pad & m;
            bool hit = false;
            switch (mode) {
            case 0: hit = !now; break;
            case 1: hit = now; break;
            case 2: hit = before && !now; break;
            case 3: hit = !before && now; break;
            }
            if (hit && (g.intenb & m)) g.det |= m;
        }
        g.last_pad = pad;
        if (g.det & g.intenb) any = true;
    }
    irq_set(IRQ_GPIO, any);
}

u32 Didj::gpio_read(u32 off) {
    int p = (off >> 6) & 3;
    if (p > 2) return 0;
    GpioPort& g = gpio[p];
    switch (off & 0x3F) {
    case 0x00: return g.out;
    case 0x04: return g.outenb;
    case 0x08: return g.detmode0;
    case 0x0C: return g.detmode1;
    case 0x10: return g.intenb;
    case 0x14: return g.det;
    case 0x18: return gpio_pad(p);
    case 0x1C: return g.puenb;
    case 0x20: return g.alt0;
    case 0x24: return g.alt1;
    }
    return 0;
}

void Didj::gpio_write(u32 off, u32 v) {
    int p = (off >> 6) & 3;
    if (p > 2) return;
    GpioPort& g = gpio[p];
    switch (off & 0x3F) {
    case 0x00: g.out = v; break;
    case 0x04: g.outenb = v; break;
    case 0x08: g.detmode0 = v; break;
    case 0x0C: g.detmode1 = v; break;
    case 0x10: g.intenb = v; break;
    case 0x14: g.det &= ~v; break;
    case 0x1C: g.puenb = v; break;
    case 0x20: g.alt0 = v; break;
    case 0x24: g.alt1 = v; break;
    }
    gpio_eval_irq();
}

// ---------------------------------------------------------------------------
// DMA
// ---------------------------------------------------------------------------
enum { DM_STOP = 1u << 20, DM_RUN = 1u << 19, DM_INTENB = 1u << 18, DM_INTPEND = 1u << 17, DM_BUSY = 1u << 16,
       DM_DSTNOTINC = 1u << 12, DM_DSTIO = 1u << 10, DM_SRCNOTINC = 1u << 4, DM_SRCIO = 1u << 2 };

u32 Didj::dma_read(int ch, u32 off, int size) {
    DmaCh& d = dma[ch];
    switch (off) {
    case 0x00: return d.src;
    case 0x04: return d.dst;
    case 0x08: return size == 4 ? (d.len | ((u32)d.req << 16)) : d.len;
    case 0x0A: return d.req;
    case 0x0C: return d.mode | (d.active ? (DM_RUN | DM_BUSY) : 0);
    case 0x10: return d.wb_src;
    case 0x14: return d.wb_dst;
    case 0x18: return d.wb_len;
    case 0x1A: return d.wb_req;
    case 0x28: return 0;                 // command buffer not busy
    case 0x2C: return 7 - d.qn;          // free command slots
    }
    return 0;
}

void Didj::dma_write(int ch, u32 off, u32 v, int size) {
    DmaCh& d = dma[ch];
    switch (off) {
    case 0x00: d.src = v; break;
    case 0x04: d.dst = v; break;
    case 0x08: d.len = (u16)v; if (size == 4) d.req = (u16)(v >> 16); break;
    case 0x0A: d.req = (u16)v; break;
    case 0x0C: {
        u32 pend = d.mode & DM_INTPEND;
        if (v & DM_INTPEND) pend = 0;
        d.mode = (v & ~(DM_INTPEND | DM_BUSY | DM_RUN)) | pend;
        if (v & DM_STOP) { d.active = false; d.qn = 0; }
        else if (v & DM_RUN) dma_start(ch);
        break;
    }
    case 0x10: d.wb_src = v; break;
    case 0x14: d.wb_dst = v; break;
    case 0x18: d.wb_len = (u16)v; if (size == 4) d.wb_req = (u16)(v >> 16); break;
    case 0x1A: d.wb_req = (u16)v; break;
    case 0x1C: {
        DmaCmd c{d.wb_src, d.wb_dst, d.wb_len, d.wb_req, v};
        if (!d.active && d.qn == 0) {
            d.src = c.src; d.dst = c.dst; d.len = c.len; d.req = c.req;
            u32 pend = d.mode & DM_INTPEND;
            d.mode = (c.mode & ~(DM_INTPEND | DM_BUSY | DM_RUN)) | pend;
            if (c.mode & DM_RUN) dma_start(ch);
        } else if (d.qn < 8) d.q[d.qn++] = c;
        break;
    }
    case 0x24: if (v & 1) { d.qn = 0; } break;  // command stop: flush queue
    default: break;
    }
    dma_update_irq();
}

void Didj::dma_start(int ch) {
    DmaCh& d = dma[ch];
    u32 bytes = (u32)d.len + 1;
    d.active = true;
    bool dst_io = d.mode & DM_DSTIO, src_io = d.mode & DM_SRCIO;
    // Audio output (destination in the audio block): pace at the PCM rate and capture samples
    if (dst_io && (d.dst & 0xFFFFF000u) == 0xC000D000u) {
        u32 frames = bytes / 4;
        for (u32 i = 0; i < frames && audio_buf.size() < (size_t)AUDIO_RATE * 2; i++) {
            u32 s = read32(d.src + i * 4);
            audio_buf.push_back((int16_t)(s & 0xFFFF));
            audio_buf.push_back((int16_t)(s >> 16));
        }
        d.remaining = (s64)((u64)frames * CPU_HZ / AUDIO_RATE);
        if (d.remaining < 1) d.remaining = 1;
        return;
    }
    // Memory/peripheral copy, done instantly
    u32 s = d.src, t = d.dst;
    for (u32 i = 0; i < bytes; i += 4) {
        u32 v = read32(s);
        write32(t, v);
        if (!(src_io || (d.mode & DM_SRCNOTINC))) s += 4;
        if (!(dst_io || (d.mode & DM_DSTNOTINC))) t += 4;
    }
    d.remaining = bytes / 4 + 16;
}

void Didj::dma_finish(int ch) {
    DmaCh& d = dma[ch];
    d.active = false;
    d.mode |= DM_INTPEND;
    if (d.qn > 0) {
        DmaCmd c = d.q[0];
        memmove(d.q, d.q + 1, sizeof(DmaCmd) * (d.qn - 1));
        d.qn--;
        d.src = c.src; d.dst = c.dst; d.len = c.len; d.req = c.req;
        u32 pend = d.mode & DM_INTPEND;
        d.mode = (c.mode & ~(DM_INTPEND | DM_BUSY | DM_RUN)) | pend;
        dma_start(ch);
    }
}

void Didj::dma_advance(u64 dt) {
    for (int ch = 0; ch < 8; ch++) {
        DmaCh& d = dma[ch];
        if (!d.active) continue;
        d.remaining -= (s64)dt;
        if (d.remaining <= 0) dma_finish(ch);
    }
    dma_update_irq();
}

void Didj::dma_update_irq() {
    bool any = false;
    for (auto& d : dma) if ((d.mode & DM_INTPEND) && (d.mode & DM_INTENB)) any = true;
    irq_set(IRQ_DMA, any);
}

size_t Didj::audio_read(int16_t* out, size_t frames) {
    size_t n = audio_buf.size() / 2;
    if (n > frames) n = frames;
    memcpy(out, audio_buf.data(), n * 4);
    audio_buf.erase(audio_buf.begin(), audio_buf.begin() + n * 2);
    return n;
}

// ---------------------------------------------------------------------------
// SPI (SSP) - instant transfers; LCD controller (ILI9322) answers chip id 0x96
// ---------------------------------------------------------------------------
static const int spi_irq[3] = {12, 39, 40};

u32 Didj::spi_read(int n, u32 off) {
    Spi& s = spi[n];
    switch (off) {
    case 0x00: return s.cont0;
    case 0x02: return s.cont1;
    case 0x04: {
        u16 v = 0;
        if (s.rxn > 0) { v = s.rx[0]; memmove(s.rx, s.rx + 1, (s.rxn - 1) * 2); s.rxn--; }
        return v;
    }
    case 0x06: {
        u16 st = s.state & 0xE070;
        st |= 1u << 2;                     // TX FIFO empty
        if (s.rxn == 0) st |= 1u << 0;     // RX FIFO empty
        if (s.rxn >= 16) st |= 1u << 1;
        return st;
    }
    case 0x40: return s.clkenb;
    case 0x44: return s.clkgen;
    }
    return 0;
}

void Didj::spi_write(int n, u32 off, u32 v) {
    Spi& s = spi[n];
    switch (off) {
    case 0x00:
        s.cont0 = (u16)v;
        if (v & (1u << 10)) s.rxn = 0;     // FIFO clear
        break;
    case 0x02: s.cont1 = (u16)v; break;
    case 0x04: {
        u16 resp = (n == 0) ? 0x0096 : 0;
        if (s.rxn < 16) s.rx[s.rxn++] = resp;
        s.state |= (1u << 6) | (1u << 4) | (1u << 5);   // transfer done, rx ready, tx empty
        break;
    }
    case 0x06: {
        u16 clr = (u16)v & 0x0070;           // write 1 clears status
        s.state = (u16)((s.state & ~clr & 0x0070) | (v & 0xE000));
        break;
    }
    case 0x40: s.clkenb = v; break;
    case 0x44: s.clkgen = v; break;
    }
    bool lvl = ((s.state & (1u << 15)) && (s.state & (1u << 6))) ||
               ((s.state & (1u << 14)) && (s.state & (1u << 5))) ||
               ((s.state & (1u << 13)) && (s.state & (1u << 4)));
    irq_set(spi_irq[n], lvl);
}

// ---------------------------------------------------------------------------
// NAND controller window (static bank 11)
// ---------------------------------------------------------------------------
u32 Didj::nand_io_read(u32 off, int size) {
    NandChip& n = cur_nand();
    if (off < 0x10) {
        u32 v = 0;
        for (int i = 0; i < size; i++) v |= (u32)n.read_byte() << (8 * i);
        return v;
    }
    return 0;
}

void Didj::nand_io_write(u32 off, u32 v, int size) {
    NandChip& n = cur_nand();
    if (off < 0x10) {
        for (int i = 0; i < size; i++) n.write_byte((u8)(v >> (8 * i)));
    } else if (off < 0x18) {
        n.command((u8)v);
        nf_control |= 1u << 15; // IRQPEND (ready)
    } else {
        n.address((u8)v);
    }
}

// ---------------------------------------------------------------------------
// I/O dispatch
// ---------------------------------------------------------------------------
static int ga3d_log = getenv("GA3D_LOG") ? 4000 : 0;
u32 Didj::io_read(u32 pa, int size) {
    if (log_user_io > 0 && (cpu->cpsr & 0x1F) == MODE_USR) {
        log_user_io--;
        char b[96]; snprintf(b, sizeof b, "[uio] R%d %08X pc=%08X", size * 8, pa, cpu->r[15]); log(b);
    }
    // NAND data/cmd/addr window
    u32 nand_win = shadow ? 0xAC000000u : 0x2C000000u;
    if (pa >= nand_win && pa < nand_win + 0x20) return nand_io_read(pa - nand_win, size);

    if (pa >= 0xC0000000u && pa < 0xC0020000u) {
        u32 off = pa - 0xC0000000u;
        if (off < 0x400) return dma_read(off >> 7, off & 0x7F, size);
        // Interrupt controller
        if (off >= 0x800 && off < 0x900) {
            switch (off - 0x800) {
            case 0x08: return (u32)ic_mode;
            case 0x0C: return (u32)(ic_mode >> 32);
            case 0x10: return (u32)ic_mask;
            case 0x14: return (u32)(ic_mask >> 32);
            case 0x18: return ic_prior;
            case 0x20: return (u32)(ic_pend & ~ic_mask);
            case 0x24: return (u32)((ic_pend & ~ic_mask) >> 32);
            }
            return 0;
        }
        if (off >= 0x1800 && off < 0x1A80) {
            int i = (off - 0x1800) >> 7;
            return timer_read(i, off & 0x7F);
        }
        if (off >= 0x3000 && off < 0x3200) { // DPC
            u32 r = (off - 0x3000) >> 1;
            u32 v = dpc_regs[r];
            if (size == 4 && r + 1 < 0x100) v |= (u32)dpc_regs[r + 1] << 16;
            return v;
        }
        if (off >= 0x4000 && off < 0x4400) return mlc_regs[(off - 0x4000) >> 2];
        if (off >= 0x5000 && off < 0x5100) { // ADC
            switch (off - 0x5000) {
            case 0x00: return adc_con;
            case 0x04: return adc_dat;
            case 0x08: return adc_intenb;
            }
            return 0;
        }
        if (off >= 0xA000 && off < 0xA100) return gpio_read(off - 0xA000);
        if (off >= 0x7800 && off < 0x8900 && ((off & 0x7FF) < 0x48)) {
            int n = (off - 0x7800) >> 11;
            return spi_read(n, off & 0x7FF);
        }
        if (off >= 0xF000 && off < 0xF080) { // clock & power
            u32 r = off - 0xF000;
            if (r == 0x00) return clk_mode;
            if (r == 0x04) return pll[0];
            if (r == 0x08) return pll[1];
            return pwr_regs[r >> 2];
        }
        if (off >= 0xF080 && off < 0xF0A0) { // RTC
            u32 r = (off - 0xF080) >> 2;
            if (r == 1) {
                u64 secs = cycles / CPU_HZ;
                return rtc_regs[0] + (u32)secs;
            }
            return rtc_regs[r];
        }
        if (off >= 0x14800 && off < 0x14880) return mcuy_regs[(off - 0x14800) >> 1];
        if (off >= 0x15800 && off < 0x15900) {
            u32 r = off - 0x15800;
            if (r == 0x74) return nf_control | (1u << 9);
            if (r == 0x8C) return 0x3;     // ECC enc/dec done, no error
            if (r >= 0x78 && r <= 0x94) return 0;
            return mcus_regs[r >> 2];
        }
        if (off >= 0x16000 && off < 0x16080) { // UART0
            u32 r = off - 0x16000;
            switch (r) {
            case 0x08: return 0x6;          // TX buffer + transmitter empty
            case 0x0C: return 0;            // FIFO counts
            case 0x12: return 0;
            case 0x40: return uart_clkenb;
            case 0x44: return uart_clkgen;
            }
            if (r < 0x40) return uart_regs[r >> 1];
            return 0;
        }
        if (off >= 0x1A000 && off < 0x1B000) { // GA3D (3D engine)
            u32 r = (off - 0x1A000) >> 2;
            if (r == 0x14 / 4) return 0x00BF0000;  // idle: all units/FIFOs empty
            if (r == 0x10 / 4) return ga3d_regs[0x0C / 4]; // read pointer caught up with write pointer
            return ga3d_regs[r];
        }
        if (off >= 0x19000 && off < 0x19100) { // ALIVE
            switch (off - 0x19000) {
            case 0x00: return alive_pwrgate;
            case 0x0C: return alive_gpio;
            case 0x18: return alive_scratch;
            }
            auto it = misc_regs.find(pa);
            return it == misc_regs.end() ? 0 : it->second;
        }
    }
    auto it = misc_regs.find(pa);
    u32 v = it == misc_regs.end() ? 0 : it->second;
    unknown("io", pa, v, false, size);
    return v;
}

void Didj::io_write(u32 pa, u32 v, int size) {
    if (ga3d_log > 0 && ((pa >= 0xC001A000u && pa < 0xC001B000u) || (pa >= 0xE0000000u && pa < 0xE0010000u))) {
        ga3d_log--;
        char b[96]; snprintf(b, sizeof b, "[ga3d] W%d %08X <= %08X pc=%08X", size * 8, pa, v, cpu->r[15]); log(b);
    }
    if (log_user_io > 0 && (cpu->cpsr & 0x1F) == MODE_USR) {
        log_user_io--;
        char b[96]; snprintf(b, sizeof b, "[uio] W%d %08X <= %08X pc=%08X", size * 8, pa, v, cpu->r[15]); log(b);
    }
    u32 nand_win = shadow ? 0xAC000000u : 0x2C000000u;
    if (pa >= nand_win && pa < nand_win + 0x20) { nand_io_write(pa - nand_win, v, size); return; }

    if (pa >= 0xC0000000u && pa < 0xC0020000u) {
        u32 off = pa - 0xC0000000u;
        if (off < 0x400) { dma_write(off >> 7, off & 0x7F, v, size); return; }
        if (off >= 0x800 && off < 0x900) {
            switch (off - 0x800) {
            case 0x08: ic_mode = (ic_mode & ~0xFFFFFFFFull) | v; break;
            case 0x0C: ic_mode = (ic_mode & 0xFFFFFFFFull) | ((u64)v << 32); break;
            case 0x10: ic_mask = (ic_mask & ~0xFFFFFFFFull) | v; break;
            case 0x14: ic_mask = (ic_mask & 0xFFFFFFFFull) | ((u64)v << 32); break;
            case 0x18: ic_prior = v; break;
            case 0x20: ic_pend &= ~(u64)v; ic_pend |= ic_level; break;
            case 0x24: ic_pend &= ~((u64)v << 32); ic_pend |= ic_level; break;
            }
            irq_update();
            return;
        }
        if (off >= 0x1800 && off < 0x1A80) {
            int i = (off - 0x1800) >> 7;
            timer_write(i, off & 0x7F, v);
            irq_update();
            return;
        }
        if (off >= 0x3000 && off < 0x3200) {
            u32 r = (off - 0x3000) >> 1;
            for (int k = 0; k < (size == 4 ? 2 : 1); k++, r++) {
                u16 hv = (u16)(v >> (16 * k));
                if (r == (0x8C >> 1)) { // DPCCTRL0: INTPEND write-1-clear
                    u16 pend = dpc_regs[r] & (1u << 10);
                    if (hv & (1u << 10)) pend = 0;
                    dpc_regs[r] = (hv & ~(1u << 10)) | pend;
                    if (!pend) irq_set(IRQ_PDISPLAY, false);
                } else if (r < 0x100) dpc_regs[r] = hv;
            }
            return;
        }
        if (off >= 0x4000 && off < 0x4400) {
            u32 r = (off - 0x4000) >> 2;
            mlc_regs[r] = v;
            // palette writes: PALETTEADDR(31:24) PALETTEDATA(15:0)
            if (r == (0x3C >> 2) || r == (0x70 >> 2)) {
                int l = (r == (0x3C >> 2)) ? 0 : 1;
                u32 c = v & 0xFFFF;
                u32 rr = ((c >> 11) & 0x1F) * 255 / 31, gg = ((c >> 5) & 0x3F) * 255 / 63, bb = (c & 0x1F) * 255 / 31;
                palette[l][(v >> 24) & 0xFF] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
            }
            return;
        }
        if (off >= 0x5000 && off < 0x5100) {
            switch (off - 0x5000) {
            case 0x00: {
                adc_con = v & ~1u;
                if (v & 1) { // start conversion
                    u32 ch = (v >> 3) & 7;
                    switch (ch) {
                    case 2: adc_dat = 760; break;  // battery ~6 V
                    case 0: adc_dat = 400; break;
                    case 1: adc_dat = 512; break;  // volume
                    case 3: adc_dat = 512; break;  // battery temperature
                    default: adc_dat = 0; break;
                    }
                    if (adc_intenb & 1) irq_pulse(IRQ_ADC);
                }
                break;
            }
            case 0x08: adc_intenb = v; break;
            case 0x0C: break;
            }
            return;
        }
        if (off >= 0xA000 && off < 0xA100) { gpio_write(off - 0xA000, v); return; }
        if (off >= 0x7800 && off < 0x8900 && ((off & 0x7FF) < 0x48)) {
            int n = (off - 0x7800) >> 11;
            spi_write(n, off & 0x7FF, v);
            return;
        }
        if (off >= 0xF000 && off < 0xF080) {
            u32 r = off - 0xF000;
            if (r == 0x00) { clk_mode = v; return; }
            if (r == 0x04) { pll[0] = v; return; }
            if (r == 0x08) { pll[1] = v; return; }
            if (r == 0x7C) {
                if (v & (1u << 12)) { log("software reset requested"); reset(); return; }
                v &= ~(1u << 15); // CHGPLL completes immediately
            }
            pwr_regs[r >> 2] = v;
            return;
        }
        if (off >= 0xF080 && off < 0xF0A0) {
            u32 r = (off - 0xF080) >> 2;
            if (r == 0) { rtc_regs[0] = v - (u32)(cycles / CPU_HZ); return; }
            rtc_regs[r] = v;
            return;
        }
        if (off >= 0x14800 && off < 0x14880) { mcuy_regs[(off - 0x14800) >> 1] = (u16)v; return; }
        if (off >= 0x15800 && off < 0x15900) {
            u32 r = off - 0x15800;
            if (r == 0x74) {
                u32 pend = nf_control & (1u << 15);
                if (v & (1u << 15)) pend = 0;
                nf_control = (v & ~((1u << 15) | (1u << 9))) | pend;
                shadow = (nf_control >> 5) & 1;
                return;
            }
            mcus_regs[r >> 2] = v;
            return;
        }
        if (off >= 0x16000 && off < 0x16080) {
            u32 r = off - 0x16000;
            if (r == 0x10) {
                char c = (char)(v & 0xFF);
                if (on_uart) on_uart(c); else fputc(c, stdout);
                return;
            }
            if (r == 0x40) { uart_clkenb = v; return; }
            if (r == 0x44) { uart_clkgen = v; return; }
            if (r < 0x40) uart_regs[r >> 1] = (u16)v;
            return;
        }
        if (off >= 0x1A000 && off < 0x1B000) {
            ga3d_regs[(off - 0x1A000) >> 2] = v;
            return;
        }
        if (off >= 0x19000 && off < 0x19100) {
            switch (off - 0x19000) {
            case 0x00: alive_pwrgate = v; return;
            case 0x04: misc_regs[pa] = v; alive_gpio &= ~v; return;
            case 0x08: misc_regs[pa] = v; alive_gpio |= v; return;
            case 0x10: misc_regs[pa] = v; alive_scratch &= ~v; return;
            case 0x14: misc_regs[pa] = v; alive_scratch |= v; return;
            }
            misc_regs[pa] = v;
            return;
        }
    }
    misc_regs[pa] = v;
    unknown("io", pa, v, true, size);
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
static inline u32 conv_pixel(const u8* p, u32 fmt, u32 bpp, const u32* pal, u32* alpha) {
    bool bgr = fmt & 0x8000;
    u32 r = 0, g = 0, b = 0, a = 255;
    u32 f = fmt & 0x7FFF;
    if (bpp == 1) {
        return pal[p[0]];
    } else if (bpp == 2) {
        u16 v = (u16)(p[0] | (p[1] << 8));
        switch (f) {
        case 0x4432: r = (v >> 11) & 0x1F; g = (v >> 5) & 0x3F; b = v & 0x1F; r = r * 255 / 31; g = g * 255 / 63; b = b * 255 / 31; break;
        case 0x4342: case 0x3342:
            r = ((v >> 10) & 0x1F) * 255 / 31; g = ((v >> 5) & 0x1F) * 255 / 31; b = (v & 0x1F) * 255 / 31;
            if (f == 0x3342) a = (v & 0x8000) ? 255 : 0;
            break;
        case 0x4211: case 0x2211:
            r = ((v >> 8) & 0xF) * 17; g = ((v >> 4) & 0xF) * 17; b = (v & 0xF) * 17;
            if (f == 0x2211) a = ((v >> 12) & 0xF) * 17;
            break;
        case 0x4120: case 0x1120:
            r = ((v >> 5) & 7) * 255 / 7; g = ((v >> 2) & 7) * 255 / 7; b = (v & 3) * 85;
            if (f == 0x1120) a = v >> 8;
            break;
        case 0x4003: case 0x1003: r = g = b = v & 0xFF; if (f == 0x1003) a = v >> 8; break;
        default:
            r = (v >> 11) & 0x1F; g = (v >> 5) & 0x3F; b = v & 0x1F; r = r * 255 / 31; g = g * 255 / 63; b = b * 255 / 31; break;
        }
    } else if (bpp == 3) {
        b = p[0]; g = p[1]; r = p[2];
    } else {
        b = p[0]; g = p[1]; r = p[2];
        if (f == 0x0653) a = p[3];
    }
    if (bgr) { u32 t = r; r = b; b = t; }
    if (alpha) *alpha = a;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

void Didj::render() {
    u32 ctrlt = mlc_regs[0];
    u32 bg = mlc_regs[0x08 >> 2] & 0xFFFFFF;
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) fb[i] = 0xFF000000u | bg;
    if (!(ctrlt & (1u << 1))) {
        // MLC disabled: show black
        return;
    }
    // Draw layer 1 then layer 0 on top (layer 0 is the topmost RGB layer)
    for (int l = 1; l >= 0; l--) {
        u32 base = (0x0C + l * 0x34) >> 2;
        u32 lr = mlc_regs[base + 0], tb = mlc_regs[base + 1];
        u32 ctrl = mlc_regs[base + 6];
        if (!(ctrl & (1u << 5))) continue; // LAYERENB
        int left = (lr >> 16) & 0xFFF, right = lr & 0xFFF;
        int top = (tb >> 16) & 0xFFF, bottom = tb & 0xFFF;
        if (left & 0x800) left -= 0x1000;
        if (top & 0x800) top -= 0x1000;
        s32 hstride = (s32)mlc_regs[base + 7];
        s32 vstride = (s32)mlc_regs[base + 8];
        u32 tp = mlc_regs[base + 9];
        u32 addr = mlc_regs[base + 11];
        u32 fmt = ctrl >> 16;
        u32 bpp = hstride > 0 && hstride <= 4 ? (u32)hstride : 2;
        bool tpen = ctrl & 1, blend = ctrl & 4;
        u32 lalpha = ((tp >> 28) & 0xF) * 17;
        for (int y = top; y <= bottom && y < SCREEN_H; y++) {
            if (y < 0) continue;
            u32 rowaddr = addr + (u32)((y - top) * vstride);
            for (int x = left; x <= right && x < SCREEN_W; x++) {
                if (x < 0) continue;
                u32 pa = rowaddr + (u32)((x - left) * hstride);
                u8* pg = page_ptr(pa & ~0xFFFu);
                if (!pg) { pg = page_ptr((pa & ~0xFFFu) ^ 0x80000000u); if (!pg) continue; }
                u8 tmp[4];
                const u8* px = pg + (pa & 0xFFF);
                if ((pa & 0xFFF) + bpp > 0x1000) { for (u32 k = 0; k < bpp; k++) tmp[k] = read8(pa + k); px = tmp; }
                u32 a = 255;
                u32 c = conv_pixel(px, fmt, bpp, palette[l], &a);
                if (tpen && ((c & 0xFFFFFF) == (tp & 0xFFFFFF))) continue;
                if (blend) a = (fmt == 0x0653 || fmt == 0x3342 || fmt == 0x2211 || fmt == 0x1120 || fmt == 0x1003) ? a : lalpha;
                u32& d = fb[y * SCREEN_W + x];
                if (a >= 255) d = c;
                else if (a > 0) {
                    u32 dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
                    u32 sr = (c >> 16) & 0xFF, sg = (c >> 8) & 0xFF, sb = c & 0xFF;
                    dr = (sr * a + dr * (255 - a)) / 255; dg = (sg * a + dg * (255 - a)) / 255; db = (sb * a + db * (255 - a)) / 255;
                    d = 0xFF000000u | (dr << 16) | (dg << 8) | db;
                }
            }
        }
    }
}

void Didj::vsync() {
    u16& ctrl0 = dpc_regs[0x8C >> 1];
    ctrl0 |= 1u << 10;
    if (ctrl0 & (1u << 11)) irq_set(IRQ_PDISPLAY, true);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void Didj::run_frame() {
    const u64 frame = CPU_HZ / 60;
    const u64 slice = 16384;
    u64 done = 0;
    while (done < frame) {
        u64 n = (frame - done < slice) ? frame - done : slice;
        // ARM926 averages ~1.5 cycles per instruction (cache/memory stalls)
        cpu->run((s64)(n * 2 / 3));
        timers_advance(n);
        dma_advance(n);
        cycles += n;
        done += n;
    }
    render();
    vsync();
    frame_count++;
}

} // namespace didj
