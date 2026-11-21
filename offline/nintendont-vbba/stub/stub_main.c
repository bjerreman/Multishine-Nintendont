/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-2.0-only (it is linked with libogc / libfat and carries a Nintendont loader DOL as its embedded fallback shell) */
/* The Multishine Nintendont STUB: the ONE file copied to SD:/apps/Nintendont-Multishine/boot.dol by hand and never overwritten by any update.
 * It does only this, in this order:
 *   1. mount the SD card (libfat, SD only), read SD:/multishine/shl.st (the shell's rollback state) and SD:/multishine/shell.dol (a downloaded shell, a signed MSCF container ABI 6);
 *   2. verify the container with the pinned release keys (Ed25519) and the DOL rules (msdol_check), apply the rollback rules (msota_decide: a downloaded shell that never wrote the
 *      'alive' marker shl.ok in two starts is dropped and renamed shell.bad), write shl.st, delete the marker (the shell writes it again when it starts);
 *   3. copy the chosen DOL (the verified downloaded one, or the EMBEDDED shell inside this file) into MEM2, shut the SD card down, recreate the Homebrew Channel argument block and jump
 *      into it through a tiny trampoline (tramp.S) that copies the sections to their addresses, clears the bss and starts the entry point.
 * Any failure at any step means the EMBEDDED shell.  There is no network code, no screen and no setting here: the less this file does, the less can ever go wrong with it. */
#include <gccore.h>
#ifndef MS_STUB_TEST
#include <fat.h>
#include <sdcard/wiisd_io.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ms_ota_core.h"
#include "ms_ota_keys.h"

#define DIR_  "sd:/multishine"
#define SHL_BIN "sd:/multishine/shell.dol"
#define SHL_BAD "sd:/multishine/shell.bad"
#define SHL_ST  "sd:/multishine/shl.st"
#define SHL_OK  "sd:/multishine/shl.ok"

#define TRAMP_ADDR 0x81300000u           /* the trampoline runs here (MSDOL_HI: no shell section may land at or above it; the Nintendont loader itself parks its game launcher there much later) */
#define ARGS_ADDR  0x81308000u           /* the command line of the Homebrew Channel, copied here for the shell (above MSDOL_HI too) */
#define ARGS_MAX   0x1000u

extern const u8 ms_embedded_shell[];     /* shell_dol.S: the loader DOL this stub falls back to */
extern const u32 ms_embedded_shell_len;
extern void ms_tramp_start(void), ms_tramp_end(void);

struct tramp_sec { u32 src, dst, len; };
struct tramp_args { u32 entry, n, bss, bss_len; struct tramp_sec sec[MSDOL_NSEC]; };    /* tramp.S reads this layout */

/* memory from the bottom of the MEM2 arena (the Nintendont loader that starts next re-initialises the arenas itself) */
#ifdef MS_STUB_TEST                      /* the emulator test (stub/tests/chain_test.sh): a GameCube build with no SD card and no MEM2: the buffers come from MEM1 at 0x80C00000 */
static void *alloc2(u32 size)
{
	static u32 next = 0x80C00000u;
	u8 *p = (u8 *)next;
	next += (size + 31u) & ~31u;
	return p;
}
#else
static void *alloc2(u32 size)
{
	u8 *p = (u8 *)(((u32)SYS_GetArena2Lo() + 31u) & ~31u);
	u8 *hi = (u8 *)SYS_GetArena2Hi();
	size = (size + 31u) & ~31u;
	if (!p || p + size > hi) return NULL;
	SYS_SetArena2Lo(p + size);
	return p;
}
#endif

static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (!f) return 0; fclose(f); return 1; }

/* read a whole file into MEM2 memory (the shell is copied out of MEM1-landing ranges later, so the source must not live in MEM1) */
static u8 *read_mem2(const char *path, u32 max, u32 *len)
{
	FILE *f = fopen(path, "rb");
	u32 size;
	u8 *b;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END); size = (u32)ftell(f); fseek(f, 0, SEEK_SET);
	if (size == 0 || size > max) { fclose(f); return NULL; }
	b = (u8 *)alloc2(size);
	if (!b) { fclose(f); return NULL; }
	if (fread(b, 1, size, f) != size) { fclose(f); return NULL; }
	fclose(f);
	*len = size;
	return b;
}

static int small_read(const char *path, u8 *buf, u32 max, u32 *len)
{
	FILE *f = fopen(path, "rb");
	size_t n;
	if (!f) return -1;
	n = fread(buf, 1, max, f);
	fclose(f);
	*len = (u32)n;
	return 0;
}

static void write_small(const char *path, const u8 *b, u32 n)
{
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fwrite(b, 1, n, f);
	fclose(f);
}

