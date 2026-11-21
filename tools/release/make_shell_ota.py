#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""make_shell_ota: turn the SHELL DOL of a Nintendont-Multishine build into the signed OTA file shl-N.mscf.

  make_shell_ota.py --dol shell.dol --seq N --key PATH_TO_PROD_KEY --out-dir DIR        (the key never lives in the repo: mscf_sign.py keygen refuses a git work tree)
  make_shell_ota.py --dol shell.dol --seq N --dev --out-dir DIR                          LAB: signs with the published RFC 8032 test key (a stub built with the dev key only)

shell.dol is the Nintendont loader DOL that build_wii.sh produces (the same file that is embedded in the stub as the fallback shell).  N is the SHELL sequence: any integer above the last
published one (the stub refuses a lower number than the highest it ever installed).  The result is checked the way the stub checks it (signature under the given key, ABI 6, payload CRCs,
the DOL rules) before it is written.  Publish with:   (the publish step, outside this repository) DIR/shl-N.mscf
The STUB (apps/Nintendont-Multishine/boot.dol) is never published: it is copied by hand once."""
import argparse, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "card"))
sys.path.insert(0, os.path.join(REPO, "offline", "cardloader"))
import shl_data as K        # noqa: E402
import mscf_sign as S       # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--seq", type=int, required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--key")
    g.add_argument("--dev", action="store_true")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--allow-untagged", action="store_true", help="LAB: accept a DOL without the MSSHELLSEQ tag")
    a = ap.parse_args(argv)
    dol = open(a.dol, "rb").read()
    tag = re.search(rb"MSSHELLSEQ:(\d+)\x00", dol)           # the loader build carries its own sequence (build_wii.sh with NIN_MS_SHELL_SEQ=N): a shell that does not know its number would re-download itself
    if not a.allow_untagged and (not tag or int(tag.group(1)) != a.seq):
        raise SystemExit("REFUSED: the DOL carries %s but --seq is %d: build it with NIN_MS_SHELL_SEQ=%d" % ("tag %s" % tag.group(1).decode() if tag else "no MSSHELLSEQ tag", a.seq, a.seq))
    try:
        gci = K.build_gci(K.build_payload(dol), a.seq)
    except K.ShlError as e:
        raise SystemExit("REFUSED: %s" % e)
    with tempfile.TemporaryDirectory() as t:
        src, dst = os.path.join(t, "s.gci"), os.path.join(t, "s.signed.gci")
        open(src, "wb").write(gci)
        cmd = [sys.executable, os.path.join(REPO, "tools", "card", "mscf_sign.py"), "sign", src, "--out", dst, "--seq", str(a.seq)] + (["--dev"] if a.dev else ["--key", a.key])
        subprocess.check_call(cmd)
        signed = open(dst, "rb").read()
    served = signed[K.GCI_HDR:]
    pubs = [bytes.fromhex(S.DEV_PUB)] if a.dev else [bytes.fromhex(k.split()[1]) for k in open(os.path.join(REPO, "tools", "release", "prod_pubkeys.txt")) if k.strip() and not k.startswith("#")]
    r = S.check(signed, pubs, min_seq=1, abi=K.SHL_ABI)
    ok, msg = K.guard(served)
    if r != "OK" or not ok:
        raise SystemExit("not publishable: signature check %s, %s (is this the production key?)" % (r, msg))
    os.makedirs(a.out_dir, exist_ok=True)
    out = os.path.join(a.out_dir, "shl-%d.mscf" % a.seq)
    open(out, "wb").write(served)
    print("%s: %d bytes, %s\npublish: (the publish step, outside this repository) %s" % (out, len(served), msg, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
