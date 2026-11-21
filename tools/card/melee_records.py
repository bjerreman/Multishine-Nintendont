#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later
"""Melee single-player records (Target Test, Home-Run Contest, 10-Man Melee, Event Matches) in Melee v1.02 NTSC-U's own save (card file 'MultishineMelee'), COPY ONLY.

  melee_records.py show  FILE [--data D]           records of the Melee save in a raw card image / .gci, dataset version vs the sidecar (nothing is written)
  melee_records.py decode FILE [--name N]          the four sets decoded from the save with the game's own layout, as JSON (nothing is written)
  melee_records.py apply --in CARD --out NEW.raw [--set target,hrc,tenman,events] [--source resolved|tasvideos] [--basis rta|tas] [--dry-run] [--force] [--data D] [--template T]
                                                   NEW.raw = CARD with the records; CARD is never modified, --out must be a new path; NEW.raw.records.json is the sidecar

Where the numbers live (vendor/melee + Dolphin).  File offsets are relative to Melee's logical file 1 (GmSaveData, 0x1790 B),
the same space as the coins (melee_coins.py):
  fighter table  x1F2C[25] at 0x6C4, stride 0xAC, indexed by SELKIND (gm_CKindToSelKind, NOT the internal character id); inside a slot x7C is at +0x7C:
    +0x7C u16 flags, bit b0 = 0x8000 (big-endian bit-field: first member = top bit) = Target Test completed (gmMainLib_8015D48C)
    +0x84 s32 Home-Run distance in CENTIMETRES (CSS: toMeters = /100, toFeet = /30.4788; gmMainLib_8015D06C)
    +0x94 u32 Target Test best time in FRAMES, 60/s (gmMainLib_8015D438; shown as h:mm:ss.cc, cc = 99*(frames%60)/59 truncated, gm_80163374)
  record mirror EDBC at 0x30: +0x08 s32 mask of slots with a Target Test best (bit = 1<<selkind), +0x4C s32[25] Target Test best frames, +0xB0 s32[25] Home-Run best
    (same unit as +0x84).  The match writes the mirror, the character-select screen reads the fighter table: both are kept in step (a real clear writes both).
  10-Man Melee (vendor/melee gmmultiman.c gm_801B6F44 + gm_181A.c): +0x98 s32 = best time in FRAMES (gm_GetFrameCount when the 10th opponent falls), flag b1 = 0x4000 in the +0x7C word =
    "completed" (while b1 is clear the word is the KO count reached).  100-Man (+0x9C, b2) and 3-Min/15-Min/Endless/Cruel (+0xA0/+0xA2/+0xA4/+0xA8) are NOT written (no page data).
  Event Matches (gmevent.c onExitVs, mnevent.c): s64 mask of completed events at 0x200 (big-endian, bit k = save index k; gmMainLib_8015CEFC), s32 best[51] at 0x208 + 4*k (gmMainLib_8015CF5C):
    a TIME event stores FRAMES (cap 0x34BBF, lower is better), a KO event stores the KO count (higher is better).  Save index k = gm_803DF918[menu position]; that table is its own inverse
    (swaps 1<->17, 14<->36, 22<->26, 34<->47), so the page's event N (menu position N-1) is stored at k = EVENT_INDEX[N-1].
Dataset: target/hrc by default from the project's other tooling (round-3 'latest date wins'), or the plain TASVideos page (--source tasvideos); tenman and events always from
the project's other tooling (TASVideos, retrieved date inside).  --basis rta (default; the console column, the emulator column only when a row has no console value) or tas.
Older note, target/hrc rows: the project's other tooling, REAL-TIME rows only (tas=false), non-null values only, a row with a resolution (resolved_value, status resolved) uses the resolved value; unresolved cross-check conflicts are skipped and listed (--include-conflicts keeps them).
A record the PLAYER beat on their own save is never made worse (the better of existing and dataset is kept) unless --force.  Stdlib only."""
import argparse
import hashlib
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import melee_coins as MC   # noqa: E402
import vmc_card as C       # noqa: E402
import vmc_gci as G        # noqa: E402

