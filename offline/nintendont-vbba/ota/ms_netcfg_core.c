/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The Wii network config block, the pure part.  Layout sources and the privacy rule are in ms_netcfg_core.h.  Tested by tests/netcfg_test.c. */
#include "ms_netcfg_core.h"

static void m_set(void *d, int v, msn_u32 n) { msn_u8 *p = (msn_u8 *)d; msn_u32 i; for (i = 0; i < n; i++) p[i] = (msn_u8)v; }
static void m_cpy(void *d, const void *s, msn_u32 n) { msn_u8 *p = (msn_u8 *)d; const msn_u8 *q = (const msn_u8 *)s; msn_u32 i; for (i = 0; i < n; i++) p[i] = q[i]; }
static int m_cmp(const void *a, const void *b, msn_u32 n) { const msn_u8 *p = (const msn_u8 *)a, *q = (const msn_u8 *)b; msn_u32 i; for (i = 0; i < n; i++) if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1; return 0; }

void msn_wipe(void *p, msn_u32 n)
{
    volatile msn_u8 *v = (volatile msn_u8 *)p;
    msn_u32 i;
    for (i = 0; i < n; i++) v[i] = 0;
}

msn_u32 msn_row_off(msn_u32 profile)
{
    if (profile < 1u || profile > MSN_ROWS) return 0;
    return MSN_ROW0 + (profile - 1u) * MSN_ROW_SIZE;
}

int msn_sec_ok(int sec)
{
    return sec == MSN_SEC_OPEN || sec == MSN_SEC_WEP64 || sec == MSN_SEC_WEP128 || sec == MSN_SEC_WPA_TKIP || sec == MSN_SEC_WPA2_AES || sec == MSN_SEC_WPA_AES;
}
msn_u32 msn_sec_next(int sec, int dir)
{
    static const int order[6] = { MSN_SEC_OPEN, MSN_SEC_WEP64, MSN_SEC_WEP128, MSN_SEC_WPA_TKIP, MSN_SEC_WPA2_AES, MSN_SEC_WPA_AES };
    int i, at = 4;                                                    /* an unknown value starts from WPA2, the usual one */
    for (i = 0; i < 6; i++) if (order[i] == sec) at = i;
    at += dir >= 0 ? 1 : 5;
    return (msn_u32)order[at % 6];
}
const char *msn_sec_name(int sec)
{
    switch (sec) {
    case MSN_SEC_OPEN: return "Open";
    case MSN_SEC_WEP64: return "WEP 64";
    case MSN_SEC_WEP128: return "WEP 128";
    case MSN_SEC_WPA_TKIP: return "WPA (TKIP)";
    case MSN_SEC_WPA2_AES: return "WPA2 (AES)";
    case MSN_SEC_WPA_AES: return "WPA (AES)";
    default: return "Unknown";
    }
}

static msn_u32 wep_len(int sec) { return sec == MSN_SEC_WEP64 ? 5u : sec == MSN_SEC_WEP128 ? 13u : 0u; }

int msn_parse(const msn_u8 *cfg, msn_u32 len, msn_u32 profile, struct msn_row *r)
{
    msn_u32 o = msn_row_off(profile), n, i;
    const msn_u8 *p;
    if (!cfg || !r) return MSN_E_ARG;
    m_set(r, 0, (msn_u32)sizeof *r);
    if (len < MSN_CFG_LEN) return MSN_E_SHORT;
    if (!o) return MSN_E_PROFILE;
    p = cfg + o;
    r->flags = p[MSN_O_FLAGS];
    r->selected = (r->flags & MSN_F_SELECTED) != 0;
    r->wired = (r->flags & MSN_F_WIRED) != 0;
    r->ip_dhcp = (r->flags & MSN_F_IP_DHCP) != 0;
    r->dns_dhcp = (r->flags & MSN_F_DNS_DHCP) != 0;
    r->use_proxy = p[MSN_O_PROXY] != 0;
    r->ssid_len = p[MSN_O_SSIDLEN];
    n = r->ssid_len > 32u ? 32u : r->ssid_len;
    for (i = 0; i < n; i++) r->ssid[i] = (char)p[MSN_O_SSID + i];
    r->ssid[n] = 0;
    r->sec = p[MSN_O_ENC];
    r->unk = p[MSN_O_UNK];
    r->rateset = (msn_u16)(((msn_u32)p[MSN_O_RATESET] << 8) | p[MSN_O_RATESET + 1u]);      /* big endian, like every Wii structure */
    r->mtu = (msn_u16)(((msn_u32)p[MSN_O_MTU] << 8) | p[MSN_O_MTU + 1u]);
    if (r->sec == MSN_SEC_WEP64 || r->sec == MSN_SEC_WEP128) r->key_len = wep_len(r->sec);
    else if (r->sec == MSN_SEC_OPEN) r->key_len = 0;
    else r->key_len = p[MSN_O_KEYLEN];
    n = r->key_len > 64u ? 64u : r->key_len;
    for (i = 0; i < n; i++) r->key[i] = (char)p[MSN_O_KEY + i];
    r->key[n] = 0;
    r->present = (r->flags != 0 || r->ssid_len != 0);
    return MSN_OK;
}

