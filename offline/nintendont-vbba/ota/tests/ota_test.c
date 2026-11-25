/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later */
/* Host driver for ms_ota_core.c (tests/test_ota.py builds the containers and runs this).
 *   ota_test state                       the rollback state machine (msota_decide) and the state file format, asserts
 *   ota_test check PUBHEX MINSEQ FILE    msota_check on FILE with one pinned key; prints "RESULT <code> <name> seq <n> zip <len> crc <hex>"
 *   ota_test shell PUBHEX MINSEQ FILE    msota_check_shell (the stub's check of shl-N.mscf); prints "RESULT <code> <name> seq <n> dol <len> crc <hex>"
 *   ota_test dol FILE                    msdol_check on a bare DOL; prints "DOL <code> <name> n <sections> entry <hex> bss <hex> <len>"
 *   ota_test model FILE                  the stub's trampoline as a reference model: copies the sections of a bare DOL into a fake MEM1 and zeroes bss, prints the CRC-32 of
 *                                        every destination range and of the bss, so the test can compare them with the DOL's own bytes */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ms_ota_core.h"
#include "ms_dlrule.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static int do_check(const char *pub, const char *minseq, const char *path)
{
    mw2_u8 key[1][32];
    FILE *f = fopen(path, "rb");
    static mw2_u8 buf[MSOTA_MAX_FILE + 0x10000];
    struct msota_kernel k;
    size_t n;
    int i, r;
    for (i = 0; i < 32; i++) key[0][i] = (mw2_u8)(hexval(pub[2 * i]) * 16 + hexval(pub[2 * i + 1]));
    if (!f) { printf("RESULT -1 nofile\n"); return 2; }
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    r = msota_check(buf, (mw2_u32)n, (const mw2_u8 (*)[32])key, 1, (mw2_u32)strtoul(minseq, 0, 10), &k);
    if (r == MSOTA_OK) printf("RESULT 0 ok seq %lu zip %lu crc %08lx\n", (unsigned long)k.seq, (unsigned long)k.zip_len, (unsigned long)msota_crc32(k.zip, k.zip_len));
    else printf("RESULT %d %s\n", r, msota_err(r));
    return 0;
}

/* a fake card: up to 6 files (name, content id); failure injection per rename call: mode 0 ok, 1 error and nothing done, 2 error although it was done (the loader's f_rename can do that) */
static struct { char name[40]; int id; } ffiles[6];
static int fmode[4], frenames;
static int f_find(const char *n) { int i; for (i = 0; i < 6; i++) if (ffiles[i].name[0] && !strcmp(ffiles[i].name, n)) return i; return -1; }
static void f_put(const char *n, int id) { int i = f_find(n); if (i < 0) for (i = 0; i < 6 && ffiles[i].name[0]; i++) {} strcpy(ffiles[i].name, n); ffiles[i].id = id; }
static int f_exists(void *c, const char *n) { (void)c; return f_find(n) >= 0; }
static int f_unlink(void *c, const char *n) { int i = f_find(n); (void)c; if (i < 0) return 4; ffiles[i].name[0] = 0; return 0; }
static int f_rename(void *c, const char *a, const char *b)
{
    int i = f_find(a), m = frenames < 4 ? fmode[frenames] : 0;
    (void)c;
    frenames++;
    if (i < 0) return 4;                                   /* no such file */
    if (f_find(b) >= 0) return 8;                          /* the target exists */
    if (m == 1) return 13;
    strcpy(ffiles[i].name, b);
    return m == 2 ? 1 : 0;
}
static int f_create(void *c, const char *n) { (void)c; if (fmode[0] == 1) return 1; f_put(n, 1); return 0; }
static void f_reset(int m0, int m1, int m2) { memset(ffiles, 0, sizeof ffiles); fmode[0] = m0; fmode[1] = m1; fmode[2] = m2; fmode[3] = 0; frenames = 0; }
static const struct msota_fs fakefs = { 0, f_exists, f_unlink, f_rename, f_create };

static void test_swap(void)
{
    int i;
    /* first install: no old file */
    f_reset(0, 0, 0); f_put("new", 2);
    CHECK(msota_swap_in(&fakefs, "bin", "new", "bak") == MSOTA_SW_OK && f_find("bin") >= 0 && ffiles[f_find("bin")].id == 2 && f_find("new") < 0 && f_find("bak") < 0);
    /* our card: the rename that worked reports an error (nothing to move aside, then the install itself 'fails') */
    f_reset(0, 2, 0); f_put("new", 2);
    CHECK(msota_swap_in(&fakefs, "bin", "new", "bak") == MSOTA_SW_OK && f_find("bin") >= 0 && ffiles[f_find("bin")].id == 2 && f_find("new") < 0);
    /* replace: the old file becomes the backup */
    f_reset(0, 0, 0); f_put("bin", 1); f_put("new", 2);
    CHECK(msota_swap_in(&fakefs, "bin", "new", "bak") == MSOTA_SW_OK && ffiles[f_find("bin")].id == 2 && ffiles[f_find("bak")].id == 1 && f_find("new") < 0);
    /* an old backup is replaced by the newer one */
    f_reset(0, 0, 0); f_put("bin", 1); f_put("bak", 9); f_put("new", 2);
    CHECK(msota_swap_in(&fakefs, "bin", "new", "bak") == MSOTA_SW_OK && ffiles[f_find("bak")].id == 1 && ffiles[f_find("bin")].id == 2);
    /* a rename that reports an error although it worked, at each step of a replace */
    for (i = 0; i < 2; i++) {
        f_reset(i == 0 ? 2 : 0, i == 1 ? 2 : 0, 0); f_put("bin", 1); f_put("new", 2);
        CHECK(msota_swap_in(&fakefs, "bin", "new", "bak") == MSOTA_SW_OK && ffiles[f_find("bin")].id == 2 && ffiles[f_find("bak")].id == 1 && f_find("new") < 0);
    }
    /* a real failure moving the old file aside: nothing changes, the new file is still there */
    f_reset(1, 0, 0); f_put("bin", 1); f_put("new", 2);
    CHECK(msota_swap_in(&fakefs, "bin", "new", "bak") == MSOTA_SW_BAK && ffiles[f_find("bin")].id == 1 && f_find("new") >= 0);
    /* a real failure moving the new file in: the old file is put back */
    f_reset(0, 1, 0); f_put("bin", 1); f_put("new", 2);
    CHECK(msota_swap_in(&fakefs, "bin", "new", "bak") == MSOTA_SW_FAIL && f_find("bin") >= 0 && ffiles[f_find("bin")].id == 1 && f_find("new") >= 0 && f_find("bak") < 0);
    /* ... and on a first install it just fails */
    f_reset(1, 0, 0); f_put("new", 2);
    CHECK(msota_swap_in(&fakefs, "bin", "new", "bak") == MSOTA_SW_FAIL && f_find("bin") < 0 && f_find("new") >= 0);
}