MELEE_NAME = MC.MELEE_NAME
DATA_OFF = MC.DATA_OFF
FD_OFF, STRIDE, NSLOT = 0x6C4, 0xAC, 25
F_FLAGS, F_HRC, F_TARGET = 0x7C, 0x84, 0x94
DONE_BIT = 0x8000
ED_OFF = 0x30
ED_MASK, ED_TARGET, ED_HRC = ED_OFF + 0x08, ED_OFF + 0x4C, ED_OFF + 0xB0
HRC_CAP_CM = 345850                       # 11,347.2 ft (SmashWiki); the game cannot score farther
MAX_FRAMES = 60 * 60 * 60 * 99            # sanity ceiling for a Target Test time
DEFAULT_DATA = os.path.normpath(os.path.join(HERE, "..", "..", "docs", "data", "melee_records.json"))
SELKIND = ["Captain Falcon", "Donkey Kong", "Fox", "Mr. Game & Watch", "Kirby", "Bowser", "Link", "Luigi", "Mario", "Marth", "Mewtwo", "Ness", "Peach",
           "Pikachu", "Ice Climbers", "Jigglypuff", "Samus", "Yoshi", "Zelda/Sheik", "Falco", "Young Link", "Dr. Mario", "Roy", "Pichu", "Ganondorf"]
CATS = {"target": "target_test_time", "hrc": "homerun_distance"}
F_TENMAN, TENMAN_DONE = 0x98, 0x4000     # s32 frames; flag b1 of the +0x7C word
EV_MASK, EV_BEST, EV_N = 0x200, 0x208, 51
EV_CAP_FRAMES = 0x34BBF
EV_KO = (11, 13, 19, 31, 32)              # page event numbers whose score is a KO count (higher is better)
EVENT_INDEX = [0, 17, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 36, 15, 16, 1, 18, 19, 20, 21, 26, 23, 24, 25, 22, 27, 28, 29, 30, 31, 32, 33,
               47, 35, 14, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 34, 48, 49, 50]    # gm_803DF918 (gmevent.c): menu position -> save index
DEFAULT_TV = os.path.normpath(os.path.join(HERE, "..", "..", "docs", "data", "melee_records_tasvideos.json"))
TV_KEYS = {"target": "btt", "hrc": "hrc", "tenman": "tenman", "events": "events"}
ALL_SETS = ("target", "hrc", "tenman", "events")
DEFAULT_SETS = ALL_SETS


class RecError(Exception):
    pass


# -- units ----------------------------------------------------------------------------------------------------------------------------------------------
def frames_from_seconds(sec):
    """Seconds as the leaderboards print them (two decimals) -> frames whose on-screen centiseconds are the same."""
    whole = int(sec + 1e-9)
    cs = int(round((sec - whole) * 100))
    if cs >= 100:
        whole, cs = whole + 1, 0
    best = None
    for r in range(60):
        if int(99.0 * r / 59.0) == cs:
            best = r
            break
    if best is None:                       # the 60 fractions skip some centisecond values: take the nearest one
        best = min(range(60), key=lambda r: abs(99.0 * r / 59.0 - cs))
    return whole * 60 + best