static int printable(const char *s, msn_u32 n)
{
    msn_u32 i;
    for (i = 0; i < n; i++) if ((msn_u8)s[i] < 0x20u || (msn_u8)s[i] > 0x7Eu) return 0;
    return 1;
}
static msn_u32 slen(const char *s, msn_u32 cap) { msn_u32 n = 0; while (n < cap && s[n]) n++; return n; }

int msn_validate(const struct msn_row *r)
{
    msn_u32 kn, sn;
    if (!r) return MSN_E_ARG;
    if (r->wired) return MSN_OK;
    sn = slen(r->ssid, 33u);
    if (sn == 0) return MSN_E_SSID_EMPTY;
    if (sn > 32u) return MSN_E_SSID_LONG;
    if (!printable(r->ssid, sn)) return MSN_E_SSID_CHARS;
    if (!msn_sec_ok(r->sec)) return MSN_E_SEC;
    kn = slen(r->key, 65u);
    if (r->sec == MSN_SEC_OPEN) return MSN_OK;
    if (!printable(r->key, kn)) return MSN_E_KEY_CHARS;
    if (r->sec == MSN_SEC_WEP64 || r->sec == MSN_SEC_WEP128) return kn == wep_len(r->sec) ? MSN_OK : MSN_E_WEP_LEN;
    if (kn < 8u) return MSN_E_KEY_SHORT;
    if (kn > 63u) return MSN_E_KEY_LONG;
    return MSN_OK;
}

const char *msn_v_text(int code)
{
    switch (code) {
    case MSN_OK: return "Fine";
    case MSN_E_SHORT: return "The Wii network settings could not be read.";
    case MSN_E_PROFILE: return "Choose connection 1, 2 or 3 first.";
    case MSN_E_SSID_EMPTY: return "Type the network name.";
    case MSN_E_SSID_LONG: return "The network name is longer than 32 characters.";
    case MSN_E_SSID_CHARS: return "The network name has a character that cannot be typed here.";
    case MSN_E_SEC: return "Choose the security type.";
    case MSN_E_KEY_SHORT: return "The password needs at least 8 characters.";
    case MSN_E_KEY_LONG: return "The password can have at most 63 characters.";
    case MSN_E_KEY_CHARS: return "The password has a character that cannot be typed here.";
    case MSN_E_WEP_LEN: return "A WEP password has 5 characters (WEP 64) or 13 (WEP 128).";
    case MSN_E_STORED: return "The Wii did not keep what was written.";
    default: return "Something is wrong with these settings.";
    }
}

/* Edits ONLY the row: flag bit 0 (wired / wireless) and, when the row was empty, the DHCP bits (a new row asks for its address like the Wii menu's default); for a wireless row the rateset when it was 0 is
   left alone (UNVERIFIED which value the Wii writes; our layout screen shows it), ssid + length, encryption, key length, key (WEP: stored four times, key_length 0), the 0x7EE byte only when the security type
   changed.  A wired row zeroes the whole wireless part ([WB]: "all related to ssid, encryption and keys is set to 0x00").  Static addresses, mtu, proxy and padding are never touched. */
