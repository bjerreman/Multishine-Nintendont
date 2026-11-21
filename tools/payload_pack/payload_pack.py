#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Reference packer / verifier for the signed MSPK container.  Reference and slow (pure Python Ed25519).

  payload_pack.py keygen   --out SEEDFILE            new 32-byte seed (refused inside a git work tree; mode 0600; prints the public key hex)
  payload_pack.py pubkey   SEEDFILE
  payload_pack.py pack     RAW --seed SEEDFILE --seq N --build-id ID [--codeset FILE] [--no-compress] --out FILE
  payload_pack.py verify   FILE --pub HEX [--min-seq N] [--out RAW]
  payload_pack.py info     FILE
  payload_pack.py ratios   FILE...                   size of the file under each candidate algorithm
Nothing is written to a card; no network; keys are only created at a path the user names."""
import argparse, hashlib, json, os, subprocess, sys, zlib, lzma

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import container as K, ed25519_ref as ed, mslz


def in_git_tree(path):
    d = os.path.dirname(os.path.abspath(path)) or "."
    while not os.path.isdir(d):
        d = os.path.dirname(d)
    r = subprocess.run(["git", "-C", d, "rev-parse", "--is-inside-work-tree"], capture_output=True, text=True)
    return r.returncode == 0 and r.stdout.strip() == "true"


def ratios(data):
    return {"raw": len(data), "mslz": len(mslz.compress(data)), "lzss12": len(mslz.lzss12_compress(data)),
            "zlib9": len(zlib.compress(data, 9)), "lzma9e": len(lzma.compress(data, preset=9 | lzma.PRESET_EXTREME))}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    k = sp.add_parser("keygen"); k.add_argument("--out", required=True)
    p = sp.add_parser("pubkey"); p.add_argument("seed")
    q = sp.add_parser("pack"); q.add_argument("raw"); q.add_argument("--seed", required=True); q.add_argument("--seq", type=int, required=True)
    q.add_argument("--build-id", required=True); q.add_argument("--codeset"); q.add_argument("--no-compress", action="store_true"); q.add_argument("--out", required=True)
    v = sp.add_parser("verify"); v.add_argument("file"); v.add_argument("--pub", required=True); v.add_argument("--min-seq", type=int, default=0); v.add_argument("--out")
    i = sp.add_parser("info"); i.add_argument("file")
    r = sp.add_parser("ratios"); r.add_argument("files", nargs="+")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "keygen":
            if in_git_tree(a.out):
                print("refused: key path is inside a git work tree", file=sys.stderr); return 2
            fd = os.open(a.out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            seed = ed.generate_seed()
            with os.fdopen(fd, "wb") as f: f.write(seed)
            print(ed.public_key(seed).hex())
        elif a.cmd == "pubkey":
            print(ed.public_key(open(a.seed, "rb").read()).hex())
        elif a.cmd == "pack":
            cs = hashlib.sha256(open(a.codeset, "rb").read()).digest() if a.codeset else b"\0" * 32
            blob = K.pack(open(a.raw, "rb").read(), open(a.seed, "rb").read(), a.seq, a.build_id, cs, not a.no_compress)
            open(a.out, "wb").write(blob)
            print(json.dumps({k2: v2 for k2, v2 in K.parse(blob).items() if k2 not in ("header", "sig", "stored")}, indent=1))
        elif a.cmd == "verify":
            info, raw = K.verify(open(a.file, "rb").read(), bytes.fromhex(a.pub), a.min_seq)
            if a.out: open(a.out, "wb").write(raw)
            print("OK build_seq=%d build_id=%s raw=%d" % (info["build_seq"], info["build_id"], len(raw)))
        elif a.cmd == "info":
            print(json.dumps({k2: v2 for k2, v2 in K.parse(open(a.file, "rb").read()).items() if k2 not in ("header", "sig", "stored")}, indent=1))
        elif a.cmd == "ratios":
            for f in a.files: print(f, json.dumps(ratios(open(f, "rb").read())))
    except (K.PackError, OSError, ValueError) as x:
        print("error: %s" % x, file=sys.stderr); return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
