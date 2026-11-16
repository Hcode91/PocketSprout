#!/usr/bin/env python3
"""Differential fuzzer: PocketSprout ARM926 core vs Unicorn (QEMU arm926).
Usage: python3 cpu_fuzz.py path/to/libcputest.so [iterations]
"""
import ctypes, random, sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_THUMB, UcError, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE

class Unaligned(Exception): pass
from unicorn.arm_const import *

lib = ctypes.CDLL(sys.argv[1])
N = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
lib.tb_init()

MEM = 1 << 20
CODE = 0x1000
DATA = 0x40000
UREGS = [UC_ARM_REG_R0 + i for i in range(13)] + [UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC]

def u32(x): return x & 0xFFFFFFFF

def run_unicorn(code, regs, cpsr, mem, thumb):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB if thumb else UC_MODE_ARM)
    try:
        uc.ctl_set_cpu_model(UC_CPU_ARM_926)
    except Exception:
        pass
    uc.mem_map(0, MEM)
    uc.mem_write(DATA, mem)
    uc.mem_write(CODE, code)
    uc.reg_write(UC_ARM_REG_CPSR, cpsr)
    for i, r in enumerate(UREGS[:15]):
        uc.reg_write(r, regs[i])
    bad = []
    def hook(uc_, access, addr, size, value, ud):
        if addr % size: bad.append(addr)
    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, hook, begin=DATA, end=DATA + len(mem))
    start = CODE | (1 if thumb else 0)
    uc.emu_start(start, CODE + len(code), count=1)
    if bad: raise Unaligned()
    out = [uc.reg_read(r) for r in UREGS]
    return out, uc.reg_read(UC_ARM_REG_CPSR), bytes(uc.mem_read(DATA, len(mem)))

def run_mine(code, regs, cpsr, mem, thumb):
    r = (ctypes.c_uint32 * 16)(*(regs[:15] + [CODE]))
    lib.tb_set(r, cpsr | (0x20 if thumb else 0))
    buf = (ctypes.c_uint8 * len(code)).from_buffer_copy(code)
    lib.tb_mem_write(CODE, buf, len(code))
    mb = (ctypes.c_uint8 * len(mem)).from_buffer_copy(mem)
    lib.tb_mem_write(DATA, mb, len(mem))
    lib.tb_step(1)
    out = (ctypes.c_uint32 * 16)()
    c = ctypes.c_uint32()
    lib.tb_get(out, ctypes.byref(c))
    mo = (ctypes.c_uint8 * len(mem))()
    lib.tb_mem_read(DATA, mo, len(mem))
    return list(out), c.value, bytes(mo)

def gen_arm(rng):
    cond = rng.choice([0xE] * 6 + list(range(0xF)))
    kind = rng.randrange(8)
    if kind <= 2:   # data processing
        op = rng.randrange(16); s = rng.randrange(2)
        if 8 <= op <= 11: s = 1
        rn, rd = rng.randrange(15), rng.randrange(15)
        if rng.randrange(2):
            op2 = (1 << 25) | rng.randrange(1 << 12)
        else:
            if rng.randrange(2):
                op2 = rng.randrange(1 << 12) & ~0x10
            else:
                op2 = (rng.randrange(15) << 8) | (rng.randrange(4) << 5) | 0x10 | rng.randrange(15)
        return (cond << 28) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12) | op2
    if kind == 3:   # multiply
        op = rng.choice([0, 1, 4, 5, 6, 7]); s = rng.randrange(2)
        regs = rng.sample(range(15), 4)
        return (cond << 28) | (op << 21) | (s << 20) | (regs[0] << 16) | (regs[1] << 12) | (regs[2] << 8) | 0x90 | regs[3]
    if kind == 4:   # ldr/str
        I = rng.randrange(2); P = rng.randrange(2); U = rng.randrange(2); B = rng.randrange(2)
        W = rng.randrange(2) if P else 0; L = rng.randrange(2)
        rn = rng.randrange(15); rd = rng.randrange(15)
        while rd == rn and (W or not P): rd = rng.randrange(15)
        if I: off = (rng.randrange(4) << 7) | (rng.randrange(4) << 5) | rng.randrange(15)
        else: off = rng.randrange(256)
        while I and (off & 0xF) == rn: off = (off & ~0xF) | rng.randrange(15)
        return (cond << 28) | (1 << 26) | (I << 25) | (P << 24) | (U << 23) | (B << 22) | (W << 21) | (L << 20) | (rn << 16) | (rd << 12) | off, 'mem', rn, (off & 0xF) if I else None
    if kind == 5:   # halfword / signed
        P = rng.randrange(2); U = rng.randrange(2); I = rng.randrange(2); W = rng.randrange(2) if P else 0
        L = rng.randrange(2); sh = rng.choice([1, 2, 3]) if L else 1
        rn = rng.randrange(15); rd = rng.randrange(15)
        while rd == rn: rd = rng.randrange(15)
        rm = rng.randrange(15)
        while rm == rn: rm = rng.randrange(15)
        lo = (rng.randrange(16) << 8) | rng.randrange(16) if I else rm
        return (cond << 28) | (P << 24) | (U << 23) | (I << 22) | (W << 21) | (L << 20) | (rn << 16) | (rd << 12) | (lo & 0xF00) | 0x90 | (sh << 5) | (lo & 0xF), 'mem', rn, None if I else rm
    if kind == 6:   # ldm/stm
        P = rng.randrange(2); U = rng.randrange(2); W = rng.randrange(2); L = rng.randrange(2)
        rn = rng.randrange(15)
        lst = rng.randrange(1, 1 << 15)
        if W: lst &= ~(1 << rn)
        if lst == 0: lst = 1 if rn != 0 else 2
        return (cond << 28) | (4 << 25) | (P << 24) | (U << 23) | (W << 21) | (L << 20) | (rn << 16) | lst, 'mem', rn, None
    # misc: clz, qadd, smla, mrs
    sub = rng.randrange(4)
    a, b, c, d = rng.sample(range(15), 4)
    if sub == 0: return (cond << 28) | 0x016F0F10 | (a << 12) | b
    if sub == 1: return (cond << 28) | 0x01000050 | (rng.randrange(4) << 21) | (a << 16) | (b << 12) | c
    if sub == 2: return (cond << 28) | 0x01000080 | (rng.choice([0, 1, 3]) << 21) | (a << 16) | (b << 12) | (c << 8) | (rng.randrange(4) << 5) | d
    return (cond << 28) | 0x010F0000 | (a << 12)

