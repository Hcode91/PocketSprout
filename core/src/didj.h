// DidjDroid - LeapFrog Didj (LF1000 / MagicEyes Pollux) system emulation
#pragma once
#include "arm926.h"
#include <string>
#include <vector>
#include <functional>
#include <map>

namespace didj {

// Didj buttons (bit positions in the button mask)
enum Button {
    BTN_LEFT = 0, BTN_RIGHT, BTN_UP, BTN_DOWN, BTN_L, BTN_R, BTN_A, BTN_B,
    BTN_HOME, BTN_HINT, BTN_PAUSE, BTN_BRIGHTNESS, BTN_COUNT
};

// Raw NAND chip (SLC, large page) with spare area.
class NandChip {
public:
    bool load(const std::string& path, std::string* err);
    bool save(const std::string& path) const;   // writes data+spare image
    bool present() const { return !data.empty(); }
    void reset_state();

    void command(u8 cmd);
    void address(u8 a);
    u8 read_byte();
    void write_byte(u8 v);
    bool ready() const { return true; }

    u32 page_size = 2048, spare_size = 64, pages_per_block = 64;
    u32 num_pages = 0;
    u8 id[5] = {0x2C, 0xDA, 0x10, 0x95, 0x44};
    std::vector<u8> data;   // num_pages * (page_size + spare_size)
    bool dirty = false;
    bool write_protect = false;

private:
    u8 cmd = 0xFF;
    u8 addr_cycles[8];
    int naddr = 0;
    u32 column = 0, row = 0;
    enum { S_IDLE, S_READ, S_ID, S_STATUS, S_PROGRAM } state = S_IDLE;
    u32 id_pos = 0;
    u32 page_bytes() const { return page_size + spare_size; }
    void decode_addr();
};

class Didj : public Bus {
public:
    Didj();
    ~Didj();

    bool load_nand(const std::string& path, std::string* err);
    bool load_cart(const std::string& path, std::string* err);
    void eject_cart();
    bool save_nand(const std::string& path) { bool ok = nand_base.save(path); if (ok) nand_base.dirty = false; return ok; }
    void reset();

    // Runs one video frame (~1/60 s). Returns false if CPU stopped.
    void run_frame();

    // 320x240 ARGB8888 output
    static const int SCREEN_W = 320, SCREEN_H = 240;
    const u32* framebuffer() const { return fb; }

    void set_buttons(u32 mask);
    void set_touch(bool down, int x, int y);

    // Audio: interleaved stereo s16 at AUDIO_RATE. Returns frames copied.
    static const int AUDIO_RATE = 32000;
    size_t audio_read(int16_t* out, size_t frames);
    size_t audio_available() const { return audio_buf.size() / 2; }

    // Console output from UART (bootloader / kernel log)
    std::function<void(char)> on_uart;
    std::function<void(const std::string&)> on_log;

    // Bus interface
    u8* page_ptr(u32 pa) override;
    u32 read32(u32 pa) override;
    u16 read16(u32 pa) override;
    u8 read8(u32 pa) override;
    void write32(u32 pa, u32 v) override;
    void write16(u32 pa, u16 v) override;
    void write8(u32 pa, u8 v) override;

    ARM926* cpu;
    u64 frame_count = 0;
    int log_unknown = 200;      // max unknown-register log lines
    int log_user_io = 0;        // log I/O accesses made from user mode (mmap'd registers)

    static const u32 RAM_SIZE = 32 * 1024 * 1024;
    static const u32 CPU_HZ = 393216000;

    NandChip nand_base, nand_cart;

private:
    u8* ram;
    bool shadow = true;          // NAND boot: SDRAM at 0, static at 0x80000000
    u32 fb[SCREEN_W * SCREEN_H];
    u32 buttons = 0;
    u64 cycles = 0;              // emulated time in CPU cycles

    // ---- interrupt controller ----
    u64 ic_mode = 0, ic_mask = ~0ull, ic_pend = 0, ic_level = 0;
    u32 ic_prior = 0;
    void irq_set(int n, bool level);
    void irq_pulse(int n);
    void irq_update();

