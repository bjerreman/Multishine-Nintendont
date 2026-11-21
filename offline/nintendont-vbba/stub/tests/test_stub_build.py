#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Build check of the Nintendont STUB (stub/build_stub.sh): needs devkitPPC (skipped without it) and a shell DOL (the one of the shell package, or a synthetic one).
The stub must build, embed the shell DOL byte for byte, carry the pinned production keys and NOT the dev test key, have a trampoline that fits one page, and contain no stock updater URL.
Run: python3.14 test_stub_build.py"""
import os, re, struct, subprocess, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
STUB = os.path.normpath(os.path.join(HERE, ".."))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
SHELL_REAL = os.path.join(REPO, "build", "wii_ota_shell_pkg", "apps", "Nintendont-Multishine", "boot.dol")
DEV_PUB = bytes.fromhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
HAVE_TOOLS = os.path.exists("/opt/devkitpro/devkitPPC/bin/powerpc-eabi-gcc") and os.path.exists("/opt/devkitpro/tools/bin/elf2dol")


def mkdol(tsize=0x400, dsize=0x200):
    h = bytearray(0x100)
    struct.pack_into(">I", h, 0x00, 0x100); struct.pack_into(">I", h, 0x1C, 0x100 + tsize)
    struct.pack_into(">I", h, 0x48, 0x80004000); struct.pack_into(">I", h, 0x64, 0x80008000)
    struct.pack_into(">I", h, 0x90, tsize); struct.pack_into(">I", h, 0xAC, dsize)
    struct.pack_into(">III", h, 0xD8, 0x8000A000, 0x1000, 0x80004000)
    return bytes(h) + bytes((i * 11) & 0xFF for i in range(tsize + dsize))


@unittest.skipUnless(HAVE_TOOLS, "devkitPPC not installed")
class StubBuild(unittest.TestCase):
    def build(self, shell_bytes, extra=()):
        d = tempfile.mkdtemp()
        shell = os.path.join(d, "shell.dol")
        open(shell, "wb").write(shell_bytes)
        out = os.path.join(d, "out")
        subprocess.run(["bash", os.path.join(STUB, "build_stub.sh"), shell, out] + list(extra), check=True, capture_output=True)
        return open(os.path.join(out, "boot.dol"), "rb").read(), shell_bytes

    def test_synthetic_shell_is_embedded_verbatim_and_keys_are_pinned(self):
        sh = mkdol()
        dol, _ = self.build(sh)
        self.assertIn(sh, dol)                                              # the fallback shell, byte for byte
        pubs = [bytes.fromhex(l.split()[1]) for l in open(os.path.join(REPO, "tools", "release", "prod_pubkeys.txt")) if l.strip() and not l.startswith("#")]
        for p in pubs:
            self.assertIn(p, dol)
        self.assertNotIn(DEV_PUB, dol)                                      # the release stub never trusts the published test key
        self.assertNotIn(b"githubusercontent", dol)

    def test_dev_key_only_with_the_flag(self):
        dol, _ = self.build(mkdol(), ["--devkey"])
        self.assertIn(DEV_PUB, dol)

    def test_a_shell_that_breaks_the_rules_is_not_embedded(self):
        bad = bytearray(mkdol()); struct.pack_into(">I", bad, 0x48, 0x81300000)
        with self.assertRaises(subprocess.CalledProcessError):
            self.build(bytes(bad))

    def test_the_stub_is_a_valid_dol_and_the_trampoline_is_small(self):
        dol, _ = self.build(mkdol())
        off = struct.unpack_from(">18I", dol, 0); addr = struct.unpack_from(">18I", dol, 0x48); size = struct.unpack_from(">18I", dol, 0x90)
        entry = struct.unpack_from(">I", dol, 0xE0)[0]
        self.assertTrue(any(addr[i] <= entry < addr[i] + size[i] for i in range(7) if size[i]))
        for i in range(18):
            if size[i]:
                self.assertLessEqual(off[i] + size[i], len(dol))
                self.assertGreaterEqual(addr[i], 0x80003100)
        o = os.path.join(tempfile.mkdtemp(), "t.o")
        gcc = "/opt/devkitpro/devkitPPC/bin/powerpc-eabi-gcc"
        subprocess.run([gcc, "-DGEKKO", "-mrvl", "-mcpu=750", "-meabi", "-mhard-float", "-mregnames", "-D_LANGUAGE_ASSEMBLY", "-c", os.path.join(STUB, "tramp.S"), "-o", o], check=True)
        nm = subprocess.run(["/opt/devkitpro/devkitPPC/bin/powerpc-eabi-nm", o], capture_output=True, text=True, check=True).stdout
        pos = {l.split()[2]: int(l.split()[0], 16) for l in nm.splitlines() if len(l.split()) == 3}
        self.assertLess(pos["ms_tramp_end"] - pos["ms_tramp_start"], 0x1000)         # stub_main.c copies at most one page
        text = subprocess.run(["/opt/devkitpro/devkitPPC/bin/powerpc-eabi-objdump", "-d", o], capture_output=True, text=True, check=True).stdout
        for bad in ("bl ", "lis ", "addis "):                                         # position independent: no calls, no absolute address building
            self.assertNotIn("\t" + bad, text)

    @unittest.skipUnless(os.path.exists(SHELL_REAL), "no real shell dol")
    def test_the_real_shell_builds_into_a_stub(self):
        dol, sh = self.build(open(SHELL_REAL, "rb").read())
        self.assertIn(sh, dol)
        self.assertLess(len(dol), len(sh) + 600 * 1024)                     # the stub itself stays small: libogc, libfat and the signature code


if __name__ == "__main__":
    unittest.main()
