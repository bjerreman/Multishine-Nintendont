# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for multishine_provision. Run: python3 -m unittest discover -s offline/provision/tests
All data is synthetic; no real Slippi file is touched."""
import contextlib
import io
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import multishine_provision as sp  # noqa: E402

FAKE_KEY = "FAKEKEY-zz9Plural-Z-Alpha-0123456789"
FAKE_UID = "FAKEUID-yy8Plural-Alpha-01234567"
GOOD = {"uid": FAKE_UID, "playKey": FAKE_KEY, "displayName": "Tester",
        "connectCode": "TEST#42", "latestVersion": "9.9.9",
        "chatMessages": ["msg %d" % i for i in range(16)]}


def run(argv):
    out, err = io.StringIO(), io.StringIO()
    rc = sp.main(argv, out, err)
    return rc, out.getvalue(), err.getvalue()


def git_available():
    try:
        subprocess.run(["git", "--version"], stdout=subprocess.PIPE, check=True)
        return True
    except Exception:
        return False


# Independent reference reader: different code path from the tool (no shared helpers).
def ref_sum(data):
    s1 = s2 = 0
    for i in range(0, len(data), 2):
        w = (data[i] << 8) | data[i + 1]
        s1 += w
        s2 += (~w) & 0xFFFF
    s1 &= 0xFFFF
    s2 &= 0xFFFF
    return (0 if s1 == 0xFFFF else s1), (0 if s2 == 0xFFFF else s2)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="sdprov-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def p(self, name):
        return os.path.join(self.tmp, name)

    def write_src(self, obj, name="user.json"):
        path = self.p(name)
        with open(path, "w") as f:
            json.dump(obj, f)
        return path


class RoundTrip(Base):
    def test_gci_roundtrip(self):
        src, out = self.write_src(GOOD), self.p("c.gci")
        rc, so, se = run(["import", "--from", src, "--out", out])
        self.assertEqual(rc, 0, se)
        rc, so, se = run(["show", "--card", out])
        self.assertEqual(rc, 0, se)
        self.assertIn("TEST#42", so)
        self.assertIn("Tester", so)
        buf = open(out, "rb").read()
        self.assertEqual(len(buf), 0x40 + 0x2000)
        self.assertEqual(buf[:6], b"GALE01")
        self.assertEqual(buf[0x38:0x3A], b"\x00\x01")  # block count
        self.assertEqual(buf[0x3A:0x3C], b"\xff\xff")
        self.assertEqual(buf[8:8 + len(b"MultishineIdentity")], b"MultishineIdentity")
        ident = sp.read_gci(buf)
        self.assertEqual(ident["uid"], FAKE_UID)
        self.assertEqual(ident["playKey"], FAKE_KEY)

    def test_raw_roundtrip_and_independent_reparse(self):
        src, out = self.write_src(GOOD), self.p("c.raw")
        rc, so, se = run(["import", "--from", src, "--out", out])
        self.assertEqual(rc, 0, se)
        buf = open(out, "rb").read()
        self.assertEqual(len(buf), 0x80000)
        rc, so, se = run(["show", "--card", out])
        self.assertEqual(rc, 0, se)
        self.assertIn("TEST#42", so)
        # independent structural check
        hdr = buf[:0x2000]
        self.assertEqual(struct.unpack(">HH", hdr[0x1FC:0x200]), ref_sum(hdr[:0x1FC]))
        self.assertEqual(struct.unpack(">H", hdr[0x22:0x24])[0], 4)  # megabits
        for off in (0x2000, 0x4000):
            d = buf[off:off + 0x2000]
            self.assertEqual(struct.unpack(">HH", d[0x1FFC:0x2000]), ref_sum(d[:0x1FFC]))
        self.assertEqual(buf[0x2000:0x4000], buf[0x4000:0x6000])
        for off in (0x6000, 0x8000):
            b = buf[off:off + 0x2000]
            self.assertEqual(struct.unpack(">HH", b[0:4]), ref_sum(b[4:]))
            free = struct.unpack(">H", b[6:8])[0]
            self.assertEqual(free, 64 - 5 - 1)
            self.assertEqual(struct.unpack(">H", b[10:12])[0], 0xFFFF)  # block 5 chain end
        e = buf[0x2000:0x2040]
        self.assertEqual(e[:6], b"GALE01")
        self.assertEqual(struct.unpack(">HH", e[0x36:0x3A]), (5, 1))
        # second dir entry is empty (0xFF)
        self.assertEqual(buf[0x2040:0x2044], b"\xff\xff\xff\xff")
        # payload at block 5 + 0x1000
        pl = buf[5 * 0x2000 + 0x1000:5 * 0x2000 + 0x1000 + sp.PAYLOAD_LEN]
        self.assertEqual(pl[:4], b"SDID")
        import zlib
        self.assertEqual(struct.unpack(">I", pl[-4:])[0], zlib.crc32(pl[:-4]))

    def test_larger_card(self):
        buf = sp.build_card(sp.validate_identity(GOOD), size_bytes=0x200000)
        self.assertEqual(len(buf), 0x200000)
        self.assertEqual(sp.read_card(buf)["connectCode"], "TEST#42")

    def test_placeholder_export(self):
        for ext in ("gci", "raw"):
            out = self.p("ph." + ext)
            rc, so, se = run(["export-placeholder", "--out", out])
            self.assertEqual(rc, 0, se)
            rc, so, se = run(["show", "--card", out])
            self.assertIn("EXMP#123", so)
            self.assertIn("ExampleName", so)

    def test_protect_bits(self):
        out = self.p("p.gci")
        run(["export-placeholder", "--out", out, "--protect"])
        self.assertEqual(open(out, "rb").read()[0x34], 0x1C)

    def test_unicode_names(self):
        g = dict(GOOD, displayName="Ünï", connectCode="ＡＢ#123")
        buf = sp.build_gci(sp.validate_identity(g))
        self.assertEqual(sp.read_gci(buf)["displayName"], "Ünï")


class Corruption(Base):
    def setUp(self):
        super().setUp()
        ident = sp.validate_identity(GOOD)
        self.gci = sp.build_gci(ident)
        self.raw = sp.build_card(ident)

    def flip(self, buf, off):
        b = bytearray(buf)
        b[off] ^= 0x01
        return bytes(b)

    def test_gci_payload_crc(self):
        with self.assertRaises(sp.ProvisionError):
            sp.read_gci(self.flip(self.gci, 0x40 + sp.PAYLOAD_OFFSET + 20))

    def test_gci_magic_and_size(self):
        with self.assertRaises(sp.ProvisionError):
            sp.read_gci(self.flip(self.gci, 0x40 + sp.PAYLOAD_OFFSET))
        with self.assertRaises(sp.ProvisionError):
            sp.read_gci(self.gci[:-1])
        with self.assertRaises(sp.ProvisionError):
            sp.read_gci(self.flip(self.gci, 0))  # game code

    def test_raw_header(self):
        with self.assertRaises(sp.ProvisionError):
            sp.read_card(self.flip(self.raw, 0x20))

    def test_raw_dir_one_copy_survives(self):
        buf = self.flip(self.raw, 0x2000 + 0x10)  # corrupt primary dir only
        self.assertEqual(sp.read_card(buf)["connectCode"], "TEST#42")

    def test_raw_dir_both_bad(self):
        buf = self.flip(self.flip(self.raw, 0x2000 + 0x10), 0x4000 + 0x10)
        with self.assertRaises(sp.ProvisionError):
            sp.read_card(buf)

    def test_raw_bat_both_bad(self):
        buf = self.flip(self.flip(self.raw, 0x6000 + 0x20), 0x8000 + 0x20)
        with self.assertRaises(sp.ProvisionError):
            sp.read_card(buf)

    def test_raw_payload(self):
        with self.assertRaises(sp.ProvisionError):
            sp.read_card(self.flip(self.raw, 5 * 0x2000 + sp.PAYLOAD_OFFSET + 100))

    def test_raw_bad_size(self):
        with self.assertRaises(sp.ProvisionError):
            sp.read_card(self.raw[:-0x2000])

    def test_checksum_ffff_rule(self):
        # sum word 0xFFFF must become 0
        self.assertEqual(sp.checksum(b"\xff\xff")[0], 0)
        self.assertEqual(sp.checksum(b"\x00\x00")[1], 0)

    def test_show_corrupt_cli(self):
        path = self.p("bad.gci")
        open(path, "wb").write(self.flip(self.gci, 0x40 + sp.PAYLOAD_OFFSET + 5))
        rc, so, se = run(["show", "--card", path])
        self.assertEqual(rc, 2)
        self.assertNotIn(FAKE_KEY, so + se)


class Validation(Base):
    def bad(self, **changes):
        o = dict(GOOD)
        for k, v in changes.items():
            if v is None:
                o.pop(k, None)
            else:
                o[k] = v
        with self.assertRaises(sp.ProvisionError):
            sp.validate_identity(o)

    def test_ok(self):
        self.assertEqual(sp.validate_identity(GOOD)["connectCode"], "TEST#42")
        o = dict(GOOD)
        del o["chatMessages"], o["latestVersion"]
        sp.validate_identity(o)

    def test_missing_and_types(self):
        for k in ("uid", "playKey", "displayName", "connectCode"):
            self.bad(**{k: None})
            self.bad(**{k: ""})
            self.bad(**{k: 5})
        self.bad(latestVersion=3)

    def test_connect_code(self):
        for c in ("NOHASH", "TEST#", "#123", "TOOLONGTAG#1", "A B#1", "TEST#12345", "TEST#1a"):
            self.bad(connectCode=c)

    def test_lengths_and_control(self):
        self.bad(playKey="k" * 128)
        self.bad(uid="u" * 64)
        self.bad(displayName="n" * 64)
        self.bad(displayName="a\x1b[31mb")

    def test_chat_messages(self):
        self.bad(chatMessages=["x"] * 15)
        self.bad(chatMessages=["x"] * 17)
        self.bad(chatMessages=["x"] * 15 + [1])
        self.bad(chatMessages="x")

    def test_not_object_and_bad_json(self):
        with self.assertRaises(sp.ProvisionError):
            sp.validate_identity([])
        path = self.p("junk.json")
        open(path, "w").write("{ not json " + FAKE_KEY)
        rc, so, se = run(["import", "--from", path, "--out", self.p("o.gci")])
        self.assertEqual(rc, 2)
        self.assertNotIn(FAKE_KEY, so + se)
        self.assertFalse(os.path.exists(self.p("o.gci")))


class Output(Base):
    def test_mode_0600(self):
        old = os.umask(0)  # permissive umask must not matter
        try:
            for ext in ("gci", "raw"):
                out = self.p("m." + ext)
                run(["export-placeholder", "--out", out])
                self.assertEqual(stat.S_IMODE(os.stat(out).st_mode), 0o600)
                run(["export-placeholder", "--out", out, "--force"])
                self.assertEqual(stat.S_IMODE(os.stat(out).st_mode), 0o600)
        finally:
            os.umask(old)

    def test_no_overwrite_without_force(self):
        out = self.p("o.gci")
        self.assertEqual(run(["export-placeholder", "--out", out])[0], 0)
        first = open(out, "rb").read()
        src = self.write_src(GOOD)
        rc, so, se = run(["import", "--from", src, "--out", out])
        self.assertEqual(rc, 2)
        self.assertEqual(open(out, "rb").read(), first)
        self.assertEqual(run(["import", "--from", src, "--out", out, "--force"])[0], 0)
        self.assertIn("TEST#42", run(["show", "--card", out])[1])

    def test_missing_parent(self):
        rc, so, se = run(["export-placeholder", "--out", self.p("nodir/x.gci")])
        self.assertEqual(rc, 2)

    def test_secrets_never_printed(self):
        src = self.write_src(GOOD)
        outputs = []
        for ext in ("gci", "raw"):
            out = self.p("s." + ext)
            outputs.append(run(["import", "--from", src, "--out", out]))
            outputs.append(run(["import", "--from", src, "--out", out]))  # exists error
            outputs.append(run(["show", "--card", out]))
        bad = dict(GOOD, connectCode="nope")
        outputs.append(run(["import", "--from", self.write_src(bad, "b.json"), "--out", self.p("b.gci")]))
        outputs.append(run(["import", "--from", self.p("missing" + FAKE_KEY), "--out", self.p("q.gci")]))
        for rc, so, se in outputs:
            for secret in (FAKE_KEY, FAKE_UID):
                self.assertNotIn(secret, so)
                self.assertNotIn(secret, se)
        # also with the real logging module captured
        import logging
        stream = io.StringIO()
        h = logging.StreamHandler(stream)
        logging.getLogger().addHandler(h)
        logging.getLogger().setLevel(logging.DEBUG)
        try:
            run(["import", "--from", src, "--out", self.p("l.gci")])
        finally:
            logging.getLogger().removeHandler(h)
        self.assertNotIn(FAKE_KEY, stream.getvalue())

    def test_internal_error_suppressed(self):
        orig = sp.build_gci

        def boom(*a, **k):
            raise RuntimeError("secret " + FAKE_KEY)
        sp.build_gci = boom
        try:
            rc, so, se = run(["import", "--from", self.write_src(GOOD), "--out", self.p("e.gci")])
        finally:
            sp.build_gci = orig
        self.assertEqual(rc, 3)
        self.assertNotIn(FAKE_KEY, so + se)

    def test_hostile_card_name_sanitised(self):
        g = dict(GOOD)
        ident = sp.validate_identity(g)
        buf = bytearray(sp.build_gci(ident))
        # inject an escape char into displayName bytes then fix CRC: show must neutralise it
        o = 0x40 + sp.PAYLOAD_OFFSET + 8 + sp.UID_LEN + sp.KEY_LEN + sp.CODE_LEN
        buf[o] = 0x1B
        import zlib
        body = bytes(buf[0x40 + sp.PAYLOAD_OFFSET:0x40 + sp.PAYLOAD_OFFSET + sp.PAYLOAD_BODY])
        struct.pack_into(">I", buf, 0x40 + sp.PAYLOAD_OFFSET + sp.PAYLOAD_BODY, zlib.crc32(body))
        path = self.p("h.gci")
        open(path, "wb").write(bytes(buf))
        rc, so, se = run(["show", "--card", path])
        self.assertEqual(rc, 0)
        self.assertNotIn("\x1b", so)


@unittest.skipUnless(git_available(), "git not available")
class GitGuard(Base):
    def git(self, *a):
        env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
        subprocess.run(["git"] + list(a), cwd=self.repo, env=env, check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def setUp(self):
        super().setUp()
        self.repo = self.p("repo")
        os.mkdir(self.repo)
        self.git("init", "-q")
        os.mkdir(os.path.join(self.repo, "build"))
        os.mkdir(os.path.join(self.repo, "src"))
        with open(os.path.join(self.repo, ".gitignore"), "w") as f:
            f.write("build/\n*.gci\n")
        with open(os.path.join(self.repo, "src", "tracked.raw"), "wb") as f:
            f.write(b"x")
        self.git("add", ".gitignore", "src/tracked.raw")

    def test_refuse_unignored(self):
        out = os.path.join(self.repo, "src", "card.raw")
        rc, so, se = run(["export-placeholder", "--out", out])
        self.assertEqual(rc, 2)
        self.assertIn("gitignored", se)
        self.assertFalse(os.path.exists(out))

    def test_refuse_repo_root(self):
        rc, so, se = run(["export-placeholder", "--out", os.path.join(self.repo, "card.raw")])
        self.assertEqual(rc, 2)

    def test_refuse_tracked_even_with_force(self):
        out = os.path.join(self.repo, "src", "tracked.raw")
        rc, so, se = run(["export-placeholder", "--out", out, "--force"])
        self.assertEqual(rc, 2)
        self.assertEqual(open(out, "rb").read(), b"x")

    def test_allow_ignored(self):
        for rel in ("build/card.raw", "src/card.gci"):
            out = os.path.join(self.repo, rel)
            rc, so, se = run(["export-placeholder", "--out", out])
            self.assertEqual(rc, 0, se)
            self.assertTrue(os.path.exists(out))

    def test_outside_repo_ok(self):
        self.assertEqual(run(["export-placeholder", "--out", self.p("free.raw")])[0], 0)

    def test_symlink_into_repo_refused(self):
        link = self.p("lnk")
        os.symlink(os.path.join(self.repo, "src"), link)
        rc, so, se = run(["export-placeholder", "--out", os.path.join(link, "card.raw")])
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main()
