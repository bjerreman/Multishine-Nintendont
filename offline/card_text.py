# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""The two comment lines (what the GameCube / Wii memory card manager, Swiss and Dolphin show) of every card file WE create: ONE table.

Builders import it; the overlay repair pass
(upd_card.c upc_icon_repair) uses the generated header the project's other tooling (the project's other tooling; the project's other tooling fails when the header
drifts from this table).  Each line: plain ASCII, at most 31 characters (32 byte field, NUL padded).  No build numbers, nothing private.
KNOWN_OLD = the exact (line1, line2) pairs older builds wrote; the repair pass rewrites a file ONLY when both lines match one of them (anything else, e.g. a DEV-SIGNED watermark or a
foreign file, is left alone).  Files whose header Melee owns (MultishineMelee, the loader save SuperSmashBros0110290334) and the invisible MultishineBackup marker are not in here.
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import projname  # noqa: E402

S = projname.SHORT
NEEDED = "Needed to play. Do not delete."

# suffix (card file = SHORT + suffix) -> (line1, line2)
TEXT = {
    "Code":     (S + " game code", NEEDED),
    "CodeB":    (S + " game code (old)", "Backup of the previous build"),
    "Ovl":      (S + " menus", NEEDED),
    "OvlB":     (S + " menus (old)", "Backup of the previous build"),
    "State":    (S + " boot guard", NEEDED),
    "Pack":     (S + " game files", "Data for menus and stages"),
    "Options":  (S + " settings", "No account data in here"),
    "Online":   (S + " online login", "Private. Do not share."),
    "Identity": (S + " identity", "Private. Do not share."),
    "Codes":    (S + " friend codes", "Direct play history"),
    "Diag":     (S + " diagnostics", "Safe to delete"),
    "Advisory": (S + " update notice", "Safe to delete"),
    "Chat":     (S + " quick chat", "Your chat phrases"),
}

KNOWN_OLD = {
    "Code":     [(S, "Game code")],
    "CodeB":    [(S, "Game code")],
    "Ovl":      [(S, "Menus")],
    "OvlB":     [(S, "Menus")],
    "State":    [(S, "Build floor")],
    "Pack":     [(S, "Disc file pack")],
    "Options":  [(S, "settings, no account data"), (S + " options", "settings, no account data")],
    "Online":   [(S, "Online setup (secret)")],
    "Identity": [(S, "Identity (keep private)"), (S + " identity", "DO NOT SHARE")],
    "Codes":    [(S, "Direct code history")],
    "Diag":     [(S, "network report, no secrets")],
    "Advisory": [(S, "Update notice")],
    "Chat":     [],
}

for _k, (_a, _b) in TEXT.items():
    for _s in (_a, _b):
        assert len(_s) <= 31 and all(32 <= ord(c) < 127 for c in _s), (_k, _s)


def pair(suffix):
    """-> (line1, line2) str for a card file SHORT + suffix"""
    return TEXT[suffix]


def put(block, suffix):
    """write the two comment lines of `suffix` into block 0 (bytearray), NUL padded to 32 bytes each"""
    a, b = TEXT[suffix]
    block[0:32] = a.encode("ascii").ljust(32, b"\0")
    block[32:64] = b.encode("ascii").ljust(32, b"\0")


def suffix_of(name):
    """card file name -> table key, or None"""
    if name.startswith(S) and name[len(S):] in TEXT:
        return name[len(S):]
    return None


# The overlay repair pass (upd_card.c upc_icon_repair) works through these, in this order.  flags: 1 = also gives a missing icon (files the card engine creates without one),
# 2 = only while MultishineCodeB exists (the loader's fallback if a torn block-0 write ever damaged the active file).  Ovl / OvlB are NOT rewritten in place (no fallback for the
# overlay container): their text arrives with the next overlay OTA.
REPAIR = [("Options", 1), ("Codes", 1), ("Online", 1), ("Diag", 1), ("Advisory", 0), ("Chat", 0), ("Identity", 1), ("State", 0), ("Pack", 0), ("CodeB", 0), ("Code", 2)]
