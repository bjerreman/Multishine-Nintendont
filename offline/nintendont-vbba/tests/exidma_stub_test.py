#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""patch 0023: the guest-side EXIDMA stub (kernel/asm/EXIDMA.S) run as REAL machine code in a small PowerPC interpreter.

What it proves, with the stub assembled from the pinned Nintendont source (old) and from the same file with patch 0023's asm hunk (new):
  1. old: a DMA with callback 0 (the BBA driver's synchronous transfers) erases the callback of a card transfer whose transfer-complete
     interrupt has not been taken yet -> the card library never gets its completion (the guest then sleeps forever in the OS idle loop);
     new: the outstanding callback survives, a DMA with a callback still stores it.
  2. old: the read-modify-write of the OS interrupt mask word 0x800000C4 is not atomic (an interrupt between the lwz and the stw has its mask
     change overwritten); new: interrupts are off around it and the caller's MSR[EE] is restored exactly.
  3. the operands Patch.c patches in (table address, channel stride) are still at +2 / +6 / +0x12, the EXI command words are the same.
Needs the devkitPPC assembler; prints SKIP and exits 0 when it is absent."""
import os, re, struct, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, "..", "..", ".."))
tc = os.path.join(os.environ.get("DKP_R35", os.path.join(root, "toolchains", "dkp-r35")), "devkitPPC", "bin")
if not os.path.exists(os.path.join(tc, "powerpc-eabi-as")):
    tc = "/opt/devkitpro/devkitPPC/bin"
AS, STRIP = os.path.join(tc, "powerpc-eabi-as"), os.path.join(tc, "powerpc-eabi-strip")
if not os.path.exists(AS):
    print("exidma stub test: SKIP (no devkitPPC assembler)"); sys.exit(0)
pin = os.environ.get("NIN_SRC", os.path.join(root, "third_party", "Nintendont"))
PIN = "bbc0a208e81d6ab0a828a431dc0b2ba6fc06aec5"
def sh(*a, **k): return subprocess.run(a, check=True, capture_output=True, **k).stdout
try:
    if sh("git", "-C", pin, "rev-parse", "HEAD").decode().strip() != PIN: raise Exception()
except Exception:
    print("exidma stub test: SKIP (pinned Nintendont clone missing)"); sys.exit(0)

tmp = tempfile.mkdtemp(prefix="exidma_")
def archive(path):
    return subprocess.run(["git", "-C", pin, "show", "%s:%s" % (PIN, path)], check=True, capture_output=True).stdout
S_old = archive("kernel/asm/EXIDMA.S"); consts = archive("kernel/asm/constants.inc")
# the asm hunk(s) of patch 0023 for kernel/asm/EXIDMA.S, applied with git apply to a throw-away tree
os.makedirs(os.path.join(tmp, "kernel", "asm"))
open(os.path.join(tmp, "kernel", "asm", "EXIDMA.S"), "wb").write(S_old)
open(os.path.join(tmp, "kernel", "asm", "constants.inc"), "wb").write(consts)
patch = open(os.path.join(here, "..", "patches", "0023-multishine-exidma-keep-cb.patch"), "rb").read().decode("latin1")
blocks = re.split(r"(?m)^(?=diff --git )", patch)
asm = [b for b in blocks if b.startswith("diff --git a/kernel/asm/EXIDMA.S")]
assert len(asm) == 1, "patch has exactly one EXIDMA.S block"
subprocess.run(["git", "init", "-q", tmp], check=True)
subprocess.run(["git", "apply", "--whitespace=nowarn", "-"], input=asm[0].encode("latin1"), cwd=tmp, check=True)
S_new = open(os.path.join(tmp, "kernel", "asm", "EXIDMA.S"), "rb").read()
assert S_new != S_old
def build(src, name):
    d = os.path.join(tmp, name); os.makedirs(d)
    open(os.path.join(d, "EXIDMA.S"), "wb").write(src); open(os.path.join(d, "constants.inc"), "wb").write(consts)
    subprocess.run([AS, "EXIDMA.S", "-mregnames", "-o", "x.elf"], cwd=d, check=True)
    subprocess.run([STRIP, "x.elf", "-O", "binary", "-S", "-s", "-o", "x.bin"], cwd=d, check=True)
    return bytearray(open(os.path.join(d, "x.bin"), "rb").read())
OLD, NEW = build(S_old, "old"), build(S_new, "new")

def patch_operands(code, tab=0x803F0CB4, chanlen=0x40):
    """what Patch.c does after the memcpy: W16(+2) = lis immediate, W16(+6) = addi immediate, W16(+0x12) = channel stride."""
    hi, lo = (tab + 0x8000) >> 16 & 0xFFFF, tab & 0xFFFF
    code = bytearray(code)
    code[2:4] = struct.pack(">H", hi); code[6:8] = struct.pack(">H", lo); code[0x12:0x14] = struct.pack(">H", chanlen)
    return code

M32 = 0xFFFFFFFF
def s16(v): return v - 0x10000 if v & 0x8000 else v
class Cpu:
    def __init__(self, code, mem, msr=0x8000):
        self.code = code; self.mem = mem; self.r = [0] * 32; self.cr = 0; self.pc = 0; self.msr = msr; self.lr = 0xDEAD0000
        self.msr_trace = []; self.irq = None; self.cmd_done = False; self.cmds = []
    def rd(self, a):
        a &= M32
        if a == 0xD3026800:       # EXI_CMD_0: the ARM answered (clears the word) as soon as the PowerPC has written the command
            return 0 if self.cmd_done else self.mem.get(a, 0)
        return self.mem.get(a, 0)
    def wr(self, a, v):
        a &= M32; v &= M32
        self.mem[a] = v
        if a == 0xD3026800: self.cmds.append((v, self.mem.get(0xD3026804, 0))); self.cmd_done = True
    def setcr(self, a, b):
        a = a - (1 << 32) if a & 0x80000000 else a; b = b - (1 << 32) if b & 0x80000000 else b
        self.cr = (0x8 if a < b else 0x4 if a > b else 0x2)
    def run(self, irq_at=None, irq_fn=None):
        n = 0
        while True:
            if irq_at is not None and n == irq_at and (self.msr & 0x8000):
                irq_fn(self); irq_at = None                       # delivered only while MSR[EE] is set
            elif irq_at is not None and n == irq_at: irq_at = n + 1   # held off: delivered at the first instruction with EE set again
            n += 1
            ins, = struct.unpack(">I", bytes(self.code[self.pc:self.pc + 4])); self.pc += 4
            op = ins >> 26; rd = (ins >> 21) & 31; ra = (ins >> 16) & 31; rb = (ins >> 11) & 31
            imm = ins & 0xFFFF
            if op == 15: self.r[rd] = ((0 if ra == 0 else self.r[ra]) + (imm << 16)) & M32
            elif op == 14: self.r[rd] = ((0 if ra == 0 else self.r[ra]) + s16(imm)) & M32
            elif op == 11: self.setcr(self.r[ra], s16(imm) & M32)
            elif op == 16:
                bo, bi, bd = rd, ra, s16(ins & 0xFFFC)
                bit = {0: 0x8, 1: 0x4, 2: 0x2}[bi]; taken = bool(self.cr & bit) if (bo & 8) else not (self.cr & bit)
                if taken: self.pc = self.pc - 4 + bd
            elif op == 36: self.wr(self.r[ra] + s16(imm), self.r[rd])
            elif op == 32: self.r[rd] = self.rd(self.r[ra] + s16(imm))
            elif op == 21:
                sh, mb, me = rb, (ins >> 6) & 31, (ins >> 1) & 31; rs = rd; v = self.r[rs]
                rot = ((v << sh) | (v >> (32 - sh))) & M32 if sh else v
                mask = (M32 >> mb) & ~(M32 >> me >> 1) if mb <= me else ((M32 >> mb) | ~(M32 >> me >> 1)) & M32
                self.r[ra] = rot & mask
            elif op == 31:
                xo = (ins >> 1) & 0x3FF
                if xo == 444: self.r[ra] = self.r[rd] | self.r[rb]                      # or ra,rs,rb
                elif xo == 60: self.r[ra] = self.r[rd] & ~self.r[rb] & M32            # andc ra,rs,rb
                elif xo == 83: self.r[rd] = self.msr                                    # mfmsr
                elif xo == 146: self.msr = self.r[rd]; self.msr_trace.append(self.msr)  # mtmsr
                elif xo == 0: self.setcr(self.r[ra], self.r[rb])                        # cmpw
                else: raise Exception("opcode 31/%d at %x" % (xo, self.pc - 4))
            elif op == 19 and ((ins >> 1) & 0x3FF) == 16: return n                    # blr
            else: raise Exception("opcode %d at %x (%08x)" % (op, self.pc - 4, ins))
            if n > 400: raise Exception("runaway")

TAB, MASK, CB_CARD = 0x803F0CB4, 0x800000C4, 0x80123456
def call(code, chan, cb, table_cb, mask=0xFFFFFFFF, msr=0x8000, irq=None):
    mem = {TAB + 4: table_cb, MASK: mask}
    cpu = Cpu(patch_operands(code), mem, msr)
    cpu.r[3], cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7] = chan, 0x80700000, 0x200, 0, cb
    if irq: cpu.run(irq_at=irq[0], irq_fn=irq[1])
    else: cpu.run()
    return cpu, mem
fails = 0
def chk(c, m):
    global fails
    if not c:
        fails += 1; print("FAIL", m)
    else: print("ok  ", m)

# --- 1. the callback of an outstanding card transfer
# the card transfer's completion interrupt is pending; the net slice's DMA (callback 0) comes in before the interrupt is taken
cpu, mem = call(OLD, 0, 0, CB_CARD)
chk(mem[TAB + 4] == 0, "OLD stub: a DMA with callback 0 erases the pending card callback (the bug: the TC handler finds nothing to call)")
cpu, mem = call(NEW, 0, 0, CB_CARD)
chk(mem[TAB + 4] == CB_CARD, "NEW stub: the pending card callback survives a DMA with callback 0")
cpu, mem = call(NEW, 0, 0xCAFE0000, 0)
chk(mem[TAB + 4] == 0xCAFE0000, "NEW stub: a DMA with a callback stores it (card reads work as before)")
cpu, mem = call(NEW, 0, 0xCAFE0000, CB_CARD)
chk(mem[TAB + 4] == 0xCAFE0000, "NEW stub: a DMA with a callback still replaces the stored one (same as before)")
cpu, mem = call(NEW, 1, 0, CB_CARD)
chk(mem.get(TAB + 0x40 + 4, 0) == 0 and mem[TAB + 4] == CB_CARD, "NEW stub: channel 1 uses the stride 0x40 and touches only its own entry")
cpu, mem = call(NEW, 1, 0xBEEF0000, 0)
chk(mem[TAB + 0x40 + 4] == 0xBEEF0000 and mem[TAB + 4] == 0, "NEW stub: channel 1 callback stored at table + 0x40 + 4")
# a whole sequence: card DMA (cb C) -> BBA DMA (cb 0) -> TC handler reads the callback
for name, code, want in (("OLD", OLD, False), ("NEW", NEW, True)):
    mem = {TAB + 4: 0, MASK: 0xFFFFFFFF}
    c1 = Cpu(patch_operands(code), mem); c1.r[3:8] = [0, 0x806EE0A0, 0x200, 0, CB_CARD]; c1.run()
    c2 = Cpu(patch_operands(code), mem); c2.r[3:8] = [0, 0x80700000, 0x200, 0, 0]; c2.run()
    delivered = mem[TAB + 4] == CB_CARD
    chk(delivered == want, "%s: card DMA, BBA DMA before the interrupt is taken, TC handler %s the card callback" % (name, "finds" if delivered else "finds NO"))

# --- 2. the mask word
cpu, mem = call(NEW, 0, CB_CARD, 0, mask=0xFFFFFFFF)
chk(mem[MASK] == 0xFFFFFFFF & ~0x00600000, "NEW stub: channel 0 unmasks EXI0 EXI + TC (bits 0x00600000) in the OS mask word")
cpu, mem = call(NEW, 1, CB_CARD, 0, mask=0xFFFFFFFF)
chk(mem[MASK] == 0xFFFFFFFF & ~0x000C0000, "NEW stub: channel 1 unmasks its bits (0x000C0000)")
def handler(cpu):    # an interrupt handler that masks another source (bit 0x00000001) while the stub sits between its lwz and stw
    cpu.mem[MASK] = cpu.mem[MASK] | 0x1
cpu, mem = call(OLD, 0, CB_CARD, 0, mask=0x0, irq=(18, handler))   # old stub: instruction 18 is between the lwz and the stw
oldlost = (mem[MASK] & 1) == 0
cpu, mem2 = call(NEW, 0, CB_CARD, 0, mask=0x0, irq=(18, handler))
chk(oldlost, "OLD stub: an interrupt between lwz and stw has its mask change overwritten (the race)")
chk((mem2[MASK] & 1) == 1, "NEW stub: the same interrupt is held off until the stub restores MSR[EE]: its mask change survives")
for msr0 in (0x8000, 0x0000, 0x9032):
    cpu, mem = call(NEW, 0, CB_CARD, 0, msr=msr0)
    chk(cpu.msr == msr0, "NEW stub: MSR %04X restored exactly after the call (%s)" % (msr0, "EE was on" if msr0 & 0x8000 else "EE was off"))
    chk(all((m & 0x8000) == 0 for m in cpu.msr_trace[:1]), "NEW stub: the first mtmsr clears EE")

# --- 3. the contract with Patch.c and the ARM side
chk(OLD[:4] == NEW[:4] and OLD[4:8] == NEW[4:8] and OLD[0x10:0x14] == NEW[0x10:0x14], "operands at +2 / +6 / +0x12 are in the same instructions as before (Patch.c W16 offsets)")
cpu, mem = call(NEW, 0, CB_CARD, 0)
chk(cpu.cmds == [(0x12000200, 0x80700000)], "NEW stub: EXI_CMD_1 = buffer, EXI_CMD_0 = 0x12 | chan<<20 | mode<<16 | len (same words as the old stub)")
cpu0, mem0 = call(OLD, 0, CB_CARD, 0)
chk(cpu0.cmds == cpu.cmds, "OLD and NEW post the same EXI command")
chk(len(NEW) <= 0xE8, "NEW stub fits the smallest SDK EXIDMA it replaces (0xE8 bytes): %d" % len(NEW))
print("exidma stub test: %s" % ("FAIL %d" % fails if fails else "PASS"))
sys.exit(1 if fails else 0)