static void test_late_flag(void)
{
    CHECK(msota_chain_is_late(MSOTA_CHAIN_GUARD, MSOTA_CHAIN_LATE) == 1);       /* hand-over from the late path: skip IOS reload / patch and the kernel load */
    CHECK(msota_chain_is_late(MSOTA_CHAIN_GUARD, 0) == 0);                       /* early hand-over: everything as before */
    CHECK(msota_chain_is_late(0, MSOTA_CHAIN_LATE) == 0);                        /* no guard word (a cold start with random memory): never */
    CHECK(msota_chain_is_late(MSOTA_CHAIN_GUARD, 0x4C415446u) == 0);             /* another word */
    CHECK(msota_chain_is_late(0x4D534347u, MSOTA_CHAIN_LATE) == 0);
}

/* the mock kernel of msota_kernel_probe: after 'after' polls it clears RESET_STATUS (as kernel/main.c's loader loop does for any word but 0x0DEA / 0x1DEA); hid_at: it raises a controller.ini request
   at that poll and only goes on once the loader answered it (HIDOpen busy-waits on HID_CHANGE) */
struct mockk { volatile mw2_u32 *rs, *chg, *sz; int alive, after, hid_at, polls, blocked; };
static void mockk_wait(void *c)
{
    struct mockk *k = c;
    k->polls++;
    if (!k->alive) return;
    if (k->hid_at >= 0 && k->polls == k->hid_at) { *k->chg = 0x057Eu; *k->sz = 0x0337u; k->blocked = 1; return; }
    if (k->blocked) { if (*k->chg != 0) return; k->blocked = 0; }
    if (k->polls >= k->after && *k->rs != 0 && *k->rs != 0x0DEAu && *k->rs != 0x1DEAu) *k->rs = 0;
}

static void test_kernel_probe(void)
{
    volatile mw2_u32 rs, chg, sz, m[2];
    struct mockk k;
    int marker, late;
    CHECK(MSOTA_KPROBE != 0 && MSOTA_KPROBE != 0x0DEAu && MSOTA_KPROBE != 0x1DEAu && MSOTA_KPROBE != 0x3DEAu && MSOTA_KPROBE != 0x5DEAu && MSOTA_KPROBE != 0x6DEAu && MSOTA_KPROBE != 0x7DEAu);
    /* a running kernel clears the word: alive */
    rs = 0; chg = 0; sz = 0; memset(&k, 0, sizeof k); k.rs = &rs; k.chg = &chg; k.sz = &sz; k.alive = 1; k.after = 3; k.hid_at = -1;
    CHECK(msota_kernel_probe(&rs, &chg, &sz, 1000, mockk_wait, &k) == 1 && rs == 0 && k.polls == 3);
    /* no kernel (the case: 170 handed over before its kernel load), MEM2 holds garbage: not alive, the word is put back to 0, HID words answered */
    rs = 0x20000000u; chg = 0xFFFFFFFFu; sz = 0x12345678u; memset(&k, 0, sizeof k); k.rs = &rs; k.chg = &chg; k.sz = &sz; k.alive = 0; k.hid_at = -1;
    CHECK(msota_kernel_probe(&rs, &chg, &sz, 1000, mockk_wait, &k) == 0 && rs == 0 && chg == 0 && sz == 0 && k.polls == 1000);
    /* garbage that happens to equal the probe word cannot fake an answer: only a 0 counts */
    rs = MSOTA_KPROBE; chg = 0; sz = 0; memset(&k, 0, sizeof k); k.rs = &rs; k.chg = &chg; k.sz = &sz; k.hid_at = -1;
    CHECK(msota_kernel_probe(&rs, &chg, &sz, 50, mockk_wait, &k) == 0 && rs == 0);
    /* a kernel busy in HIDOpen waiting for its controller.ini answer (a USB pad was attached during the hand-over): answered 'no file', then it clears the word: alive */
    rs = 0; chg = 0; sz = 0; memset(&k, 0, sizeof k); k.rs = &rs; k.chg = &chg; k.sz = &sz; k.alive = 1; k.after = 1; k.hid_at = 1;
    CHECK(msota_kernel_probe(&rs, &chg, &sz, 1000, mockk_wait, &k) == 1 && chg == 0 && sz == 0);
    /* a request already pending when the probe starts is answered first */
    rs = 0; chg = 0x057Eu; sz = 0x0337u; memset(&k, 0, sizeof k); k.rs = &rs; k.chg = &chg; k.sz = &sz; k.alive = 1; k.after = 1; k.hid_at = -1; k.blocked = 1;
    CHECK(msota_kernel_probe(&rs, &chg, &sz, 1000, mockk_wait, &k) == 1 && chg == 0 && sz == 0);
    /* polls 0: one look, nothing waits */
    rs = 0; chg = 0; sz = 0;
    CHECK(msota_kernel_probe(&rs, &chg, &sz, 0, NULL, NULL) == 0 && rs == 0);
    /* the guard word is read once and cleared */
    m[0] = MSOTA_CHAIN_GUARD; m[1] = MSOTA_CHAIN_LATE;
    msota_chain_take(m, &marker, &late);
    CHECK(marker == 1 && late == 1 && m[0] == 0 && m[1] == 0);
    msota_chain_take(m, &marker, &late);                     /* a warm reload after a crash: nothing left */
    CHECK(marker == 0 && late == 0);
    m[0] = MSOTA_CHAIN_GUARD; m[1] = 0;
    msota_chain_take(m, &marker, &late);
    CHECK(marker == 1 && late == 0 && m[0] == 0);
    m[0] = 0x20000000u; m[1] = MSOTA_CHAIN_LATE;               /* random memory on a cold start */
    msota_chain_take(m, &marker, &late);
    CHECK(marker == 0 && late == 0 && m[0] == 0 && m[1] == 0);
}

static void test_restart(void)
{
    test_late_flag();
    test_kernel_probe();
    struct msota_restart_in r;
    memset(&r, 0, sizeof r);
    CHECK(msota_restart_decide(&r) == MSOTA_RS_NONE);                                                       /* nothing installed: nothing to do */
    r.installed = 1; r.reboot_ok = 1; r.inplace_ok = 1;
    CHECK(msota_restart_decide(&r) == MSOTA_RS_BAD);                                                        /* the installed file does not verify again */
    r.verified = 1;
    CHECK(msota_restart_decide(&r) == MSOTA_RS_INPLACE);                                                    /* DEFAULT: the in-place hand-over, no marker needed */
    r.inplace_marker = 1; CHECK(msota_restart_decide(&r) == MSOTA_RS_INPLACE);                              /* the old marker means nothing */
    r.inplace_marker = 0; r.inplace_ok = 0; CHECK(msota_restart_decide(&r) == MSOTA_RS_TEXT);               /* refused (no_chain, chain.fail, guard, buffer, DOL): the calm words, never a hang */
    r.reboot_marker = 1; CHECK(msota_restart_decide(&r) == MSOTA_RS_REBOOT);                                /* the dev marker SD:/multishine/reboot */
    r.inplace_ok = 1; CHECK(msota_restart_decide(&r) == MSOTA_RS_REBOOT);
    r.reboot_ok = 0; CHECK(msota_restart_decide(&r) == MSOTA_RS_INPLACE);
}

