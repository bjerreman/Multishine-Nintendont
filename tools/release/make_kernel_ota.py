#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""make_kernel_ota: turn the kernel ZIP of a Nintendont-Multishine build into the signed OTA file krn-N.mscf.

  make_kernel_ota.py --kernel-zip $VBBA_OUT/kernel.zip --seq N --key PATH_TO_PROD_KEY --out-dir DIR      (the key never lives in the repo: mscf_sign.py keygen refuses a git work tree)
  make_kernel_ota.py --kernel-zip K.zip --seq N --dev --out-dir DIR                                        LAB: signs with the published RFC 8032 test key (a shell with the dev key only)

The kernel ZIP is loader/data/kernel.zip of the build tree; build_wii.sh copies it to $VBBA_OUT/kernel.zip.  N is the KERNEL sequence: any integer above the last published one (the shell
refuses a lower number than the highest it ever installed); `ls DIR/krn-*.mscf` and the server catalog show the last.  The result is checked the way the shell checks it (signature under the
given key, ABI 5, payload CRCs, the RUNID marker in the kernel) before it is written.  Publish it with:   (the publish step, outside this repository) DIR/krn-N.mscf"""
import argparse, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "card"))
sys.path.insert(0, os.path.join(REPO, "offline", "cardloader"))
import krn_data as K        # noqa: E402
import mscf_sign as S       # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel-zip", required=True)
    ap.add_argument("--seq", type=int, required=True)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--key")
    g.add_argument("--dev", action="store_true")
    ap.add_argument("--out-dir", required=True)
    a = ap.parse_args(argv)
    try:
        gci = K.build_gci(K.build_payload(open(a.kernel_zip, "rb").read()), a.seq)
    except K.KrnError as e:
        raise SystemExit("REFUSED: %s" % e)
    with tempfile.TemporaryDirectory() as t:
        src, dst = os.path.join(t, "k.gci"), os.path.join(t, "k.signed.gci")
        open(src, "wb").write(gci)
        cmd = [sys.executable, os.path.join(REPO, "tools", "card", "mscf_sign.py"), "sign", src, "--out", dst, "--seq", str(a.seq)] + (["--dev"] if a.dev else ["--key", a.key])
        subprocess.check_call(cmd)
        signed = open(dst, "rb").read()
    served = signed[K.GCI_HDR:]
    pubs = [bytes.fromhex(S.DEV_PUB)] if a.dev else [bytes.fromhex(k.split()[1]) for k in open(os.path.join(REPO, "tools", "release", "prod_pubkeys.txt")) if k.strip() and not k.startswith("#")]
    r = S.check(signed, pubs, min_seq=1, abi=K.KRN_ABI)
    ok, msg = K.guard(served)
    if r != "OK" or not ok:
        raise SystemExit("not publishable: signature check %s, %s (is this the production key?)" % (r, msg))
    os.makedirs(a.out_dir, exist_ok=True)
    out = os.path.join(a.out_dir, "krn-%d.mscf" % a.seq)
    open(out, "wb").write(served)
    print("%s: %d bytes, %s\npublish: (the publish step, outside this repository) %s" % (out, len(served), msg, out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