int msn_serialize(msn_u8 *cfg, msn_u32 len, msn_u32 profile, const struct msn_row *r)
{
    msn_u32 o = msn_row_off(profile), n, k, w, i;
    msn_u8 *p;
    int v, oldsec;
    if (!cfg || !r) return MSN_E_ARG;
    if (len < MSN_CFG_LEN) return MSN_E_SHORT;
    if (!o) return MSN_E_PROFILE;
    v = msn_validate(r);
    if (v != MSN_OK) return v;
    p = cfg + o;
    if (p[MSN_O_FLAGS] == 0 && p[MSN_O_SSIDLEN] == 0) p[MSN_O_FLAGS] = (msn_u8)(MSN_F_IP_DHCP | MSN_F_DNS_DHCP);    /* UNVERIFIED default of a never used row */
    if (r->wired) {
        p[MSN_O_FLAGS] = (msn_u8)(p[MSN_O_FLAGS] | MSN_F_WIRED);
        m_set(p + MSN_O_RATESET, 0, MSN_O_WIFI_END - MSN_O_RATESET);
        return MSN_OK;
    }
    p[MSN_O_FLAGS] = (msn_u8)(p[MSN_O_FLAGS] & ~MSN_F_WIRED);
    oldsec = p[MSN_O_ENC];
    n = slen(r->ssid, 32u);
    m_set(p + MSN_O_SSID, 0, 32u);
    m_cpy(p + MSN_O_SSID, r->ssid, n);
    p[MSN_O_SSIDLEN] = (msn_u8)n;
    p[MSN_O_ENC] = (msn_u8)r->sec;
    if (oldsec != r->sec) p[MSN_O_UNK] = (msn_u8)(r->sec == MSN_SEC_WPA_TKIP ? 1 : 0);
    m_set(p + MSN_O_KEY, 0, 64u);
    k = slen(r->key, 64u);
    w = wep_len(r->sec);
    if (r->sec == MSN_SEC_OPEN) p[MSN_O_KEYLEN] = 0;
    else if (w) {
        p[MSN_O_KEYLEN] = 0;
        for (i = 0; i < 4u; i++) m_cpy(p + MSN_O_KEY + i * w, r->key, w);
    } else {
        p[MSN_O_KEYLEN] = (msn_u8)k;
        m_cpy(p + MSN_O_KEY, r->key, k);
    }
    return MSN_OK;
}

int msn_select(msn_u8 *cfg, msn_u32 len, msn_u32 profile)
{
    msn_u32 k, o = msn_row_off(profile);
    if (!cfg) return MSN_E_ARG;
    if (len < MSN_CFG_LEN) return MSN_E_SHORT;
    if (!o) return MSN_E_PROFILE;
    for (k = 1; k <= MSN_ROWS; k++) cfg[msn_row_off(k)] = (msn_u8)(cfg[msn_row_off(k)] & 0x7Fu);
    cfg[o] = (msn_u8)(cfg[o] | (MSN_F_SELECTED | MSN_F_TESTED));
    cfg[MSN_HDR_SEL] = (cfg[o] & MSN_F_WIRED) ? 2u : 1u;
    return MSN_OK;
}

int msn_restore(msn_u8 *cfg, msn_u32 len, msn_u32 profile, const msn_u8 *old)
{
    msn_u32 k, o = msn_row_off(profile);
    if (!cfg || !old) return MSN_E_ARG;
    if (len < MSN_CFG_LEN) return MSN_E_SHORT;
    if (!o) return MSN_E_PROFILE;
    m_cpy(cfg + o, old + o, MSN_ROW_SIZE);
    for (k = 1; k <= MSN_ROWS; k++) {
        msn_u32 ko = msn_row_off(k);
        if (ko == o) continue;
        cfg[ko] = (msn_u8)((cfg[ko] & 0x7Fu) | (old[ko] & 0x80u));
    }
    cfg[MSN_HDR_SEL] = old[MSN_HDR_SEL];
    return MSN_OK;
}