/* the restart for a newer kernel, one start after another: ota.st as msota_decide and the boot check's install keep it (an install sets active to 0), krn.rst as the loader keeps it
   (written before the restart, cleared once the new kernel runs), kernel.bin = the seq of the verified file (0 = none) */
struct kworld { struct msota_state st; mw2_u32 kbin, krst, running; };
static void kw_install(struct kworld *w, mw2_u32 seq) { w->kbin = seq; w->st.pending = 0; w->st.fail = 0; w->st.active = 0; if (seq > w->st.floor) w->st.floor = seq; }   /* ota_boot_check_body + the swap */
static void kw_load(struct kworld *w)                                   /* ms_ota_select: msota_decide, the kernel load */
{
    int action = 0;
    int pick = msota_decide(&w->st, w->kbin != 0, w->kbin, 1, &action);
    if (action == 1) w->kbin = 0;
    w->running = pick == MSOTA_PICK_DOWNLOADED ? w->kbin : 1000;        /* 1000 = the embedded kernel */
    if (w->krst && msota_krestart_after(w->krst, pick == MSOTA_PICK_DOWNLOADED ? w->kbin : 0) == MSOTA_KA_RUNS) w->krst = 0;
}
static int kw_check(struct kworld *w, int kernel_runs, int dol_ok)       /* ota_chain_after_check with a kernel running (or not) */
{
    struct msota_krestart_in in;
    int d;
    memset(&in, 0, sizeof in);
    in.file_seq = w->kbin; in.floor = w->st.floor; in.bad = w->st.bad; in.active = w->st.active; in.marker_seq = w->krst; in.running = kernel_runs; in.dol_ok = dol_ok;
    d = msota_krestart_decide(&in);
    if (d == MSOTA_KR_GO) w->krst = w->kbin;
    return d;
}

static void test_krestart(void)
{
    struct kworld w;
    int i, restarts = 0;
    /* an older loader (175..177) runs ; its check installs and in one start, after its kernel was loaded, and hands over late to 178 */
    memset(&w, 0, sizeof w); w.kbin = 61; kw_load(&w); CHECK(w.running == 61 && w.st.active == 61);
    kw_install(&w, 62);
    /* 178 starts late with 61 running; its own check finds nothing new: kernel.bin 62 is newer than the running kernel: ONE restart */
    CHECK(kw_check(&w, 1, 1) == MSOTA_KR_GO && w.krst == 62); restarts++;
    /* the restart: the early flavour, the kernel is loaded from kernel.bin: 62 runs, krn.rst cleared; the next late start (a later shell update) sees kernel.bin = the running kernel: no restart */
    kw_load(&w); CHECK(w.running == 62 && w.st.active == 62 && w.krst == 0);
    if (kw_check(&w, 1, 1) == MSOTA_KR_GO) restarts++;
    CHECK(restarts == 1);
    /* kernel.bin equal to the running kernel (a late start, nothing installed): no restart */
    CHECK(kw_check(&w, 1, 1) == MSOTA_KR_NONE && w.krst == 0);
    /* this start's own check installs 63 while 62 runs: one restart, then it runs */
    kw_install(&w, 63); CHECK(kw_check(&w, 1, 1) == MSOTA_KR_GO && w.krst == 63);
    kw_load(&w); CHECK(w.running == 63 && w.krst == 0 && kw_check(&w, 1, 1) == MSOTA_KR_NONE);
    /* a kernel installed before the kernel load (a normal start: no kernel runs yet): no restart, the load takes it */
    kw_install(&w, 64); CHECK(kw_check(&w, 0, 1) == MSOTA_KR_NONE); kw_load(&w); CHECK(w.running == 64);
    /* the restart did not bring the new kernel (the restarted start could not load kernel.bin): no loop, whatever comes next */
    kw_install(&w, 65); CHECK(kw_check(&w, 1, 1) == MSOTA_KR_GO && w.krst == 65);
    for (i = 0; i < 5; i++) CHECK(kw_check(&w, 1, 1) == MSOTA_KR_ONCE && w.krst == 65);
    CHECK(msota_krestart_after(65, 64) == MSOTA_KA_NOT);
    /* kernel.bin that msota_decide would not start: below the floor, or the rolled-back seq: no restart */
    memset(&w, 0, sizeof w); w.st.floor = 70; w.st.active = 70; w.kbin = 69; CHECK(kw_check(&w, 1, 1) == MSOTA_KR_NONE);
    w.st.bad = 71; w.st.floor = 71; w.st.active = 0; w.kbin = 71; CHECK(kw_check(&w, 1, 1) == MSOTA_KR_NONE);
    /* no kernel.bin (it did not verify, or a rollback renamed it): no restart */
    w.kbin = 0; CHECK(kw_check(&w, 1, 1) == MSOTA_KR_NONE);
    /* rollback rules stay: a downloaded kernel that never reached the game twice is dropped by msota_decide, and no restart follows for it */
    memset(&w, 0, sizeof w); w.kbin = 80; kw_load(&w); w.st.fail = MSOTA_MAX_FAIL; { int action = 0; CHECK(msota_decide(&w.st, 1, 80, 0, &action) == MSOTA_PICK_EMBEDDED && action == 1); } w.kbin = 0;
    CHECK(kw_check(&w, 1, 1) == MSOTA_KR_NONE);
    /* no loader file to restart into: no restart, krn.rst untouched */
    memset(&w, 0, sizeof w); w.kbin = 61; kw_load(&w); kw_install(&w, 62);
    CHECK(kw_check(&w, 1, 0) == MSOTA_KR_NODOL && w.krst == 0);
    CHECK(msota_krestart_after(0, 62) == MSOTA_KA_NONE);
    CHECK(strcmp(msota_krestart_name(MSOTA_KR_ONCE), "already restarted once for it") == 0);
}

/* the download time rules (ms_dlrule.h), with a simulated link: rate in bytes per ms, stall_at = the byte after which nothing more arrives (0 = never) */
static int dl_sim(unsigned size, unsigned rate, unsigned stall_at, unsigned head_ms, unsigned deadline, unsigned *end_ms)
{
    unsigned t = 0, got = 0, tp = 0;
    for (;;) {
        if (t >= head_ms && got < size && (!stall_at || got < stall_at)) { got += rate; if (got > size) got = size; if (stall_at && got > stall_at) got = stall_at; tp = t; }
        if (got >= size) { *end_ms = t; return 1; }
        t += 1;
        if (msdl_expired(t, got ? t - tp : 0u, deadline, got != 0)) { *end_ms = t; return 0; }
    }
}

