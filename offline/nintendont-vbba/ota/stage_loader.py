#!/usr/bin/env python3.14
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
"""Stage the kernel OTA sources into a Nintendont build tree (called by build_wii.sh after patch 0028):
  stage_loader.py REPO TREE [--devkey]
copies ms_ota_core.[ch], ms_ota_loader.[ch] (the loader's Update screen REPLACES loader/source/update.c: the stock updater downloads from github.com/FIX94/Nintendont and never runs in this build),
the portable crypto (mw2_crypto.[ch], ms_sha256.[ch], mscf_verify.h; the ONE signed-message definition) into TREE/loader/source, and writes ms_ota_keys.h: the pinned production public keys of
tools/release/prod_pubkeys.txt (A then B).  --devkey adds the published RFC 8032 test key (a LAB build only: a release never carries it; build_wii.sh refuses it with NIN_MS_QUIET)."""
import json, os, re, shutil, sys

DEV_PUB = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"


PRINT_W = r"""void GRRLIB_PrintfTTFW(int x, int y, GRRLIB_ttfFont *myFont, const wchar_t *utf32, unsigned int fontSize, const u32 color) {
	if(myFont == NULL || utf32 == NULL)
		return;

	/* Multishine: proportional advances (FreeType, light hinting, pen in 26.6).  The old font was monospace (10 px per character at size 16) and the loader pads with spaces to
	   line columns up: a run of 2 or more spaces, or one space before a ':', jumps to the next column of that 10 px grid; a glyph the font lacks prints '?'. */
	FT_Face Face = (FT_Face)myFont->face;
	FT_GlyphSlot slot = Face->glyph;
	const int penY = fontSize;
	const long cell = (long)((fontSize * 5u + 4u) / 8u);
	long pen = 0, idx = 0;
	FT_UInt g, prev = 0;
	u8 cR = R(color), cG = G(color), cB = B(color);

	if (FT_Set_Pixel_Sizes(Face, 0, fontSize)) {
		FT_Set_Pixel_Sizes(Face, 0, 12);
	}
	while(*utf32) {
		wchar_t ch = *utf32++;
		if (ch == L' ') {
			long k = 1;
			while (*utf32 == L' ') { utf32++; k++; }
			if (k >= 2 || *utf32 == L':') {
				long col = (idx + k) * cell * 64;
				if (col > pen) pen = col;
				idx += k;
				prev = 0;
				continue;
			}
		}
		g = FT_Get_Char_Index(Face, ch);
		if (!g) g = FT_Get_Char_Index(Face, '?');
		if (myFont->kerning && prev && g) {
			FT_Vector delta;
			FT_Get_Kerning(Face, prev, g, FT_KERNING_DEFAULT, &delta);
			pen += delta.x;
		}
		idx++;
		if (FT_Load_Glyph(Face, g, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT))
			continue;
		DrawBitmap(&slot->bitmap, (int)((pen + 32) >> 6) + slot->bitmap_left + x, penY - slot->bitmap_top + y, cR, cG, cB);
		pen += slot->advance.x;
		prev = g;
	}
}
"""

WIDTH_W = r"""unsigned int GRRLIB_WidthTTFW(GRRLIB_ttfFont *myFont, const wchar_t *utf32, unsigned int fontSize) {
	if(myFont == NULL || utf32 == NULL) {
		return 0;
	}

	/* Multishine: the same advances and column rules as GRRLIB_PrintfTTFW */
	FT_Face Face = (FT_Face)myFont->face;
	const long cell = (long)((fontSize * 5u + 4u) / 8u);
	long pen = 0, idx = 0;
	FT_UInt g, prev = 0;

	if(FT_Set_Pixel_Sizes(Face, 0, fontSize)) {
		FT_Set_Pixel_Sizes(Face, 0, 12);
	}
	while(*utf32) {
		wchar_t ch = *utf32++;
		if (ch == L' ') {
			long k = 1;
			while (*utf32 == L' ') { utf32++; k++; }
			if (k >= 2 || *utf32 == L':') {
				long col = (idx + k) * cell * 64;
				if (col > pen) pen = col;
				idx += k;
				prev = 0;
				continue;
			}
		}
		g = FT_Get_Char_Index(Face, ch);
		if (!g) g = FT_Get_Char_Index(Face, '?');
		if(myFont->kerning && prev && g) {
			FT_Vector delta;
			FT_Get_Kerning(Face, prev, g, FT_KERNING_DEFAULT, &delta);
			pen += delta.x;
		}
		idx++;
		if(FT_Load_Glyph(Face, g, FT_LOAD_TARGET_LIGHT))
			continue;
		pen += Face->glyph->advance.x;
		prev = g;
	}
	return (unsigned int)((pen + 32) >> 6);
}
"""

NAVFIX = r"""/* Multishine: the settings cursor follows the order on screen (ms_rows.h: Network Profile, Auto Boot, the other left rows, the rest of the right column; wraps round).  The loader moves the
   cursor with its own row arithmetic (Down / Up); this runs right after and redoes the move from where the cursor stood, so rows that are gone are never entered.  Called with the move
   direction after a Down / Up and without a move (every frame): then it only makes sure the cursor stands on a visible row. */
static void ms_nav_fix(MenuCtx *ctx, int dir)
{
	extern void ms_cursor_step(int part, int row, int dir, int *np, int *nr);
	static int pp = 0, pr = -1;
	int np, nr;
	if (pr >= 0 && (ctx->settings.settingPart != pp || ctx->settings.posX != pr)) ms_cursor_step(pp, pr, dir ? dir : 1, &np, &nr);      /* the loader moved the cursor: redo it in screen order */
	else ms_cursor_step(ctx->settings.settingPart, ctx->settings.posX, 0, &np, &nr);
	ctx->settings.settingPart = np; ctx->settings.posX = nr;
	pp = np; pr = nr;
}
"""

LEGEND = "\t\t/* Multishine: a short legend of the format colours, only when the list holds a coloured (not 1:1) entry; above the first row, clear of the footer and the mark */\n" \
         "\t\t{ u32 q; int any = 0, lx = MENU_POS_X; static const char *const lnm[6] = { \"1:1\", \"Shrunk\", \"FST\", \"CISO\", \"Multi\", \"Over\" };\n" \
         "\t\t  for (q = 0; q < ctx->games.gamecount; q++) if ((ctx->games.gi[q].Flags & GIFLAG_FORMAT_MASK) != GIFLAG_FORMAT_FULL) any = 1;\n" \
         "\t\t  if (any) { PrintFormat(14, 0x98ABC2FF, lx, 76, \"Colors:\"); lx += (int)GRRLIB_WidthTTF(myFont, \"Colors: \", 14);\n" \
         "\t\t    for (q = 0; q < 6; q++) { PrintFormat(14, DiscFormatColors[q], lx, 76, \"%s\", lnm[q]); lx += (int)GRRLIB_WidthTTF(myFont, lnm[q], 14) + 12; } } }\n"

