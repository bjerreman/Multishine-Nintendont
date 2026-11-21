/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_slotb.h (tests/test_slotb.py builds and runs it).
 *   slotb_test decide            the decisions and the create procedure on a fake directory; prints OK
 *   slotb_test blank SIZE OUT    the empty card the loader writes (mscard_format_head + mscard_check, as sb_write_tmp) into OUT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms_card_core.h"
#define MS_HOST_TEST 1
int ms_vusb;
#include "ms_slotb.h"
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* a fake directory: a set of names, and switches for the failures */
struct fake { int tmp, raw, mark, failsz, wr_rc, ren_rc, ren_does, marker_rc, race, writes, renames; };
static int f_exists(void *c, const char *p) { struct fake *f = c; return !strcmp(p, MSB_TMP) ? f->tmp : !strcmp(p, MSB_RAW) ? f->raw : !strcmp(p, MSB_MARK) ? f->mark : 0; }
static int f_unlink(void *c, const char *p) { struct fake *f = c; if (!strcmp(p, MSB_TMP)) f->tmp = 0; return 0; }
static int f_write(void *c) { struct fake *f = c; f->writes++; f->tmp = 1; if (f->race) f->raw = 1; return f->wr_rc; }
static int f_rename(void *c, const char *a, const char *b)
{
	struct fake *f = c; (void)a; (void)b; f->renames++;
	if (f->ren_rc == 0 || f->ren_does) { f->tmp = 0; f->raw = 1; }      /* ren_does: the SD driver reports an error for a rename that happened */
	return f->ren_rc;
}
static int f_marker(void *c, const char *p, int n) { struct fake *f = c; if (!strcmp(p, MSB_MARK)) { if (f->marker_rc == 0) f->mark = 1; } else if (!strcmp(p, MSB_FAILS)) f->failsz = n; return f->marker_rc; }
static int logged;
static void f_log(void *c, const char *w, int rc) { (void)c; (void)w; (void)rc; logged++; }
static struct msb_ops ops(struct fake *f) { struct msb_ops o; o.ctx = f; o.exists = f_exists; o.unlink = f_unlink; o.write_tmp = f_write; o.rename = f_rename; o.put_marker = f_marker; o.log = f_log; return o; }