static void test_dlrule(void)
{
    unsigned b, e;
    /* the budget follows the announced size: the 1,916,928-byte shell gets about 56 s, the 221 KB kernel about 13.5 s, never under the floor, never over the cap */
    b = msdl_budget(1916928u, 30000u); CHECK(b >= 55000u && b <= 57000u);
    b = msdl_budget(221184u, 12000u); CHECK(b >= 13000u && b <= 14000u);
    CHECK(msdl_budget(1000u, 30000u) == 30000u);
    CHECK(msdl_budget(0u, 12000u) == 12000u);
    CHECK(msdl_budget(200u * 1024u * 1024u, 1000u) == MSDL_CAP_MS);
    /* the field case: the 1.9 MB shell over a slow link (about 20 KB/s): the old fixed 25.8 s (even with the progress rule) is not enough; the size-based budget is */
    CHECK(dl_sim(1916928u, 20u, 0u, 1500u, 25806u, &e) == 0);
    CHECK(dl_sim(1916928u, 20u, 0u, 1500u, msdl_budget(1916928u, 30000u), &e) == 1);
    /* about 50 KB/s: within the budget itself, no extension needed */
    CHECK(dl_sim(1916928u, 50u, 0u, 1500u, msdl_budget(1916928u, 30000u), &e) == 1 && e < msdl_budget(1916928u, 30000u));
    /* slower than the floor rate (30 KB/s), bytes still arriving: it goes on past the deadline (up to twice it) and arrives */
    b = msdl_budget(1916928u, 30000u);
    CHECK(dl_sim(1916928u, 30u, 0u, 1500u, b, &e) == 1 && e > b && e < 2u * b);
    /* a stalled link: half the file, then nothing: it ends MSDL_STALL_MS after the last bytes once the deadline is past, never later */
    CHECK(dl_sim(1916928u, 50u, 900000u, 1500u, b, &e) == 0 && e >= b && e <= b + MSDL_STALL_MS);
    /* a stall after the deadline: cut MSDL_STALL_MS after the last bytes */
    CHECK(dl_sim(1916928u, 20u, 1500000u, 1500u, b, &e) == 0 && e > b && e < 2u * b);
    /* no body at all (the server never answers): the deadline is hard */
    CHECK(dl_sim(1000u, 1u, 0u, 1000000u, 5000u, &e) == 0 && e == 5000u);
    /* the hard end: twice the deadline even while bytes trickle in */
    CHECK(dl_sim(1916928u, 5u, 0u, 0u, 10000u, &e) == 0 && e == 20000u);
}

static void test_chain(void)
{
    test_krestart();
    test_dlrule();
    test_restart();
    struct msota_chain_in in;
    char blk[64];
    const char *av[3];
    mw2_u32 n;
    in.installed = 1; in.early = 1; in.marker = 0; in.disabled = 0; in.verified = 1; in.dol_ok = 1; in.mem2_ok = 1; in.reload_ok = 0;
    CHECK(msota_chain_decide(&in) == MSOTA_CH_GO);
    in.installed = 0; CHECK(msota_chain_decide(&in) == MSOTA_CH_NOINST); in.installed = 1;
    in.early = 0; CHECK(msota_chain_decide(&in) == MSOTA_CH_LATE);                                          /* late (menu up) without the IOS58 reload route: refused */
    in.reload_ok = 1; CHECK(msota_chain_decide(&in) == MSOTA_CH_GO);                                       /* the manual check / the check at the game start: allowed, through the IOS58 reload */
    in.marker = 1; CHECK(msota_chain_decide(&in) == MSOTA_CH_GUARD); in.marker = 0;                        /* the loop guard holds late too: marker = chain.try still present (this start did not confirm) */
    in.early = 1; in.marker = 0; CHECK(msota_chain_decide(&in) == MSOTA_CH_GO);                             /* a start that IS a hand-over but confirmed itself (chain.try cleared) hands over again */
    in.early = 0;
    in.disabled = 1; CHECK(msota_chain_decide(&in) == MSOTA_CH_DISABLED); in.disabled = 0;
    in.verified = 0; CHECK(msota_chain_decide(&in) == MSOTA_CH_NOVERIFY); in.verified = 1;
    in.dol_ok = 0; CHECK(msota_chain_decide(&in) == MSOTA_CH_DOL); in.dol_ok = 1;
    in.mem2_ok = 0; CHECK(msota_chain_decide(&in) == MSOTA_CH_MEM); in.mem2_ok = 1;
    in.early = 1;
    in.marker = 1; CHECK(msota_chain_decide(&in) == MSOTA_CH_GUARD); in.marker = 0;                       /* the loop guard */
    in.disabled = 1; CHECK(msota_chain_decide(&in) == MSOTA_CH_DISABLED); in.disabled = 0;
    in.verified = 0; CHECK(msota_chain_decide(&in) == MSOTA_CH_NOVERIFY); in.verified = 1;
    in.dol_ok = 0; CHECK(msota_chain_decide(&in) == MSOTA_CH_DOL); in.dol_ok = 1;
    in.mem2_ok = 0; CHECK(msota_chain_decide(&in) == MSOTA_CH_MEM); in.mem2_ok = 1;
    /* the order: an installed + early + guarded start is a guard refusal even when everything else is wrong */
    in.marker = 1; in.verified = 0; in.dol_ok = 0; CHECK(msota_chain_decide(&in) == MSOTA_CH_GUARD);
    /* the argument block */
    av[0] = "sd:/apps/Nintendont-Multishine/boot.dol"; av[1] = "x"; av[2] = 0;
    n = msota_args_build(blk, sizeof blk, 2, av);
    CHECK(n == 40u + 2u + 1u && !memcmp(blk, "sd:/apps/Nintendont-Multishine/boot.dol", 40) && blk[39] == 0 && blk[40] == 'x' && blk[41] == 0 && blk[42] == 0);
    n = msota_args_build(blk, sizeof blk, 1, av);
    CHECK(n == 41u && blk[39] == 0 && blk[40] == 0);
    CHECK(msota_args_build(blk, sizeof blk, 0, av) == 0 && msota_args_build(blk, sizeof blk, 1, 0) == 0);
    CHECK(msota_args_build(blk, 41, 1, av) == 41u);                                                          /* exactly fits */
    CHECK(msota_args_build(blk, 40, 1, av) == 0);                                                            /* the first argument alone does not fit: nothing */
    av[0] = "a"; av[1] = "this one is much too long for the little buffer";
    n = msota_args_build(blk, 8, 2, av);
    CHECK(n == 3u && blk[0] == 'a' && blk[1] == 0 && blk[2] == 0);                                           /* the second is dropped, the block stays valid */
}