PRINTINFO = r"""/* the three technical rows (loader title, Multishine build tag with the duty setting, build date and firmware) as text: row i = 0..2; shown on About only with SD:/multishine/dev, logged at every start */
void MsAboutLine(int i, char *out, unsigned n)
{
	const char *consoleType = (isWiiVC ? (IsWiiUFastCPU() ? "WiiVC 5x CPU" : "Wii VC") : (IsWiiUFastCPU() ? "WiiU 5x CPU" : (IsWiiU() ? "Wii U" : "Wii")));
	char dutyText[56], built[24];
	const char *d = __DATE__;
	int w = 0;
	out[0] = 0;
	if (i == 0) {
#ifdef NIN_SPECIAL_VERSION
		snprintf(out, n, "Nintendont Loader v%u.%u" NIN_SPECIAL_VERSION " (%s)", NIN_VERSION>>16, NIN_VERSION&0xFFFF, consoleType);
#else
		snprintf(out, n, "Nintendont Loader v%u.%u (%s)", NIN_VERSION>>16, NIN_VERSION&0xFFFF, consoleType);
#endif
	} else if (i == 1) {
		VbbaDutyText(dutyText, sizeof dutyText);
		snprintf(out, n, MULTISHINE_TAG "%s", dutyText);
	} else {
		for (; *d && w < 20; d++) if (!(*d == ' ' && w && built[w - 1] == ' ')) built[w++] = *d;     /* "Nov  3": one space */
		built[w] = 0;
		snprintf(out, n, "Built %s %s, firmware %u.%u.%u", built, __TIME__, *(vu16*)0x80003140, *(vu8*)0x80003142, *(vu8*)0x80003143);
	}
}

void PrintInfo(void)
{
	/* Multishine: the title rows moved to the About screen (dev marker) and ota.log; screens draw their own title (ms_title) and one footer line (ms_foot) */
}
"""


PLACEHOLDER_IP, PLACEHOLDER_HOST = "192.0.2.1", "server.example"     # documentation values (RFC 5737, RFC 2606): a build without a server profile talks to nobody


def server_values(repo):
    """-> (ip, host, source).  $MS_SERVER_IP + $MS_SERVER_HOST, else the 'online' profile of the private server profile ($MS_SERVER_PROFILE, default
    private/ms_target.local.json: keys server_ip and ota_http_host), else the documentation placeholder with a loud warning.  A release or shell build
    ($NIN_MS_QUIET=1 or $NIN_MS_SHELL_SEQ set) refuses the placeholder unless $MS_SERVER_PLACEHOLDER_OK=1."""
    ip, host, how = os.environ.get("MS_SERVER_IP", "").strip(), os.environ.get("MS_SERVER_HOST", "").strip(), "environment"
    if not (ip and host):
        pf = os.environ.get("MS_SERVER_PROFILE", "") or os.path.join(repo, "private", "ms_target.local.json")
        if os.path.isfile(pf):
            o = json.load(open(pf)).get("online", {})
            ip, host, how = ip or str(o.get("server_ip", "")), host or str(o.get("ota_http_host", "")), "private server profile"
    if not (ip and host):
        if (os.environ.get("NIN_MS_QUIET") == "1" or "NIN_MS_SHELL_SEQ" in os.environ) and os.environ.get("MS_SERVER_PLACEHOLDER_OK") != "1":
            raise SystemExit("REFUSED: a release/shell build needs the update server: set MS_SERVER_IP and MS_SERVER_HOST (or MS_SERVER_PROFILE); "
                             "MS_SERVER_PLACEHOLDER_OK=1 builds one that talks to nobody")
        sys.stderr.write("WARNING: no update server given (MS_SERVER_IP / MS_SERVER_HOST / MS_SERVER_PROFILE): this shell uses the placeholder %s / %s and never finds an update\n"
                         % (PLACEHOLDER_IP, PLACEHOLDER_HOST))
        ip, host, how = PLACEHOLDER_IP, PLACEHOLDER_HOST, "placeholder"
    if not re.fullmatch(r"(\d{1,3}\.){3}\d{1,3}", ip) or any(int(x) > 255 for x in ip.split(".")):
        raise SystemExit("MS_SERVER_IP must be a dotted IPv4 address")
    if not re.fullmatch(r"[A-Za-z0-9]([A-Za-z0-9.-]{0,61}[A-Za-z0-9])?", host):
        raise SystemExit("MS_SERVER_HOST must be a host name")
    return ip, host, how