int msn_row_equal(const msn_u8 *a, const msn_u8 *b, msn_u32 len, msn_u32 profile)
{
    msn_u32 o = msn_row_off(profile);
    if (!a || !b || len < MSN_CFG_LEN || !o) return 0;
    return m_cmp(a + o, b + o, MSN_ROW_SIZE) == 0;
}

int msn_rest_equal(const msn_u8 *a, const msn_u8 *b, msn_u32 len, msn_u32 profile)
{
    msn_u32 o = msn_row_off(profile), i;
    if (!a || !b || len < MSN_CFG_LEN || !o) return 0;
    for (i = 0; i < MSN_CFG_LEN; i++) {
        msn_u32 k, flagbyte = 0;
        if (i >= o && i < o + MSN_ROW_SIZE) continue;
        if (a[i] == b[i]) continue;
        if (i == MSN_HDR_SEL) continue;
        for (k = 1; k <= MSN_ROWS; k++) if (i == msn_row_off(k)) flagbyte = 1;
        if (flagbyte && ((a[i] ^ b[i]) & 0x7Fu) == 0) continue;
        return 0;
    }
    return 1;
}

int msn_row_stored(const msn_u8 *cfg, msn_u32 len, msn_u32 profile, const struct msn_row *want)
{
    struct msn_row g;
    msn_u32 n;
    int ok;
    if (!want || msn_parse(cfg, len, profile, &g) != MSN_OK) return 0;
    ok = g.wired == want->wired;
    if (ok && !want->wired) {
        n = slen(want->ssid, 32u);
        ok = g.ssid_len == n && m_cmp(g.ssid, want->ssid, n) == 0 && g.sec == want->sec;
        if (ok && want->sec != MSN_SEC_OPEN) {
            n = slen(want->key, 64u);
            ok = g.key_len == n && m_cmp(g.key, want->key, n) == 0;
        }
    }
    msn_wipe(&g, (msn_u32)sizeof g);
    return ok;
}

int msn_describe(const msn_u8 *cfg, msn_u32 len, msn_u32 profile, struct msn_info *o)
{
    struct msn_row r;
    msn_u32 off = msn_row_off(profile), i;
    int rc;
    if (!o) return MSN_E_ARG;
    m_set(o, 0, (msn_u32)sizeof *o);
    rc = msn_parse(cfg, len, profile, &r);
    if (rc != MSN_OK) return rc;
    o->flags = r.flags; o->enc = (msn_u8)r.sec; o->unk = r.unk; o->hdr_sel = cfg[MSN_HDR_SEL];
    o->ssid_len = r.ssid_len; o->key_len = r.key_len;
    o->wired = r.wired; o->selected = r.selected; o->ip_dhcp = r.ip_dhcp; o->dns_dhcp = r.dns_dhcp; o->use_proxy = r.use_proxy;
    o->tested = (r.flags & MSN_F_TESTED) != 0;
    o->rateset = r.rateset; o->mtu = r.mtu;
    for (i = 0; i < 64u; i++) { const msn_u8 kb = cfg[off + MSN_O_KEY + i]; if (kb) { o->key_nonzero = 1; o->key_nz++; if (i >= r.key_len) o->key_tail++; if (kb < 32u || kb > 126u) o->key_bin++; } }
    msn_wipe(&r, (msn_u32)sizeof r);
    return MSN_OK;
}

static msn_u32 put_str(char *out, msn_u32 cap, msn_u32 at, const char *s)
{
    while (*s && at + 1u < cap) out[at++] = *s++;
    return at;
}
static msn_u32 put_dec(char *out, msn_u32 cap, msn_u32 at, msn_u32 v)
{
    char t[11];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v && n < 10) { t[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n && at + 1u < cap) out[at++] = t[--n];
    return at;
}
static msn_u32 put_hex(char *out, msn_u32 cap, msn_u32 at, msn_u32 v, int digits)
{
    int d;
    for (d = digits - 1; d >= 0 && at + 1u < cap; d--) out[at++] = "0123456789ABCDEF"[(v >> (4 * d)) & 15u];
    return at;
}