static void test_backoff(void)
{
    mw2_u32 now = 1793711803u;
    CHECK(msota_backoff(MSOTA_BO_NET_FAIL, now, 0) == now);                              /* the network was not up in time: no pause, the next start looks again */
    CHECK(msota_backoff(MSOTA_BO_SERVER_FAIL, now, 0) == now);                           /* up but the server was silent: the same */
    CHECK(msota_backoff(MSOTA_BO_NET_FAIL, now, now + 3600u) == now);                    /* never a longer pause */
    CHECK(msota_backoff(MSOTA_BO_SUCCESS, now, now + 3600u) == 0u);                      /* any successful contact clears it */
    CHECK(msota_backoff(MSOTA_BO_INSTALLED, now, now + 600u) == 0u);                     /* so does an install */
    CHECK(msota_backoff(MSOTA_BO_SKIPPED, now, 0) == 0u && msota_backoff(MSOTA_BO_SKIPPED, now, now + 123u) == now + 123u);   /* a skip by the user sets and clears nothing */
    CHECK(msota_backoff(99, now, 77u) == 77u);
    /* the sequence of our card: seq 3 fails (network), a manual check succeeds, the next start must not be skipped */
    { mw2_u32 next = msota_backoff(MSOTA_BO_NET_FAIL, now, 0); CHECK(!(next > now)); next = msota_backoff(MSOTA_BO_SUCCESS, now + 20u, next); CHECK(!(next > now + 30u)); }
}

/* the stub's shell chain (stub_main.c: msota_decide with prev_confirmed = shl.ok present, shl.ok removed before a downloaded shell runs) driven start by start.
   how: 'c' = crashes before its start screen, 'm' = start screen only (menus, no game), 'g' = reaches the game start.  rule167 = shl.ok also at the start screen (msota_alive_due).
   Returns the stub's pick at the start AFTER the sequence (1 = still the downloaded shell). */
static int stub_run(const char *how, int rule167, int *dropped)
{
    struct msota_state st;
    int shl_ok = 0, act = 0, pick = MSOTA_PICK_EMBEDDED;
    msota_state_init(&st);
    *dropped = 0;
    for (;; how++) {
        pick = msota_decide(&st, 1, 166, st.pending ? shl_ok : 0, &act);
        if (act == 1) *dropped = 1;
        if (!*how) return pick == MSOTA_PICK_DOWNLOADED;
        if (pick != MSOTA_PICK_DOWNLOADED) continue;                     /* the embedded shell runs: it proves nothing about the downloaded one */
        shl_ok = 0;                                                      /* the stub removes it before it jumps */
        if (*how == 'g') shl_ok = 1;                                     /* ms_ota_select */
        if (*how == 'm' && rule167 && msota_alive_due(1, 1, 1, 0)) shl_ok = 1;
    }
}

static void test_alive(void)
{
    int dropped;
    CHECK(msota_alive_due(1, 1, 1, 0) == 1);
    CHECK(msota_alive_due(0, 1, 1, 0) == 0);                             /* before FPAD_Init: not yet */
    CHECK(msota_alive_due(1, 0, 1, 0) == 0);                             /* no SD: nothing to write anyway */
    CHECK(msota_alive_due(1, 1, 0, 0) == 0);                             /* the screen was never drawn */
    CHECK(msota_alive_due(1, 1, 1, 1) == 0);                             /* once per run */
    CHECK(stub_run("mmm", 0, &dropped) == 0 && dropped == 1);           /* shell <= 166: three menu-only starts drop a good shell (audit C2) */
    CHECK(stub_run("mmm", 1, &dropped) == 1 && dropped == 0);           /* kept */
    CHECK(stub_run("mmmmmmg", 1, &dropped) == 1 && dropped == 0);
    CHECK(stub_run("cc", 1, &dropped) == 0 && dropped == 1);            /* a shell that crashes before its start screen is still dropped after MSOTA_MAX_FAIL starts */
    CHECK(stub_run("mc", 1, &dropped) == 1 && dropped == 0);            /* one crash after a proven start: one more chance (the stub's rule: two in a row) */
    CHECK(stub_run("mcc", 1, &dropped) == 0 && dropped == 1);
    CHECK(stub_run("ggg", 0, &dropped) == 1 && dropped == 0);           /* the game start still proves it, old rule and new */
}

static void test_state(void)
{
    test_alive();
    struct msota_state s, t;
    mw2_u8 b[MSOTA_ST_LEN];
    int act, pick;
    msota_state_init(&s);
    /* no file: embedded, nothing recorded */
    pick = msota_decide(&s, 0, 0, 0, &act); CHECK(pick == MSOTA_PICK_EMBEDDED && act == 0 && !s.pending);
    /* a first download, seq 5: start it, pending */
    pick = msota_decide(&s, 1, 5, 0, &act); CHECK(pick == MSOTA_PICK_DOWNLOADED && s.pending == 1 && s.floor == 5 && s.active == 5 && s.fail == 0);
    /* the next start confirms it (the previous log had RUNID): pending cleared then set again for this start */
    pick = msota_decide(&s, 1, 5, 1, &act); CHECK(pick == MSOTA_PICK_DOWNLOADED && s.pending == 1 && s.fail == 0);
    /* a newer file */
    pick = msota_decide(&s, 1, 7, 1, &act); CHECK(pick == MSOTA_PICK_DOWNLOADED && s.floor == 7 && s.active == 7);
    /* an OLDER file than the floor is never started (anti-rollback) */
    pick = msota_decide(&s, 1, 6, 1, &act); CHECK(pick == MSOTA_PICK_EMBEDDED && act == 2 && s.floor == 7);
    /* kernel 9 never reaches the game: start 1 */
    pick = msota_decide(&s, 1, 9, 1, &act); CHECK(pick == MSOTA_PICK_DOWNLOADED && s.pending == 1 && s.floor == 9);
    /* start 2: unconfirmed, fail 1, still tried */
    pick = msota_decide(&s, 1, 9, 0, &act); CHECK(pick == MSOTA_PICK_DOWNLOADED && s.fail == 1 && s.pending == 1);
    /* start 3: unconfirmed again, fail 2: dropped, embedded, remembered */
    pick = msota_decide(&s, 1, 9, 0, &act); CHECK(pick == MSOTA_PICK_EMBEDDED && act == 1 && s.bad == 9 && s.pending == 0 && s.fail == 0);
    /* the same seq is not tried again, a newer one is */
    pick = msota_decide(&s, 1, 9, 0, &act); CHECK(pick == MSOTA_PICK_EMBEDDED && act == 2);
    pick = msota_decide(&s, 1, 10, 0, &act); CHECK(pick == MSOTA_PICK_DOWNLOADED && s.floor == 10);
    /* a file that vanished or does not verify: embedded, pending cleared (nothing to confirm) */
    pick = msota_decide(&s, 0, 0, 0, &act); CHECK(pick == MSOTA_PICK_EMBEDDED && !s.pending);
    /* confirmation in time: fail never grows */
    msota_state_init(&s);
    msota_decide(&s, 1, 3, 0, &act); msota_decide(&s, 1, 3, 1, &act); msota_decide(&s, 1, 3, 1, &act); CHECK(s.fail == 0 && s.pending == 1);
    /* state file */
    msota_state_pack(&s, b);
    CHECK(msota_state_parse(&t, b, MSOTA_ST_LEN) && t.floor == s.floor && t.active == s.active && t.bad == s.bad && t.pending == s.pending && t.fail == s.fail);
    b[9] ^= 1; CHECK(!msota_state_parse(&t, b, MSOTA_ST_LEN) && t.floor == 0 && !t.pending);          /* a damaged file is the empty state */
    msota_state_pack(&s, b); CHECK(!msota_state_parse(&t, b, MSOTA_ST_LEN - 1));
    /* RUNID scan */
    CHECK(msota_has_runid((const mw2_u8 *)"x RUNID: game foo", 17));
    CHECK(!msota_has_runid((const mw2_u8 *)"RUNID: loader tag", 17));
    CHECK(msota_crc32((const mw2_u8 *)"123456789", 9) == 0xCBF43926u);
}

