# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""MSCF compression: the image of a code file stored as one MSLZ stream (tools/payload_pack/mslz.py; decoder
offline/payload_pack/ms_lz.c, run by the card loader when built with CL_COMP=1).

The stream is read to the TAIL of the hole and decoded in place: compressed bytes at hole_base + gap, gap = hole_size - zpad (zpad = whole blocks of
the stream), so no staging area exists outside the hole.  gap must be large enough that the decoder never writes over a compressed byte it has not
read yet; inplace_gap() computes the exact minimum by walking the stream the way ms_lz_decode does.  stdlib only."""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools", "payload_pack"))
import mslz  # noqa: E402

BLOCK = 0x2000


def compress(image, depth=200):
    z = mslz.compress(image, depth)
    assert mslz.decompress(z, len(image)) == bytes(image)
    return z


inplace_gap = mslz.inplace_gap      # the walk lives next to the stream format (mslz.py) so that compress_best can check a cap without this module


def plan(image, hole_size, depth=200):
    """-> (stream, gap, zpad, min_hole_size).  gap = hole_size - zpad is what the loader uses; raises if the hole is too small."""
    z = mslz.compress_best(image)
    zpad = (len(z) + BLOCK - 1) // BLOCK * BLOCK
    g = inplace_gap(z, len(image))
    minsize = ((max(len(image), g + zpad) + BLOCK - 1) // BLOCK) * BLOCK
    return z, g, zpad, minsize
