// Test shim: exposes ARM926 core with flat RAM to Python (ctypes) for differential testing.
#include "../core/src/arm926.h"
#include <vector>

using namespace didj;

struct FlatBus : Bus {
    std::vector<u8> ram;
    FlatBus() : ram(1 << 20) {}
    u8* page_ptr(u32 pa) override { return pa < ram.size() ? &ram[pa & ~0xFFFu] : nullptr; }
    u32 read32(u32 pa) override { u32 v = 0; if (pa + 4 <= ram.size()) memcpy(&v, &ram[pa], 4); return v; }
    u16 read16(u32 pa) override { u16 v = 0; if (pa + 2 <= ram.size()) memcpy(&v, &ram[pa], 2); return v; }
    u8 read8(u32 pa) override { return pa < ram.size() ? ram[pa] : 0; }
    void write32(u32 pa, u32 v) override { if (pa + 4 <= ram.size()) memcpy(&ram[pa], &v, 4); }
    void write16(u32 pa, u16 v) override { if (pa + 2 <= ram.size()) memcpy(&ram[pa], &v, 2); }
    void write8(u32 pa, u8 v) override { if (pa < ram.size()) ram[pa] = v; }
};

static FlatBus* bus;
static ARM926* cpu;

extern "C" {
void tb_init() { bus = new FlatBus(); cpu = new ARM926(bus); }
void tb_set(const u32* regs, u32 cpsr) {
    cpu->reset();
    cpu->switch_mode(cpsr & 0x1F);
    cpu->cpsr = cpsr;
    for (int i = 0; i < 16; i++) cpu->r[i] = regs[i];
}
void tb_get(u32* regs, u32* cpsr) {
    for (int i = 0; i < 16; i++) regs[i] = cpu->r[i];
    *cpsr = cpu->cpsr;
}
void tb_mem_write(u32 addr, const u8* data, u32 len) { memcpy(&bus->ram[addr], data, len); }
void tb_mem_read(u32 addr, u8* data, u32 len) { memcpy(data, &bus->ram[addr], len); }
void tb_step(int n) { cpu->run(n); }
}