static int do_shell(const char *pub, const char *minseq, const char *path)
{
    mw2_u8 key[1][32];
    FILE *f = fopen(path, "rb");
    static mw2_u8 buf[MSOTA_MAX_SHELL_FILE + 0x10000];
    struct msota_shell k;
    size_t n;
    int i, r;
    for (i = 0; i < 32; i++) key[0][i] = (mw2_u8)(hexval(pub[2 * i]) * 16 + hexval(pub[2 * i + 1]));
    if (!f) { printf("RESULT -1 nofile\n"); return 2; }
    n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    r = msota_check_shell(buf, (mw2_u32)n, (const mw2_u8 (*)[32])key, 1, (mw2_u32)strtoul(minseq, 0, 10), &k);
    if (r == MSOTA_OK) printf("RESULT 0 ok seq %lu dol %lu crc %08lx\n", (unsigned long)k.seq, (unsigned long)k.dol_len, (unsigned long)msota_crc32(k.dol, k.dol_len));
    else printf("RESULT %d %s\n", r, msota_err(r));
    return 0;
}

static mw2_u8 *read_all(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    mw2_u8 *b = (mw2_u8 *)malloc(MSOTA_MAX_DOL + 0x10000);
    if (!f || !b) return 0;
    *n = fread(b, 1, MSOTA_MAX_DOL + 0x10000, f);
    fclose(f);
    return b;
}

static int do_dol(const char *path)
{
    size_t n = 0;
    mw2_u8 *b = read_all(path, &n);
    struct msdol_plan p;
    int r;
    if (!b) { printf("DOL -1 nofile\n"); return 2; }
    r = msdol_check(b, (mw2_u32)n, &p);
    if (r == MSDOL_OK) printf("DOL 0 ok n %u entry %08lx bss %08lx %lu\n", p.n, (unsigned long)p.entry, (unsigned long)p.bss, (unsigned long)p.bss_len);
    else printf("DOL %d %s\n", r, msdol_err(r));
    free(b);
    return 0;
}

/* the trampoline of the stub (tramp.S) as C: for every planned section copy src -> dst, then zero the bss; the fake MEM1 is 24 MiB at 0x80000000 */
static int do_model(const char *path)
{
    size_t n = 0;
    mw2_u8 *b = read_all(path, &n);
    mw2_u8 *mem = (mw2_u8 *)calloc(1, 0x01800000u);
    struct msdol_plan p;
    unsigned i;
    mw2_u32 k;
    if (!b || !mem) { printf("MODEL -1\n"); return 2; }
    memset(mem, 0xA5, 0x01800000u);                                  /* stale memory the copy must overwrite and the bss must clear */
    if (msdol_check(b, (mw2_u32)n, &p) != MSDOL_OK) { printf("MODEL -2 refused\n"); return 0; }
    for (i = 0; i < p.n; i++)
        for (k = 0; k < p.sec[i].len; k++) mem[p.sec[i].dst - 0x80000000u + k] = b[p.sec[i].src + k];
    for (k = 0; k < p.bss_len; k++) mem[p.bss - 0x80000000u + k] = 0;
    for (i = 0; i < p.n; i++) printf("SEC %08lx %lu %08lx\n", (unsigned long)p.sec[i].dst, (unsigned long)p.sec[i].len, (unsigned long)msota_crc32(mem + (p.sec[i].dst - 0x80000000u), p.sec[i].len));
    printf("BSS %08lx %lu %08lx\n", (unsigned long)p.bss, (unsigned long)p.bss_len, (unsigned long)msota_crc32(mem + (p.bss - 0x80000000u), p.bss_len));
    printf("TRAMP-AREA untouched %d\n", mem[0x01300000u] == 0xA5 && mem[0x017FFFFFu] == 0xA5);
    printf("LOW untouched %d\n", mem[0x1800] == 0xA5 && mem[0x3000] == 0xA5);
    free(mem); free(b);
    return 0;
}

