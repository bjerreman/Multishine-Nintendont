#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
"""Multishine's Homebrew Channel texts, in one place.  The Nintendont loader rewrites meta.xml in its launch dir on every start (main.c updateMetaXml),
so the texts must be compiled into the loader AND be identical in the package's meta.xml.
  ms_meta.py header <tree> <release>        rewrite common/include/Metadata.h and set NIN_SPECIAL_VERSION in NintendontVersion.h of the build tree
  ms_meta.py xml <tree> <release> <out>     write the meta.xml exactly as the loader will (same bytes, so the loader does not rewrite it)"""
import os, re, sys
NOBBA = os.environ.get("NIN_MS_NOBBA") == "1"      # test variant without the network
LAZY = os.environ.get("NIN_MS_LAZY") == "1"
DELAY = bool(os.environ.get("NIN_MS_NETDELAY"))
QUIET = os.environ.get("NIN_MS_QUIET") == "1"      # release candidate: name "Nintendont (Multishine)", release date of the build day
RDATE = os.environ.get("NIN_MS_RELDATE", "20261031000000") if QUIET else "20160710000000"
NAME = ("Nintendont (Multishine)" if QUIET else "Nintendont Multishine") + (" (no network test)" if NOBBA else " (lazy test)" if LAZY else " (delay test)" if DELAY else "")
AUTHOR = "FIX94, crediar, Nigzel"
SHORT = "Nintendont made to run Multishine"
LONG1 = "Multishine is Super Smash Bros. Melee online on GameCube and Wii: www.multishine.link \\r\\n\\r\\n Commiters: GerbilSoft, JoostinOnline, GreyRogue, Howard, Cyan, nastys. Multishine by Nigzel. Project website: https://github.com/bjerreman/Multishine-Nintendont "
LONG2 = "This build of Nintendont is made specifically to run Multishine (memory card route and network)."
XMLH = '<?xml version="1.0" encoding="ISO-8859-1" standalone="yes"?>'
mode, tree, rel = sys.argv[1:4]
if NOBBA: rel += " no network test"
elif LAZY: rel += " lazy test"
elif DELAY: rel += " delay test"
vh = tree + "/common/include/NintendontVersion.h"
v = open(vh, newline='').read()
if mode == "header":
    open(tree + "/common/include/Metadata.h", "w", newline='').write(
        '#ifndef __META_DATA_H__\r\n#define __META_DATA_H__\r\n\r\n'
        '#define META_NAME\t\t\t"%s"\r\n#define META_AUTHOR\t\t\t"%s"\r\n\r\n#define META_LONG1\t\t\t"%s"\r\n#define META_LONG2\t\t\t"%s"\r\n'
        '#define META_SHORT\t\t\t"%s"\r\n\r\n#define META_XML\t\t\t\'%s\'\r\n\r\n#endif\r\n' % (NAME, AUTHOR, LONG1, LONG2, SHORT, XMLH) if False else
        '#ifndef __META_DATA_H__\r\n#define __META_DATA_H__\r\n\r\n'
        '#define META_NAME\t\t\t"%s"\r\n#define META_AUTHOR\t\t\t"%s"\r\n\r\n#define META_LONG1\t\t\t"%s"\r\n#define META_LONG2\t\t\t"%s"\r\n'
        '#define META_SHORT\t\t\t"%s"\r\n\r\n#define META_XML\t\t\t"%s"\r\n\r\n#endif\r\n' % (NAME, AUTHOR, LONG1, LONG2, SHORT, XMLH.replace('"', '\\"')))
    if QUIET:
        mc = tree + "/loader/source/main.c"; m = open(mc, newline='').read()
        assert m.count("<release_date>20160710000000</release_date>") == 1
        open(mc, "w", newline='').write(m.replace("<release_date>20160710000000</release_date>", "<release_date>%s</release_date>" % RDATE))
    assert "NIN_SPECIAL_VERSION" not in re.sub(r'//[^\n]*', '', v)
    nl = "\r\n" if "\r\n" in v else "\n"
    i = v.rindex("#endif")
    v = v[:i] + '#define NIN_SPECIAL_VERSION\t\t\t" Multishine %s"' % rel + nl + nl + v[i:]
    open(vh, "w", newline='').write(v)
else:
    m = re.search(r'#define\s+NIN_VERSION\s+0x([0-9A-Fa-f]+)|NIN_MAJOR_VERSION\s+(\d+)', v)
    ma = re.search(r'NIN_MAJOR_VERSION\s+(\d+)', v).group(1); mi = re.search(r'NIN_MINOR_VERSION\s+(\d+)', v).group(1)
    x = (XMLH + "\r\n<app version=\"1\">\r\n\t<name>%s</name>\r\n\t<coder>%s</coder>\r\n\t<version>%s.%s Multishine %s</version>\r\n"
         "\t<release_date>%s</release_date>\r\n\t<short_description>%s</short_description>\r\n\t<long_description>%s\r\n\r\n%s</long_description>\r\n"
         "\t<no_ios_reload/>\r\n\t<ahb_access/>\r\n</app>\r\n") % (NAME, AUTHOR, ma, mi, rel, RDATE, SHORT, LONG1.replace("\\r\\n", "\r\n"), LONG2)
    open(sys.argv[4], "w", newline='').write(x)
