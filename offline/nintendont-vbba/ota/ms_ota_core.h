/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* Nintendont OTA, the pure part: verify a kernel container, keep the rollback state, decide which kernel starts.
 * Platform neutral (no libc beyond memcpy-free byte loops, no heap): the loader (ms_ota_loader.c) and the host test (tests/ota_test.c) compile this same file.
 * Needs offline/client_net/mw2_crypto.[ch] (SHA-512, Ed25519 verify) and offline/cardloader/cl/mscf_verify.h (the ONE signed-message definition of every MSCF file). */
#ifndef MS_OTA_CORE_H
#define MS_OTA_CORE_H
#include "mw2_crypto.h"

#define MSOTA_ABI          5u          /* MSCF ABI of the kernel container (krn-N.mscf) */
#define MSOTA_SHELL_ABI    1u          /* what this shell offers to a kernel: a kernel that needs more is refused */
#define MSOTA_MAX_KERNEL   (1536u * 1024u)
#define MSOTA_MAX_FILE     (MSOTA_MAX_KERNEL + 0x2000u * 3u)
#define MSOTA_MAX_FAIL     2u          /* starts of a downloaded kernel that never reached the game before it is dropped */

enum { MSOTA_OK = 0, MSOTA_E_SIZE, MSOTA_E_HDR, MSOTA_E_ABI, MSOTA_E_SEQ, MSOTA_E_CRC, MSOTA_E_SIG, MSOTA_E_PAYLOAD, MSOTA_E_SHELL, MSOTA_E_DOL };

struct msota_kernel {
    const mw2_u8 *zip;                 /* points into the checked file buffer */
    mw2_u32 zip_len;
    mw2_u32 seq;
};

/* file = a served krn-N.mscf (header at 0x1000, payload at 0x2000, no 0x40-byte directory entry).  pubs = the pinned release keys.  min_seq = the floor: a file with a
   lower build_seq is refused (anti-rollback).  -> MSOTA_OK and *out, or the first failing check in the loader's order (size, header, ABI, seq, header CRC, payload CRC,
   signature, payload layout, shell ABI). */
int msota_check(const mw2_u8 *file, mw2_u32 len, const mw2_u8 (*pubs)[32], unsigned npub, mw2_u32 min_seq, struct msota_kernel *out);
const char *msota_err(int e);

/* ---- the rollback state: SD:/multishine/ota.st, 24 bytes ------------------------------------------------------------------------------------------------------------
   'MKOS' | u8 version 1 | u8 pending | u8 fail | u8 0 | u32 floor | u32 active | u32 bad | u32 CRC-32 of the first 20 bytes   (big endian)
   floor  = the highest kernel seq ever installed (a kernel below it is never started again); active = the seq started last; bad = a seq that was rolled back (not retried);
   pending = the downloaded kernel was started and has not yet been confirmed; fail = consecutive unconfirmed starts. */
#define MSOTA_ST_LEN 24u
struct msota_state { mw2_u32 floor, active, bad; mw2_u8 pending, fail; };
void msota_state_init(struct msota_state *st);
int msota_state_parse(struct msota_state *st, const mw2_u8 *b, mw2_u32 n);          /* 1 = valid; otherwise *st is the empty state and 0 is returned */
void msota_state_pack(const struct msota_state *st, mw2_u8 out[MSOTA_ST_LEN]);

enum { MSOTA_PICK_EMBEDDED = 0, MSOTA_PICK_DOWNLOADED = 1 };
/* The decision at every start.  file_ok / file_seq = the result of msota_check on SD:/multishine/kernel.bin (0 / 0 when there is none or it does not verify);
   prev_confirmed = the previous run's /ndebug.log has the 'RUNID: game' line (the kernel reached the game).  Updates *st (the caller writes it before the kernel starts).
   *action: 0 nothing special, 1 = a downloaded kernel was dropped now (rolled back: the caller renames kernel.bin so it is not tried again), 2 = refused (older than the floor). */
int msota_decide(struct msota_state *st, int file_ok, mw2_u32 file_seq, int prev_confirmed, int *action);
/* when the shell writes shl.ok (the stub's proof that a downloaded shell works, msota_decide's prev_confirmed for the shell chain).  Up to 166 only the game start wrote it, so starts that
   only opened the menus (Settings, the network editor) dropped a good shell after MSOTA_MAX_FAIL starts.  Now the start screen writes it as well, once per run, when it has been drawn with the
   pads up (FPAD_Init done: ms_ota_pads_ready) and the SD mounted.  A shell that crashes or hangs before its start screen still writes nothing, so the stub still drops it.  1 = write it now */