static void test_helpers(void)
{
    mw2_u32 st, cl, he, ip;
    unsigned port;
    const char *r1 = "HTTP/1.1 200 OK\r\nServer: x\r\nContent-Length: 184533\r\nConnection: close\r\n\r\nBODY";
    const char *r2 = "HTTP/1.1 404 Not Found\r\ncontent-length: 21\r\n\r\n";
    const char *r3 = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    const char *r4 = "HTTP/1.1 200 OK\r\nX-Content-Length: 9\r\n\r\n";
    CHECK(msota_http_head((const mw2_u8 *)r1, (mw2_u32)strlen(r1), &st, &cl, &he) == 0 && st == 200 && cl == 184533u && he == (mw2_u32)(strlen(r1) - 4));
    CHECK(msota_http_head((const mw2_u8 *)r2, (mw2_u32)strlen(r2), &st, &cl, &he) == 0 && st == 404 && cl == 21u && he == (mw2_u32)strlen(r2));
    CHECK(msota_http_head((const mw2_u8 *)r3, (mw2_u32)strlen(r3), &st, &cl, &he) == 0 && st == 200 && cl == 0xFFFFFFFFu);        /* chunked: no length, the loader refuses it */
    CHECK(msota_http_head((const mw2_u8 *)r4, (mw2_u32)strlen(r4), &st, &cl, &he) == 0 && cl == 0xFFFFFFFFu);                      /* only a header that STARTS the line counts */
    CHECK(msota_http_head((const mw2_u8 *)"HTTP/1.1 200 OK\r\nContent-Len", 29, &st, &cl, &he) == -1);                              /* incomplete head */
    CHECK(msota_http_head((const mw2_u8 *)"HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n", 49, &st, &cl, &he) == -1);        /* absurd length */
    CHECK(msota_http_head((const mw2_u8 *)"garbage\r\n\r\n", 11, &st, &cl, &he) == 0 && st == 0);
    CHECK(msota_parse_ip("198.51.100.214", &ip, &port) == 0 && ip == 0xC63364D6u && port == 0);
    CHECK(msota_parse_ip("10.0.0.5:8080/v1/x", &ip, &port) == 0 && ip == 0x0A000005u && port == 8080);
    CHECK(msota_parse_ip("1.2.3", &ip, &port) == -1 && msota_parse_ip("256.1.1.1", &ip, &port) == -1 && msota_parse_ip("a.b.c.d", &ip, &port) == -1);
    CHECK(msota_parse_ip("1.2.3.4:", &ip, &port) == -1 && msota_parse_ip("1.2.3.4:70000", &ip, &port) == -1 && msota_parse_ip("1.2.3.4x", &ip, &port) == -1 && msota_parse_ip("multishine.link", &ip, &port) == -1);
    CHECK(msota_json_uint("{\"latest\":7,\"size\":184320}", "\"latest\":") == 7u && msota_json_uint("{\"latest\":7,\"size\":184320}", "\"size\":") == 184320u);
    CHECK(msota_json_uint("{\"latest\":null,\"abi\":5}", "\"latest\":") == 0u && msota_json_uint("{}", "\"latest\":") == 0u && msota_json_uint("{\"latest\": 12}", "\"latest\":") == 12u);
    { char hdr[64]; msota_fmt_header(hdr, sizeof hdr, 0, 1); CHECK(!strcmp(hdr, "Multishine update (kernel 0, shell 1)")); msota_fmt_header(hdr, sizeof hdr, 12, 3405); CHECK(!strcmp(hdr, "Multishine update (" "kernel" " 12, " "shell" " 3405)")); msota_fmt_header(hdr, 12, 7, 8); CHECK(strlen(hdr) == 11); }
    CHECK(msota_json_uint("{\"latest\":99999999999}", "\"latest\":") == 0u);
    {   /* the real answers of http://multishine.link/v1/update/shl/latest?have=1: the server in front, headers in alphabetical order, Via, a 149 byte body */
        const char *real_head = "HTTP/1.1 200 OK\r\nCache-Control: no-store\r\nContent-Length: 149\r\nContent-Type: application/json\r\nDate: Tue, 03 Nov 2026 09:53:49 GMT\r\n"
                                "Server: MultishineServer/0.1 Python/3.12.3\r\nVia: 1.0 Caddy\r\nX-Content-Type-Options: nosniff\r\n\r\n";
        const char *shl = "{\"latest\":2,\"size\":1548288,\"blocks\":189,\"sha256\":\"0398a44b890cc7eb0a5af66828750158a0f3626b5c0f68fc4d469105233b4f5f\",\"abi\":6,\"url\":\"/v1/update/shl/2\"}";
        const char *krn = "{\"latest\":null,\"abi\":5}";
        char all[1024];
        mw2_u32 n, k;
        CHECK(strlen(shl) == 149u);
        snprintf(all, sizeof all, "%s%s", real_head, shl);
        n = (mw2_u32)strlen(all);
        CHECK(msota_http_head((const mw2_u8 *)all, n, &st, &cl, &he) == 0 && st == 200 && cl == 149u && he == (mw2_u32)strlen(real_head) && n == he + cl);
        for (k = 0; k + 1 < he; k++) CHECK(msota_http_head((const mw2_u8 *)all, k, &st, &cl, &he) == -1);       /* every shorter prefix of the head is 'not complete yet' (the loader keeps reading) */
        CHECK(msota_http_head((const mw2_u8 *)all, he = (mw2_u32)strlen(real_head), &st, &cl, &k) == 0 && st == 200 && cl == 149u);
        CHECK(msota_json_uint(all + strlen(real_head), "\"latest\":") == 2u && msota_json_uint(all + strlen(real_head), "\"size\":") == 1548288u);
        CHECK(msota_json_uint(krn, "\"latest\":") == 0u);
    }
}

static int hexn(const char *h, mw2_u8 *out, int max)
{
    int n = 0;
    while (h[0] && h[1] && n < max) { out[n++] = (mw2_u8)(hexval(h[0]) * 16 + hexval(h[1])); h += 2; }
    return n;
}

/* a scripted resolver: packets appear at fake times (ms from the start; the clock only moves when the code waits) */
static struct { mw2_u8 p[600]; int n; mw2_u32 at; int taken; } fk_pk[64];
static int fk_n, fk_sends, fk_fail_send;
static mw2_u32 fk_t;
static long fk_send(void *c, const mw2_u8 *p, mw2_u32 n) { (void)c; (void)p; (void)n; fk_sends++; return fk_fail_send ? -1 : 0; }
static long fk_recv(void *c, mw2_u8 *b, mw2_u32 cap)
{
    int i;
    (void)c;
    for (i = 0; i < fk_n; i++)
        if (!fk_pk[i].taken && fk_pk[i].at <= fk_t) { fk_pk[i].taken = 1; if (fk_pk[i].n < 0) return -1; memcpy(b, fk_pk[i].p, (size_t)fk_pk[i].n < cap ? (size_t)fk_pk[i].n : cap); return fk_pk[i].n; }
    return 0;
}
static mw2_u32 fk_now(void *c) { (void)c; return fk_t; }
static void fk_wait(void *c, mw2_u32 ms) { (void)c; fk_t += ms; }

/* ota_test dns resolve ID NAME TIMEOUT TRIES BUDGET HEX@MS,HEX@MS,...|-   [sendfail]    "RES rc parse N ip A.B.C.D sends N at MS" */
static int do_dns_resolve(int argc, char **argv)
{
    struct msdns_io io;
    mw2_u32 ip = 0;
    int rc, lp = -1;
    char *s, *tok, copy[20000];
    fk_n = 0; fk_sends = 0; fk_t = 0; fk_fail_send = argc > 9 && !strcmp(argv[9], "sendfail");
    memset(fk_pk, 0, sizeof fk_pk);
    strncpy(copy, argv[8], sizeof copy - 1); copy[sizeof copy - 1] = 0;
    if (strcmp(copy, "-")) for (s = copy; (tok = strtok(s, ",")); s = 0) {
        char *at = strchr(tok, '@');
        if (!at) return 2;
        *at = 0;
        fk_pk[fk_n].n = !strcmp(tok, "E") ? -1 : hexn(tok, fk_pk[fk_n].p, 600); fk_pk[fk_n].at = (mw2_u32)strtoul(at + 1, 0, 10); fk_n++;
    }
    io.ctx = 0; io.send = fk_send; io.recv = fk_recv; io.now = fk_now; io.wait = fk_wait;
    rc = msdns_resolve(&io, (unsigned)strtoul(argv[3], 0, 0), argv[4], (mw2_u32)strtoul(argv[5], 0, 10), (unsigned)strtoul(argv[6], 0, 10), (mw2_u32)strtoul(argv[7], 0, 10), &ip, &lp);
    printf("RES %d parse %d ip %u.%u.%u.%u sends %d at %lu\n", rc, lp, (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255), fk_sends, (unsigned long)fk_t);
    return 0;
}

