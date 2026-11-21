#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""patch 0024: the PowerPC interrupt sampler added to kernel/asm/FakeInterrupt.S, run as machine code (same small interpreter idea as exidma_stub_test.py).
Checks: it stores srr0 / srr1 / lr / r1 of the saved context, the real PI INTSR / INTMR and the OS mask word into the ring at 0xD3026700 (4 entries, newest = counter & 3),
the counter counts, it touches only r5 / r6 / r7 (and cr0) so the original code after it sees the registers it expects (r3 = 0xCC000000, r30 = context, r31 = real INTSR),
and a context pointer outside MEM1 is skipped.  Needs the devkitPPC assembler; SKIP without."""
import os, re, struct, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, "..", "..", ".."))
tc = os.path.join(os.environ.get("DKP_R35", os.path.join(root, "toolchains", "dkp-r35")), "devkitPPC", "bin")
if not os.path.exists(os.path.join(tc, "powerpc-eabi-as")): tc = "/opt/devkitpro/devkitPPC/bin"
AS, STRIP = os.path.join(tc, "powerpc-eabi-as"), os.path.join(tc, "powerpc-eabi-strip")
pin = os.environ.get("NIN_SRC", os.path.join(root, "third_party", "Nintendont")); PIN = "bbc0a208e81d6ab0a828a431dc0b2ba6fc06aec5"
try:
    assert os.path.exists(AS)
    assert subprocess.run(["git", "-C", pin, "rev-parse", "HEAD"], capture_output=True, check=True).stdout.decode().strip() == PIN
except Exception:
    print("fakeint sample test: SKIP (no devkitPPC assembler or pinned clone)"); sys.exit(0)
def show(p): return subprocess.run(["git", "-C", pin, "show", "%s:%s" % (PIN, p)], check=True, capture_output=True).stdout
tmp = tempfile.mkdtemp(prefix="fakeint_")
os.makedirs(os.path.join(tmp, "kernel", "asm"))
S0 = show("kernel/asm/FakeInterrupt.S"); consts = show("kernel/asm/constants.inc")
open(os.path.join(tmp, "kernel", "asm", "FakeInterrupt.S"), "wb").write(S0)
patch = open(os.path.join(here, "..", "patches", "0024-multishine-pc-sample.patch"), "rb").read().decode("latin1")
blk = [b for b in re.split(r"(?m)^(?=diff --git )", patch) if b.startswith("diff --git a/kernel/asm/FakeInterrupt.S")]
assert len(blk) == 1
subprocess.run(["git", "init", "-q", tmp], check=True)
subprocess.run(["git", "apply", "--whitespace=nowarn", "-"], input=blk[0].encode("latin1"), cwd=tmp, check=True)
S1 = open(os.path.join(tmp, "kernel", "asm", "FakeInterrupt.S"), "rb").read()
def build(src, name):
    d = os.path.join(tmp, name); os.makedirs(d)
    open(os.path.join(d, "F.S"), "wb").write(src); open(os.path.join(d, "constants.inc"), "wb").write(consts)
    subprocess.run([AS, "F.S", "-mregnames", "-o", "x.elf"], cwd=d, check=True)
    subprocess.run([STRIP, "x.elf", "-O", "binary", "-S", "-s", "-o", "x.bin"], cwd=d, check=True)
    return bytearray(open(os.path.join(d, "x.bin"), "rb").read())
OLD, NEW = build(S0, "old"), build(S1, "new")
M32 = 0xFFFFFFFF
def s16(v): return v - 0x10000 if v & 0x8000 else v
class Cpu:
    def __init__(self, code, mem):
        self.code, self.mem, self.r, self.cr, self.pc, self.touched = code, mem, [0] * 32, 0, 0, set()
    def setcr(self, a, b, signed):
        if signed:
            a = a - (1 << 32) if a & 0x80000000 else a; b = b - (1 << 32) if b & 0x80000000 else b
        self.cr = 0x8 if a < b else 0x4 if a > b else 0x2
    def run(self):
        while True:
            ins, = struct.unpack(">I", bytes(self.code[self.pc:self.pc + 4])); op = ins >> 26
            rd = (ins >> 21) & 31; ra = (ins >> 16) & 31; rb = (ins >> 11) & 31; imm = ins & 0xFFFF
            if op == 28 and rd == 31 and imm == 0x4000: return        # the original first test: the sampler is done
            self.pc += 4; before = list(self.r)
            if op == 15: self.r[rd] = ((0 if ra == 0 else self.r[ra]) + (imm << 16)) & M32
            elif op == 14: self.r[rd] = ((0 if ra == 0 else self.r[ra]) + s16(imm)) & M32
            elif op == 32: self.r[rd] = self.mem.get((self.r[ra] + s16(imm)) & M32, 0)
            elif op == 36: self.mem[(self.r[ra] + s16(imm)) & M32] = self.r[rd]
            elif op == 28: self.r[ra] = self.r[rd] & imm; self.setcr(self.r[ra], 0, True)
            elif op == 21:
                sh, mb, me = rb, (ins >> 6) & 31, (ins >> 1) & 31; v = self.r[rd]
                rot = ((v << sh) | (v >> (32 - sh))) & M32 if sh else v
                mask = (M32 >> mb) & ~(M32 >> me >> 1) if mb <= me else ((M32 >> mb) | ~(M32 >> me >> 1)) & M32
                self.r[ra] = rot & mask
            elif op == 31 and ((ins >> 1) & 0x3FF) == 266: self.r[rd] = (self.r[ra] + self.r[rb]) & M32      # add
            elif op == 31 and ((ins >> 1) & 0x3FF) == 32: self.setcr(self.r[ra], self.r[rb], False)         # cmplw
            elif op == 16:
                bo, bi, bd = rd, ra, s16(ins & 0xFFFC); bit = {0: 0x8, 1: 0x4, 2: 0x2}[bi]
                if (bool(self.cr & bit) if (bo & 8) else not (self.cr & bit)): self.pc = self.pc - 4 + bd
            else: raise Exception("opcode %d at %x" % (op, self.pc - 4))
            for i in range(32):
                if self.r[i] != before[i]: self.touched.add(i)
fails = 0
def chk(c, m):
    global fails
    if not c: fails += 1; print("FAIL", m)
    else: print("ok  ", m)
CTX = 0x804EE000
def ctx_mem(srr0, srr1, lr, r1):
    return {CTX + 0x198: srr0, CTX + 0x19C: srr1, CTX + 0x84: lr, CTX + 4: r1, 0xCC003000: 0x40004010, 0xCC003004: 0x000040F4, 0x800000C4: 0xF07F8360}
def run(code, ctx, mem, prime=None):
    cpu = Cpu(code, dict(mem))
    if prime: cpu.mem.update(prime)
    cpu.r[3], cpu.r[4], cpu.r[30] = 0xCC000000, ctx, ctx
    cpu.run()
    return cpu
# samples in a row
mem = ctx_mem(0x8037DCF4, 0x9932, 0x8034B130, 0x804EE9A0)
cpu = run(NEW, CTX, mem)
chk(cpu.mem[0xD3026700] == 1, "counter 1 after the first sample")
e = 0xD3026700 + 0x20 + 0x20 * 1
chk([cpu.mem.get(e + 4 * k) for k in range(7)] == [0x8037DCF4, 0x9932, 0x8034B130, 0x804EE9A0, 0x40004010, 0x000040F4, 0xF07F8360], "entry 1 = srr0 srr1 lr r1 | real INTSR INTMR | OS mask word")
prev = dict(cpu.mem)
for k in range(2, 7):
    cpu = run(NEW, CTX, ctx_mem(0x80000000 + k, 0x9032, 0x8034B000 + k, 0x804EE000 + k), prime={0xD3026700: prev[0xD3026700], **{a: v for a, v in prev.items() if 0xD3026700 <= a < 0xD30267A0}})
    prev = dict(cpu.mem)
chk(prev[0xD3026700] == 6, "counter 6 after six samples")
chk(prev[0xD3026700 + 0x20 + 0x20 * 2 + 0] == 0x80000006, "the ring wraps: sample 6 sits in entry 6 & 3 = 2")
chk(prev[0xD3026700 + 0x20 + 0x20 * 3] == 0x80000003, "sample 3 is still in entry 3")
# the original contract: r3 / r4 / r30 / r31 unchanged, only r5 r6 r7 touched
chk(cpu.r[3] == 0xCC000000 and cpu.r[4] == CTX and cpu.r[30] == CTX and cpu.r[31] == 0x40004010, "r3 r4 r30 and the real INTSR in r31 are as the original code expects")
chk(cpu.touched <= {5, 6, 7, 31}, "besides the original lwz r31 only r5, r6, r7 are scratch: %s" % sorted(cpu.touched))
# context outside MEM1: nothing stored
cpu = run(NEW, 0x00001000, ctx_mem(1, 2, 3, 4))
chk(0xD3026700 not in cpu.mem, "a context pointer outside 0x80000000..0x81800000 is skipped")
cpu = run(NEW, 0xC0001000, ctx_mem(1, 2, 3, 4))
chk(0xD3026700 not in cpu.mem, "an uncached-looking pointer is skipped as well")
# old code stops at the same place with nothing stored
cpu = run(OLD, CTX, mem)
chk(not [a for a in cpu.mem if 0xD3026700 <= a < 0xD30267A0], "old FakeInterrupt stores nothing")
chk(len(NEW) - len(OLD) < 0x100, "added %d bytes (the patch copy area has room)" % (len(NEW) - len(OLD)))
print("fakeint sample test: %s" % ("FAIL %d" % fails if fails else "PASS"))
sys.exit(1 if fails else 0)