int msota_alive_due(int pads_up, int sd_ok, int screen_drawn, int already);

mw2_u32 msota_crc32(const mw2_u8 *b, mw2_u32 n);
/* does b[0..n) contain the 11 bytes 'RUNID: game'? */
int msota_has_runid(const mw2_u8 *b, mw2_u32 n);

/* ---- the SHELL container shl-N.mscf (MSCF ABI 6, tools/card/shl_data.py): a signed Nintendont loader DOL that the immutable STUB chain-loads ------------------------------------------ */
#define MSOTA_SHL_ABI      6u
#define MSOTA_STUB_ABI     1u          /* what the stub offers to a shell: a shell that needs more is refused */
#define MSOTA_MAX_DOL      (3u * 1024u * 1024u)
#define MSOTA_MAX_SHELL_FILE (MSOTA_MAX_DOL + 0x2000u * 3u)
struct msota_shell {
    const mw2_u8 *dol;                 /* points into the checked file buffer */
    mw2_u32 dol_len;
    mw2_u32 seq;
};
/* same order of checks as msota_check, payload 'MSHL', and the DOL must pass msdol_check (MSOTA_E_DOL) */
int msota_check_shell(const mw2_u8 *file, mw2_u32 len, const mw2_u8 (*pubs)[32], unsigned npub, mw2_u32 min_seq, struct msota_shell *out);

/* the checks every MSCF kind shares (size, header, ABI, seq, header CRC, payload CRC, signature, nothing after the payload) -> MSOTA_OK with *img_out / *ilen_out / *seq_out; used by the kind checkers (also ms_card_core.c) */
int msota_check_container(const mw2_u8 *file, mw2_u32 len, mw2_u32 max_file, unsigned abi, const mw2_u8 (*pubs)[32], unsigned npub, mw2_u32 min_seq,
                          const mw2_u8 **img_out, mw2_u32 *ilen_out, mw2_u32 *seq_out);

/* ---- the DOL rules the stub applies before it jumps (the same rules as tools/card/shl_data.py dol_check) ----------------------------------------------------------------------- */
#define MSDOL_LO 0x80003100u           /* below it: the Homebrew Channel's reload stub and arguments stay intact */
#define MSDOL_HI 0x81300000u           /* from here up: the stub's trampoline and the Homebrew Channel arguments (Nintendont's own loader parks its DOL launcher here only after it started) */
#define MSDOL_NSEC 18u
struct msdol_sec { mw2_u32 src, dst, len; };          /* src = offset in the DOL file */
struct msdol_plan {
    struct msdol_sec sec[MSDOL_NSEC];
    unsigned n;                        /* non-empty sections, in file order */
    mw2_u32 bss, bss_len, entry;
};
enum { MSDOL_OK = 0, MSDOL_E_SIZE, MSDOL_E_SECTION, MSDOL_E_ADDR, MSDOL_E_BSS, MSDOL_E_ENTRY, MSDOL_E_OVERLAP };
int msdol_check(const mw2_u8 *dol, mw2_u32 len, struct msdol_plan *plan);          /* MSDOL_OK and *plan, or the first failing rule */
const char *msdol_err(int e);
/* ---- small pure helpers of the loader's bounded HTTP client (host-tested) -------------------------------------------------------------------------------------------------- */
/* the head of an HTTP response in b[0..n): -1 = the head is not complete yet; 0 = complete: *status (0 if the line is not 'HTTP/1.x NNN'), *clen = the Content-Length (0xFFFFFFFF if absent or chunked), *hdr_end = offset of the body */
int msota_http_head(const mw2_u8 *b, mw2_u32 n, mw2_u32 *status, mw2_u32 *clen, mw2_u32 *hdr_end);
/* "A.B.C.D", optionally ":port", optionally followed by "/..." (a URL tail): 0 = ok (*ip host order, *port 0 if none), -1 otherwise */
int msota_parse_ip(const char *s, mw2_u32 *ip, unsigned *port);
/* the unsigned number after "key": in a small JSON text (0 if absent or null) */
mw2_u32 msota_json_uint(const char *s, const char *key);

/* 'Multishine update (kernel K, shell S)' with the real values (the Update screen's first line) */
void msota_fmt_header(char *out, unsigned long n, unsigned long kernel, unsigned long shell);