int msn_info_line(char *out, msn_u32 cap, msn_u32 profile, const struct msn_info *o)
{
    msn_u32 at = 0;
    if (!out || !o || cap < 8u) return 0;
    at = put_dec(out, cap, at, profile);
    at = put_str(out, cap, at, ": ");
    if (o->flags == 0 && o->ssid_len == 0) { at = put_str(out, cap, at, "empty"); out[at] = 0; return (int)at; }
    at = put_str(out, cap, at, o->selected ? "sel " : "off ");
    at = put_str(out, cap, at, o->wired ? "cable" : "wifi");
    at = put_str(out, cap, at, " fl ");
    at = put_hex(out, cap, at, o->flags, 2);
    if (!o->wired) {
        at = put_str(out, cap, at, " name ");
        at = put_dec(out, cap, at, o->ssid_len);
        at = put_str(out, cap, at, " sec ");
        at = put_dec(out, cap, at, o->enc);
        at = put_str(out, cap, at, " key ");
        at = put_dec(out, cap, at, o->key_len);
        at = put_str(out, cap, at, o->key_nonzero ? "+" : "-");
        at = put_str(out, cap, at, " nz "); at = put_dec(out, cap, at, o->key_nz);          /* counts only, never a byte of the key: non-zero bytes of the 64, of them past the length, of them not printable (tells a stored passphrase from a derived binary key) */
        at = put_str(out, cap, at, "/"); at = put_dec(out, cap, at, o->key_tail);
        at = put_str(out, cap, at, "/"); at = put_dec(out, cap, at, o->key_bin);
        at = put_str(out, cap, at, " u ");
        at = put_dec(out, cap, at, o->unk);
        at = put_str(out, cap, at, " rate ");
        at = put_hex(out, cap, at, o->rateset, 4);
    }
    at = put_str(out, cap, at, o->ip_dhcp ? " ip dhcp" : " ip static");
    at = put_str(out, cap, at, o->dns_dhcp ? " dns dhcp" : " dns static");
    if (o->use_proxy) at = put_str(out, cap, at, " proxy");
    at = put_str(out, cap, at, " mtu ");
    at = put_dec(out, cap, at, o->mtu);
    out[at] = 0;
    return (int)at;
}

/* ---- the Network Profile byte of nincfg.bin ---- */
int msn_nincfg_profile(const msn_u8 *b, msn_u32 n)
{
    if (!b || n < MSN_NINCFG_LEN || b[0] != 0x01 || b[1] != 0x07 || b[2] != 0x0C || b[3] != 0xF6) return -1;
    return (int)(b[MSN_NINCFG_PROFILE] & 3u);
}
int msn_nincfg_set_profile(msn_u8 *b, msn_u32 n, msn_u32 profile)
{
    int cur = msn_nincfg_profile(b, n);
    if (cur < 0 || profile > 3u) return -1;
    if ((msn_u32)cur == profile) return 0;
    b[MSN_NINCFG_PROFILE] = (msn_u8)((b[MSN_NINCFG_PROFILE] & 0xFCu) | profile);
    return 1;
}
int msn_start_line(char *out, msn_u32 cap, msn_u32 profile, int wired)
{
    char t[64];
    msn_u32 i = 0, k;
    const char *a = "Used at start: ", *c = wired > 0 ? " (cable)" : wired == 0 ? " (Wi-Fi)" : "";
    if (!out || cap == 0) return -1;
    t[0] = 0;
    if (profile >= 1u && profile <= 3u) {
        const char *p = "connection ";
        for (k = 0; p[k]; k++) t[i++] = p[k];
        t[i++] = (char)('0' + (int)profile);
        for (k = 0; c[k]; k++) t[i++] = c[k];
        t[i] = 0;
    } else {
        const char *p = "Auto (the Wii's own choice)";
        for (k = 0; p[k]; k++) t[i++] = p[k];
        t[i] = 0;
    }
    for (i = 0, k = 0; a[k] && i + 1 < cap; k++) out[i++] = a[k];
    for (k = 0; t[k] && i + 1 < cap; k++) out[i++] = t[k];
    out[i] = 0;
    return (int)i;
}