    // ---- timers ----
    struct Timer {
        u32 count = 0, match = 0, control = 0, clkenb = 0, clkgen = 0;
        double frac = 0;
    } timers[5];
    static const int timer_irq[5];
    double timer_rate(int i);
    void timers_advance(u64 dt);
    u32 timer_read(int i, u32 off);
    void timer_write(int i, u32 off, u32 v);

    // ---- clock/power ----
    u32 clk_mode = 0, pll[2] = {0, 0};
    u32 pwr_regs[0x80 / 4] = {};
    double pll_hz(int n);

    // ---- GPIO ----
    struct GpioPort {
        u32 out = 0, outenb = 0, detmode0 = 0, detmode1 = 0, intenb = 0, det = 0, puenb = 0, alt0 = 0, alt1 = 0;
        u32 in = 0xFFFFFFFF;
        u32 last_pad = 0xFFFFFFFF;
    } gpio[3];
    u32 alive_gpio = 0, alive_scratch = 0, alive_pwrgate = 0;
    u32 gpio_pad(int port);
    void gpio_update_inputs();
    void gpio_eval_irq();
    u32 gpio_read(u32 off);
    void gpio_write(u32 off, u32 v);

    // ---- UART0 ----
    u16 uart_regs[0x20] = {};
    u32 uart_clkenb = 0, uart_clkgen = 0;

    // ---- NAND controller (MCU_S) ----
    u32 mcus_regs[0x100 / 4] = {};
    u32 nf_control = 0;
    NandChip& cur_nand() { return (nf_control & 1) ? nand_cart : nand_base; }

    // ---- SDRAM controller (MCU_Y) ----
    u16 mcuy_regs[0x40] = {};

    // ---- RTC ----
    u32 rtc_regs[8] = {};
    u64 rtc_base_secs = 0;

    // ---- DMA (8 channels with command queue) ----
    struct DmaCmd { u32 src, dst; u16 len, req; u32 mode; };
    struct DmaCh {
        u32 src = 0, dst = 0, mode = 0; u16 len = 0, req = 0;
        u32 wb_src = 0, wb_dst = 0; u16 wb_len = 0, wb_req = 0;
        DmaCmd q[8]; int qn = 0;
        bool active = false; s64 remaining = 0;   // cycles until completion
    } dma[8];
    u32 dma_read(int ch, u32 off, int size);
    void dma_write(int ch, u32 off, u32 v, int size);
    void dma_start(int ch);
    void dma_finish(int ch);
    void dma_advance(u64 dt);
    void dma_update_irq();

    // ---- SPI (3 ports) ----
    struct Spi { u16 cont0 = 0, cont1 = 0, state = 0; u32 clkenb = 0, clkgen = 0; u16 rx[16]; int rxn = 0; } spi[3];
    u32 spi_read(int n, u32 off);
    void spi_write(int n, u32 off, u32 v);

    // ---- ADC ----
    u32 adc_con = 0, adc_dat = 0, adc_intenb = 0;

    std::vector<int16_t> audio_buf;

    // ---- GA3D 3D engine (command buffer at regs 0x04..0x10) ----
    u32 ga3d_regs[0x1000 / 4] = {};

    // ---- display ----
    u32 mlc_regs[0x400 / 4] = {};
    u16 dpc_regs[0x200 / 2] = {};
    u32 palette[2][256] = {};
    void render();
    void vsync();

    // ---- generic register file for unmodelled devices ----
    std::map<u32, u32> misc_regs;
    void log(const std::string& s);
    void unknown(const char* what, u32 pa, u32 v, bool write, int size);

    u32 io_read(u32 pa, int size);
    void io_write(u32 pa, u32 v, int size);
    u32 nand_io_read(u32 off, int size);
    void nand_io_write(u32 off, u32 v, int size);

    void boot_from_nand();
};

} // namespace didj