/* The back-off of the boot check.  A start whose check cannot reach the server waits before it tries again; what it waited for decides how long: the network was not up in time = 10 minutes,
   the network was up but the server did not answer = 1 hour.  Any successful contact with the server (the boot check or the manual check) and any installed file clear it; a skip by the user
   changes nothing.  -> the new 'next' epoch (0 = no back-off).  A check is skipped while next > now. */
enum { MSOTA_BO_NET_FAIL = 1, MSOTA_BO_SERVER_FAIL, MSOTA_BO_SUCCESS, MSOTA_BO_INSTALLED, MSOTA_BO_SKIPPED };
#define MSOTA_BO_NET_S 0u       /* a failed check is made again at the NEXT start (Zell: a 10 minute or 1 hour pause meant "Up to date" on the next start; a start is bounded by 12 s and Hold B skips it) */
#define MSOTA_BO_SERVER_S 0u
mw2_u32 msota_backoff(int event, mw2_u32 now, mw2_u32 cur);

/* the file operations the swap needs (the loader backs them with FatFs, the host test with a fake card); rename returns 0 or an error code, unlink of a missing file is fine */
struct msota_fs {
    void *ctx;
    int (*exists)(void *ctx, const char *path);
    int (*unlink)(void *ctx, const char *path);
    int (*rename)(void *ctx, const char *from, const char *to);
    int (*create)(void *ctx, const char *path);       /* write a small marker file; 0 or an error code */
};
enum { MSOTA_SW_OK = 0, MSOTA_SW_BAK = 1, MSOTA_SW_FAIL = 2 };   /* BAK = the old file could not be moved aside (nothing changed); FAIL = the new file could not be moved in (the old one is back) */
int msota_swap_in(const struct msota_fs *fs, const char *bin, const char *knew, const char *bak);

/* The DNS fallback: build a recursive A query for `name` (dotted, <= 63 per label) into out; -> its length, 0 if it does not fit.  Parse the answer: the id and the question must be ours, the
   header a clean response; CNAMEs are followed (an answer must be for the name asked or for a CNAME target), the first A record that is a public address (msdns_ip_ok) is returned in *ip
   (host order).  Every offset, label, pointer hop and length is checked; a truncated packet gives what was complete before the cut. */
enum { MSDNS_OK = 0, MSDNS_E_SHORT, MSDNS_E_ID, MSDNS_E_FLAGS, MSDNS_E_QUESTION, MSDNS_E_NOA, MSDNS_E_FORMAT };
mw2_u32 msdns_query(mw2_u8 *out, mw2_u32 cap, unsigned id, const char *name);
int msdns_parse(const mw2_u8 *p, mw2_u32 n, unsigned id, const char *name, mw2_u32 *ip);
int msdns_ip_ok(mw2_u32 ip);
const char *msdns_err(int e);

/* the resolver loop: send the query, wait up to timeout_ms for an answer (a wrong id or a broken packet is ignored and the wait goes on), up to `tries` times and never longer than budget_ms in all.
   0 = *ip set; -1 no usable answer in time; -2 send / receive error; -3 the resolver said no (NXDOMAIN, nothing usable).  *last_parse = the last msdns_parse result. */
struct msdns_io {
    void *ctx;
    long (*send)(void *ctx, const mw2_u8 *pkt, mw2_u32 n);          /* >= 0 ok */
    long (*recv)(void *ctx, mw2_u8 *buf, mw2_u32 cap);              /* > 0 bytes of an answer from the right server, 0 nothing yet, < 0 error */
    mw2_u32 (*now)(void *ctx);                                       /* milliseconds */
    void (*wait)(void *ctx, mw2_u32 ms);
};
int msdns_resolve(const struct msdns_io *io, unsigned id, const char *name, mw2_u32 timeout_ms, unsigned tries, mw2_u32 budget_ms, mw2_u32 *ip, int *last_parse);

/* SD:/multishine/ota.ip: "A.B.C.D EPOCH".  parse: 1 and *ip when the address is public and the entry is at most 30 days old (now 0 = clock unknown: the age is not checked); fmt writes the text. */
int msota_ipcache_parse(const char *txt, mw2_u32 now, mw2_u32 *ip);
int msota_ipcache_fmt(char *out, unsigned n, mw2_u32 ip, mw2_u32 now);

/* Writing a file in checked chunks: write returns 0 or a FatFs code and how many bytes it took; a short write or an error stops at once and says where (*at) and with what (*rc);
   sync() is called every sync_every bytes and at the end, a failing sync is only counted (*sync_fail).  MSOTA_W_OK / _WRITE (an error code) / _SHORT (fewer bytes than asked). */
