#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
"""Writes the Multishine default nincfg.bin (struct NIN_CFG of the pinned Nintendont, big-endian PowerPC layout).
Same values as the first-run defaults in the patched loader/source/main.c: emulated memory card, virtual BBA, progressive scan,
Language AUTO, MaxPads 4, MemCardBlocks 4 (1019 blocks).  Bit positions and version are read from CommonConfig.h.
usage: gen_nincfg.py <CommonConfig.h> <out nincfg.bin>"""
import re, struct, sys
h = open(sys.argv[1]).read()
def num(name):
    m = re.search(r'\b%s\s*=?\s*\(?\s*(0x[0-9A-Fa-f]+|\d+)' % name, h); return int(m.group(1), 0)
ver = int(re.search(r'#define\s+NIN_CFG_VERSION\s+(0x[0-9A-Fa-f]+|\d+)', h).group(1), 0)
maxpad = int(re.search(r'#define\s+NIN_CFG_MAXPAD\s+(\d+)', h).group(1))
cfg = (1 << num('NIN_CFG_BIT_MEMCARDEMU')) | (0 if len(sys.argv) > 3 and sys.argv[3] == 'nobba' else (1 << num('NIN_CFG_BIT_BBA_EMU'))) | (1 << num('NIN_CFG_BIT_LOG')) | (1 << num('NIN_CFG_BIT_REMLIMIT'))   # REMLIMIT = Unlock Read Speed (on: the tested configuration); LOG: /ndebug.log on the SD (alpha: testers send it with a report)
vid = 1 << num('NIN_VID_INDEX_PROG')
b = struct.pack('>IIIII', 0x01070CF6, ver, cfg, vid, 0xFFFFFFFF) + bytes(255) + bytes(255) + bytes(2)   # Language -1 = AUTO; GamePath, CheatPath, pad
b += struct.pack('>II', maxpad, 0) + struct.pack('>BbbB', 4, 0, 0, 0) + struct.pack('>I', 0)           # MaxPads, GameID, MemCardBlocks, VideoScale, VideoOffset, NetworkProfile, WiiUGamepadSlot
assert len(b) == 548, len(b)
open(sys.argv[2], 'wb').write(b)
