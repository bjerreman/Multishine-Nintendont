/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host test of ms_diag_core.h (tests/test_diag.py builds and runs it).
 *   diag_test            asserts: ring, scrubber, builder (truncation, the 16 KB cap, tail kept), console id, proof, code
 *   diag_test emit [big] writes a complete upload body (payload + proof line) to stdout, for the server's parser to check; "big" = a log far over the cap
 *   diag_test id MAC6HEX DEVIDHEX   prints the console id */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms_diag_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static struct msd_info info(void)
{
	struct msd_info i;
	memset(&i, 0, sizeof i);
	i.shell_seq = 60; i.kernel_floor = 32; i.kernel_active = 0; i.stub_floor = 59; i.stub_active = 60; i.abi_kernel = 5; i.abi_shell = 6; i.abi_stub = 1;
	i.dev = "SD"; i.sd_mounted = 0; i.log_fail = 3; i.uptime_ms = 12345; i.reason = "update failed/net";
	i.pad_valid = 1; i.pad[1].si = 0x09000000; i.pad[1].err = -2; i.pad[1].btn = 0x0100;
	i.si_poll = 0x00F70200; i.si_comcsr = 0x00000000; i.si_sisr = 0x00000000; i.si_exilk = 0x80000000;
	i.sm.sym = 1; i.sm.sic_chan = -1; i.sm.sic_poll = 0xF7; i.sm.si_type[0] = 0x09000000; i.sm.pkt_chan[0] = -1; i.sm.pkt_chan[1] = -1; i.sm.pkt_chan[2] = -1; i.sm.pkt_chan[3] = -1;
	i.sm.rchan = 32; i.sm.en = 0x80000000u; i.sm.pi_intsr = 0x00000008; i.sm.pi_intmr = 0x00000008;
	i.last_key = "Wiimote"; i.console_id = "0123456789abcdef";
	return i;
}

static char *mklog(unsigned lines)
{
	char *b = malloc(lines * 80u + 16u), *p = b;
	unsigned k;
	for (k = 0; k < lines; k++) p += snprintf(p, 80, "line %06u of the log, a kernel state floor 32 pending 0 fail 0 padding..\n", k);
	return b;
}