struct msota_wio {
    void *ctx;
    int (*write)(void *ctx, const mw2_u8 *p, mw2_u32 n, mw2_u32 *put);
    int (*sync)(void *ctx);
};
enum { MSOTA_W_OK = 0, MSOTA_W_WRITE = 1, MSOTA_W_SHORT = 2 };
int msota_write_chunks(const struct msota_wio *io, const mw2_u8 *data, mw2_u32 len, mw2_u32 chunk, mw2_u32 sync_every, mw2_u32 *at, int *rc, mw2_u32 *sync_fail);
/* the 'SD card full' text and the check (0 = room for need_bytes plus 256 KB of slack, 1 = full) */
void msota_fmt_full(char *out, unsigned n, mw2_u32 free_bytes, mw2_u32 need_bytes);
int msota_space_ok(mw2_u32 free_bytes, mw2_u32 need_bytes);

/* The in-place hand-over to a freshly installed shell, from the early check (before the IOS58 reload).  The decision, in this order: a shell was installed; the state is the early one; this start is
   not itself a hand-over (the marker word in MEM1, so never twice per power-on); the hand-over is not switched off (SD:/multishine/no_chain, or an earlier hand-over that never confirmed); the file
   verifies again from the card; the DOL passes msdol_check; a MEM2 buffer holds it.  Anything else: the shell is applied at the next start, as before. */
struct msota_chain_in { int installed, early, marker, disabled, verified, dol_ok, mem2_ok, reload_ok; };   /* early = before the IOS58 reload; late (the menu is up: manual check, or the check at the game start) needs reload_ok = the clean IOS58 reload that brings the machine back to the state the stub hands over */
enum { MSOTA_CH_GO = 0, MSOTA_CH_NOINST, MSOTA_CH_LATE, MSOTA_CH_GUARD, MSOTA_CH_DISABLED, MSOTA_CH_NOVERIFY, MSOTA_CH_DOL, MSOTA_CH_MEM };
int msota_chain_decide(const struct msota_chain_in *in);
/* WHAT happens after a shell install (design rule: the app restarts into the new shell by itself).  DEFAULT = the in-place hand-over (ms_tramp2.S, minimal teardown, breadcrumbs in ota.log) on every
   path (early check, manual Update, About check), no marker needed.  It is skipped, and the calm words 'It starts the next time you open Nintendont.' are shown instead, when it is switched off or unsafe:
   SD:/multishine/no_chain (also written by the shell itself after a hand-over that never came alive), two failed hand-overs in a row (chain.fail), the loop guard, the DOL or the staging buffer refused.
   Dev marker SD:/multishine/reboot asks for the console restart (STM_RebootSystem) instead; SD:/multishine/inplace means nothing any more (inplace_marker is kept in the struct, ignored).
   installed: a shell was installed; verified: the installed file verifies again from the card; reboot_marker / reboot_ok: the dev marker and the availability of the reset; inplace_ok: msota_chain_decide says go.
   -> MSOTA_RS_NONE (nothing installed), MSOTA_RS_BAD (the installed file did not verify), MSOTA_RS_REBOOT, MSOTA_RS_INPLACE, MSOTA_RS_TEXT (the calm words). */
struct msota_restart_in { int installed, verified, reboot_marker, reboot_ok, inplace_marker, inplace_ok; };
enum { MSOTA_RS_NONE = 0, MSOTA_RS_BAD, MSOTA_RS_INPLACE, MSOTA_RS_REBOOT, MSOTA_RS_TEXT };
/* what ms_chain_run calls around its steps: a breadcrumb line to the SD (while it is mounted) and the last-moment shutdown of the SD / USB */
/* the hand-over flavour the new shell was started in: the guard word 'MSCH' says 'this start is a hand-over', the word after it says 'LATE' when the old shell had ALREADY reloaded and patched IOS58 and
   loaded the Nintendont kernel (the menu was up); the early path leaves it 0 (IOS not yet touched).  1 = skip the new shell's IOS reload / patch and the kernel load, it continues on what runs. */
#define MSOTA_CHAIN_GUARD 0x4D534348u
#define MSOTA_CHAIN_LATE  0x4C415445u
int msota_chain_is_late(mw2_u32 guard, mw2_u32 flavour);
/* the guard word is read ONCE and cleared (m[0] guard, m[1] flavour): a warm reload after a crash must not look like a hand-over again (the words used to stay in MEM1 for good).
   *marker = 'this start is a hand-over', *late = msota_chain_is_late. */