/* the Homebrew Channel's raw argument block for the shell: magic, commandLine, length (argc / argv are left for crt0 to derive, exactly as the Channel provides it) */
static void restore_args(int argc, char **argv)
{
	struct __argv *a = (struct __argv *)0x80001808;
	char *dst = (char *)ARGS_ADDR;
	u32 len = 0;
	int i;
	if (argc < 1 || !argv || !argv[0]) return;
	for (i = 0; i < argc; i++) {
		u32 l = (u32)strlen(argv[i]) + 1u;
		if (len + l + 1u > ARGS_MAX) break;
		memcpy(dst + len, argv[i], l);
		len += l;
	}
	dst[len++] = 0;
	DCFlushRange(dst, len);
	a->argvMagic = ARGV_MAGIC;
	a->commandLine = dst;
	a->length = (int)len;
	a->argc = 0; a->argv = NULL; a->endARGV = NULL;
	DCFlushRange(a, sizeof *a);
}

/* copy the DOL (already in MEM2) through the trampoline into MEM1 and run it; never returns */
static void jump(const u8 *dol, u32 len, int argc, char **argv)
{
	struct msdol_plan plan;
	struct tramp_args *args;
	u32 code_len = (u32)((u8 *)ms_tramp_end - (u8 *)ms_tramp_start), i;
	void (*go)(const struct tramp_args *);
	if (msdol_check(dol, len, &plan) != MSDOL_OK) return;
	args = (struct tramp_args *)alloc2(sizeof *args);
	if (!args || code_len > 0x1000u) return;
	args->entry = plan.entry; args->n = plan.n; args->bss = plan.bss; args->bss_len = plan.bss_len;
	for (i = 0; i < plan.n; i++) { args->sec[i].src = (u32)dol + plan.sec[i].src; args->sec[i].dst = plan.sec[i].dst; args->sec[i].len = plan.sec[i].len; }
	DCFlushRange(args, sizeof *args);
	DCFlushRange((void *)dol, len);
	restore_args(argc, argv);
	memcpy((void *)TRAMP_ADDR, (void *)ms_tramp_start, code_len);
	DCFlushRange((void *)TRAMP_ADDR, code_len);
	ICInvalidateRange((void *)TRAMP_ADDR, code_len);
	go = (void (*)(const struct tramp_args *))TRAMP_ADDR;
	go(args);
}

int main(int argc, char **argv)
{
	struct msota_state st;
	struct msota_shell sh;
	u8 sb[MSOTA_ST_LEN + 8];
	u32 n = 0, flen = 0;
	u8 *file = NULL, *dol = NULL;
	u32 dol_len = 0;
	int mounted, file_ok = 0, pick = MSOTA_PICK_EMBEDDED, action = 0, confirmed = 0, r;
	msota_state_init(&st);
#if defined(MS_STUB_TEST) || defined(MS_STUB_NOSD)
	mounted = 0;                                 /* MS_STUB_NOSD: the Wii bring-up diagnostic builds A: not one SD access in the stub, the embedded shell runs at once */
#else
	mounted = fatMountSimple("sd", &__io_wiisd);
#endif
	if (mounted) {
		if (small_read(SHL_ST, sb, sizeof sb, &n) != 0 || !msota_state_parse(&st, sb, n)) msota_state_init(&st);
		file = read_mem2(SHL_BIN, MSOTA_MAX_SHELL_FILE, &flen);
		if (file) {
			r = msota_check_shell(file, flen, ms_ota_pubs, MS_OTA_NPUB, 1, &sh);
			file_ok = (r == MSOTA_OK);
		}
		confirmed = st.pending ? file_exists(SHL_OK) : 0;
		pick = msota_decide(&st, file_ok, file_ok ? sh.seq : 0, confirmed, &action);
		if (file_exists("sd:/multishine/ota_off")) pick = MSOTA_PICK_EMBEDDED;   /* the marker SD:/multishine/ota_off: the embedded shell, whatever is downloaded (and the shell skips its boot check) */
		if (action == 1) {                                       /* two starts of a downloaded shell without the alive marker: out, and never retried */
			remove(SHL_BAD);
			rename(SHL_BIN, SHL_BAD);
		}
		if (file_ok || st.floor || st.pending || action) {
			u8 pk[MSOTA_ST_LEN];
			msota_state_pack(&st, pk);
			mkdir(DIR_, 0777);
			write_small(SHL_ST, pk, MSOTA_ST_LEN);
		}
		if (pick == MSOTA_PICK_DOWNLOADED) {
			remove(SHL_OK);                                      /* the shell writes it again as soon as it is alive */
			dol = (u8 *)alloc2(sh.dol_len);
			if (dol) { memcpy(dol, sh.dol, sh.dol_len); dol_len = sh.dol_len; }
		}
#ifndef MS_STUB_TEST
		fatUnmount("sd:");
		__io_wiisd.shutdown();
#endif
	}
	if (dol) jump(dol, dol_len, argc, argv);                    /* returns only when the DOL was refused or memory was short */
	dol = (u8 *)alloc2(ms_embedded_shell_len);
	if (dol) {
		memcpy(dol, ms_embedded_shell, ms_embedded_shell_len);
		jump(dol, ms_embedded_shell_len, argc, argv);
	}
	return 1;                                                   /* nothing could run: back to the Homebrew Channel */
}
