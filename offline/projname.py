# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Working project name, read from offline/name.json (single source).

    sys.path.insert(0, <repo>/offline); import projname
    projname.DISPLAY   "Multishine"      human-readable name
    projname.SHORT     <= 20 chars        banner short title
    projname.SLUG      lowercase          images / files / tags
    projname.FILE      alnum-only display name, for generated file names
"""
import json
import pathlib
import re

_PATH = pathlib.Path(__file__).resolve().parent / "name.json"
_cfg = json.loads(_PATH.read_text(encoding="utf-8"))

DISPLAY = _cfg["display"]
SHORT = _cfg["short"]
SLUG = _cfg["slug"]
FILE = re.sub(r"[^A-Za-z0-9]", "", DISPLAY)  # display name without spaces/punctuation, for file names

if len(SHORT) > 20:
    raise ValueError("name.json: 'short' must be <= 20 characters: %r" % SHORT)


def offline_title():
    return DISPLAY + " Offline"


def offline_iso_name(profile):
    return "{}_Offline_{}.iso".format(FILE, profile)


def ladder_title(rung, name):
    return "{} {} {}".format(DISPLAY, rung, name)


def ladder_iso_name(rung, name):
    return "{}_{}_{}.iso".format(FILE, rung, name)