void msota_chain_take(volatile mw2_u32 *m, int *marker, int *late);
/* (DSI of 170 -> 171 on Wi-Fi): 'LATE' only says the old shell had reloaded and patched IOS58; 170 also wrote it when it handed over from ms_ota_select, i.e. BEFORE its kernel
   was loaded.  The new shell asks the kernel: the Nintendont kernel's loader loop clears any RESET_STATUS word other than 0x0DEA (game) / 0x1DEA (exit) (kernel/main.c), so a probe word that
   goes back to 0 proves a running kernel; stale MEM2 can never clear it.  rs = RESET_STATUS (0xD3003420), hid_change / hid_size = HID_CHANGE / HID_CFG_SIZE (0xD3003444 / 0xD3003448): a
   pending controller.ini request of the kernel (it busy-waits for the loader in HIDOpen) is answered 'no file' before every poll, so a busy kernel is not taken for a missing one.  wait() is
   called between polls (the loader: 1 ms).  1 = the kernel answered; 0 = nobody did (the probe word is put back to 0). */
#define MSOTA_KPROBE 0x4D535052u
int msota_kernel_probe(volatile mw2_u32 *rs, volatile mw2_u32 *hid_change, volatile mw2_u32 *hid_size, unsigned polls, void (*wait)(void *ctx), void *ctx);
struct ms_chain_hooks { void (*crumb)(const char *text); void (*shutdown)(void); int full; int late; int kreload; };   /* kreload: the restart after a kernel install (early flavour on a machine whose IOS stays: the STM hook is released as on a late hand-over) */   /* late: the machine was already reloaded / patched (see msota_chain_is_late); full: run every teardown step (dev: SD:/multishine/teardown_full) */
int msota_restart_decide(const struct msota_restart_in *in);
const char *msota_chain_name(int decision);
/* The restart for a newer kernel.  When a kernel already runs (this start is a late hand-over: the previous loader's kernel goes on, or the menu is up) and a verified SD:/multishine/kernel.bin is
   newer than that kernel, the new kernel only starts after a fresh start.  That happens when it was installed after the running kernel was loaded: by this start's boot check, or by an older
   loader's check in the start that handed over to this one.  The app then restarts once by itself: the running kernel is stopped and the loader is started again in the early flavour, so the
   new start reloads IOS58 and loads kernel.bin.  A shell installed in the same check is the DOL of that one restart.  The loop guard is SD:/multishine/krn.rst (the kernel seq the last such
   restart was made for): never twice for one seq.
   file_seq: the seq of the verified kernel.bin (0 = none, or it does not verify); floor / bad / active: ota.st (active = the seq the last kernel load started; an install sets it to 0, so a
   kernel installed after the last load never equals it); running: a kernel runs now; marker_seq: krn.rst (0 = absent); dol_ok: a DOL to restart into verifies (the new shell, or this
   shell's own shell.dol).  -> MSOTA_KR_NONE (no restart needed: no kernel.bin, a kernel.bin msota_decide would refuse (below the floor, the rolled-back seq), or it is the kernel that
   runs), _GO (restart now, write krn.rst first), _ONCE (already restarted for this seq: never again), _NODOL (no DOL to start). */
struct msota_krestart_in { mw2_u32 file_seq, floor, bad, active, marker_seq; int running, dol_ok; };
enum { MSOTA_KR_NONE = 0, MSOTA_KR_GO, MSOTA_KR_ONCE, MSOTA_KR_NODOL };
int msota_krestart_decide(const struct msota_krestart_in *in);
/* the first start after such a restart: marker_seq = krn.rst, picked_seq = the kernel this start loads (0 = the embedded one).  -> MSOTA_KA_NONE (no restart was made), _RUNS (the new kernel
   runs), _NOT (it does not: no second restart, krn.rst stays) */
enum { MSOTA_KA_NONE = 0, MSOTA_KA_RUNS, MSOTA_KA_NOT };
int msota_krestart_after(mw2_u32 marker_seq, mw2_u32 picked_seq);
const char *msota_krestart_name(int decision);
/* the argument block the stub also builds (stub_main.c restore_args): argv strings NUL terminated plus a final NUL, never more than cap bytes */
mw2_u32 msota_args_build(char *dst, mw2_u32 cap, int argc, const char *const *argv);

#endif
