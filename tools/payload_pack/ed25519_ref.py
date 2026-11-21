# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Ed25519 (RFC 8032, section 5.1) in pure Python.  REFERENCE ONLY: slow (about 5 to 20 ms per operation), not constant time,
not for protecting anything but a test or an offline signing step on our own machine.  Written from the public spec;
tests use the RFC 8032 section 7.1 vectors.  Stdlib only (hashlib.sha512)."""
import hashlib, os

P = 2 ** 255 - 19
Q = 2 ** 252 + 27742317777372353535851937790883648493       # group order L
D = -121665 * pow(121666, P - 2, P) % P
I = pow(2, (P - 1) // 4, P)


def _sha512(b):
    return hashlib.sha512(b).digest()


def _inv(x):
    return pow(x, P - 2, P)


def _recover_x(y, sign):
    if y >= P:
        return None
    x2 = (y * y - 1) * _inv(D * y * y + 1) % P
    if x2 == 0:
        return None if sign else 0
    x = pow(x2, (P + 3) // 8, P)
    if (x * x - x2) % P:
        x = x * I % P
    if (x * x - x2) % P:
        return None
    if (x & 1) != sign:
        x = P - x
    return x


_GY = 4 * _inv(5) % P
_GX = _recover_x(_GY, 0)
G = (_GX, _GY, 1, _GX * _GY % P)          # extended homogeneous coordinates (X, Y, Z, T)
ZERO = (0, 1, 1, 0)


def _add(p, q):
    a = (p[1] - p[0]) * (q[1] - q[0]) % P
    b = (p[1] + p[0]) * (q[1] + q[0]) % P
    c = 2 * p[3] * q[3] * D % P
    d = 2 * p[2] * q[2] % P
    e, f, g, h = b - a, d - c, d + c, b + a
    return (e * f % P, g * h % P, f * g % P, e * h % P)


def _mul(s, p):
    r = ZERO
    while s:
        if s & 1:
            r = _add(r, p)
        p = _add(p, p)
        s >>= 1
    return r


def _eq(p, q):
    return (p[0] * q[2] - q[0] * p[2]) % P == 0 and (p[1] * q[2] - q[1] * p[2]) % P == 0


def _enc(p):
    zi = _inv(p[2])
    x, y = p[0] * zi % P, p[1] * zi % P
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _dec(s):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _recover_x(y, sign)
    if x is None:
        return None
    return (x, y, 1, x * y % P)


def _clamp(h):
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a


def generate_seed():
    """32 random bytes from the OS.  Callers write them only to a path the user names (never into the repo)."""
    return os.urandom(32)


def public_key(seed):
    if len(seed) != 32:
        raise ValueError("seed must be 32 bytes")
    return _enc(_mul(_clamp(_sha512(seed)), G))


def sign(seed, msg):
    if len(seed) != 32:
        raise ValueError("seed must be 32 bytes")
    h = _sha512(seed)
    a = _clamp(h)
    pub = _enc(_mul(a, G))
    r = int.from_bytes(_sha512(h[32:] + msg), "little") % Q
    rs = _enc(_mul(r, G))
    k = int.from_bytes(_sha512(rs + pub + msg), "little") % Q
    s = (r + k * a) % Q
    return rs + int.to_bytes(s, 32, "little")


def verify(pub, msg, sig):
    """True only for a valid signature: 64-byte sig, canonical S < L, R and A decode as curve points, [S]B = R + [k]A."""
    if len(pub) != 32 or len(sig) != 64:
        return False
    a = _dec(pub)
    r = _dec(sig[:32])
    if a is None or r is None:
        return False
    s = int.from_bytes(sig[32:], "little")
    if s >= Q:
        return False
    k = int.from_bytes(_sha512(sig[:32] + pub + msg), "little") % Q
    return _eq(_mul(s, G), _add(r, _mul(k, a)))