def main(argv):
    repo, tree = argv[1], argv[2]
    dev = "--devkey" in argv
    src = os.path.join(tree, "loader", "source")
    ota = os.path.join(repo, "offline", "nintendont-vbba", "ota")
    for f in ("ms_ota_core.c", "ms_ota_core.h", "ms_card_core.c", "ms_card_core.h", "ms_probe_core.c", "ms_probe_core.h", "ms_netcfg_core.c", "ms_netcfg_core.h", "ms_netedit.h", "ms_soak.h", "ms_soak_core.c", "ms_soak_core.h", "ms_ota_loader.h", "ms_disc_lock.c", "ms_disc_lock.h", "ms_disc_fp.h", "ms_disc_gate.c", "ms_start.h", "ms_discsearch.h", "ms_browse.h", "ms_ui.c", "ms_progress.h", "ms_say.h", "ms_bak.h", "ms_slotb.h", "ms_driveprobe.h", "ms_speed.h", "ms_vroot.h", "ms_reset.h", "ms_cardroute.h", "ms_srvpick.h", "ms_rows.h", "ms_cfg_force.h", "ms_chain.c", "ms_inject.c", "ms_inject_apply.h", "ms_chain_steps.h", "ms_diag_core.h", "ms_padsyms.h"):
        shutil.copy(os.path.join(ota, f), os.path.join(src, f))
    sys.path.insert(0, os.path.join(repo, "tools", "release"))
    shutil.copy(os.path.join(ota, "ms_inject_tab.h"), os.path.join(src, "ms_inject_tab.h"))     # the Name Entry inject table (data, ota/ms_inject_tab.h)
    shutil.copy(os.path.join(ota, "ms_ota_loader.c"), os.path.join(src, "update.c"))         # the stock updater is replaced, not extended
    if os.path.exists(os.path.join(src, "main.c")):                # the loader tree: the stub's trampoline for the in-place hand-over (the stub tree has its own copy)
        shutil.copy(os.path.join(repo, "offline", "nintendont-vbba", "stub", "tramp.S"), os.path.join(src, "tramp.S"))
        shutil.copy(os.path.join(ota, "ms_tramp2.S"), os.path.join(src, "ms_tramp2.S"))               # the shell's own trampoline (verifies its copy, flash-invalidates the I-cache)
    shutil.copy(os.path.join(repo, "offline", "cardloader", "cl", "mscf_verify.h"), os.path.join(src, "mscf_verify.h"))
    shutil.copy(os.path.join(repo, "offline", "client_net", "mw2_crypto.h"), os.path.join(src, "mw2_crypto.h"))
    c = open(os.path.join(repo, "offline", "client_net", "mw2_crypto.c")).read().replace('#include "../payload_pack/ms_sha256.h"', '#include "ms_sha256.h"')
    open(os.path.join(src, "mw2_crypto.c"), "w").write(c)
    for f in ("ms_sha256.c", "ms_sha256.h", "ms_lz.h", "ms_lz.c"):
        shutil.copy(os.path.join(repo, "offline", "payload_pack", f), os.path.join(src, f))
    shutil.copy(os.path.join(ota, "ms_pack_core.c"), os.path.join(src, "ms_pack_core.c"))        # the Multishine system files built into a new virtual card:
    shutil.copy(os.path.join(ota, "ms_pack_core.h"), os.path.join(src, "ms_pack_core.h"))        # the SAME builder as the game and the system-files builder (offline/seed), staged flat
    for f in ("seed_lean.c", "seed_lean.h", "seed_slp.c", "seed_slp.h", "seed_slp_tab.h"):
        body = open(os.path.join(repo, "offline", "seed", f)).read().replace('#include "../payload_pack/ms_lz.h"', '#include "ms_lz.h"')
        open(os.path.join(src, f), "w").write(body)
    if os.path.exists(os.path.join(src, "main.c")):                # the loader tree: the in-house TLS 1.3 client (offline/net/tls/) and the stream adapter, for the pinned-TLS update fetch
        for f in ("sdtls.c", "sdtls.h", "sdcrypto.c", "sdcrypto.h", "sdc25519.c", "sdc25519.h", "sdc25519_fe.inc", "sdhttp.c", "sdhttp.h"):
            body = open(os.path.join(repo, "offline", "net", "tls", f)).read().replace('"../../client_net/mw2_crypto.h"', '"mw2_crypto.h"')       # the staged crypto sits next to it
            open(os.path.join(src, f), "w").write(body)
        for f in ("ms_tlsget.c", "ms_tlsget.h"):
            shutil.copy(os.path.join(ota, f), os.path.join(src, f))
    pins = []                                                      # the pin of the update server's TLS key (a PUBLIC key; tools/release/ota_tls_pin.txt); none = TLS off in this build
    pf = os.path.join(repo, "tools", "release", "ota_tls_pin.txt")
    if os.path.exists(pf):
        for ln in open(pf):
            ln = ln.split("#", 1)[0].strip()
            if ln:
                n, h = ln.split()[:2] if len(ln.split()) >= 2 else (None, None)
                if not h or not re.fullmatch(r"[0-9a-f]{64}", h):
                    raise SystemExit("bad pin line %r in ota_tls_pin.txt" % ln)
                pins.append(h)
    if os.environ.get("NIN_MS_TLS_PIN"):                           # LAB / compile check: a pin from the environment replaces the file (never for a shipped build)
        pins = [os.environ["NIN_MS_TLS_PIN"].lower()]
        if not re.fullmatch(r"[0-9a-f]{64}", pins[0]):
            raise SystemExit("NIN_MS_TLS_PIN must be 64 hex digits")
    if len(pins) > 4:
        raise SystemExit("at most 4 TLS pins")
    ph = ["/* generated by stage_loader.py from tools/release/ota_tls_pin.txt: the pinned Ed25519 public key(s) of the update server's TLS listener (none = TLS off, plain signed HTTP) */",
          "#ifndef MS_OTA_TLSPIN_H", "#define MS_OTA_TLSPIN_H", "#define MS_OTA_NTLSPIN %du" % len(pins), "static const unsigned char ms_ota_tls_pins[%d][32] = {" % max(1, len(pins))]
    for h in (pins or ["00" * 32]):
        ph.append("\t{ %s }," % ", ".join("0x" + h[i:i + 2] for i in range(0, 64, 2)))
    ph += ["};", "#endif", ""]
    open(os.path.join(src, "ms_ota_tlspin.h"), "w").write("\n".join(ph))
    keys = []
    for ln in open(os.path.join(repo, "tools", "release", "prod_pubkeys.txt")):
        ln = ln.split("#", 1)[0].strip()
        if ln:
            n, h = ln.split()
            if not re.fullmatch(r"[0-9a-f]{64}", h):
                raise SystemExit("bad key line %r" % ln)
            keys.append((n, h))
    if not keys:
        raise SystemExit("no production keys in tools/release/prod_pubkeys.txt")
    if dev:
        keys.append(("DEV (RFC 8032 test vector, lab builds only)", DEV_PUB))
    out = ["/* generated by offline/nintendont-vbba/ota/stage_loader.py: the pinned public keys of the kernel OTA (tools/release/prod_pubkeys.txt)%s */" % (" + the published dev test key" if dev else ""),
           "#ifndef MS_OTA_KEYS_H", "#define MS_OTA_KEYS_H", "static const mw2_u8 ms_ota_pubs[][32] = {"]
    for n, h in keys:
        out.append("\t{ %s },   /* %s */" % (", ".join("0x" + h[i:i + 2] for i in range(0, 64, 2)), n))
    out += ["};", "#define MS_OTA_NPUB %du" % len(keys), "#endif", ""]
    open(os.path.join(src, "ms_ota_keys.h"), "w").write("\n".join(out))
    seq = int(os.environ.get("NIN_MS_SHELL_SEQ", "0") or "0")          # the shell's own OTA sequence (0 = the shell embedded in the stub); tools/release/make_shell_ota.py checks the DOL carries it
    if not 0 <= seq <= 99999999:
        raise SystemExit("NIN_MS_SHELL_SEQ out of range")
    dev_ui = os.environ.get("NIN_MS_DEV_UI") == "1"                # a DEVELOPER loader: the hidden tools (controller / speed / layout / soak pages, the dev marker rows, the launch text) are compiled in; never in a release build
    if dev_ui and os.environ.get("NIN_MS_RELEASE_BUILD") == "1":
        raise SystemExit("NIN_MS_DEV_UI=1 is for developer loaders only, not for a release build")
    open(os.path.join(src, "ms_shell_seq.h"), "w").write("/* generated by stage_loader.py */\n#define MS_SHELL_SEQ %d\n%s" % (seq, "#define MS_DEV_UI 1\n" if dev_ui else ""))
    mode = os.environ.get("NIN_MS_OTA_MODE", "full")             # diagnostic builds: full = the design, nonet = SD step only (no boot check / network), nosd = no SD access and no network before the kernel (stock behaviour)
    if mode not in ("full", "nonet", "nosd"):
        raise SystemExit("NIN_MS_OTA_MODE must be full, nonet or nosd")
    ip, host, how = server_values(repo)                         # the update server the shell talks to: build-time values, never in the public source
    open(os.path.join(src, "ms_ota_server.h"), "w").write("/* generated by stage_loader.py (%s): the update server of this build */\n#ifndef MS_OTA_SERVER_H\n#define MS_OTA_SERVER_H\n"
                                                         "#define MSOTA_IP       \"%s\"\n#define MSOTA_HOSTNAME \"%s\"\n#endif\n" % (how, ip, host))
    open(os.path.join(src, "ms_ota_cfg.h"), "w").write("/* generated by stage_loader.py */\n#define MS_OTA_MODE_%s 1\n#define MS_OTA_MODE_NAME \"%s\"\n" % (mode.upper(), mode))
    mc = os.path.join(src, "main.c")
    if os.path.exists(mc):                                        # the loader tree (not the stub's scratch tree): the 'Waiting for Nintendont...' message becomes the short 'Starting...'
        m = open(mc).read()
        a = 'ShowMessageScreen("Waiting for Nintendont...");'
        if m.count(a) != 1:
            raise SystemExit("main.c: the Waiting for Nintendont anchor is not unique")
        m = m.replace(a, 'ShowMessageScreen("Starting...");')       # was 'Waiting for Nintendont... [<OTA step trail>]': a bring-up breadcrumb that grew to the full screen width (the chain: main: lines in ota.log say how far a start got)
        open(mc, "w").write(m)
    # the disc lock: three anchored edits, each must match exactly once
    def edit(path, anchor, repl, what):
        if not os.path.exists(path):
            return False
        t = open(path).read()
        if t.count(anchor) != 1:
            raise SystemExit("%s: the %s anchor is not unique (%d)" % (os.path.basename(path), what, t.count(anchor)))
        open(path, "w").write(t.replace(anchor, repl))
        return True
    # Name Entry inject ('Inject'): the loader leaves the table before the jump into the multi-DOL launcher, the launcher applies it after the apploader loaded the game (loader tree only)
    mdol = os.path.join(tree, "multidol", "apploader.c")
    if os.path.exists(mc) and os.path.exists(mdol):
        edit(mc, "\t_jmp813();\n", "\t{ extern void ms_inject_arm(void); ms_inject_arm(); }   /* Multishine: the Name Entry inject table (SD:/multishine/inject), applied by the launcher */\n\t_jmp813();\n", "inject arm")
        shutil.copy(os.path.join(ota, "ms_inject_apply.h"), os.path.join(tree, "multidol", "ms_inject_apply.h"))
        # a LATE hand-over (the old shell had reloaded + patched IOS58): skip the IOS reload / patch block; skip the kernel load only when the kernel answers the probe, else load it as on any start
        edit(mc, "extern void _jmp813();\n", "extern void _jmp813();\nextern int ms_chain_late(void);\nextern int ms_ota_late_kernel(void);\nextern int ms_autoboot_off(void);\n", "late decl")
        edit(mc, "\tif(!isWiiVC)\n\t{\n\t\t// Preparing IOS58 Kernel...", "\t{ extern void ms_main_crumb(const char *); ms_main_crumb(ms_chain_late() ? \"precheck done; LATE hand-over: the IOS58 reload / patch block is skipped\" : \"precheck done; IOS58 reload next\"); }\n\tif(!isWiiVC && !ms_chain_late())\n\t{\n\t\t// Preparing IOS58 Kernel...", "late ios block")
        edit(mc, "\t// Preparing Nintendont Kernel...\n", "\tif (ms_chain_late() && ms_ota_late_kernel()) { KernelLoaded = 1; }   /* skipped only when the old shell's kernel ANSWERED the probe (170 also flagged LATE before its kernel load: DSI) */\n\telse {\n\t// Preparing Nintendont Kernel...\n", "late kernel start")
        for old, new in (("Preparing IOS58 Kernel...", "Starting..."), ("Loading IOS58 Kernel...", "Starting..."), ("Preparing Nintendont Kernel...", "Starting..."), ("Loading Nintendont Kernel...", "Starting..."), ("Checking storage devices...", "Checking storage...")):
            edit(mc, 'ShowMessageScreen("%s");' % old, 'ShowMessageScreen("%s");' % new, "start-up message " + old)   # Multishine: short start-up messages
        edit(mc, "\tKernelLoaded = 1;\n", "\tKernelLoaded = 1;\n\t}\n", "late kernel end")
        edit(mc, "\tInitialise(argsboot);\n", "\tInitialise(argsboot);\n\t{ extern void ms_main_crumb(const char *); ms_main_crumb(\"Initialise done\"); }\n", "crumb 1")
        edit(mc, "\tWiiDRC_Init();\n", "\t{ extern void ms_main_crumb(const char *); ms_main_crumb(\"network start and pad devices done, before WiiDRC_Init\"); }\n\tWiiDRC_Init();\n", "crumb 2")
        edit(mc, "\ts32 fd;\n", "\ts32 fd;\n\t{ extern void ms_main_crumb(const char *); ms_main_crumb(\"before the precheck\"); }\n", "crumb 3")
        edit(mc, "\t__exception_setreload(10);\n", "\t{ extern void ms_ota_alive_early(void); ms_ota_alive_early(); }   /* Multishine: handed over to? the first log line, before anything else (NO writes to the old shell's XFB: that memory is the new shell's heap) */\n\t__exception_setreload(10);\n", "alive paint")
        edit(mdol, '#include "apploader.h"\n', '#include "apploader.h"\n#define MSIJ_FLUSH(a, n) sync_before_exec((void *)(a), (n))\n#include "ms_inject_apply.h"\n', "inject include")
        edit(mdol, "\t/* Set entry point from apploader */\n\treturn (u32)appldr_final();", "\t/* Set entry point from apploader */\n\t{ u32 entry = (u32)appldr_final();\n\t  msij_apply((volatile msij_u32 *)MSIJ_AREA);   /* Multishine: the Name Entry inject (a no-op without the table the shell leaves); the game is loaded, nothing of it has run */\n\t  return entry; }", "inject run")
    mm = os.path.join(src, "menu.c")
    edit(mm, "\t// Sort the list alphabetically.\n\t// On Wii, the pseudo-entry for GameCube discs is always",
         "\t/* Multishine disc lock: only the stock NTSC-U Melee v1.02 image stays in the list */\n"
         "\t{ extern int ms_disc_list_ok(const char *id, unsigned rev, unsigned flags, const char *path); int k, w = 0;\n"
         "\t  for (k = 0; k < gamecount; k++) if (ms_disc_list_ok(gi[k].ID, gi[k].Revision, gi[k].Flags, gi[k].Path)) gi[w++] = gi[k];\n"
         "\t  gamecount = w; }\n"
         "\t// Sort the list alphabetically.\n\t// On Wii, the pseudo-entry for GameCube discs is always", "list filter")
    edit(mm, '"WARNING: %s:/games/ contains no GC titles.", GetRootDevice());',
         '"Put your own Melee v1.02 (NTSC-U) disc in %s:/games/.", GetRootDevice());', "empty list text")
    edit(mc, "//Init DI and set correct ID if needed",
         "{ extern void ms_disc_gate(void); ms_disc_gate(); }   /* Multishine disc lock: stock v1.02 only (before anything is started) */\n//Init DI and set correct ID if needed", "game start")
    # the Multishine look: Aileron font, navy background with our mark, brand text colours; anchored edits, each must match exactly once
    data = os.path.join(tree, "loader", "data")
    if os.path.exists(mc) and os.path.isdir(data):
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import gen_shell_theme as T
        T.write(data)
        os.utime(os.path.join(data, "font.zip")); os.utime(os.path.join(data, "background.png"))
        gh = os.path.join(src, "..", "include", "global.h")
        for a, b in (("#define BLACK\t\t\t\t0x000000FF", "#define BLACK\t\t\t\t0xE3EDF7FF   /* Multishine: the body text colour on the navy background (brand #e3edf7) */"),
                     ("#define MAROON\t\t\t\t0x800000FF", "#define MAROON\t\t\t\t0xFF7B7BFF   /* Multishine: error text, a readable red on navy */"),
                     ('#define ARROW_LEFT\t\t\t"\\xE2\\x97\\x80"', '#define ARROW_LEFT\t\t\t"<"           /* Multishine: Aileron has no triangle glyphs */'),
                     ('#define ARROW_RIGHT\t\t\t"\\xE2\\x96\\xB6"', '#define ARROW_RIGHT\t\t\t">"')):
            edit(gh, a, b, "colour / arrow define")
        edit(mm, "#define DARK_GRAY 0x666666FF", "#define DARK_GRAY 0x98ABC2FF   /* Multishine: muted text on navy */", "dark gray")
        for a, b in (("\tBLACK,\t\t// Full\n\t0x551A00FF,\t// Shrunken (dark brown)\n\t0x00551AFF,\t// Extracted FST\n\t0x001A55FF,\t// CISO\n\t0x551A55FF,\t// Multi-Game\n\t0x55551AFF,\t// Oversized (dark yellow)\n",
                      "\tBLACK,\t\t// Full\n\t0xF3DD9AFF,\t// Shrunken (Multishine warning)\n\t0x6FDC9CFF,\t// Extracted FST (ok)\n\t0x37D6FFFF,\t// CISO (accent)\n\t0xC9B5FFFF,\t// Multi-Game\n\t0xF3DD9AFF,\t// Oversized (warning)\n"),
                     ("\t\tu32 cursor_color = BLACK;", "\t\tu32 cursor_color = 0x37D6FFFF;   /* Multishine accent */"),
                     ("\tif (ctx.games.listMax > 15) {\n\t\tctx.games.listMax = 15;", "\tif (ctx.games.listMax > 9) {   /* Multishine: the mark owns the lower third (y 339..411), the footer sits at y 430 */\n\t\tctx.games.listMax = 9;")):
            edit(mm, a, b, "colour table / cursor / list length")
        gr = os.path.join(src, "grrlib.c")      # proportional text with the old 10 px grid kept where the loader pads with spaces; a glyph Aileron lacks prints '?'
        t_gr = open(gr).read()
        for name, body in (("void GRRLIB_PrintfTTFW(", PRINT_W), ("unsigned int GRRLIB_WidthTTFW(", WIDTH_W)):
            k = t_gr.index(name)
            e = t_gr.index("\n}\n", k) + 3
            if t_gr.count(name) != 1:
                raise SystemExit("grrlib.c: %s is not unique" % name)
            t_gr = t_gr[:k] + body + t_gr[e:]
        open(gr, "w").write(t_gr)
        ft = os.path.join(src, "font.c")        # while the launch bar is up only error text (MAROON) is drawn
        edit(ft, "\tGRRLIB_PrintfTTF(x, y, myFont, astr, size, color);", "\t{ extern int ms_ui_quiet; if (ms_ui_quiet && color != MAROON) return; }\n\tGRRLIB_PrintfTTF(x, y, myFont, astr, size, color);", "quiet print")
        edit(gh, "extern GRRLIB_texImg *screen_buffer;", "extern GRRLIB_texImg *screen_buffer;\nvoid ms_list_row(u32 color, int y, const char *name, int disc, const char *id, int selected);\nvoid ms_title(const char *t);\nvoid ms_foot(unsigned disabled, ...);", "list row prototype")
        import re as _re
        def sub_once(path, pattern, repl, what):
            tx = open(path).read()
            n = len(_re.findall(pattern, tx, _re.S))
            if n != 1:
                raise SystemExit("%s: the %s pattern matches %d times" % (os.path.basename(path), what, n))
            open(path, "w").write(_re.sub(pattern, lambda m: repl, tx, flags=_re.S))
        sub_once(mm, r'PrintFormat\(DEFAULT_SIZE, color, MENU_POS_X, gamelist_y,\s*"%50\.50s \[%\.6s\]%s",\s*gi->Name, gi->ID,\s*i == ctx->games\.posX \? ARROW_LEFT : " "\);', "ms_list_row(color, gamelist_y, gi->Name, 0, gi->ID, i == ctx->games.posX);", "list row 1")
        sub_once(mm, r'PrintFormat\(DEFAULT_SIZE, color, MENU_POS_X, gamelist_y,\s*"%46\.46s \(%d\) \[%\.6s\]%s",\s*gi->Name, discNumber\+1, gi->ID,\s*i == ctx->games\.posX \? ARROW_LEFT : " "\);', "ms_list_row(color, gamelist_y, gi->Name, discNumber + 1, gi->ID, i == ctx->games.posX);", "list row 2")
        sub_once(mc, r'PrintFormat\(DEFAULT_SIZE, color, MENU_POS_X, MENU_POS_Y \+ 20\*4 \+ i \* 20, "%50\.50s \[%\.6s\]%s",\s*gi\[i\]\.Name, gi\[i\]\.ID, i == PosX \? ARROW_LEFT : " " \);', "ms_list_row(color, MENU_POS_Y + 20*4 + i * 20, gi[i].Name, 0, gi[i].ID, i == PosX);", "multi-game row")
        sub_once(mm, r'void PrintInfo\(void\)\n\{.*?\n\}\n', PRINTINFO, "PrintInfo")
        sub_once(mm, r'void PrintButtonActions\(const char \*btn_home, const char \*btn_a, const char \*btn_b, const char \*btn_x1\)\n\{.*?\n\}\n',
                 "void PrintButtonActions(const char *btn_home, const char *btn_a, const char *btn_b, const char *btn_x1)\n{\n\t/* Multishine: one footer line instead of the hint column */\n\t(void)btn_home; (void)btn_a; (void)btn_b; (void)btn_x1;   /* the screens call ms_foot themselves */\n}\n", "PrintButtonActions")
        # game list: no legend (it lives on the About screen), title + footer, B = About, Y/2 = Settings
        sub_once(mm, r'\t\t// Print the color codes\.\n.*?DiscFormatColors\[5\], MENU_POS_X\+\(36\*10\), MENU_POS_Y \+ 20\*3, "Over"\);\n', LEGEND, "legend")
        sub_once(mm, r'\t\t\tPrintInfo\(\);\n\t\t\tif \(ctx\.menuMode == 0\)\n\t\t\t\{\n\t\t\t\t// Game List menu\..*?PrintButtonActions\("Go Back", "Select", "Settings", "Update"\);\n\t\t\t\}\n',
                 "\t\t\tms_title(ctx.menuMode == 0 ? \"Select a game\" : \"Settings\");\n\t\t\tif (ctx.menuMode == 0)\n\t\t\t\tms_foot(ctx.games.canBeBooted ? 0u : 2u, \"B\", \"Back\", \"A\", \"Select\", \"Y\", \"Settings\", NULL);\n\t\t\telse\n\t\t\t{\n\t\t\t\textern int ms_net_edit_available(void);\n\t\t\t\tif (ms_net_edit_available()) ms_foot(0, \"B\", \"Back\", \"A\", \"Change\", \"X\", \"Network\", NULL);   /* Wii only: X opens the network profile editor */\n\t\t\t\telse ms_foot(0, \"B\", \"Back\", \"A\", \"Change\", NULL);\n\t\t\t}\n", "list header")
        # the game info box (X on the list: the MD5 hash screen) is gone: the start flow finds and verifies the stock disc by itself (the disc lock and its cache stay).  The X key, the include and the
        # three source files that only it used (ShowGameInfo.c, md5.c, md5_db.c) are removed from the loader tree.
        edit(mm, "\tif(FPAD_X(0))\n\t{\n\t\t// Can we show information for the selected game?\n\t\tif (ctx->games.canShowInfo)\n\t\t{\n\t\t\t// Show game information.\n\t\t\tShowGameInfo(&ctx->games.gi[ctx->games.posX + ctx->games.scrollX]);\n\t\t\tctx->redraw = true;\n\t\t}\n\t}\n\n", "", "game info key")
        edit(mm, '#include "ShowGameInfo.h"\n', "", "game info include")
        for gone in ("ShowGameInfo.c", "md5.c", "md5_db.c"):
            if os.path.exists(os.path.join(src, gone)):
                os.remove(os.path.join(src, gone))
        # the start screen: one stock image = A Launch (or A Set up Multishine without the files), X Browse (the game list), Y Settings, Start About; the old
        # device screen stays as the fallback when no storage device has a games folder.  SelectGame learns two things: it can open on the Settings page (ms_sel_mode 1, B then returns), and it reports a changed setting.
        edit(mm, "static int SelectGame(void)\n{\n", "static int SelectGame(void)\n{\n\textern int ms_sel_mode, ms_sel_saved;\n", "select game decl")
        edit(mm, "\tctx.menuMode = 0;\t// Start in the games list.\n", "\tctx.menuMode = ms_sel_mode;\t// Start in the games list (Multishine: or on the Settings page when the start screen's Y opened it)\n", "select game mode")
        edit(mm, "\tif(ctx.games.canBeBooted)\n\t{\n\t\t// Save the selected game to the configuration.", "\tif (ctx.saveSettings) ms_sel_saved = 1;   /* Multishine: a changed setting reaches the start screen even when no game was chosen */\n\tif(ctx.games.canBeBooted && ctx.selected)   /* Multishine: B leaves the game that was only highlighted alone */\n\t{\n\t\t// Save the selected game to the configuration.", "select game saved")
        edit(mm, "bool SelectDevAndGame(void)\n{\n\t// Select the source device. (SD or USB)\n\tbool SaveSettings = false;\n",
             "bool SelectDevAndGame(void)\n{\n\t// Select the source device. (SD or USB)\n\tbool SaveSettings = false;\n\t{ extern int ms_start_screen(int (*sel)(void), bool *save); if (ms_start_screen(SelectGame, &SaveSettings)) return SaveSettings; }   /* Multishine: the start screen (ms_ota_loader.c); 0 = the device screen below */\n", "start screen hook")
        edit(mc, "ncfg->Config |= NIN_CFG_BBA_EMU;           /* Multishine: the virtual BBA is always on, also for an older nincfg.bin that has it off */\n",
             "ncfg->Config |= NIN_CFG_BBA_EMU;           /* Multishine: the virtual BBA is always on, also for an older nincfg.bin that has it off */\n\t\tif (ncfg->Config & NIN_CFG_AUTO_BOOT) { extern int ms_autoboot_setup_needed(void); ms_autoboot_setup_needed(); }   /* Multishine: no Multishine files yet = no autoboot for this start (the config keeps the setting), the start screen offers the set-up */\n", "autoboot files check")
        edit(mc, "while((ncfg->Config & NIN_CFG_AUTO_BOOT) && i < 1000000)", "while((ncfg->Config & NIN_CFG_AUTO_BOOT) && !ms_autoboot_off() && i < 1000000)", "autoboot wait loop")
        edit(mc, "\tif(!(ncfg->Config & NIN_CFG_AUTO_BOOT))\n\t{\n\t\t// Not autobooting.", "\tif(!(ncfg->Config & NIN_CFG_AUTO_BOOT) || ms_autoboot_off())\n\t{\n\t\t// Not autobooting.", "autoboot branch")
        edit(mc, '\t\tgprintf("Autobooting:\\"%s\\"\\r\\n", ncfg->GamePath );', '\t\t{ extern void ms_autoboot_fix(void); extern int ms_autoboot_saved(void); ms_autoboot_fix(); if (ms_autoboot_saved()) SaveSettings = true; }   /* Multishine: a saved game that is missing or not stock is replaced by the stock image found, and saved */\n\t\tgprintf("Autobooting:\\"%s\\"\\r\\n", ncfg->GamePath );', "autoboot game fix")
        edit(mm, "\t\tif( FPAD_Cancel(0) )\n\t\t{\n\t\t\t// Switch menu modes.",
             "\t\tif (ctx.menuMode == 0 && FPAD_Cancel(0))\n\t\t{\n\t\t\tctx.selected = false;   /* B on the game list: back to the device screen (Start is no longer a back key) */\n\t\t\tbreak;\n\t\t}\n\t\tif (ctx.menuMode == 1 && ms_sel_mode == 1 && FPAD_Cancel(0))\n\t\t{\n\t\t\tctx.selected = false;   /* Settings opened by Y on the start screen: B goes straight back to it */\n\t\t\tbreak;\n\t\t}\n\t\tif ((ctx.menuMode == 1 && FPAD_Cancel(0)) || (ctx.menuMode == 0 && FPAD_Y(0)))\n\t\t{\n\t\t\t// Switch menu modes (Y / 2 opens Settings, B leaves them).", "menu keys")
        edit(mm, "\t\tif( FPAD_Start(0) )\n\t\t{\n\t\t\t// Go back to the Device Select menu.\n\t\t\tctx.selected = false;\n\t\t\tbreak;\n\t\t}\n", "", "game list start key")
        # device screen: title, footer, Start = About, hold B = exit
        edit(mm, '\t\t\tPrintInfo();\n\t\t\tPrintButtonActions("Exit", "Select", NULL, NULL);', '\t\t\tms_title("Select a device");\n\t\t\tms_foot(0, "A", "Select", "Start", "About", "X", "Memory card", "Hold B", "Exit", NULL);\n\t\t\t{ extern void ms_hold_draw(void); ms_hold_draw(); }', "device screen")
        edit(mm, '\t\telse if (FPAD_Start(0))\n\t\t{\n\t\t\tShowMessageScreenAndExit("Returning to loader...", 0);\n\t\t}',
             '\t\telse if (FPAD_Start(0))\n\t\t{\n\t\t\t{ extern void ms_about(void); ms_about(); }   /* Start on the device screen: About (B held for 1 s exits, see below) */\n\t\t\tredraw = true;\n\t\t}\n\t\telse if (FPAD_X(0))\n\t\t{\n\t\t\t{ extern void ms_card_menu(void); ms_card_menu(); }   /* X on the device screen: Download Multishine (the Memory card screen) */\n\t\t\tredraw = true;\n\t\t}', "device keys")
        edit(mm, "\t\tif (redraw)\n\t\t{\n\t\t\tUseSD = ", "\t\t{ extern int ms_hold_b(void); const int h = ms_hold_b(); if (h == 2) ShowMessageScreenAndExit(\"Returning to loader...\", 0); if (h) redraw = true; }   /* Multishine: hold B 1 s = exit (the old Home exit) */\n\t\tif (redraw)\n\t\t{\n\t\t\tUseSD = ", "hold B")
        # autoboot hint: centred by the real text width (it sat at a fixed x of 230)
        edit(os.path.join(src, "main.c"), 'PrintFormat(DEFAULT_SIZE, BLACK, 320 - 90, MENU_POS_Y + 20*10, "B: Cancel Autoboot");',
             'PrintFormat(DEFAULT_SIZE, BLACK, (640 - (int)GRRLIB_WidthTTF(myFont, "B: Cancel Autoboot", DEFAULT_SIZE)) / 2, MENU_POS_Y + 20*10, "B: Cancel Autoboot");', "autoboot hint centring")
        tx = open(mm).read()
        if tx.count("const int len = strlen(msg);\n\tconst int x = (640 - (len*10)) / 2;") != 2:
            raise SystemExit("menu.c: the message centring anchor count is not 2")
        open(mm, "w").write(tx.replace("const int len = strlen(msg);\n\tconst int x = (640 - (len*10)) / 2;", "const int x = (640 - (int)GRRLIB_WidthTTF(myFont, msg, DEFAULT_SIZE)) / 2;"))
        # the launch progress bar: the steps the loader goes through after A, and the kernel's own stage counter
        edit(mc, "{ extern void ms_disc_gate(void); ms_disc_gate(); }", "{ extern void ms_prog_begin(void), ms_prog_step(int); extern void ms_disc_gate(void); ms_prog_begin(); ms_disc_gate(); ms_prog_step(1); }", "progress begin")
        edit(mc, "\t// Save the ISO shift value for multi-game discs.", "\t{ extern void ms_prog_step(int); ms_prog_step(2); }\n\t// Save the ISO shift value for multi-game discs.", "progress read")
        edit(mc, "\t//make sure the cfg gets to the kernel", "\t{ extern void ms_prog_step(int); ms_prog_step(3); }\n\t//make sure the cfg gets to the kernel", "progress config")
        edit(mc, "//set status for kernel to start running", "{ extern void ms_prog_step(int); ms_prog_step(4); }\n//set status for kernel to start running", "progress handover")
        edit(mc, "\t\t\tGRRLIB_Screen2Texture(0, 0, screen_buffer, GX_FALSE); // Copy all status messages", "\t\t\t{ extern void ms_prog_status(s32); ms_prog_status((s32)STATUS_LOADING); }\n\t\t\tGRRLIB_Screen2Texture(0, 0, screen_buffer, GX_FALSE); // Copy all status messages", "progress stages")
        edit(mc, '\t\tPrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20*17, "Nintendont kernel looping, loading game...");', "\t\t{ extern void ms_prog_finish(void); ms_prog_finish(); }", "progress finish")
        edit(mc, "\tbool redraw = true;\n\tClearScreen();\n\tu32 PosX = 0;", "\t{ extern int ms_ui_quiet; ms_ui_quiet = 0; }          /* the multi-game picker is a screen to read */\n\tbool redraw = true;\n\tClearScreen();\n\tu32 PosX = 0;", "multi-game picker")
        # the previous run's debug log (the loader rotates it, patch 0011) moves into SD:/multishine/ so it is not shared with stock Nintendont; the CURRENT log and the ram / shutdown dumps are written by the ARM kernel at the SD root and stay
        edit(mc, 'snprintf(lq, sizeof(lq), "%s:/ndebug.old", GetRootDevice());', 'snprintf(lq, sizeof(lq), "%s:/multishine/ndebug.old", GetRootDevice());\n\t\t{ char md[24]; snprintf(md, sizeof md, "%s:/multishine", GetRootDevice()); f_mkdir_char(md); }', "previous log path")
        edit(mc, "\tInitialise(argsboot);\n", "\tInitialise(argsboot);\n\t{ extern void ms_ota_args(int, char **); ms_ota_args(argc, argv); }\n\t{ extern void ms_ota_net_early(void); ms_ota_net_early(); }   /* Multishine: the network starts first of all, it needs 4+ s */\n", "net early")
        edit(os.path.join(src, "FPad.c"), "\tif( WPAD_Pressed == 0 && PAD_Pressed == 0 && WiiDRC_Pressed == 0 &&", "\t{ extern void ms_fpad_src(u32, u32, u32); ms_fpad_src(WPAD_Pressed, PAD_Pressed, WiiDRC_Pressed); }   /* Multishine: which kind of controller drove the last key (diagnostics screen) */\n\tif( WPAD_Pressed == 0 && PAD_Pressed == 0 && WiiDRC_Pressed == 0 &&", "pad source note")
        edit(mc, "\tReconfigVideo(rmode);\n\tUseSD = (ncfg->Config & NIN_CFG_USB) == 0;", "\tif (ncfg->Config & NIN_CFG_AUTO_BOOT) { extern void ms_ota_pad_settle(void); ms_ota_pad_settle(); }   /* Multishine: autoboot waits (5 s at most) for a controller before the kernel starts */\n\tReconfigVideo(rmode);\n\tUseSD = (ncfg->Config & NIN_CFG_USB) == 0;", "autoboot pad settle")
        edit(mc, "\tFPAD_Init();\n\tFPAD_Update();", "\tFPAD_Init();\n\tFPAD_Update();\n\t{ extern void ms_ota_pads_ready(void); ms_ota_pads_ready(); }", "pads ready")
        edit(mc, "\t/* Wii VC fw.img is pre-patched but Wii/vWii isnt, so we", "\t{ extern void ms_ota_precheck(void); ms_ota_precheck(); }   /* Multishine: the update check runs here, before IOS58 is patched and reloaded (the Homebrew Channel's own IOS and network state) */\n\t/* Wii VC fw.img is pre-patched but Wii/vWii isnt, so we", "ota precheck")
        # the physical disc: the loader has just read its header; the lock checks it there (no extra DI access)
        edit(mc, "\t\t\tmemcpy(&(ncfg->GameID), DIBuf, 4);\n\t\t\tfree(DIBuf);\n\t\t}\n\t}\n\n\tif(SaveSettings)",
             "\t\t\tmemcpy(&(ncfg->GameID), DIBuf, 4);\n\t\t\t{ extern void ms_disc_gate_drive(const unsigned char *hdr); ms_disc_gate_drive(DIBuf); }   /* Multishine disc lock for the disc in the drive */\n\t\t\tfree(DIBuf);\n\t\t}\n\t}\n\n\tif(SaveSettings)", "drive disc check")
        # the settings screen: rows removed or hidden, rows below move up, the cursor skips them, hidden values forced
        edit(mm, "\treturn 127 + 16 * row;\n", "\t{ extern u32 ms_setting_y(u32 row); return ms_setting_y(row); }   /* Multishine: hidden rows have no slot */\n", "SettingY")
        edit(mm, "/**\n * Update the Settings menu.", NAVFIX + "/**\n * Update the Settings menu.", "nav helper")
        edit(mm, "static bool UpdateSettingsMenu(MenuCtx *ctx)\n{\n", "static bool UpdateSettingsMenu(MenuCtx *ctx)\n{\n\t{ extern void ms_cfg_apply(void); ms_cfg_apply(); }   /* Multishine: hidden rows keep their fixed values */\n\tms_nav_fix(ctx, 1);\n", "settings entry")
        t_set = open(mm).read()              # one column: the right column's x and its cursor follow the helper; X (update) is gone from the game settings page
        for pat, rep, nn in ((r"MENU_POS_X ?\+ ?320, SettingY\(ListLoopIndex\)", "ms_set_x1(ListLoopIndex), SettingY(ListLoopIndex)", 10),
                             (r"MENU_POS_X ?\+ ?300, SettingY\(ctx->settings\.posX\)", "ms_set_x1(ctx->settings.posX) - 20, SettingY(ctx->settings.posX)", 3)):
            if len(re.findall(pat, t_set)) != nn:
                raise SystemExit("menu.c: settings column pattern %r matches %d times, not %d" % (pat, len(re.findall(pat, t_set)), nn))
            t_set = re.sub(pat, rep, t_set)
        a_x = "\tif(FPAD_X(0))\n\t{\n\t\t// Start the updater.\n\t\tUpdateNintendont();\n\t\tctx->redraw = 1;\n\t}\n"
        if t_set.count(a_x) != 1:
            raise SystemExit("menu.c: the settings X (update) anchor is not unique")
        a_x_new = "\tif(FPAD_X(0))\n\t{\n\t\t{ extern int ms_net_edit_available(void); extern int ms_net_screen(void); if (ms_net_edit_available() && ms_net_screen()) ctx->saveSettings = true; }   /* Multishine: X = the Network screen (Wii only; true = Network Profile changed, the settings file is saved) */\n\t\tctx->redraw = 1;\n\t}\n"
        t_set = t_set.replace(a_x, a_x_new)   # Multishine: the loader's own Update key is gone from the Settings page (the opt-in diagnostics screen is Y on the About page); X opens the Network editor instead
        t_set = t_set.replace("static bool UpdateSettingsMenu(MenuCtx *ctx)\n{\n", "extern u32 ms_set_x1(u32 row);\nstatic bool UpdateSettingsMenu(MenuCtx *ctx)\n{\n", 1)
        open(mm, "w").write(t_set)
        edit(mm, "\t\tctx->redraw = true;\n\n\t}\n\telse if (FPAD_Up_Repeat(ctx))", "\t\tms_nav_fix(ctx, 1);\n\t\tctx->redraw = true;\n\n\t}\n\telse if (FPAD_Up_Repeat(ctx))", "down")
        edit(mm, "\t\tctx->redraw = true;\n\t}\n\n\tif (FPAD_Left_Repeat(ctx))", "\t\tms_nav_fix(ctx, -1);\n\t\tctx->redraw = true;\n\t}\n\n\tif (FPAD_Left_Repeat(ctx))", "up")
        edit(mm, "\t\t// Redraw the settings menu.\n\t\tu32 ListLoopIndex = 0;", "\t\t// Redraw the settings menu.\n\t\tu32 ListLoopIndex = 0;\n\t\t{ extern int ms_row_part; ms_row_part = 0; }", "left column")
        edit(mm, "\t\tu32 ListLoopIndex = 0;\n\t\t{ extern int ms_row_part; ms_row_part = 0; }", "\t\tu32 ListLoopIndex = 0;\n\t\t{ extern void ms_set_pill(int, int); ms_set_pill(ctx->settings.settingPart, ctx->settings.posX); }   /* Multishine: the selection pill goes under the rows */\n\t\t{ extern int ms_row_part; ms_row_part = 0; }", "settings pill")
        edit(mm, "\t\t\tPrintFormat(MENU_SIZE, cursor_color, MENU_POS_X + 30, SettingY(ctx->settings.posX), ARROW_RIGHT);", "\t\t\t(void)cursor_color;   /* Multishine: the pill marks the row */", "settings arrow 0")
        edit(mm, "\t\t\tPrintFormat(MENU_SIZE, cursor_color, ms_set_x1(ctx->settings.posX) - 20, SettingY(ctx->settings.posX), ARROW_RIGHT);", "\t\t\t(void)cursor_color;", "settings arrow 1")
        edit(mm, '\t\t\tPrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 53 * 6 - 8, MENU_POS_Y + 20 * 6, UseSD ? ARROW_LEFT : "");\n\t\t\tPrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 53 * 6 - 8, MENU_POS_Y + 20 * 7, UseSD ? "" : ARROW_LEFT);\n',
             '\t\t\t{ extern void ms_dev_rows(int, int); ms_dev_rows(MENU_POS_Y + 20 * 6, !UseSD); }   /* Multishine: SD / USB centred, the selected one on a tight pill */\n', "device pill")
        edit(mm, '\t\t\tPrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 47 * 6 - 8, MENU_POS_Y + 20 * 6, " SD  ");\n\t\t\tPrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 47 * 6 - 8, MENU_POS_Y + 20 * 7, "USB  ");\n', '', "device labels")
        edit(mm, "\t\t/** Right column **/\n", "\t\t/** Right column **/\n\t\t{ extern int ms_row_part; ms_row_part = 1; }\n", "right column")
        edit(mm, "\t\t// Draw the cursor.", "\t\t{ extern int ms_row_part; ms_row_part = ctx->settings.settingPart; }\n\t\t// Draw the cursor.", "cursor")
        edit(mm, "\t\t// Print a description for the selected option.", "\t\t{ extern int ms_row_part; ms_row_part = 2; }   /* description lines are plain line numbers */\n\t\t// Print a description for the selected option.", "description")
        gl0 = os.path.join(src, "global.c")
        tg = open(gl0).read()
        for a, b, n in (("static bool bg_isWidescreen", "bool bg_isWidescreen", 1), ("static float bg_xScale", "float bg_xScale", 1), ("static int bg_xPos", "int bg_xPos", 1),
                        ("inline void ClearScreen()\n{\n", "inline void ClearScreen()\n{\n\t{ extern void ms_ui_aspect_refresh(void); ms_ui_aspect_refresh(); }\n", 1),
                        ("GRRLIB_Rectangle(0, 0, 80, 480,", "GRRLIB_Rectangle(0, 0, bg_xPos, 480,", 2), ("GRRLIB_Rectangle(80+480, 0, 80, 480,", "GRRLIB_Rectangle(640 - bg_xPos, 0, bg_xPos, 480,", 2),
                        ("\tif(autoboot == false)\n\t{\n\t\tfor (i=0; i<255; i +=5)", "\t{ extern void ms_ui_aspect_refresh(void); ms_ui_aspect_refresh(); }\n\tif(autoboot == false)\n\t{\n\t\tfor (i=0; i<255; i +=5)", 1)):
            if tg.count(a) != n:
                raise SystemExit("global.c: anchor %r count %d, not %d" % (a, tg.count(a), n))
            tg = tg.replace(a, b)
        open(gl0, "w").write(tg)
        gl = os.path.join(src, "global.c")      # the sides of a widescreen picture and the fade-in were the old light grey
        t = open(gl).read()
        if t.count("RGBA(222, 223, 224, ") != 4:
            raise SystemExit("global.c: the background side colour anchor count is %d, not 4" % t.count("RGBA(222, 223, 224, "))
        open(gl, "w").write(t.replace("RGBA(222, 223, 224, ", "RGBA(10, 20, 36, "))
    print("kernel / shell OTA staged: %d key(s)%s, shell seq %d" % (len(keys), " (DEV KEY INCLUDED)" if dev else "", seq))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