int main(int argc, char **argv)
{
	static char out[MSD_PAYLOAD_MAX + 64];
	struct msd_info in = info();
	if (argc >= 3 && !strcmp(argv[1], "id")) {
		unsigned char mac[6]; unsigned v, k; char id[17];
		for (k = 0; k < 6; k++) { sscanf(argv[2] + 2 * k, "%2x", &v); mac[k] = (unsigned char)v; }
		msd_console_id(id, mac, strtoul(argv[3], 0, 16)); printf("%s\n", id); return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "emit")) {
		char *sd = mklog(argc > 2 ? 400 : 20), *ram = mklog(argc > 2 ? 400 : 30), pr[17];
		unsigned n;
		in.console_id = "00aa11bb22cc33dd";
		n = msd_build(out, sizeof out - 64, &in, sd, (unsigned)strlen(sd), ram, (unsigned)strlen(ram));
		msd_proof(pr, in.console_id, out, n);
		fwrite(out, 1, n, stdout); printf("proof %s\n", pr);
		return 0;
	}
	{   /* ring: whole lines, oldest dropped, never over the cap, newest kept */
		static struct msd_ring r;
		unsigned k; char l[40];
		for (k = 0; k < 2000; k++) { snprintf(l, sizeof l, "ring line %05u\n", k); msd_ring_add(&r, l, (unsigned)strlen(l)); }
		CHECK(r.n <= MSD_RING && r.n > MSD_RING / 2 - 40);
		CHECK(memcmp(r.b + r.n - 16, "ring line 01999\n", 16) == 0);
		CHECK(memcmp(r.b, "ring line ", 10) == 0);                                  /* starts at a line start */
		{ char *g = mklog(300); msd_ring_add(&r, g, (unsigned)strlen(g)); free(g); }                                   /* a chunk larger than half the ring: still fits */
		CHECK(r.n <= MSD_RING);
	}
	{   /* scrubber */
		char b[512];
		unsigned n;
		const char *t = "net up after 4000 ms, ip 192.168.1.77 gw 10.0.0.1\nmac 00:11:22:33:44:55\nVBBA: 00:11:22:33:44:55 bound\nwrote sd:/games/Super Smash Bros Melee.iso ok\nwrote sd:/multishine/ota.log ok\n"
		                "disc lock: sd:/games/x.iso size 1 header GALE01\nusb:/secret/a.bin, done\nversion 1.2.3 and 12.34.56.78.9 and 300.1.1.1\nssid=MyWifi\n";
		strcpy(b, t);
		n = msd_scrub(b, (unsigned)strlen(t));
		b[n] = 0;
		CHECK(n <= strlen(t));
		CHECK(strstr(b, "192.168.1.77") == 0 && strstr(b, "10.0.0.1") == 0);
		CHECK(strstr(b, "ip x.x.x.x gw x.x.x.x") != 0);
		CHECK(strstr(b, "00:11:22:33:44:55") == 0 && strstr(b, "xx:xx:xx:xx:xx:xx bound") != 0);
		CHECK(strstr(b, "Smash") == 0 && strstr(b, "<path>") != 0);
		CHECK(strstr(b, "sd:/multishine/ota.log") != 0);                           /* our own files stay */
		CHECK(strstr(b, "GALE01") == 0 && strstr(b, "disc lock") == 0);
		CHECK(strstr(b, "secret") == 0);
		CHECK(strstr(b, "version 1.2.3 ") != 0);                                    /* three groups is not an address */
		CHECK(strstr(b, "MyWifi") == 0);
		{ char e[8] = "ssid"; CHECK(msd_scrub(e, 4) == 3); }                        /* never longer than the input */
	}
	{   /* builder: fields, truncation, cap */
		char *sd = mklog(400), *ram = mklog(400);
		unsigned n = msd_build(out, sizeof out, &in, sd, (unsigned)strlen(sd), ram, (unsigned)strlen(ram));
		CHECK(n <= MSD_PAYLOAD_MAX && n > MSD_PAYLOAD_MAX - 400);
		CHECK(strlen(out) == n);
		CHECK(strncmp(out, "MSDIAG1\nconsole 0123456789abcdef\n", 33) == 0);
		CHECK(strstr(out, "abi stub 1 shell 6 kernel 5") != 0);
		CHECK(strstr(out, "reason update_failed_net") != 0);                         /* sanitised */
		CHECK(strstr(out, "port 1: si 09000000 err -2 btn 0100") != 0);
		CHECK(strstr(out, "SIPOLL 00F70200") != 0);
		CHECK(strstr(out, "pad: sic.chan -1 poll F7 | type 09000000 00000000 00000000 00000000 | pkt -1 -1 -1 -1 | fix0 00000000 | rchan 32 rbits 00000000 pend 00000000 en 80000000 wait 00000000 | PI_INTSR 00000008 INTMR 00000008\n") != 0);
		{   /* the worst case (every field at its widest) still fits the line buffer and stays a single scrubbed-safe line */
			struct msd_sm w; char l[256];
			memset(&w, 0xFF, sizeof w); w.sym = 1;
			msd_sm_line(l, sizeof l, &w);
			CHECK(strlen(l) < 250 && strchr(l, '\n') == 0);
			w.sym = 0; msd_sm_line(l, sizeof l, &w);
			CHECK(strncmp(l, "pad: no symbols | PI_INTSR FFFFFFFF", 35) == 0);
		}
		CHECK(strstr(out, "last menu key from Wiimote") != 0);
		CHECK(strstr(out, "line 000399 of the log") != 0);                            /* the TAIL is kept ... */
		CHECK(strstr(out, "line 000000 of the log") == 0);                            /* ... the start is cut */
		CHECK(strstr(out, "---- ota.log tail (SD)") != 0 && strstr(out, "---- ram log (this run)") != 0);
		CHECK(out[n - 1] == '\n');
		{   /* the cut is at a line start: the first log line after the marker is whole */
			const char *m = strstr(out, "---- ram log (this run)\n") + 24;
			CHECK(strncmp(m, "line ", 5) == 0);
		}
		{   /* small logs: everything is there, nothing marked cut */
			n = msd_build(out, sizeof out, &in, "a\n", 2, "b\n", 2);
			CHECK(strstr(out, "---- ota.log tail (SD)\na\n---- ram log (this run)\nb\n") != 0);
			CHECK(n < 700);
		}
		{   /* no SD log at all (the case this feature exists for): the RAM log gets the room */
			n = msd_build(out, sizeof out, &in, "", 0, ram, (unsigned)strlen(ram));
			CHECK(n > MSD_PAYLOAD_MAX - 400);
		}
		{   /* a tiny cap never overflows */
			char tiny[64];
			n = msd_build(tiny, sizeof tiny, &in, sd, (unsigned)strlen(sd), ram, (unsigned)strlen(ram));
			CHECK(n < sizeof tiny);
		}
		{   /* what the scrubber must keep out of the payload even if the log has it */
			const char *bad = "net up ip 192.168.0.5 mac 00:11:22:33:44:55 sd:/games/Melee.iso\n";
			n = msd_build(out, sizeof out, &in, "", 0, bad, (unsigned)strlen(bad));
			CHECK(strstr(out, "192.168") == 0 && strstr(out, "Melee.iso") == 0 && strstr(out, "00:11") == 0);
		}
		free(sd); free(ram);
	}
	{   /* console id: fixed vector and the rule that it differs per MAC / device id */
		unsigned char mac[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 }, mac2[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x56 };
		char a[17], b[17], c[17], d[17];
		msd_console_id(a, mac, 0x0123abcdUL); msd_console_id(b, mac, 0x0123abcdUL); msd_console_id(c, mac2, 0x0123abcdUL); msd_console_id(d, mac, 0x0123abceUL);
		CHECK(strcmp(a, b) == 0 && strcmp(a, c) != 0 && strcmp(a, d) != 0 && strlen(a) == 16);
	}
	{   /* code: short, one line, carries the facts */
		char code[96], p1[17], p2[17];
		msd_code(code, sizeof code, &in, "abc", 3);
		CHECK(strncmp(code, "MS60 sd0 log3 pads 00/09/00/00 ", 31) == 0 && strlen(code) < 48);
		msd_proof(p1, "0123456789abcdef", "x\n", 2); msd_proof(p2, "0123456789abcdef", "y\n", 2);
		CHECK(strcmp(p1, p2) != 0);
	}
	printf(fails ? "FAILED %d\n" : "OK\n", fails);
	return fails ? 1 : 0;
}
