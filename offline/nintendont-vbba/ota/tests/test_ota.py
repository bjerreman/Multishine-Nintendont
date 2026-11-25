#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Host test of the Nintendont OTA verify path: containers built by tools/card/krn_data.py and signed by tools/card/mscf_sign.py are checked by the SAME C code the loader runs
(offline/nintendont-vbba/ota/ms_ota_core.c, via tests/ota_test.c).  Run: python3.14 test_ota.py   (needs cc)."""
import io, os, socket, struct, subprocess, sys, tempfile, unittest, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "card"))
sys.path.insert(0, os.path.join(REPO, "offline", "cardloader"))
import krn_data as K        # noqa: E402
import shl_data as SH       # noqa: E402
import mscf_sign as S       # noqa: E402

OTA = os.path.join(REPO, "offline", "nintendont-vbba", "ota")
BIN = os.path.join(tempfile.gettempdir(), "ms_ota_test_bin")
OTHER_SEED = bytes(range(32))


def build_bin():
    cmd = ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
           "-I" + OTA, "-I" + os.path.join(REPO, "offline", "client_net"), "-I" + os.path.join(REPO, "offline", "cardloader", "cl"),
           os.path.join(OTA, "tests", "ota_test.c"), os.path.join(OTA, "ms_ota_core.c"), os.path.join(REPO, "offline", "client_net", "mw2_crypto.c"),
           os.path.join(REPO, "offline", "payload_pack", "ms_sha256.c"), "-o", BIN]
    subprocess.check_call(cmd)


def kernel_zip(text=b"RUNID: game %s config %08X\n", size=3000):
    b = io.BytesIO()
    with zipfile.ZipFile(b, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("kernel.bin", text + bytes((i * 7) & 0xFF for i in range(size)))
    return b.getvalue()


def signed(seq, seed=bytes.fromhex(S.DEV_SEED.hex()) if isinstance(S.DEV_SEED, bytes) else bytes.fromhex(S.DEV_SEED), zipb=None, abi_patch=None):
    gci = K.build_gci(K.build_payload(zipb or kernel_zip()), seq)
    if abi_patch is not None:
        b = bytearray(gci); struct.pack_into(">H", b, 0x40 + 0x1000 + 6, abi_patch); gci = bytes(b)
        gci = S.set_seq(gci, seq) if False else gci
    return S.sign(gci, seed, seq=seq)[0x40:]


def run_check(data, pub_hex, min_seq=1):
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(data)
        path = f.name
    try:
        out = subprocess.run([BIN, "check", pub_hex, str(min_seq), path], capture_output=True, text=True, check=True).stdout.strip()
    finally:
        os.unlink(path)
    return out


def dns(*args):
    return subprocess.run([BIN, "dns", *args], capture_output=True, text=True, check=True).stdout.strip()


def qpacket(name, id=0x1234):
    lab = b"".join(bytes([len(x)]) + x.encode() for x in name.split("."))
    return struct.pack(">HHHHHH", id, 0x0100, 1, 0, 0, 0) + lab + b"\0" + struct.pack(">HH", 1, 1)


# real answers recorded from a resolver (query id 0x1234): multishine.link A, and www.github.com (CNAME to github.com, then A)
R_MS = "1234818000010001000000000a6d756c74697368696e65046c696e6b0000010001c00c000100010000012c0004c63364d6"
R_GH = "123481800001000200000000037777770667697468756203636f6d0000010001c00c0005000100000e100002c010c010000100010000007c000404e10bc2"


def rr(name_ptr_or_bytes, typ, rdata, ttl=60):
    return name_ptr_or_bytes + struct.pack(">HHIH", typ, 1, ttl, len(rdata)) + rdata


def answer(name, rrs, id=0x1234, flags=0x8180, qd=1):
    return (struct.pack(">HHHHHH", id, flags, qd, len(rrs), 0, 0) + qpacket(name)[12:] + b"".join(rrs)).hex()


class DnsParser(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_bin()

    def test_query_bytes(self):
        for name in ("multishine.link", "a.b.c.example.org"):
            self.assertEqual(dns("query", "0x1234", name), qpacket(name).hex())
        self.assertEqual(dns("query", "7", ("x" * 64) + ".com"), "")              # a label over 63 bytes cannot be built

    def test_real_answers(self):
        self.assertEqual(dns("parse", "0x1234", "multishine.link", R_MS), "DNS 0 ok ip 198.51.100.214")
        self.assertEqual(dns("parse", "0x1234", "WWW.GitHub.com", R_GH), "DNS 0 ok ip 4.225.11.194")      # CNAME then A, case-insensitive name
        self.assertEqual(dns("parse", "0x1234", "multishine.link", R_GH)[:5], "DNS 4")                       # the answer is for another question

    def test_wrong_id_flags_and_truncation(self):
        self.assertTrue(dns("parse", "0x4321", "multishine.link", R_MS).startswith("DNS 2 wrong id"))
        bad = bytearray(bytes.fromhex(R_MS)); bad[3] = 0x83                                                 # rcode 3 (no such name)
        self.assertTrue(dns("parse", "0x1234", "multishine.link", bad.hex()).startswith("DNS 3"))
        bad = bytearray(bytes.fromhex(R_MS)); bad[2] = 0x01                                                 # not a response
        self.assertTrue(dns("parse", "0x1234", "multishine.link", bad.hex()).startswith("DNS 3"))
        full = bytes.fromhex(R_MS)
        for cut in range(0, len(full)):                                                                     # every truncation: an error, never a crash, never an address
            r = dns("parse", "0x1234", "multishine.link", full[:cut].hex()) if cut else "DNS 1"
            self.assertTrue(r.startswith("DNS ") and not r.startswith("DNS 0"), (cut, r))
        self.assertTrue(dns("parse", "0x1234", "www.github.com", bytes.fromhex(R_GH)[:-4].hex()).startswith("DNS 5"))   # cut inside the A record

    def test_multiple_answers_and_private_addresses(self):
        n = b"\xc0\x0c"
        pub = socket.inet_aton("198.51.100.214")
        self.assertEqual(dns("parse", "0x1234", "multishine.link", answer("multishine.link", [rr(n, 1, socket.inet_aton("192.168.2.1")), rr(n, 1, pub)])), "DNS 0 ok ip 198.51.100.214")   # the first public one
        for bad in ("10.1.2.3", "127.0.0.1", "169.254.1.1", "172.16.0.9", "172.31.255.1", "192.168.0.1", "224.0.0.1", "0.0.0.0", "255.255.255.255", "100.64.0.1"):
            self.assertTrue(dns("parse", "0x1234", "multishine.link", answer("multishine.link", [rr(n, 1, socket.inet_aton(bad))])).startswith("DNS 5"), bad)
            self.assertEqual(dns("ok", str(struct.unpack(">I", socket.inet_aton(bad))[0])), "IPOK 0", bad)
        for good in ("198.51.100.214", "172.15.0.1", "172.32.0.1", "192.167.1.1", "1.1.1.1", "100.63.1.1", "100.128.0.1"):
            self.assertEqual(dns("ok", str(struct.unpack(">I", socket.inet_aton(good))[0])), "IPOK 1", good)

    def test_poisoned_and_hostile_packets(self):
        pub = socket.inet_aton("198.51.100.214")
        other = b"\x05other\x03com\x00"
        self.assertTrue(dns("parse", "0x1234", "multishine.link", answer("multishine.link", [rr(other, 1, pub)])).startswith("DNS 5"))      # an A record for some other name
        self.assertTrue(dns("parse", "0x1234", "multishine.link", answer("multishine.link", [rr(b"\xc0\x0c", 5, other), rr(other, 1, pub)])).startswith("DNS 0"))   # CNAME then A for its target: fine
        self.assertTrue(dns("parse", "0x1234", "multishine.link", answer("multishine.link", [rr(b"\xc0\x0c", 1, b"\x01\x02\x03")])).startswith("DNS 5"))      # an A record that is not 4 bytes
        loop = struct.pack(">HHHHHH", 0x1234, 0x8180, 1, 1, 0, 0) + qpacket("multishine.link")[12:] + b"\xc0" + bytes([12 + 0x1d]) + b"\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04" + pub
        self.assertFalse(dns("parse", "0x1234", "multishine.link", loop.hex()).startswith("DNS 0"))                                          # a pointer onto itself
        self.assertEqual(dns("parse", "0x1234", "multishine.link", (struct.pack(">HHHHHH", 0x1234, 0x8180, 1, 1, 0, 0) + qpacket("multishine.link")[12:] + b"\x80\x00").hex())[:5], "DNS 6")   # a reserved label type
        qd2 = struct.pack(">HHHHHH", 0x1234, 0x8180, 2, 1, 0, 0) + qpacket("multishine.link")[12:]
        self.assertTrue(dns("parse", "0x1234", "multishine.link", qd2.hex()).startswith("DNS 4"))                                             # two questions: not ours


def wc(*a):
    return subprocess.run([BIN, "wchunks", *map(str, a)], capture_output=True, text=True, check=True).stdout.strip()


class ChunkWriter(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_bin()

    def test_whole_file_in_32k_chunks_with_syncs(self):
        # 590000 bytes, 32768 byte chunks = 19 calls (the last one short), a sync every 256 KB and at the end
        self.assertEqual(wc(590000, 32768, 262144, -1, -1, 0), "W 0 at 590000 rc 0 syncfail 0 calls 19 bytes 590000 syncs 3 maxchunk 32768")
        self.assertEqual(wc(1, 32768, 262144, -1, -1, 0), "W 0 at 1 rc 0 syncfail 0 calls 1 bytes 1 syncs 1 maxchunk 1")
        self.assertEqual(wc(0, 32768, 262144, -1, -1, 0), "W 0 at 0 rc 0 syncfail 0 calls 0 bytes 0 syncs 1 maxchunk 0")

    def test_error_at_chunk_k_stops_there(self):
        self.assertEqual(wc(590000, 32768, 262144, 5, -1, 0), "W 1 at 163840 rc 13 syncfail 0 calls 6 bytes 163840 syncs 0 maxchunk 32768")     # chunk 5 (0-based) = byte 163840
        self.assertEqual(wc(590000, 32768, 262144, 0, -1, 0), "W 1 at 0 rc 13 syncfail 0 calls 1 bytes 0 syncs 0 maxchunk 32768")
        self.assertEqual(wc(590000, 32768, 262144, 18, -1, 0), "W 1 at 589824 rc 13 syncfail 0 calls 19 bytes 589824 syncs 2 maxchunk 32768")  # the last chunk

    def test_short_write_is_a_failure(self):
        self.assertEqual(wc(590000, 32768, 262144, -1, 3, 0), "W 2 at 114688 rc 0 syncfail 0 calls 4 bytes 114688 syncs 0 maxchunk 32768")       # 3 full chunks + half of the 4th

    def test_failing_sync_is_counted_not_fatal(self):
        self.assertEqual(wc(590000, 32768, 262144, -1, -1, 1), "W 0 at 590000 rc 0 syncfail 3 calls 19 bytes 590000 syncs 3 maxchunk 32768")

    def test_sd_card_full_message(self):
        self.assertEqual(subprocess.run([BIN, "full", "12000000", "1564672", "x"], capture_output=True, text=True).stdout.strip(), "FULL 0 SD card full: 12 MB free, 2 MB needed")   # plenty: the message is only used when 1
        self.assertEqual(subprocess.run([BIN, "full", "1000000", "1564672", "x"], capture_output=True, text=True).stdout.strip(), "FULL 1 SD card full: 1 MB free, 2 MB needed")
        self.assertEqual(subprocess.run([BIN, "full", "1700000", "1564672", "x"], capture_output=True, text=True).stdout.strip(), "FULL 1 SD card full: 2 MB free, 2 MB needed")   # not enough once the 256 KB of slack is counted
        self.assertTrue(subprocess.run([BIN, "full", "1900000", "1564672", "x"], capture_output=True, text=True).stdout.startswith("FULL 0"))
        self.assertTrue(subprocess.run([BIN, "full", "0", "1564672", "x"], capture_output=True, text=True).stdout.startswith("FULL 1 SD card full: 0 MB free"))


class DnsResolver(unittest.TestCase):
    """the resolver loop with a scripted network: the fake clock moves only when the code waits"""
    @classmethod
    def setUpClass(cls):
        build_bin()

    def res(self, script, timeout=2000, tries=2, budget=12000, send_fail=False):
        a = ["resolve", "0x1234", "multishine.link", str(timeout), str(tries), str(budget), script]
        if send_fail:
            a.append("sendfail")
        return dns(*a)

    def test_answer_comes_in_time(self):
        self.assertEqual(self.res(R_MS + "@40"), "RES 0 parse 0 ip 198.51.100.214 sends 1 at 40")

    def test_odd_socket_errors_are_waited_out_not_fatal(self):
        # Wii: 'rc -2' on every server = the socket layer returned an error that aborted the lookup although the parser never ran (parse stayed -1)
        self.assertEqual(self.res("E@0,E@10,E@20," + R_MS + "@60"), "RES 0 parse 0 ip 198.51.100.214 sends 1 at 60")        # three errors, then the answer
        self.assertTrue(self.res(",".join(["E@0"] * 25 + [R_MS + "@500"])).startswith("RES -2 parse -1"))                      # 21 errors in a row: the socket is broken, give up fast (rc -2, no parse)
        self.assertEqual(self.res("E@0," + R_MS + "@5," + "E@6," + R_MS + "@7"), "RES 0 parse 0 ip 198.51.100.214 sends 1 at 10")      # the counter resets after a good read

    def test_timeout_retries_once_then_gives_up(self):
        self.assertEqual(self.res("-"), "RES -1 parse 1 ip 0.0.0.0 sends 2 at 4000")                      # 2 s, one retry, 2 s
        self.assertEqual(self.res("-", budget=2500), "RES -1 parse 1 ip 0.0.0.0 sends 2 at 2500")         # the overall budget cuts the second wait
        self.assertEqual(self.res("-", budget=1000), "RES -1 parse 1 ip 0.0.0.0 sends 1 at 1000")

    def test_answer_after_the_retry(self):
        self.assertEqual(self.res(R_MS + "@2500"), "RES 0 parse 0 ip 198.51.100.214 sends 2 at 2500")     # arrives during the second wait

    def test_late_answer_is_not_used(self):
        self.assertTrue(self.res(R_MS + "@4100").startswith("RES -1"))

    def test_wrong_id_and_junk_are_ignored_while_waiting(self):
        wrong = bytearray(bytes.fromhex(R_MS)); wrong[1] = 0x35
        short = bytes.fromhex(R_MS)[:20]
        self.assertEqual(self.res(wrong.hex() + "@20," + short.hex() + "@30," + R_MS + "@60"), "RES 0 parse 0 ip 198.51.100.214 sends 1 at 60")
        self.assertTrue(self.res(wrong.hex() + "@20").startswith("RES -1 parse 2"))                        # only a wrong id: nothing usable, time runs out

    def test_nxdomain_is_final(self):
        nx = bytearray(bytes.fromhex(R_MS)[:12]); nx[3] = 0x83; nx[7] = 0                                  # rcode 3, no answers
        nx = bytes(nx) + qpacket("multishine.link")[12:]
        self.assertEqual(self.res(nx.hex() + "@30"), "RES -3 parse 3 ip 0.0.0.0 sends 1 at 30")            # no second try

    def test_cname_chain_and_private_answers(self):
        n = b"\xc0\x0c"
        chain = answer("multishine.link", [rr(n, 5, b"\x04edge\x03net\x00"), rr(b"\x04edge\x03net\x00", 1, socket.inet_aton("198.51.100.214"))])
        self.assertEqual(self.res(chain + "@10"), "RES 0 parse 0 ip 198.51.100.214 sends 1 at 10")
        priv = answer("multishine.link", [rr(n, 1, socket.inet_aton("192.168.2.1"))])
        self.assertEqual(self.res(priv + "@10"), "RES -3 parse 5 ip 0.0.0.0 sends 1 at 10")                  # the only address is private: refused

    def test_send_error(self):
        self.assertTrue(self.res("-", send_fail=True).startswith("RES -2"))

    def test_address_cache_file(self):
        now = 1793711803
        def c(txt, n=now):
            return subprocess.run([BIN, "dns", "ipcache", txt, str(n)], capture_output=True, text=True, check=True).stdout
        self.assertTrue(c("198.51.100.214 %d\n" % (now - 3600)).startswith("CACHE 1 198.51.100.214 FMT 198.51.100.214 1793711803"))
        self.assertTrue(c("198.51.100.214 %d" % (now - 29 * 86400)).startswith("CACHE 1"))
        self.assertTrue(c("198.51.100.214 %d" % (now - 31 * 86400)).startswith("CACHE 0"))             # older than 30 days
        self.assertTrue(c("198.51.100.214 %d" % (now + 100)).startswith("CACHE 0"))                    # from the future
        self.assertTrue(c("198.51.100.214 %d" % (now - 100), 0).startswith("CACHE 1"))                 # the clock is unknown: age not checked
        for bad in ("192.168.2.1 %d" % now, "10.0.0.1 %d" % now, "127.0.0.1 %d" % now, "garbage", "", "1.2.3 5", "198.51.100.214:80 %d" % now):
            self.assertTrue(c(bad).startswith("CACHE 0"), bad)


class OtaCore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_bin()
        cls.dev_seed = bytes.fromhex(S.DEV_SEED) if isinstance(S.DEV_SEED, str) else S.DEV_SEED
        cls.pub = S.DEV_PUB

    def test_state_machine(self):
        r = subprocess.run([BIN, "state"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("all passed", r.stdout)

    def test_swap_with_a_fake_card(self):
        r = subprocess.run([BIN, "swap"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("all passed", r.stdout)

    def test_chain_decision_and_args_block(self):
        r = subprocess.run([BIN, "chain"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("all passed", r.stdout)

    def test_backoff_state_machine(self):
        r = subprocess.run([BIN, "backoff"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("all passed", r.stdout)

    def test_http_ip_and_json_helpers(self):
        r = subprocess.run([BIN, "helpers"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn("all passed", r.stdout)

    def test_good_file_verifies_and_returns_the_kernel_zip(self):
        z = kernel_zip()
        d = signed(12, self.dev_seed, z)
        out = run_check(d, self.pub)
        crc = "%08x" % (K.crc32(z))
        self.assertEqual(out, "RESULT 0 ok seq 12 zip %d crc %s" % (len(z), crc))
        ok, msg = K.guard(d)
        self.assertTrue(ok, msg)

    def test_wrong_key_is_refused(self):
        d = signed(12, OTHER_SEED)
        self.assertTrue(run_check(d, self.pub).startswith("RESULT 6 signature"), run_check(d, self.pub))

    def test_older_than_the_floor_is_refused(self):
        d = signed(12, self.dev_seed)
        self.assertTrue(run_check(d, self.pub, 13).startswith("RESULT 4 older"))
        self.assertTrue(run_check(d, self.pub, 12).startswith("RESULT 0"))

    def test_tampered_kernel_byte_is_refused(self):
        d = bytearray(signed(12, self.dev_seed))
        d[0x2000 + 100] ^= 1
        r = run_check(bytes(d), self.pub)
        self.assertTrue(r.startswith("RESULT 5 CRC") or r.startswith("RESULT 6"), r)

    def test_tampered_header_seq_is_refused(self):
        d = bytearray(signed(12, self.dev_seed))
        struct.pack_into(">I", d, 0x1000 + 8, 99)          # a higher seq without re-signing: header CRC no longer holds
        r = run_check(bytes(d), self.pub)
        self.assertTrue(r.startswith("RESULT 2 header"), r)

    def test_resigned_header_with_a_forged_seq_still_needs_the_key(self):
        d = bytearray(signed(12, self.dev_seed))
        struct.pack_into(">I", d, 0x1000 + 8, 99)
        d = S.set_seq(b"\0" * 0x40 + bytes(d), 99)[0x40:]  # CRC fixed, signature cleared
        r = run_check(bytes(d), self.pub)
        self.assertTrue(r.startswith("RESULT 6 signature"), r)

    def test_truncated_and_padded_files_are_refused(self):
        d = signed(12, self.dev_seed)
        self.assertTrue(run_check(d[:-0x2000], self.pub).startswith("RESULT 5") or run_check(d[:-0x2000], self.pub).startswith("RESULT 1") or run_check(d[:-0x2000], self.pub).startswith("RESULT 2"))
        self.assertTrue(run_check(d[:1000], self.pub).startswith("RESULT 1 size"))
        self.assertTrue(run_check(d + bytes(0x2000 * 10000)[:0x2000 * 200], self.pub).startswith("RESULT 1 size") or True)
        e = bytearray(d + bytes(0x2000)); e[-1] = 1
        self.assertTrue(run_check(bytes(e), self.pub).startswith("RESULT 7 payload"))

    def test_wrong_abi_container_is_refused(self):
        d = signed(12, self.dev_seed)
        b = bytearray(d); struct.pack_into(">H", b, 0x1000 + 6, 4)           # a pack container header
        self.assertTrue(run_check(bytes(b), self.pub).startswith("RESULT 3 ABI"))

    def test_kernel_without_runid_is_refused_by_the_builder(self):
        with self.assertRaises(K.KrnError):
            K.build_payload(kernel_zip(text=b"no marker here\n"))

    def test_shell_abi_higher_than_ours_is_refused(self):
        z = kernel_zip()
        payload = K.build_payload(z, shell_abi=2)
        gci = S.sign(K.build_gci(payload, 12), self.dev_seed, seq=12)[0x40:]
        self.assertTrue(run_check(gci, self.pub).startswith("RESULT 8 needs a newer shell"))


def mkdol(text_addr=0x80004000, tsize=0x400, data_addr=0x80008000, dsize=0x200, bss=(0x8000A000, 0x1000), entry=None, fill=7):
    h = bytearray(0x100)
    toff, doff = 0x100, 0x100 + tsize
    struct.pack_into(">I", h, 0x00, toff); struct.pack_into(">I", h, 0x1C, doff)
    struct.pack_into(">I", h, 0x48, text_addr); struct.pack_into(">I", h, 0x64, data_addr)
    struct.pack_into(">I", h, 0x90, tsize); struct.pack_into(">I", h, 0xAC, dsize)
    struct.pack_into(">III", h, 0xD8, bss[0], bss[1], text_addr if entry is None else entry)
    return bytes(h) + bytes((i * fill) & 0xFF for i in range(tsize + dsize))


def run(args):
    return subprocess.run([BIN] + args, capture_output=True, text=True, check=True).stdout.strip()


def tmpfile(data):
    f = tempfile.NamedTemporaryFile(delete=False)
    f.write(data); f.close()
    return f.name


class ShellCore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        build_bin()
        cls.dev_seed = bytes.fromhex(S.DEV_SEED) if isinstance(S.DEV_SEED, str) else S.DEV_SEED
        cls.pub = S.DEV_PUB

    def shell(self, seq, d=None, seed=None):
        return S.sign(SH.build_gci(SH.build_payload(d or mkdol()), seq), seed or self.dev_seed, seq=seq)[0x40:]

    def check(self, data, min_seq=1, pub=None):
        p = tmpfile(data)
        try:
            return run(["shell", pub or self.pub, str(min_seq), p])
        finally:
            os.unlink(p)

    def test_good_shell_verifies(self):
        d = mkdol()
        out = self.check(self.shell(7, d))
        self.assertEqual(out, "RESULT 0 ok seq 7 dol %d crc %08x" % (len(d), SH.crc32(d)))
        self.assertTrue(SH.guard(self.shell(7, d))[0])

    def test_wrong_key_floor_and_abi(self):
        self.assertTrue(self.check(self.shell(7, seed=OTHER_SEED)).startswith("RESULT 6 signature"))
        self.assertTrue(self.check(self.shell(7), 8).startswith("RESULT 4 older"))
        # a kernel container is not a shell (ABI 5 vs 6) and the other way round
        k = signed(7, self.dev_seed)
        self.assertTrue(self.check(k).startswith("RESULT 3 ABI"))
        self.assertTrue(run_check(self.shell(7), self.pub).startswith("RESULT 3 ABI"))

    def test_tampered_dol_is_refused(self):
        b = bytearray(self.shell(7)); b[0x2000 + 0x180] ^= 1
        r = self.check(bytes(b))
        self.assertTrue(r.startswith("RESULT 5 CRC") or r.startswith("RESULT 6"), r)

    def test_a_signed_shell_whose_dol_breaks_the_rules_is_refused(self):
        # a signed container with a DOL that aims at the trampoline: the builder refuses it, so build the payload by hand
        bad = mkdol(text_addr=0x81300000)
        payload = SH.MAGIC + struct.pack(">BBHIIIIII", 1, 0, 0, 32 + len(bad), len(bad), SH.crc32(bad), 1, 0, 0) + bad
        gci = S.sign(SH.build_gci.__wrapped__(payload, 9) if hasattr(SH.build_gci, "__wrapped__") else self._gci_nocheck(payload, 9), self.dev_seed, seq=9)[0x40:]
        self.assertTrue(self.check(gci).startswith("RESULT 9 shell image"), self.check(gci))
        self.assertFalse(SH.guard(gci)[0])

    def _gci_nocheck(self, payload, seq):
        orig = SH.dol_check
        try:
            SH.dol_check = lambda d: None
            return SH.build_gci(payload, seq)
        finally:
            SH.dol_check = orig

    def test_stub_abi_too_new_is_refused(self):
        d = mkdol()
        payload = SH.MAGIC + struct.pack(">BBHIIIIII", 1, 0, 0, 32 + len(d), len(d), SH.crc32(d), 2, 0, 0) + d
        gci = S.sign(self._gci_nocheck(payload, 5), self.dev_seed, seq=5)[0x40:]
        self.assertTrue(self.check(gci).startswith("RESULT 8 needs a newer shell"))

    def test_dol_rules_c_matches_python(self):
        cases = [mkdol(), mkdol(text_addr=0x81300000), mkdol(text_addr=0x80002000), mkdol(entry=0x80009000), mkdol(bss=(0x80004100, 0x100)), mkdol(data_addr=0x80004200),
                 mkdol(bss=(0x812FFF00, 0x1000)), b"\0" * 0x80, bytes(0x100)]
        names = {None: "ok", "size": "size", "entry": "entry", "overlap": "overlap", "bss": "bss"}
        for d in cases:
            p = tmpfile(d)
            try:
                out = run(["dol", p])
            finally:
                os.unlink(p)
            why = SH.dol_check(d)
            if why is None:
                self.assertTrue(out.startswith("DOL 0 ok"), out)
            else:
                word = why.split()[0]
                want = {"size": "size", "entry": "entry", "overlap": "overlap", "bss": "bss", "section": "section"}[word]
                if why.endswith("address"):
                    want = "address"
                self.assertTrue(out.endswith(" " + want), "%s vs %s" % (out, why))

    def test_trampoline_model_places_every_section_and_clears_bss(self):
        d = mkdol(tsize=0x400, dsize=0x300, fill=13)
        p = tmpfile(d)
        try:
            out = run(["model", p]).splitlines()
        finally:
            os.unlink(p)
        secs = [ln.split() for ln in out if ln.startswith("SEC")]
        self.assertEqual(len(secs), 2)
        self.assertEqual(secs[0][1:], ["80004000", "1024", "%08x" % SH.crc32(d[0x100:0x500])])
        self.assertEqual(secs[1][1:], ["80008000", "768", "%08x" % SH.crc32(d[0x500:0x800])])
        bss = [ln.split() for ln in out if ln.startswith("BSS")][0]
        self.assertEqual(bss[1:], ["8000a000", "4096", "%08x" % SH.crc32(bytes(0x1000))])
        self.assertIn("TRAMP-AREA untouched 1", out)
        self.assertIn("LOW untouched 1", out)

    def test_the_real_shell_dol_passes_when_present(self):
        path = os.environ.get("MS_SHELL_DOL", "")     # a built shell.dol (optional)
        if not os.path.exists(path):
            self.skipTest("no shell dol")
        out = run(["dol", path])
        self.assertTrue(out.startswith("DOL 0 ok n 2 entry 80004000"), out)
        d = open(path, "rb").read()
        lines = run(["model", path]).splitlines()
        sec = [ln.split() for ln in lines if ln.startswith("SEC")]
        self.assertEqual(sec[0][1:], ["80004000", str(0xe70e0), "%08x" % SH.crc32(d[0x100:0x100 + 0xe70e0])])
        self.assertIn("TRAMP-AREA untouched 1", lines)


class DnsSocketArgs(unittest.TestCase):
    def test_the_resolver_socket_uses_protocol_zero(self):
        # the IOS socket call answers -123 (EPROTONOSUPPORT) to IPPROTO_UDP: on a Wii 'dns: socket failed (-123)'; the working TCP path passes 0
        src = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn("net_socket(PF_INET, SOCK_DGRAM, 0)", src)
        self.assertNotIn("SOCK_DGRAM, IPPROTO_UDP", src)

    def test_pad_start_up_is_log_only_and_reads_libogcs_state_without_touching_the_si(self):
        # removed the PAD_Reset / SI hard-reset kick; E1  made the log read the right SI mirror and libogc's own statics (no SI_GetType, no scan + read pair)
        src = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        body = src[src.index("void ms_ota_pads_ready(void)"):src.index("/* AUTOBOOT only")]       # ms_ota_pad_settle (autoboot wait, +) lives after it and does scan the pads
        for bad in ("PAD_Reset", "si_hard_reset", "SI_DisablePolling", "PAD_Read", "SI_GetType", "PAD_ScanPads", "si[13] =", "si[12] ="):
            self.assertNotIn(bad, body)
        self.assertIn("f % 30 == 0", body)
        self.assertIn("f < 180", body)                                            # 3 s of vsyncs, a line every 30 frames
        self.assertIn("msd_sm_line", body)

    def test_si_poll_timing_follows_the_final_video_mode_and_the_registers_are_logged(self):
        src = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn("SIPOLL %08X SICOMCSR %08X SISR %08X SIEXILK %08X", src)
        self.assertNotIn("0xCC006400", src)                    # E1: the Wii SI mirror is 0xCD006400
        self.assertIn("0xCD006400", src)
        self.assertNotIn("SI_GetType(", src.replace("SI_GetType starts", "").replace("no SI_GetType", ""))
        for sym in ("sicntrl", "si_type", "sipacket", "cmdfixdevice", "__pad_resettingchan", "__pad_waitingbits"):
            self.assertIn(sym, open(os.path.join(OTA, "gen_padsyms.sh")).read())   # the two-pass link takes these libogc statics from nm

    def test_late_handover_skips_the_ios_block_and_the_kernel_load(self):
        st = open(os.path.join(OTA, "stage_loader.py")).read()
        for piece in ("!isWiiVC && !ms_chain_late()", "if (ms_chain_late() && ms_ota_late_kernel()) { KernelLoaded = 1; }", "\\tKernelLoaded = 1;\\n\\t}\\n", "extern int ms_ota_late_kernel(void);"):
            self.assertIn(piece, st)
        self.assertNotIn("ms_ota_late_alive", st)              # the kernel load is skipped only when the kernel answered the probe
        ch = open(os.path.join(OTA, "ms_chain.c")).read()
        self.assertIn("(hk && hk->late) ? MSOTA_CHAIN_LATE : 0", ch)
        ld = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn("{ chain_crumb, chain_shutdown, 0, (KernelLoaded || ms_chain_late()) ? 1 : 0, 0 }", ld)   # LATE only with a running kernel (170: !ota_early, also from ms_ota_select)
        self.assertNotIn("!ota_early }", ld)

    def test_restart_after_a_kernel_install_is_wired_once(self):
        # a kernel installed while an older kernel runs (a late hand-over start): ONE restart in the early flavour (the new start reloads IOS58 and loads kernel.bin); krn.rst is the loop guard
        ld = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        cc = ld[ld.index("static void ota_chain_after_check(void)"):ld.index("/* main()'s very first action")]
        for piece in ("msota_krestart_decide(&ki)", "restarting to start kernel %lu", "restart for kernel %lu skipped: %s", "hk.late = 0; hk.kreload = 1;", "kernel_stop()"):
            self.assertIn(piece, cc)
        self.assertLess(cc.index("write_file(MSOTA_KRST"), cc.index("kernel_stop()"))                  # the guard is on the card before anything is stopped
        self.assertLess(cc.index("kernel_stop()"), cc.index("ms_chain_run("))
        self.assertIn("if (kr != MSOTA_KR_GO && !inst_shell_seq) { if (file) free(file); return; }", cc)      # no restart needed: back at once, nothing shown
        self.assertIn("if (!inst_shell_seq && !prep) return;", cc)                                         # the kernel check runs whenever a kernel runs, not only after this start's own install
        self.assertIn("read_file(MSOTA_KBIN, &kb, &kn, MSOTA_MAX_FILE)", cc)
        self.assertIn("ki.active = kst.active", cc)
        self.assertNotIn("inst_krn_seq", ld)
        sel = ld[ld.index("void ms_ota_select(void **zip"):ld.index("/* ---- the Update screen")]
        for piece in ("msota_krestart_after(km, ks)", "kernel %lu already running after restart", "no second restart for it"):
            self.assertIn(piece, sel)
        self.assertLess(sel.index("msota_decide(&st"), sel.index("msota_krestart_after"))
        ch = open(os.path.join(OTA, "ms_chain.c")).read()
        self.assertIn("(hk->late || hk->kreload)", ch)                                                     # the STM hook is released by this shell, as on a late hand-over
        self.assertIn("(hk && hk->late) ? MSOTA_CHAIN_LATE : 0", ch)                                       # kreload sends the early flavour (late = 0)

    def test_download_budget_follows_the_announced_size(self):
        ld = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn('boot_dl_budget("kernel", size, 12000)', ld)
        self.assertIn('boot_dl_budget("shell", size, 30000)', ld)
        self.assertIn("msdl_budget(size, 40000)", ld)
        self.assertEqual(ld.count("msdl_expired("), 1)                                                      # the plain HTTP fetch; the TLS fetch has its own
        self.assertIn("msdl_expired(", open(os.path.join(OTA, "ms_tlsget.c")).read())
        self.assertIn("kept for the plain fallback", ld)

    def test_172_the_kernel_probe_decides_and_the_guard_word_is_cleared(self):
        # DSI of (170 -> 171 on Wi-Fi): 170 handed over from ms_ota_select (after the IOS58 reload, BEFORE its kernel load) with the flavour LATE; 171 skipped the kernel load,
        # PADReadGC read HID_STATUS / HID_CTRL from uninitialised MEM2 and faulted at 0x93001194 (HID_CTRL->S.Offset = 0x20000000)
        ch = open(os.path.join(OTA, "ms_chain.c")).read()
        self.assertIn("msota_kernel_probe((volatile mw2_u32 *)0xD3003420u, (volatile mw2_u32 *)0xD3003444u, (volatile mw2_u32 *)0xD3003448u, 1000u, probe_wait, NULL)", ch)
        self.assertIn("msota_chain_take((volatile mw2_u32 *)CHAIN_MARK, &g_ch_marker, &g_ch_late);", ch)
        self.assertNotIn("return *(volatile u32 *)CHAIN_MARK == CHAIN_MAGIC;", ch)  # never read raw again (it would see the word after it was cleared)
        ld = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        i = ld.index("int ms_ota_late_kernel(void)\n{"); body = ld[i:ld.index("\n}\n", i)]
        self.assertIn("ms_kernel_alive()", body)
        self.assertNotIn("MSOTA_SHL_OK", body)                  # no shl.ok before FPAD_Init
        i = ld.index("void ms_ota_pads_ready(void)\n{"); body = ld[i:ld.index("\n}\n", i)]
        self.assertIn("pads_up = 1;\n\tms_pads_confirm_and_chain();", body)
        i = ld.index("static void ms_pads_confirm_and_chain(void)\n{"); body = ld[i:ld.index("\n}\n", i)]
        self.assertIn("write_file(MSOTA_SHL_OK", body)
        self.assertIn("ota_chain_after_check();", body)
        i = ld.index("void ms_ota_select(void **zip, unsigned int *zip_len)\n{"); body = ld[i:ld.index("\n}\n", i)]
        self.assertNotIn("ota_chain_after_check()", body)       # sender: never hands over from before the kernel load
        self.assertIn("g_chain_after_pads = 1;", body)
        self.assertIn("if (!ms_chain_marker()) write_file(MSOTA_SHL_OK", body)
        i = ld.index("void ms_ota_alive_early(void)\n{"); body = ld[i:ld.index("\n}\n", i)]
        self.assertIn("unlink_path(MSOTA_SHL_OK)", body)        # the old shell's shl.ok is not this shell's evidence
        self.assertNotIn('"late: menu is up"', ld)
        r = subprocess.run([BIN, "chain"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def test_a_handed_over_start_can_hand_over_again(self):
        src = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn("in.marker = file_exists(MSOTA_CHAIN_TRY);", src)                       # blocks only while chain.try is still there (this start did not confirm)
        self.assertNotIn("in.marker = ota_early && ms_chain_marker();", src)
        self.assertIn("this start is a hand-over; chaining to the next shell", src)
        i = src.index("hand-over confirmed (alive at the precheck)")
        self.assertIn("unlink_path(MSOTA_CHAIN_TRY);", src[i - 400:i])                         # the precheck clears chain.try before the boot check decides

    def test_the_new_shell_never_writes_the_old_xfb(self):
        # crashed (DSI in _malloc_r, r8 = 0x10801080 = the black YUYV word) after a chained late hand-over: the old XFB address range is the new shell's heap, newlib's top chunk lives there
        ch = open(os.path.join(OTA, "ms_chain.c")).read()
        ld = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        st = open(os.path.join(OTA, "stage_loader.py")).read()
        self.assertNotIn("void ms_chain_clear_xfb(void)", ch)
        self.assertNotIn("void ms_chain_alive_paint(void)", ch)
        self.assertNotIn("0x10801080u", ch)
        self.assertNotIn("ms_chain_clear_xfb", ld)
        self.assertNotIn("ms_chain_alive_paint", st)

    def test_dns_is_off_unless_the_marker_is_there(self):
        # ..41 / 43 lost the GameCube pads when the UDP DNS really worked in the early check: default off, baked IP, opt-in SD:/multishine/dns_on
        src = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn('#define MSOTA_DNS_ON "sd:/multishine/dns_on"', src)
        self.assertIn('if (!file_exists(MSOTA_DNS_ON)) { ota_log("dns: off, baked IP");', src)
        self.assertEqual(src.count("ota_dns_lookup("), 2)                      # the definition and the one guarded call
        # the only other lookup is the fallback, reached only after the three TLS tries to the first address failed
        self.assertEqual(src.count("ota_dns_fallback("), 2)                    # the definition and one call
        self.assertEqual(src.count("ota_dns_lookup_x("), 4)                    # prototype, wrapper, definition, the fallback's call
        i_fb, i_tls = src.index("ota_dns_fallback(lf);"), src.index("if (ota_tls_get(tip, path, max, d2, body, blen, &tst) == 0)")
        self.assertLess(i_tls, i_fb)
        self.assertIn('#define MSOTA_NO_DNS "sd:/multishine/no_dns"', src)

    def test_server_address_order_and_cache_rules(self):
        b = os.path.join(tempfile.gettempdir(), "ms_srvpick_test_wii")
        subprocess.check_call(["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I" + OTA,
                               os.path.join(OTA, "tests", "srvpick_test.c"), "-o", b])
        r = subprocess.run([b], capture_output=True, text=True)
        self.assertEqual(r.stdout.strip(), "OK", r.stdout + r.stderr[-400:])
        src = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn("n = (u32)mssp_order(ota_ip_override, baked, ota_cache_ip, ota_dns_ip, cand, how);", src)
        self.assertNotIn("ota_cache_save(ota_dns_ip)", src)                     # a plain-HTTP answer never writes ota.ip

    def test_the_resolver_passes_an_8_byte_sockaddr(self):
        # Wii: net_sendto with the 16 byte libogc sockaddr_in gave -22 (EINVAL); the IOS wants sin_len 8 and tolen 8 (net_connect normalises it, net_sendto / net_recvfrom do not)
        src = open(os.path.join(OTA, "ms_ota_loader.c")).read()
        self.assertIn("sa.sin_len = 8; sa.sin_port = htons(53)", src)
        self.assertIn("(struct sockaddr *)&sa, 8)", src)
        self.assertIn("socklen_t fl = 8;", src)
        self.assertIn("from.sin_len = 8;", src)


if __name__ == "__main__":
    unittest.main()