def gen_thumb(rng):
    k = rng.randrange(10)
    if k == 0: return rng.randrange(0x0000, 0x1800)
    if k == 1: return rng.randrange(0x1800, 0x2000)
    if k == 2: return rng.randrange(0x2000, 0x4000)
    if k == 3: return rng.randrange(0x4000, 0x4400)
    if k == 4:
        while True:
            v = rng.randrange(0x4400, 0x4700)
            rd = (v & 7) | ((v >> 4) & 8); rm = (v >> 3) & 0xF
            if rd != 15 and rm != 15 and rd != 13 and ((v >> 8) & 3) != 3: return v
    if k == 5:  # reg offset
        rb = rng.randrange(8); ro = rng.randrange(8); rd = rng.randrange(8)
        while ro == rb: ro = rng.randrange(8)
        return (0x5000 | (rng.randrange(8) << 9) | (ro << 6) | (rb << 3) | rd), 'mem', [rb, ro]
    if k == 6:  # imm offset
        base = rng.choice([0x6000, 0x7000, 0x8000])
        rb = rng.randrange(8); rd = rng.randrange(8)
        return (base | (rng.randrange(2) << 11) | (rng.randrange(32) << 6) | (rb << 3) | rd), 'mem', [rb]
    if k == 7:  # sp rel / add sp
        return rng.choice([0x9000, 0xA800]) | (rng.randrange(2) << 11 if False else 0) | rng.randrange(0x800), 'mem', [13]
    if k == 8:  # push/pop
        return (0xB400 | (rng.randrange(2) << 11) | rng.randrange(0x100) | 1), 'mem', [13]
    rb = rng.randrange(8)
    lst = rng.randrange(1, 256) & ~(1 << rb)
    if lst == 0: lst = 1 << ((rb + 1) & 7)
    return (0xC000 | (rng.randrange(2) << 11) | (rb << 8) | lst), 'mem', [rb]

def main():
    rng = random.Random(1234)
    fails = 0
    tested = 0
    for it in range(N):
        thumb = rng.randrange(3) == 0
        g = gen_thumb(rng) if thumb else gen_arm(rng)
        memregs = []
        if isinstance(g, tuple):
            insn = g[0]
            if thumb: memregs = g[2]
            else: memregs = [g[2]] + ([g[3]] if g[3] is not None else [])
        else:
            insn = g
        regs = [rng.choice([rng.randrange(1 << 32), rng.randrange(256), 0, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF]) for _ in range(15)]
        for i, mr in enumerate(memregs):
            if i == 0: regs[mr] = DATA + 0x400 + rng.randrange(0, 0x100) * 4
            else: regs[mr] = rng.randrange(0, 0x40) * 4
        regs[13] = DATA + 0x800 if 13 in memregs else regs[13]
        cpsr = 0x1F | (rng.randrange(16) << 28)
        mem = bytes(rng.randrange(256) for _ in range(0x1000))
        code = insn.to_bytes(2 if thumb else 4, 'little')
        try:
            ue = run_unicorn(code, regs, cpsr | (0x20 if thumb else 0), mem, thumb)
        except (UcError, Unaligned):
            continue
        me = run_mine(code, regs, cpsr, mem, thumb)
        tested += 1
        ur, uc, um = ue
        mr, mc, mm = me
        # Q flag & ignore PC when unicorn didn't move (cond fail)
        if ur[:15] != mr[:15] or (uc & 0xF80000FF) != (mc & 0xF80000FF) or um != mm or u32(ur[15]) != u32(mr[15]):
            fails += 1
            if fails <= 25:
                print(f"MISMATCH {'T' if thumb else 'A'} insn={insn:08x} cpsr_in={cpsr:08x}")
                for i in range(16):
                    if ur[i] != mr[i]: print(f"  r{i}: uc={ur[i]:08x} me={mr[i]:08x} (in={regs[i] if i < 15 else CODE:08x})")
                if uc != mc: print(f"  cpsr: uc={uc:08x} me={mc:08x}")
                if um != mm:
                    for j in range(len(um)):
                        if um[j] != mm[j]: print(f"  mem[{DATA+j:x}] uc={um[j]:02x} me={mm[j]:02x}"); break
    print(f"tested={tested} fails={fails}")
    sys.exit(1 if fails else 0)

main()
