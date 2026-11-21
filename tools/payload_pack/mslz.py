# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""MSLZ: LZ4-style byte-oriented block format (Python reference compressor and decompressor) plus an Okumura LZSS-12 for ratio
comparison.  The console decoder is offline/payload_pack/ms_lz.c (freestanding C89, a few hundred bytes of code).

MSLZ stream = sequences of: token (high nibble = literal count, low nibble = match length - 4; a nibble of 15 is followed by extra
bytes, each added, until one is < 255), the literals, then (unless the stream ends here) a 16-bit little-endian offset 1..65535.
The stream ends right after a literal run (the last sequence has no match).  Stdlib only; reference speed."""

MINMATCH = 4
MAXOFF = 65535


def _ext(n):
    out = bytearray()
    while n >= 255:
        out.append(255)
        n -= 255
    out.append(n)
    return out


def compress(data, depth=48):
    data = bytes(data)
    n = len(data)
    out = bytearray()
    heads = {}
    prev = [0] * n if n else []
    ip = anchor = 0

    def key(i):
        return data[i:i + 4]

    def insert(i):
        if i + 4 <= n:
            k = key(i)
            prev[i] = heads.get(k, -1)
            heads[k] = i

    def find(i):
        if i + MINMATCH > n:
            return 0, 0
        best_len = best_off = 0
        c = heads.get(key(i), -1)
        tries = depth
        while c >= 0 and i - c <= MAXOFF and tries:
            if data[c + best_len:c + best_len + 1] == data[i + best_len:i + best_len + 1] or best_len == 0:
                l = 0
                while i + l < n and data[c + l] == data[i + l]:
                    l += 1
                if l > best_len:
                    best_len, best_off = l, i - c
            c = prev[c]
            tries -= 1
        return (best_len, best_off) if best_len >= MINMATCH else (0, 0)

    while ip < n:
        ml, off = find(ip)
        if ml and ip + 1 < n:                       # one step of lazy matching
            insert(ip)
            ml2, off2 = find(ip + 1)
            if ml2 > ml + 1:
                ip += 1
                ml, off = ml2, off2

        if not ml:
            insert(ip)
            ip += 1
            continue
        lit = ip - anchor
        tok = (min(lit, 15) << 4) | min(ml - MINMATCH, 15)
        out.append(tok)
        if lit >= 15:
            out += _ext(lit - 15)
        out += data[anchor:ip]
        out += bytes((off & 255, off >> 8))
        if ml - MINMATCH >= 15:
            out += _ext(ml - MINMATCH - 15)
        end = ip + ml
        for j in range(ip, min(end, n - 3)):
            if not (heads.get(key(j)) == j):
                insert(j)
        ip = anchor = end
    lit = n - anchor
    out.append(min(lit, 15) << 4)
    if lit >= 15:
        out += _ext(lit - 15)
    out += data[anchor:]
    return bytes(out)


# ---- optimal-parse encoder (tools/payload_pack/mslz_opt.c): same stream format, same decoder, fewer bytes -----------------------------------
_OPT_BIN = None


def _opt_binary():
    """path of the compiled mslz_opt helper (built once per source revision into build/tools/mslz_opt/<sha8>/), or None when there is no C compiler"""
    global _OPT_BIN
    if _OPT_BIN is not None:
        return _OPT_BIN or None
    import hashlib, os, shutil, subprocess, sys
    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(here, "mslz_opt.c")
    cc = shutil.which(os.environ.get("CC", "cc")) or shutil.which("clang") or shutil.which("gcc")
    if os.environ.get("MSLZ_ENCODER") == "ref" or not cc or not os.path.exists(src):
        _OPT_BIN = ""
        return None
    h = hashlib.sha1(open(src, "rb").read()).hexdigest()[:8]
    d = os.path.join(here, "..", "..", "build", "tools", "mslz_opt", h)
    exe = os.path.join(d, "mslz_opt")
    if not os.path.exists(exe):
        os.makedirs(d, exist_ok=True)
        tmp = exe + ".%d" % os.getpid()
        r = subprocess.run([cc, "-O2", "-std=c99", "-o", tmp, src], capture_output=True)
        if r.returncode:
            sys.stderr.write("mslz: cannot build the optimal encoder (%s); using the reference compressor\n" % r.stderr.decode(errors="replace")[:200])
            _OPT_BIN = ""
            return None
        os.replace(tmp, exe)
    _OPT_BIN = exe
    return exe


def compress_best(data, cap=0, depth=1024):
    """Smallest MSLZ stream the optimal-parse encoder finds; cap > 0: the smallest stream that decodes in place with
    gap + len <= cap (None when none exists).  Without a C compiler (or with MSLZ_ENCODER=ref) falls back to compress(); a capped request then returns the
    reference stream only if it meets the cap, else None.  The decoder is the unchanged ms_lz_decode."""
    import os, subprocess, tempfile
    data = bytes(data)
    exe = _opt_binary() if data else None
    if exe is None:
        z = compress(data, 200)
        if cap and inplace_gap(z, len(data)) + len(z) > cap:
            return None
        return z
    with tempfile.TemporaryDirectory() as t:
        fi, fo = os.path.join(t, "in"), os.path.join(t, "out")
        with open(fi, "wb") as f:
            f.write(data)
        r = subprocess.run([exe, fi, fo, str(depth), str(cap)], capture_output=True)
        if r.returncode == 3:
            return None
        if r.returncode:
            raise LZError("mslz_opt failed: %s" % r.stderr.decode(errors="replace")[:200])
        with open(fo, "rb") as f:
            z = f.read()
    assert decompress(z, len(data)) == data
    return z


def inplace_gap(z, out_len):
    """Smallest g such that decoding z in place with the stream at dst + g never overwrites an unread stream byte (writes at dst offset p must satisfy
    p < g + ip for the stream position ip reached when the write happens).  Walks the tokens exactly as ms_lz_decode does."""
    ip = op = 0
    n = len(z)
    need = 0
    while True:
        tok = z[ip]; ip += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = z[ip]; ip += 1; lit += b
                if b != 255:
                    break
        # literal copy: byte i is read from ip+i then written at op+i: needs op + i <= g + ip + i  (equal is fine: already read)
        need = max(need, op - ip)
        ip += lit; op += lit
        if ip == n:
            break
        ip += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = z[ip]; ip += 1; ml += b
                if b != 255:
                    break
        ml += 4
        # match copy: every unread byte is at >= g + ip, writes reach op + ml - 1: needs op + ml <= g + ip
        need = max(need, op + ml - ip)
        op += ml
    assert op == out_len, (op, out_len)
    return max(need, 0)


class LZError(Exception):
    pass


def decompress(src, max_out):
    src = bytes(src)
    out = bytearray()
    ip, n = 0, len(src)
    while True:
        if ip >= n:
            raise LZError("truncated: no token")
        tok = src[ip]; ip += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                if ip >= n:
                    raise LZError("truncated length")
                b = src[ip]; ip += 1
                lit += b
                if b != 255:
                    break
        if ip + lit > n:
            raise LZError("literals run past the input")
        if len(out) + lit > max_out:
            raise LZError("output would exceed the limit")
        out += src[ip:ip + lit]; ip += lit
        if ip == n:
            return bytes(out)
        if ip + 2 > n:
            raise LZError("truncated offset")
        off = src[ip] | (src[ip + 1] << 8); ip += 2
        if off == 0 or off > len(out):
            raise LZError("bad offset")
        ml = tok & 15
        if ml == 15:
            while True:
                if ip >= n:
                    raise LZError("truncated length")
                b = src[ip]; ip += 1
                ml += b
                if b != 255:
                    break
        ml += MINMATCH
        if len(out) + ml > max_out:
            raise LZError("output would exceed the limit")
        for _ in range(ml):
            out.append(out[-off])


# ---- Okumura LZSS (4096 window, match 3..18, 8 flag bits per group): comparison only -------------------------------------
def lzss12_compress(data, depth=32):
    data = bytes(data); n = len(data)
    out = bytearray(); heads = {}; prev = [0] * n
    ip = 0; group = []; 
    def flush():
        nonlocal group
        if not group: return
        flags = 0
        for i, (isl, _) in enumerate(group):
            if isl: flags |= 1 << i
        out.append(flags)
        for _, b in group: out.extend(b)
        group = []
    def ins(i):
        if i + 3 <= n:
            k = data[i:i + 3]; prev[i] = heads.get(k, -1); heads[k] = i
    while ip < n:
        bl = bo = 0
        if ip + 3 <= n:
            c = heads.get(data[ip:ip + 3], -1); t = depth
            while c >= 0 and ip - c <= 4095 and t:
                l = 0
                while l < 18 and ip + l < n and data[c + l] == data[ip + l]: l += 1
                if l > bl: bl, bo = l, ip - c
                c = prev[c]; t -= 1
        if bl >= 3:
            group.append((0, bytes((bo & 255, ((bo >> 8) << 4) | (bl - 3)))))
            for j in range(ip, ip + bl): ins(j)
            ip += bl
        else:
            group.append((1, bytes((data[ip],)))); ins(ip); ip += 1
        if len(group) == 8: flush()
    flush()
    return bytes(out)