static struct msb_facts base(void) { struct msb_facts f; memset(&f, 0, sizeof f); f.sd_ok = 1; f.a_exists = 1; f.a_size = 2ul * 1024 * 1024; f.free_known = 1; f.free_bytes = 100ul * 1024 * 1024; return f; }

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "vroot")) {                       /* the virtual card files follow the device the game runs from (ms_vroot.h) */
		int bad = 0;
		ms_vusb = 0; if (strcmp(MSB_RAW, "sd:/multishine/saves/GALE_B.raw") || strcmp(MSB_MARK, "sd:/multishine/slotb_done")) bad = 1;
		ms_vusb = 1; if (strcmp(MSB_RAW, "usb:/multishine/saves/GALE_B.raw") || strcmp(MSB_TMP, "usb:/multishine/saves/GALE_B.tmp") || strcmp(MSB_MARK, "usb:/multishine/slotb_done")) bad = 1;
		{ char b[200]; ms_vusb = 1; snprintf(b, sizeof b, "%s|%s", ms_vpath("saves/GALE.raw"), ms_vpath("saves")); if (strcmp(b, "usb:/multishine/saves/GALE.raw|usb:/multishine/saves")) bad = 1; }   /* two live paths in one statement */
		puts(bad ? "VROOT FAILED" : "VROOT OK");
		return bad;
	}
	if (argc == 4 && !strcmp(argv[1], "blank")) {
		unsigned size = (unsigned)strtoul(argv[2], 0, 10), head = size > MSCARD_SIZE ? MSCARD_SIZE : size;
		unsigned char *raw = calloc(size, 1);
		FILE *o;
		int r = mscard_format_head(raw, head, size);
		if (r == MSCARD_OK) r = mscard_check(raw, size);
		if (r != MSCARD_OK) { printf("format %d\n", r); return 1; }
		o = fopen(argv[3], "wb"); fwrite(raw, 1, size, o); fclose(o);       /* the tail beyond the head is zeros, as the loader writes it */
		printf("BLANK %u free %lu\n", size, (unsigned long)mscard_free_blocks(raw, size));
		return 0;
	}
	{                                                                    /* decisions */
		struct msb_facts f = base();
		CHECK(msb_decide(&f) == MSB_CREATE);
		f = base(); f.sd_ok = 0; CHECK(msb_decide(&f) == MSB_SKIP_SD);
		f = base(); f.a_exists = 0; CHECK(msb_decide(&f) == MSB_SKIP_NO_A);                      /* never a B without an A */
		f = base(); f.b_exists = 1; CHECK(msb_decide(&f) == MSB_SKIP_EXISTS);                    /* never overwritten */
		f = base(); f.b_exists = 1; f.from_setup = 1; CHECK(msb_decide(&f) == MSB_SKIP_EXISTS);  /* not even by the set-up */
		f = base(); f.marker = 1; CHECK(msb_decide(&f) == MSB_SKIP_MARKER);                      /* deleted by the player: not again */
		f = base(); f.marker = 1; f.from_setup = 1; CHECK(msb_decide(&f) == MSB_CREATE);         /* a fresh set-up wants the pair */
		f = base(); f.fails = 2; CHECK(msb_decide(&f) == MSB_CREATE);
		f = base(); f.fails = 3; CHECK(msb_decide(&f) == MSB_SKIP_FAILS);
		f = base(); f.fails = 3; f.from_setup = 1; CHECK(msb_decide(&f) == MSB_CREATE);
		f = base(); f.free_bytes = 2ul * 1024 * 1024; CHECK(msb_decide(&f) == MSB_SKIP_SPACE);   /* exactly the size: no 256 KB slack */
		f = base(); f.free_bytes = 2ul * 1024 * 1024 + 0x3FFFF; CHECK(msb_decide(&f) == MSB_SKIP_SPACE);
		f = base(); f.free_bytes = 2ul * 1024 * 1024 + 0x40000; CHECK(msb_decide(&f) == MSB_CREATE);
		f = base(); f.free_known = 0; CHECK(msb_decide(&f) == MSB_SKIP_SPACE);                   /* unknown space: not risked */
		f = base(); f.from_setup = 1; f.free_bytes = 1; CHECK(msb_decide(&f) == MSB_SKIP_SPACE); /* the set-up does not ignore space */
		f = base(); f.a_size = 0; CHECK(msb_decide(&f) == MSB_SKIP_SIZE);
		f = base(); f.a_size = 3ul * 1024 * 1024; CHECK(msb_decide(&f) == MSB_SKIP_SIZE);
		f = base(); f.a_size = 16ul * 1024 * 1024; CHECK(msb_decide(&f) == MSB_SKIP_SIZE);       /* A + B would pass the kernel's 16 MiB */
		f = base(); f.a_size = 8ul * 1024 * 1024; CHECK(msb_decide(&f) == MSB_CREATE);
		f = base(); f.a_size = 4ul * 1024 * 1024; CHECK(msb_decide(&f) == MSB_CREATE);
		CHECK(strcmp(msb_line(1), "Backup card (slot B): ready") == 0 && strcmp(msb_line(0), "Backup card (slot B): none") == 0);
	}
	{                                                                    /* create */
		struct fake f; struct msb_ops o;
		memset(&f, 0, sizeof f); o = ops(&f);
		CHECK(msb_create(&o, 0) == MSB_R_OK && f.raw && !f.tmp && f.mark && f.writes == 1 && f.renames == 1 && f.failsz == 0);
		memset(&f, 0, sizeof f); f.tmp = 1; o = ops(&f);                 /* a stale temp file of an earlier attempt: removed, the write starts clean */
		CHECK(msb_create(&o, 0) == MSB_R_OK && f.raw && !f.tmp);
		memset(&f, 0, sizeof f); f.wr_rc = -2; o = ops(&f);              /* a failed write: no B, no temp, one failure counted, no marker */
		CHECK(msb_create(&o, 1) == MSB_R_WRITE && !f.raw && !f.tmp && !f.mark && f.failsz == 2 && f.renames == 0);
		memset(&f, 0, sizeof f); f.ren_rc = 4; o = ops(&f);              /* a rename that failed and did not happen: temp removed */
		CHECK(msb_create(&o, 0) == MSB_R_RENAME && !f.raw && !f.tmp && !f.mark && f.failsz == 1);
		memset(&f, 0, sizeof f); f.ren_rc = 1; f.ren_does = 1; o = ops(&f);   /* the SD driver quirk: the rename reported 1 but happened: counted as done */
		CHECK(msb_create(&o, 0) == MSB_R_OK && f.raw && !f.tmp && f.mark);
		memset(&f, 0, sizeof f); f.race = 1; o = ops(&f);                /* B appeared meanwhile: never overwritten, the temp goes */
		CHECK(msb_create(&o, 0) == MSB_R_RACE && f.raw && !f.tmp && f.renames == 0);
		memset(&f, 0, sizeof f); f.marker_rc = 3; o = ops(&f);           /* the marker cannot be written: the card is made all the same */
		logged = 0; CHECK(msb_create(&o, 0) == MSB_R_OK && f.raw && logged == 1);
		memset(&f, 0, sizeof f); f.wr_rc = 5; o = ops(&f); o.log = 0;    /* the log hook is optional */
		CHECK(msb_create(&o, 0) == MSB_R_WRITE);
		memset(&f, 0, sizeof f); f.wr_rc = -2; o = ops(&f);              /* resume: a failed first try, then a good second one */
		CHECK(msb_create(&o, 0) == MSB_R_WRITE && f.failsz == 1);
		f.wr_rc = 0; f.marker_rc = 0;
		CHECK(msb_create(&o, 1) == MSB_R_OK && f.raw && !f.tmp && f.mark);
	}
	printf(fails ? "FAILED\n" : "OK\n");
	return fails != 0;
}
