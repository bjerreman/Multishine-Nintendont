/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The Wii network config block, the pure part: parse, validate, serialize and compare ONE connection row of the 0x1B5C-byte block that
 * /dev/net/ncd/manage hands out (ioctlv 3) and takes back (ioctlv 4), plus the "which row is selected" edit Nintendont's main.c makes.  No I/O, no heap, no libc beyond memcpy/memset/memcmp
 * (declared here by hand so the file also builds without hosted headers), no 64-bit arithmetic.  Tested by tests/netcfg_test.c (ASan + UBSan, strict C89 compile).
 *
 * LAYOUT SOURCES (every constant below is one of these; nothing is guessed except where a comment says UNVERIFIED):
 *   [WB] WiiBrew "/shared2/sys/net/02/config.dat" (https://wiibrew.org/wiki//shared2/sys/net/02/config.dat): 8-byte header + 3 connection entries of 0x91C, total 0x1B5C; the common part
 *        (flags, ip, netmask, gateway, dns1, dns2, mtu, two proxy structures) and the wireless part (rateset 0x7C0, ssid 0x7C4 x32, ssid_length 0x7E5, encryption 0x7E9, key_length 0x7ED,
 *        0x7EE "unknown", key 0x7F0 x64).  Encryption values OPN 0, WEP64 1, WEP128 2, WPA-PSK(TKIP) 4, WPA2-PSK(AES) 5, WPA-PSK(AES) 6.  WEP: key_length 0, the key stored 4 times (20 / 52 bytes).
 *   [NIN] Nintendont loader/source/main.c, the profile edit (the code this project already runs, ota/ms_ota_loader.c conn_apply_profile): rows at 0x8 / 0x924 / 0x1240, flag byte of a row
 *        |= 0xA0 selects it (&= 0x7F deselects), flag bit 0 = wired, header byte 4 = 2 wired / 1 wireless.
 *   [MS]  ota/ms_probe_core.c msp_cfg_kind (proven on our Wii): the same row offsets and bit 0.
 * KNOWN CONFLICT (WiiBrew's flag diagram is misaligned, its own page says so): WiiBrew lists bit 7 as PPPoE, Nintendont uses 0x80 as "selected" and 0x20 as "internet test passed".  This core
 * follows Nintendont and the proven code for 0x80 / 0x20 / 0x01 and WiiBrew for 0x04 (IP from DHCP) and 0x02 (DNS from DHCP); our own config is checked with the read-only layout
 * screen (msn_describe) before anything is written on his Wii.
 * Everything not named above is copied through untouched by msn_serialize (mtu, proxy, static addresses, padding).
 *
 * PRIVACY: the passphrase exists only in struct msn_row.key (and the block itself).  msn_describe never returns characters, only lengths and flags.
 */
#ifndef MS_NETCFG_CORE_H
#define MS_NETCFG_CORE_H

typedef unsigned char msn_u8;
typedef unsigned short msn_u16;
typedef unsigned int msn_u32;

#define MSN_CFG_LEN     0x1B5Cu     /* [WB] 8 + 3 * 0x91C */
#define MSN_HDR_SEL     4u          /* [NIN] 1 wireless, 2 wired ([WB] calls the byte "at least one valid connection") */
#define MSN_ROW0        0x8u        /* [WB] [NIN] */
#define MSN_ROW_SIZE    0x91Cu      /* [WB] */
#define MSN_ROWS        3u

#define MSN_O_FLAGS     0x000u      /* [WB] */
#define MSN_O_IP        0x004u      /* [WB] ip, netmask, gateway, dns1, dns2 = 5 x 4 bytes to 0x018 */
#define MSN_O_MTU       0x01Au      /* [WB] u16 */
#define MSN_O_PROXY     0x024u      /* [WB] proxy structure, byte 0 = use_proxy */
#define MSN_O_RATESET   0x7C0u      /* [WB] u16 */
#define MSN_O_SSID      0x7C4u      /* [WB] 32 bytes */
#define MSN_O_SSIDLEN   0x7E5u      /* [WB] */
#define MSN_O_ENC       0x7E9u      /* [WB] */
#define MSN_O_KEYLEN    0x7EDu      /* [WB] */
#define MSN_O_UNK       0x7EEu      /* [WB] "unknown": 0 or 1, toggles with WPA-PSK (TKIP) and hex-entered WEP */
#define MSN_O_KEY       0x7F0u      /* [WB] 64 bytes */
#define MSN_O_WIFI_END  0x830u      /* [WB] padding_13 follows (0xEC zero bytes) */

#define MSN_F_WIRED     0x01u       /* [WB] bit 0 interface, [NIN] */
#define MSN_F_DNS_DHCP  0x02u       /* [WB] bit 1 */
#define MSN_F_IP_DHCP   0x04u       /* [WB] bit 2 */
#define MSN_F_TESTED    0x20u       /* [NIN] set together with the selection (0xA0) */
#define MSN_F_SELECTED  0x80u       /* [NIN] */

enum { MSN_SEC_OPEN = 0, MSN_SEC_WEP64 = 1, MSN_SEC_WEP128 = 2, MSN_SEC_WPA_TKIP = 4, MSN_SEC_WPA2_AES = 5, MSN_SEC_WPA_AES = 6 };   /* [WB] */

/* result codes */
enum { MSN_OK = 0, MSN_E_ARG = 1, MSN_E_SHORT, MSN_E_PROFILE, MSN_E_SSID_EMPTY, MSN_E_SSID_LONG, MSN_E_SSID_CHARS, MSN_E_SEC, MSN_E_KEY_SHORT, MSN_E_KEY_LONG, MSN_E_KEY_CHARS, MSN_E_WEP_LEN, MSN_E_STORED };

/* one connection row as the editor sees it.  key is a SECRET (NUL-terminated copy of the passphrase / WEP key). */
struct msn_row {
    msn_u8 flags;           /* the raw flag byte of the row */
    int present;            /* the row holds a connection at all (flags or a name) */
    int selected;           /* flags & MSN_F_SELECTED */
    int wired;              /* flags & MSN_F_WIRED */
    int ip_dhcp, dns_dhcp;
    int use_proxy;          /* [WB] proxy byte 0 */
    msn_u32 ssid_len;       /* as stored, not clipped */
    char ssid[33];          /* NUL-terminated copy of at most 32 bytes */
    int sec;                /* MSN_SEC_* (the raw encryption byte) */
    msn_u32 key_len;        /* passphrase length: key_length byte for WPA, 5 / 13 for WEP (derived from the type), 0 for open */
    char key[65];           /* SECRET */
    msn_u8 unk;             /* the 0x7EE byte */
    msn_u16 rateset, mtu;
};

msn_u32 msn_row_off(msn_u32 profile);                                                   /* 0 for anything but 1..3, else the offset of the row in the block */
int msn_parse(const msn_u8 *cfg, msn_u32 len, msn_u32 profile, struct msn_row *r);      /* MSN_OK, MSN_E_SHORT, MSN_E_PROFILE; a damaged row still parses (ssid_len > 32 stays visible), msn_validate judges it */
int msn_validate(const struct msn_row *r);                                              /* the rules the editor enforces before a write: SSID 1..32 printable ASCII; WPA 8..63 printable; WEP 5 / 13 printable (ASCII keys only); open = no key; wired = nothing to check */
const char *msn_v_text(int code);                                                       /* plain-language text for MSN_E_* (sentence case, one line) */
int msn_sec_ok(int sec);
msn_u32 msn_sec_next(int sec, int dir);                                                 /* cycles Open, WEP 64, WEP 128, WPA (TKIP), WPA2 (AES), WPA (AES) */
const char *msn_sec_name(int sec);
int msn_serialize(msn_u8 *cfg, msn_u32 len, msn_u32 profile, const struct msn_row *r);  /* validates, then edits ONLY this row in place (see the .c comment); the selection is separate (msn_select) */
int msn_select(msn_u8 *cfg, msn_u32 len, msn_u32 profile);                              /* Nintendont's edit: 0xA0 on the row, 0x80 cleared on the other two, header byte 4 */
int msn_restore(msn_u8 *cfg, msn_u32 len, msn_u32 profile, const msn_u8 *old);          /* put the old row, its old flag bytes of all three rows and the old header byte 4 back onto a fresh read; nothing else changes */
int msn_row_equal(const msn_u8 *a, const msn_u8 *b, msn_u32 len, msn_u32 profile);      /* 1 when the row bytes are identical */
int msn_rest_equal(const msn_u8 *a, const msn_u8 *b, msn_u32 len, msn_u32 profile);     /* 1 when everything outside this row differs at most in header byte 4 and the 0x80 bit of the other rows' flag bytes (a selection) */
int msn_row_stored(const msn_u8 *cfg, msn_u32 len, msn_u32 profile, const struct msn_row *want);   /* 1 when the block's row says what `want` says (type, name, type of security, key) after a write: the read-back check */
void msn_wipe(void *p, msn_u32 n);                                                      /* zero a secret so the compiler cannot drop it */

/* the read-only layout line of one row for the screen and the log: flags, kind, name LENGTH, security, key LENGTH, DHCP/static, proxy; never a character of the name or the key */
struct msn_info {
    msn_u8 flags, enc, unk, hdr_sel;
    msn_u32 ssid_len, key_len;
    int wired, selected, ip_dhcp, dns_dhcp, use_proxy, tested;
    msn_u16 rateset, mtu;
    msn_u32 key_nz, key_tail, key_bin;   /* counts over the 64 key bytes: non-zero, non-zero past key_len, not printable ASCII */
    int key_nonzero;        /* the 64 key bytes hold at least one non-zero byte (WPA/WEP only) */
};
int msn_describe(const msn_u8 *cfg, msn_u32 len, msn_u32 profile, struct msn_info *o);
int msn_info_line(char *out, msn_u32 cap, msn_u32 profile, const struct msn_info *o);   /* "1: sel wireless, flags A6, name 9, WPA2, key 12, dhcp ip+dns, mtu 0, rates 0F" */

/* ---- Nintendont's Network Profile in SD:/multishine/nincfg.bin follows a saved connection (connection 2, a cable, saved in this editor; the profile stayed 0 = Auto and
   every boot selected connection 1, Wi-Fi, again).  The settings file is NIN_CFG (CommonConfig.h): magic 01 07 0C F6, 548 bytes, NetworkProfile = byte 543, low 2 bits (0 Auto, 1..3). */
#define MSN_NINCFG_LEN     548u
#define MSN_NINCFG_PROFILE 543u
/* set the profile in a copy of the file (b, n bytes): only the low 2 bits of byte 543 change.  1 = changed, 0 = already that profile (nothing to write), -1 = not a settings file (magic, length) or profile above 3.
   The editor writes 1..3 after a kept save; 0 (Auto) only when a put-back returns to a profile that was Auto. */
int msn_nincfg_set_profile(msn_u8 *b, msn_u32 n, msn_u32 profile);
/* the profile the file names: 0..3, or -1 when it is not a settings file */
int msn_nincfg_profile(const msn_u8 *b, msn_u32 n);
/* the editor's line: "Used at start: connection 2 (cable)" / "Used at start: connection 1 (Wi-Fi)" / "Used at start: Auto (the Wii's own choice)"; wired: 1 cable, 0 Wi-Fi, -1 unknown (no kind shown) */
int msn_start_line(char *out, msn_u32 cap, msn_u32 profile, int wired);

#endif
