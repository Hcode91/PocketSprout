// PocketSprout - ARM926 (ARMv5) -> AArch64 dynamic recompiler
#pragma once
#include "arm926.h"
#include <unordered_map>
#include <vector>

namespace didj {

class Jit {
public:
    // ram/ram_size: host RAM backing (for self-modifying-code tracking)
    Jit(ARM926* cpu, u8* ram, u32 ram_size);
    ~Jit();
    static bool available();          // true on AArch64 hosts with executable memory
    bool ok() const { return code_base != nullptr; }

    s64 run(s64 cycles);              // replaces ARM926::run when enabled
    void flush();                     // drop all compiled code
    void note_bus_write(u32 pa);      // RAM write not made by the CPU (e.g. DMA)

    bool stop_on_exception = false;   // testing: return right after an abort taken inside a block

    // statistics
    u64 blocks_compiled = 0, blocks_run = 0, interp_steps = 0, fallbacks = 0, invalidations = 0, flushes = 0;

    struct Block;
    struct Link { u32* slot; u32 target; u32 orig; Block* from; };
    struct Block {
        u32 va, pa, priv;
        s32 (*fn)(ARM926*, s32 budget);   // returns remaining budget
        u32* chain_entry;                  // entry point for direct jumps from other blocks
        u32 ninsn;
        bool dead;
        std::vector<Link> out;             // patchable exits (same-page constant targets)
        std::vector<Link*> in;             // links currently pointing at this block
    };

private:
    ARM926* cpu;
    u8* ram;
    u32 ram_size;
    std::vector<u8> code_flags;       // per RAM page

    u8* code_base = nullptr;
    size_t code_size = 0, code_used = 0;

    struct Key { u32 va, pa, priv; bool operator==(const Key& o) const { return va == o.va && pa == o.pa && priv == o.priv; } };
    struct KeyHash { size_t operator()(const Key& k) const { return ((size_t)k.va * 0x9E3779B1u) ^ ((size_t)k.pa << 1) ^ k.priv; } };
    std::unordered_map<Key, Block*, KeyHash> blocks;
    std::unordered_map<Key, std::vector<Link*>, KeyHash> pending;   // unlinked exits waiting for a block
    void link_block(Block* b);
    void patch(u32* slot, const u32* target);
    void unlink_incoming(Block* b);
    std::unordered_map<u32, std::vector<Block*>> page_blocks;
    std::vector<Block*> all_blocks;
    static const int FAST = 16384;
    Block* fast[FAST];

    Block* lookup(u32 pc, bool priv, u32* pa_out, bool* can_compile);
    Block* compile(u32 pc, u32 pa, bool priv, const u8* host_page);
    void invalidate_page(u32 page);
    static void smc_trampoline(void* user, u32 page);
};

} // namespace didj