def display_time(frames):
    """What the Target Test screen shows for `frames` (gm_80163374) -> 'h:mm:ss.cc'."""
    s = frames // 60
    return "%d:%02d:%02d.%02d" % (s // 3600, s // 60 % 60, s % 60, int(99.0 * (frames % 60) / 59.0))


def cm_from_metres(m):
    return min(int(round(m * 100)), HRC_CAP_CM)


def feet_text(cm):
    return "%.1f ft" % (cm / 30.4788)


# -- dataset ----------------------------------------------------------------------------------------------------------------------------------------------
def tv_value(rec, basis):
    """One TASVideos row -> seconds / metres / KOs (basis tas, or rta: console value, emulator value only if there is no console one) or None."""
    c = rec.get("tas") if basis == "tas" else (rec.get("rta_console") or rec.get("rta_emulator"))
    return None if not c else c["value"]


def load_dataset(path=None, include_conflicts=False, allow_pal=False, source="resolved", basis="rta", tv_path=None):
    """-> dict(version, sha1, rows={set: {slot: value}}, skipped=[...]).  rows['target'/'hrc'/'tenman']: SELKIND slot -> seconds / metres; rows['events']: save index -> (value, is_ko).
    target/hrc come from the round-3 resolved dataset (source 'resolved', basis 'rta') or from the TASVideos file; tenman and events always from the TASVideos file."""
    if basis not in ("rta", "tas") or source not in ("resolved", "tasvideos"):
        raise RecError("--basis takes rta|tas, --source takes resolved|tasvideos")
    tvp = tv_path or DEFAULT_TV
    try:
        traw = open(tvp, "rb").read()
        tv = json.loads(traw)
    except (OSError, ValueError) as x:
        raise RecError("cannot read the TASVideos dataset %s (%s)" % (tvp, x))
    if source == "resolved" and basis == "rta":
        ds = _load_resolved(path, include_conflicts, allow_pal)
        ds["version"] += " + TASVideos 10-Man/Events retrieved " + str(tv.get("retrieved_date"))
        ds["sha1"] = hashlib.sha1(bytes.fromhex(ds["sha1"]) + hashlib.sha1(traw).digest()).hexdigest()
    else:
        ds = {"version": "TASVideos %s retrieved %s" % (basis, tv.get("retrieved_date")), "sha1": hashlib.sha1(traw).hexdigest(), "asof": tv.get("retrieved_date"),
              "rows": {"target": {}, "hrc": {}}, "skipped": [], "resolved_rows": 0, "pal_used": [], "path": tvp}
        for st in ("target", "hrc"):
            for r in tv[TV_KEYS[st]]:
                v = tv_value(r, basis)
                if v is None:
                    ds["skipped"].append((st, r["character"], "no %s value on the page" % basis))
                elif r["character"] not in SELKIND:
                    ds["skipped"].append((st, r["character"], "unknown character"))
                else:
                    ds["rows"][st][SELKIND.index(r["character"])] = v
    ds["rows"]["tenman"], ds["rows"]["events"] = {}, {}
    for r in tv["tenman"]:
        v = tv_value(r, basis)
        if v is None:
            ds["skipped"].append(("tenman", r["character"], "no %s value on the page" % basis))
        elif r["character"] not in SELKIND:
            ds["skipped"].append(("tenman", r["character"], "unknown character"))
        else:
            ds["rows"]["tenman"][SELKIND.index(r["character"])] = v
    for r in tv["events"]:
        v = tv_value(r, basis)
        n = r["event"]
        if v is None or not 1 <= n <= EV_N:
            ds["skipped"].append(("events", str(n), "no %s value on the page" % basis))
        else:
            ds["rows"]["events"][EVENT_INDEX[n - 1]] = (v, n in EV_KO)
    ds["basis"], ds["source"] = basis, source
    return ds


def _load_resolved(path=None, include_conflicts=False, allow_pal=False):
    """round-3 dataset -> dict(version, sha1, rows={set: {selkind: value}}, skipped=[(category, character, why)])"""
    path = path or DEFAULT_DATA
    try:
        raw = open(path, "rb").read()
        d = json.loads(raw)
    except (OSError, ValueError) as x:
        raise RecError("cannot read the dataset %s (%s)" % (path, x))
    sha1 = hashlib.sha1(raw).hexdigest()
    meta = d.get("meta", {}) if isinstance(d, dict) else {}
    version = d.get("version") or meta.get("version")
    asof = d.get("as_of_date") or meta.get("as_of_date") or meta.get("retrieved") or d.get("built_date")
    label = (str(version) if version else "sha1:" + sha1[:12]) + (" as of " + str(asof) if asof else "")
    xc = {(c.get("category"), c.get("character"), bool(c.get("tas"))): c for c in d.get("cross_checks", [])}
    rows, skipped, used, pal_used = {"target": {}, "hrc": {}}, [], {"resolved": 0}, []
    for r in d["records"]:
        for st, cat in CATS.items():
            if r.get("category") != cat:
                continue
            ch = r.get("character")
            if r.get("tas") is not False:
                continue                                          # real time only, never TAS
            pal = str(r.get("region", "NTSC") or "NTSC").upper().startswith("PAL")
            if pal and not allow_pal:
                skipped.append((cat, ch, "PAL stand-in (use --allow-pal)"))
                continue
            x = xc.get((cat, ch, False), {})
            val, resolved = r.get("value"), False
            # round 3: resolution columns (resolved_value, resolution_basis, status resolved|unresolved) on the row and/or its cross_checks entry
            for src in (r, x):
                if src.get("resolved_value") is not None and str(src.get("status", src.get("resolution_status", "resolved"))).lower() != "unresolved":
                    val, resolved = src["resolved_value"], True
                    break
            if val is None:
                skipped.append((cat, ch, "no value"))
                continue
            status = " ".join(str(v).lower() for v in (x.get("status", ""), r.get("status", ""), r.get("resolution_status", ""), x.get("resolution_status", "")))
            if not resolved and ("conflict" in status or "unresolved" in status) and not include_conflicts:
                skipped.append((cat, ch, "unresolved cross-check conflict"))
                continue
            used["resolved"] += resolved
            if pal:
                pal_used.append((cat, ch))
            r = dict(r, value=val)
            if ch not in SELKIND:
                skipped.append((cat, ch, "unknown character"))
                continue
            rows[st][SELKIND.index(ch)] = r["value"]
    return {"version": label, "sha1": sha1, "asof": asof, "rows": rows, "skipped": skipped, "resolved_rows": used["resolved"], "pal_used": pal_used, "path": path}


# -- reading --------------------------------------------------------------------------------------------------------------------------------------------
def slot_off(s, f):
    return FD_OFF + STRIDE * s + f


def read_slots(dec):
    d = bytes(dec[DATA_OFF:DATA_OFF + MC.FILE1_SIZE])
    out = []
    mask = struct.unpack_from(">i", d, ED_MASK)[0]
    for s in range(NSLOT):
        flags = struct.unpack_from(">H", d, slot_off(s, F_FLAGS))[0]
        out.append({"slot": s, "name": SELKIND[s], "done": bool(flags & DONE_BIT), "target": struct.unpack_from(">I", d, slot_off(s, F_TARGET))[0],
                    "hrc": struct.unpack_from(">i", d, slot_off(s, F_HRC))[0], "ed_target": struct.unpack_from(">i", d, ED_TARGET + 4 * s)[0],
                    "ed_hrc": struct.unpack_from(">i", d, ED_HRC + 4 * s)[0], "ed_done": bool(mask & (1 << s)),
                    "ten_done": bool(flags & TENMAN_DONE), "tenman": struct.unpack_from(">i", d, slot_off(s, F_TENMAN))[0]})
    return out


def read_events(dec):
    """-> [{event: page number 1..51, index: save index, done, value, ko}] (value in frames for a time event, count for a KO event)."""
    d = bytes(dec[DATA_OFF:DATA_OFF + MC.FILE1_SIZE])
    mask = struct.unpack_from(">Q", d, EV_MASK)[0]
    out = []
    for n in range(1, EV_N + 1):
        k = EVENT_INDEX[n - 1]
        out.append({"event": n, "index": k, "done": bool(mask >> k & 1), "value": struct.unpack_from(">i", d, EV_BEST + 4 * k)[0], "ko": n in EV_KO})
    return out


def decode_all(dec):
    """The four record sets as plain data, read with the game's own layout (seconds / metres from the frame and centimetre units)."""
    sl = read_slots(dec)
    return {"target": {r["name"]: (display_time(r["target"]) if r["done"] else None) for r in sl},
            "target_frames": {r["name"]: (r["target"] if r["done"] else None) for r in sl},
            "hrc_cm": {r["name"]: r["hrc"] for r in sl},
            "tenman_frames": {r["name"]: (r["tenman"] if r["ten_done"] else None) for r in sl},
            "events": {e["event"]: (e["value"] if e["done"] else None) for e in read_events(dec)}}


def read_records(data):
    return [(b, read_slots(MC.decode_sector(data[b * C.BLOCK:(b + 1) * C.BLOCK])[1])) for b in MC.file1_sectors(data)]


# -- writing --------------------------------------------------------------------------------------------------------------------------------------------
def patch_dec(dec, ds, sets, force=False, mirror=True, hrc_fields="both"):
    """Patch one decoded sector in place -> list of change strings.  The better existing value is kept unless force."""
    log = []
    base = DATA_OFF
    mask = struct.unpack_from(">i", dec, base + ED_MASK)[0]
    for s in range(NSLOT):
        if "target" in sets and s in ds["rows"]["target"]:
            new = frames_from_seconds(ds["rows"]["target"][s])
            fo = base + slot_off(s, F_FLAGS)
            flags = struct.unpack_from(">H", dec, fo)[0]
            cur = struct.unpack_from(">I", dec, base + slot_off(s, F_TARGET))[0]
            if (flags & DONE_BIT) and cur and cur < new and not force:
                log.append("%s target: kept player's %s (dataset %s)" % (SELKIND[s], display_time(cur), display_time(new)))
            elif not (flags & DONE_BIT) or cur != new:
                struct.pack_into(">H", dec, fo, flags | DONE_BIT)
                struct.pack_into(">I", dec, base + slot_off(s, F_TARGET), new)
                log.append("%s target: %s -> %s" % (SELKIND[s], display_time(cur) if flags & DONE_BIT else "none", display_time(new)))
            if mirror:
                ecur = struct.unpack_from(">i", dec, base + ED_TARGET + 4 * s)[0]
                has = bool(mask & (1 << s))
                tf = struct.unpack_from(">I", dec, base + slot_off(s, F_TARGET))[0]
                if not has or ecur != tf and (force or ecur > tf):
                    struct.pack_into(">i", dec, base + ED_TARGET + 4 * s, tf)
                    mask |= 1 << s
        if "hrc" in sets and s in ds["rows"]["hrc"]:
            new = cm_from_metres(ds["rows"]["hrc"][s])
            fields = []
            if hrc_fields in ("both", "x84"):
                fields.append(base + slot_off(s, F_HRC))
            if hrc_fields in ("both", "xb0") and mirror or hrc_fields == "xb0":
                fields.append(base + ED_HRC + 4 * s)
            for o in fields:
                tag = "x84" if o == base + slot_off(s, F_HRC) else "xB0"
                cur = struct.unpack_from(">i", dec, o)[0]
                if cur > new and not force:
                    log.append("%s home-run[%s]: kept player's %s (dataset %s)" % (SELKIND[s], tag, feet_text(cur), feet_text(new)))
                elif cur != new:
                    struct.pack_into(">i", dec, o, new)
                    log.append("%s home-run[%s]: %s -> %s" % (SELKIND[s], tag, feet_text(cur), feet_text(new)))
        if "tenman" in sets and s in ds["rows"]["tenman"]:
            new = frames_from_seconds(ds["rows"]["tenman"][s])
            fo, vo = base + slot_off(s, F_FLAGS), base + slot_off(s, F_TENMAN)
            flags = struct.unpack_from(">H", dec, fo)[0]
            cur = struct.unpack_from(">i", dec, vo)[0]
            if (flags & TENMAN_DONE) and 0 < cur < new and not force:
                log.append("%s 10-man: kept player's %s (dataset %s)" % (SELKIND[s], display_time(cur), display_time(new)))
            elif not (flags & TENMAN_DONE) or cur != new:
                struct.pack_into(">H", dec, fo, flags | TENMAN_DONE)
                struct.pack_into(">i", dec, vo, new)
                log.append("%s 10-man: %s -> %s" % (SELKIND[s], display_time(cur) if flags & TENMAN_DONE else "none", display_time(new)))
    if "events" in sets:
        emask = struct.unpack_from(">Q", dec, base + EV_MASK)[0]
        for k, (val, ko) in sorted(ds["rows"]["events"].items()):
            n = EVENT_INDEX.index(k) + 1                      # the table is its own inverse: k -> page event number
            new = int(val) if ko else min(frames_from_seconds(val), EV_CAP_FRAMES)
            cur = struct.unpack_from(">i", dec, base + EV_BEST + 4 * k)[0]
            done = bool(emask >> k & 1)
            better = (new > cur) if ko else (cur == 0 or new < cur)
            fmt = (lambda v: "%d KOs" % v) if ko else display_time
            if done and cur == new:
                continue
            if done and cur and not better and not force:
                log.append("event %d: kept player's %s (dataset %s)" % (n, fmt(cur), fmt(new)))
            elif not done or cur != new:
                struct.pack_into(">i", dec, base + EV_BEST + 4 * k, new)
                emask |= 1 << k
                log.append("event %d: %s -> %s" % (n, fmt(cur) if done else "none", fmt(new)))
        struct.pack_into(">Q", dec, base + EV_MASK, emask)
    struct.pack_into(">i", dec, base + ED_MASK, mask)
    return log


def set_records(data, ds, sets, force=False, mirror=True, hrc_fields="both"):
    """-> (whole-file bytes, change log).  Only the two game-data sector copies change, checksums fixed."""
    for x in sets:
        if x not in ALL_SETS:
            raise RecError("unknown record set %r (target, hrc, tenman, events)" % x)
    out = bytearray(data)
    log = []
    for b in MC.file1_sectors(data):
        sl = slice(b * C.BLOCK, (b + 1) * C.BLOCK)
        _, dec = MC.decode_sector(data[sl])
        log = patch_dec(dec, ds, sets, force, mirror, hrc_fields)
        out[sl] = MC.encode_sector(dec)
    bad = G.melee_problems(bytes(out))
    if bad:
        raise RecError("internal check failed: " + "; ".join(bad))
    return bytes(out), log


def parse_sets(text):
    sets = [x for x in (text or ",".join(DEFAULT_SETS)).split(",") if x]
    for x in sets:
        if x not in ALL_SETS:
            raise RecError("--set takes target,hrc,tenman,events (got %r)" % x)
    return sets


def sidecar_data(ds, sets, log, card):
    return {"dataset_version": ds["version"], "dataset_sha1": ds["sha1"], "as_of_date": ds["asof"], "sets": sets, "applied_at": time.strftime("%Y-%m-%d %H:%M:%S"),
            "card": card, "changes": len(log), "pal_stand_ins_written": ["%s %s" % p for p in ds["pal_used"]], "skipped_rows": ["%s %s: %s" % s for s in ds["skipped"]]}


def apply_to_card(in_path, out_path, sets=DEFAULT_SETS, data_path=None, template=None, force=False, name=MELEE_NAME, mirror=True, hrc_fields="both",
                  include_conflicts=False, allow_pal=False, source="resolved", basis="rta"):
    """-> (message, card bytes, sidecar dict, change log).  Never writes in_path."""
    if os.path.realpath(in_path) == os.path.realpath(out_path):
        raise RecError("--out must not be the input card (copy only, never modify a card in place)")
    ds = load_dataset(data_path, include_conflicts, allow_pal, source, basis)
    card = MC.load_card(in_path)
    before = C.Card(bytes(card.img))
    ent = MC.melee_entry(card, name)
    if ent is not None:
        new, log = set_records(card.read(ent), ds, sets, force, mirror, hrc_fields)
        for i, b in enumerate(card.chain(ent)):
            card.img[b * C.BLOCK:(b + 1) * C.BLOCK] = new[i * C.BLOCK:(i + 1) * C.BLOCK]
        msg = "%s: records %s (dataset %s) in the existing save, %d changes" % (name, "+".join(sets), ds["version"], len(log))
    else:
        tpath = template or os.environ.get("MS_MELEE_TEMPLATE") or MC.DEFAULT_TEMPLATE
        if not os.path.exists(tpath):
            raise RecError("the card has no %s yet and no template was found (%s): see melee_coins.py capture" % (name, tpath))
        entry, data = MC.template_gci(tpath, name)
        try:
            G.check_region(card, bytes(entry[:4]))
        except G.GciError as x:
            raise RecError(str(x))
        new, log = set_records(data, ds, sets, force, mirror, hrc_fields)
        entry = bytearray(entry)
        struct.pack_into(">H", entry, 0x36, 0)
        try:
            card.add(bytes(entry), new)
        except C.CardError as x:
            raise RecError(str(x))
        msg = "%s: added from the template with records %s (dataset %s), %d changes" % (name, "+".join(sets), ds["version"], len(log))
    after = C.Card(bytes(card.img))
    for e in before.entries():
        if e["name"] == name:
            continue
        n = after.find(e["name"], e["gamecode"], e["maker"])
        if n is None or after.read(n) != before.read(e) or n["raw"][:0x36] != e["raw"][:0x36]:
            raise RecError("internal check failed: file %r would change; nothing written" % e["name"])
    prob = after.consistency()
    if prob:
        raise RecError("internal check failed: " + "; ".join(prob))
    return msg, bytes(card.img), sidecar_data(ds, sets, log, os.path.basename(out_path)), log


def sidecar_path(card_path):
    return card_path + ".records.json"


def write_sidecar(card_path, side):
    p = sidecar_path(card_path)
    with open(p, "w") as f:
        json.dump(side, f, indent=1)
        f.write("\n")
    return p


# -- CLI ------------------------------------------------------------------------------------------------------------------------------------------------
def cmd_show(a):
    buf = MC._read(a.file)
    try:
        _e, data, info = G.parse_gci(buf)
        where = ".gci %s" % info["name"]
    except G.GciError:
        card = MC.load_card(a.file)
        e = MC.melee_entry(card, a.name)
        if e is None:
            print("%s: no %s on this card" % (a.file, a.name))
            return 0
        data, where = card.read(e), "card file %s" % a.name
    problems = G.melee_problems(data)
    print("%s (%s): %s" % (a.file, where, "checksums OK" if not problems else "; ".join(problems)))
    ds = None
    try:
        ds = load_dataset(a.data)
        print("dataset now: %s" % ds["version"])
    except RecError as x:
        print("dataset: %s" % x)
    sp = sidecar_path(a.file)
    if os.path.exists(sp):
        try:
            s = json.load(open(sp))
            same = ds is not None and s.get("dataset_sha1") == ds["sha1"]
            print("card written with: %s (sha1 %s, %s)%s" % (s.get("dataset_version"), str(s.get("dataset_sha1"))[:12], s.get("applied_at"),
                                                           "  = current" if same else "  = DIFFERENT from the current dataset: re-apply to a copy"))
        except ValueError:
            print("sidecar %s unreadable" % sp)
    else:
        print("no sidecar %s (card not written by this tool, or sidecar not kept)" % sp)
    for b, slots in read_records(data)[:1 if not a.all_copies else None]:
        print("  block %d" % b)
        for r in slots:
            t = display_time(r["target"]) if r["done"] else "-"
            h = "%.1f m (%s)" % (r["hrc"] / 100.0, feet_text(r["hrc"])) if r["hrc"] else "-"
            mir = "" if (r["ed_target"] == (r["target"] if r["done"] else 0) and r["ed_hrc"] == r["hrc"]) else "  [mirror: target %d%s, hrc %d]" % (r["ed_target"], "" if r["ed_done"] else "(no flag)", r["ed_hrc"])
            tm = display_time(r["tenman"]) if r["ten_done"] else "-"
            print("    %-18s target %-11s home-run %-24s 10-man %-11s%s" % (r["name"], t, h, tm, mir))
        _, dec0 = MC.decode_sector(data[b * C.BLOCK:(b + 1) * C.BLOCK])
        evs = ["%d:%s" % (e["event"], ("%d KOs" % e["value"]) if e["ko"] else display_time(e["value"])) if e["done"] else "%d:-" % e["event"] for e in read_events(dec0)]
        print("    events  " + "  ".join(evs))
    return 0 if not problems else 1


def cmd_decode(a):
    buf = MC._read(a.file)
    try:
        _e, data, _info = G.parse_gci(buf)
    except G.GciError:
        card = MC.load_card(a.file)
        e = MC.melee_entry(card, a.name)
        if e is None:
            raise RecError("no %s on this card" % a.name)
        data = card.read(e)
    b = MC.file1_sectors(data)[0]
    _, dec = MC.decode_sector(data[b * C.BLOCK:(b + 1) * C.BLOCK])
    print(json.dumps(decode_all(dec), indent=1))
    return 0


def cmd_apply(a):
    sets = parse_sets(a.set)
    msg, img, side, log = apply_to_card(a.inp, a.out, sets, a.data, a.template, a.force, a.name, not a.no_mirror, a.hrc_fields, a.include_conflicts, a.allow_pal, a.source, a.basis)
    print(msg)
    for l in log:
        print("  " + l)
    for c, ch in side["pal_stand_ins_written"] and [x.split(" ", 1) for x in side["pal_stand_ins_written"]]:
        print("  PAL stand-in written: %s %s" % (c, ch))
    for sk in side["skipped_rows"]:
        print("  skipped: " + sk)
    if a.dry_run:
        print("dry run: nothing written")
        return 0
    MC._write_new(a.out, img, os.stat(a.inp).st_mode & 0o777)
    sp = write_sidecar(a.out, side)
    print("wrote %s (%d bytes) and %s; %s is untouched" % (a.out, len(img), sp, a.inp))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sp = p.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("show"); s.add_argument("file"); s.add_argument("--name", default=MELEE_NAME); s.add_argument("--data"); s.add_argument("--all-copies", action="store_true")
    s.set_defaults(fn=cmd_show)
    s = sp.add_parser("decode"); s.add_argument("file"); s.add_argument("--name", default=MELEE_NAME); s.set_defaults(fn=cmd_decode)
    s = sp.add_parser("apply")
    s.add_argument("--in", dest="inp", required=True); s.add_argument("--out", required=True)
    s.add_argument("--set", default=",".join(DEFAULT_SETS), help="target,hrc,tenman,events (default all four)")
    s.add_argument("--source", choices=("resolved", "tasvideos"), default="resolved", help="target/hrc rows: the round-3 resolved dataset (default) or the plain TASVideos page")
    s.add_argument("--basis", choices=("rta", "tas"), default="rta", help="rta = real-time console column (default); tas = tool-assisted column (forces --source tasvideos)")
    s.add_argument("--dry-run", action="store_true"); s.add_argument("--force", action="store_true", help="overwrite records the player made better than the dataset")
    s.add_argument("--data"); s.add_argument("--template"); s.add_argument("--name", default=MELEE_NAME)
    s.add_argument("--no-mirror", action="store_true", help="do not write the EDBC mirror (x4C/xB0/mask)")
    s.add_argument("--hrc-fields", choices=("both", "x84", "xb0"), default="both", help="experiments only")
    s.add_argument("--include-conflicts", action="store_true")
    s.add_argument("--allow-pal", action="store_true", help="also write PAL stand-in rows (default: NTSC only)")
    s.set_defaults(fn=cmd_apply)
    a = p.parse_args(argv)
    try:
        return a.fn(a)
    except (RecError, MC.CoinError, C.CardError, G.GciError) as x:
        print("melee_records: %s" % x, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