/* the chunk writer against a fake file: fails or short-writes at call k, a failing sync */
static struct { mw2_u32 calls, bytes, fail_at, short_at, sync_calls; int sync_fail; mw2_u32 maxn; } fw;
static int fw_write(void *c, const mw2_u8 *p, mw2_u32 n, mw2_u32 *put)
{
    (void)c; (void)p;
    if (n > fw.maxn) fw.maxn = n;
    if (fw.calls++ == fw.fail_at) { *put = 0; return 13; }
    if (fw.calls - 1 == fw.short_at) { *put = n / 2; fw.bytes += n / 2; return 0; }
    *put = n; fw.bytes += n;
    return 0;
}
static int fw_sync(void *c) { (void)c; fw.sync_calls++; return fw.sync_fail ? 1 : 0; }

/* ota_test wchunks LEN CHUNK SYNCEVERY FAILAT SHORTAT SYNCFAIL     "W <res> at N rc N syncfail N calls N bytes N syncs N maxchunk N"
   ota_test full FREE NEED                                          "FULL <space_ok> <message>" */
static int do_wchunks(int argc, char **argv)
{
    static mw2_u8 data[600000];
    struct msota_wio io;
    mw2_u32 at, sf;
    int rc, res;
    if (argc >= 5 && !strcmp(argv[1], "full")) {
        char m[96];
        msota_fmt_full(m, sizeof m, (mw2_u32)strtoul(argv[2], 0, 10), (mw2_u32)strtoul(argv[3], 0, 10));
        printf("FULL %d %s\n", msota_space_ok((mw2_u32)strtoul(argv[2], 0, 10), (mw2_u32)strtoul(argv[3], 0, 10)), m);
        return 0;
    }
    if (argc < 8) return 2;
    memset(&fw, 0, sizeof fw);
    fw.fail_at = (mw2_u32)strtol(argv[5], 0, 10); fw.short_at = (mw2_u32)strtol(argv[6], 0, 10); fw.sync_fail = atoi(argv[7]);
    io.ctx = 0; io.write = fw_write; io.sync = fw_sync;
    res = msota_write_chunks(&io, data, (mw2_u32)strtoul(argv[2], 0, 10), (mw2_u32)strtoul(argv[3], 0, 10), (mw2_u32)strtoul(argv[4], 0, 10), &at, &rc, &sf);
    printf("W %d at %lu rc %d syncfail %lu calls %lu bytes %lu syncs %lu maxchunk %lu\n", res, (unsigned long)at, rc, (unsigned long)sf, (unsigned long)fw.calls, (unsigned long)fw.bytes, (unsigned long)fw.sync_calls, (unsigned long)fw.maxn);
    return 0;
}

/* ota_test dns query ID NAME            the query packet as hex
   ota_test dns parse ID NAME HEXPACKET  "DNS <code> <name> ip A.B.C.D" */
static int do_dns(int argc, char **argv)
{
    mw2_u8 pk[1500];
    int n, i;
    if (argc >= 9 && !strcmp(argv[2], "resolve")) return do_dns_resolve(argc, argv);
    if (argc >= 5 && !strcmp(argv[2], "query")) {
        n = (int)msdns_query(pk, sizeof pk, (unsigned)strtoul(argv[3], 0, 0), argv[4]);
        for (i = 0; i < n; i++) printf("%02x", pk[i]);
        printf("\n");
        return 0;
    }
    if (argc >= 6 && !strcmp(argv[2], "parse")) {
        mw2_u32 ip = 0;
        int r;
        n = hexn(argv[5], pk, sizeof pk);
        r = msdns_parse(pk, (mw2_u32)n, (unsigned)strtoul(argv[3], 0, 0), argv[4], &ip);
        printf("DNS %d %s ip %u.%u.%u.%u\n", r, msdns_err(r), (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255));
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[2], "ipcache")) { mw2_u32 ip = 0; int ok = msota_ipcache_parse(argv[3], (mw2_u32)strtoul(argv[4], 0, 10), &ip); char o[40]; msota_ipcache_fmt(o, sizeof o, 0xC63364D6u, 1793711803u); printf("CACHE %d %u.%u.%u.%u FMT %s", ok, (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255), (unsigned)(ip & 255), o); return 0; }
    if (argc >= 4 && !strcmp(argv[2], "ok")) { mw2_u32 ip = (mw2_u32)strtoul(argv[3], 0, 0); printf("IPOK %d\n", msdns_ip_ok(ip)); return 0; }
    return 2;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "helpers")) { test_helpers(); printf(fails ? "ota helpers: %d FAILED\n" : "ota helpers: all passed\n", fails); return fails != 0; }
    if (argc >= 5 && !strcmp(argv[1], "shell")) return do_shell(argv[2], argv[3], argv[4]);
    if (argc >= 3 && !strcmp(argv[1], "dol")) return do_dol(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "model")) return do_model(argv[2]);
    if (argc >= 5 && !strcmp(argv[1], "check")) return do_check(argv[2], argv[3], argv[4]);
    if (argc >= 3 && !strcmp(argv[1], "dns")) return do_dns(argc, argv);
    if (argc >= 2 && (!strcmp(argv[1], "wchunks") || !strcmp(argv[1], "full"))) return do_wchunks(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "swap")) { test_swap(); printf(fails ? "ota swap: %d FAILED\n" : "ota swap: all passed\n", fails); return fails != 0; }
    if (argc >= 2 && !strcmp(argv[1], "chain")) { test_chain(); printf(fails ? "ota chain: %d FAILED\n" : "ota chain: all passed\n", fails); return fails != 0; }
    if (argc >= 2 && !strcmp(argv[1], "backoff")) { test_backoff(); printf(fails ? "ota backoff: %d FAILED\n" : "ota backoff: all passed\n", fails); return fails != 0; }
    if (argc >= 2 && !strcmp(argv[1], "state")) { test_state(); printf(fails ? "ota state: %d FAILED\n" : "ota state: all passed\n", fails); return fails != 0; }
    printf("usage: ota_test state | check PUBHEX MINSEQ FILE\n");
    return 2;
}
