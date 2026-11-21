/* SPDX-License-Identifier: GPL-2.0-only (host test) */
/* ota/ms_netcfg_core.c on a synthetic 0x1B5C block: parse / validate / serialize / select / restore / read-back compare, the describe line never carries a name or key character. */
#include <stdio.h>
#include <string.h>
#include "../ms_netcfg_core.h"
#include "../ms_probe_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)

static unsigned char blk[MSN_CFG_LEN], old[MSN_CFG_LEN];

/* a block like a console with row 1 = Wi-Fi WPA2 "HomeNet" / "hunter2hunter2", row 2 = cable, row 3 empty; filler patterns in the untouched parts so any stray write shows */
static void make(unsigned char *b)
{
    unsigned i;
    unsigned char *r;
    for (i = 0; i < MSN_CFG_LEN; i++) b[i] = (unsigned char)(((i * 7u + 3u) & 0x1F) | 0x40);
    b[MSN_HDR_SEL] = 1;
    r = b + MSN_ROW0;
    memset(r, 0, MSN_ROW_SIZE);
    r[0] = 0xA6; r[0x1A] = 0x05; r[0x1B] = 0xDC;                                                /* selected, tested, dhcp ip + dns, mtu 1500 */
    r[MSN_O_RATESET] = 0; r[MSN_O_RATESET + 1] = 0x0F;
    memcpy(r + MSN_O_SSID, "HomeNet", 7); r[MSN_O_SSIDLEN] = 7; r[MSN_O_ENC] = 5; r[MSN_O_KEYLEN] = 14; r[MSN_O_UNK] = 0;
    memcpy(r + MSN_O_KEY, "hunter2hunter2", 14);
    r = b + MSN_ROW0 + MSN_ROW_SIZE;
    memset(r, 0, MSN_ROW_SIZE);
    r[0] = 0x27;                                                                                /* not selected, wired, dhcp */
    r[0x1A] = 0x05; r[0x1B] = 0xDC;
    r = b + MSN_ROW0 + 2 * MSN_ROW_SIZE;
    memset(r, 0, MSN_ROW_SIZE);
}

/* Nintendont's loader/source/main.c edit, verbatim logic, as the reference for msn_select */
static void nin_select(unsigned char *s, int profile)
{
    if (profile == 1) { s[0x8] |= 0xA0; s[0x4] = (s[0x8] & 1) ? 2 : 1; s[0x924] &= 0x7F; s[0x1240] &= 0x7F; }
    else if (profile == 2) { s[0x8] &= 0x7F; s[0x924] |= 0xA0; s[0x4] = (s[0x924] & 1) ? 2 : 1; s[0x1240] &= 0x7F; }
    else { s[0x8] &= 0x7F; s[0x924] &= 0x7F; s[0x1240] |= 0xA0; s[0x4] = (s[0x1240] & 1) ? 2 : 1; }
}

static void row_init(struct msn_row *r, int wired, const char *ssid, int sec, const char *key)
{
    memset(r, 0, sizeof *r);
    r->wired = wired;
    strncpy(r->ssid, ssid, 32); r->sec = sec;
    strncpy(r->key, key, 64);
}

int main(void)
{
    struct msn_row r, w;
    struct msn_info in;
    char line[200];
    unsigned i;
    int rc;

    CHECK(sizeof(msn_u32) == 4 && sizeof(msn_u16) == 2);
    CHECK(MSN_ROW0 + 3 * MSN_ROW_SIZE == MSN_CFG_LEN);
    CHECK(msn_row_off(1) == 0x8 && msn_row_off(2) == 0x924 && msn_row_off(3) == 0x1240 && msn_row_off(0) == 0 && msn_row_off(4) == 0);

    /* parse */
    make(blk);
    CHECK(msn_parse(blk, MSN_CFG_LEN, 1, &r) == MSN_OK);
    CHECK(r.present && r.selected && !r.wired && r.ip_dhcp && r.dns_dhcp && !r.use_proxy);
    CHECK(r.ssid_len == 7 && strcmp(r.ssid, "HomeNet") == 0 && r.sec == MSN_SEC_WPA2_AES && r.key_len == 14 && strcmp(r.key, "hunter2hunter2") == 0);
    CHECK(r.rateset == 0x000F && r.mtu == 1500);
    CHECK(msn_parse(blk, MSN_CFG_LEN, 2, &r) == MSN_OK && r.present && r.wired && !r.selected && r.ssid_len == 0);
    CHECK(msn_parse(blk, MSN_CFG_LEN, 3, &r) == MSN_OK && !r.present);
    CHECK(msn_parse(blk, MSN_CFG_LEN, 0, &r) == MSN_E_PROFILE && msn_parse(blk, MSN_CFG_LEN, 4, &r) == MSN_E_PROFILE);
    CHECK(msn_parse(blk, MSN_CFG_LEN - 1, 1, &r) == MSN_E_SHORT && msn_parse(NULL, MSN_CFG_LEN, 1, &r) == MSN_E_ARG);
    /* a damaged name length stays visible and is clipped, never read past the field */
    make(blk); blk[MSN_ROW0 + MSN_O_SSIDLEN] = 200;
    CHECK(msn_parse(blk, MSN_CFG_LEN, 1, &r) == MSN_OK && r.ssid_len == 200 && strlen(r.ssid) <= 32);
    make(blk); blk[MSN_ROW0 + MSN_O_KEYLEN] = 255;
    CHECK(msn_parse(blk, MSN_CFG_LEN, 1, &r) == MSN_OK && r.key_len == 255 && strlen(r.key) <= 64);

    /* validation matrix */
    row_init(&w, 0, "Net", MSN_SEC_WPA2_AES, "12345678"); CHECK(msn_validate(&w) == MSN_OK);
    row_init(&w, 0, "", MSN_SEC_WPA2_AES, "12345678"); CHECK(msn_validate(&w) == MSN_E_SSID_EMPTY);
    row_init(&w, 0, "01234567890123456789012345678901", MSN_SEC_OPEN, ""); CHECK(msn_validate(&w) == MSN_OK);          /* 32 */
    row_init(&w, 0, "01234567890123456789012345678901", MSN_SEC_OPEN, ""); w.ssid[32] = 'x';
    CHECK(msn_validate(&w) == MSN_E_SSID_LONG);                                                                         /* 33 bytes, field full */
    row_init(&w, 0, "Caf\xc3\xa9", MSN_SEC_OPEN, ""); CHECK(msn_validate(&w) == MSN_E_SSID_CHARS);                      /* UTF-8 refused */
    row_init(&w, 0, "Net", MSN_SEC_WPA2_AES, "1234567"); CHECK(msn_validate(&w) == MSN_E_KEY_SHORT);
    row_init(&w, 0, "Net", MSN_SEC_WPA2_AES, "12345678"); CHECK(msn_validate(&w) == MSN_OK);
    { char k63[64], k64[65]; memset(k63, 'a', 63); k63[63] = 0; memset(k64, 'a', 64); k64[64] = 0;
      row_init(&w, 0, "Net", MSN_SEC_WPA_AES, k63); CHECK(msn_validate(&w) == MSN_OK);
      row_init(&w, 0, "Net", MSN_SEC_WPA_AES, k64); CHECK(msn_validate(&w) == MSN_E_KEY_LONG); }
    row_init(&w, 0, "Net", MSN_SEC_WPA_TKIP, "pass\tword1"); CHECK(msn_validate(&w) == MSN_E_KEY_CHARS);
    row_init(&w, 0, "Net", MSN_SEC_WEP64, "12345"); CHECK(msn_validate(&w) == MSN_OK);
    row_init(&w, 0, "Net", MSN_SEC_WEP64, "1234"); CHECK(msn_validate(&w) == MSN_E_WEP_LEN);
    row_init(&w, 0, "Net", MSN_SEC_WEP64, "1234567890123"); CHECK(msn_validate(&w) == MSN_E_WEP_LEN);
    row_init(&w, 0, "Net", MSN_SEC_WEP128, "1234567890123"); CHECK(msn_validate(&w) == MSN_OK);
    row_init(&w, 0, "Net", MSN_SEC_WEP128, "12345"); CHECK(msn_validate(&w) == MSN_E_WEP_LEN);
    row_init(&w, 0, "Net", 3, "12345678"); CHECK(msn_validate(&w) == MSN_E_SEC);
    row_init(&w, 0, "Net", MSN_SEC_OPEN, "ignored"); CHECK(msn_validate(&w) == MSN_OK);
    row_init(&w, 1, "", 77, ""); CHECK(msn_validate(&w) == MSN_OK);                                                       /* a cable row has nothing to check */
    CHECK(msn_validate(NULL) == MSN_E_ARG);
    for (i = 0; i <= MSN_E_STORED; i++) CHECK(msn_v_text((int)i)[0] != 0);

    /* security cycle */
    CHECK(msn_sec_next(MSN_SEC_OPEN, 1) == MSN_SEC_WEP64 && msn_sec_next(MSN_SEC_WPA_AES, 1) == MSN_SEC_OPEN && msn_sec_next(MSN_SEC_OPEN, -1) == MSN_SEC_WPA_AES);
    CHECK(msn_sec_next(99, 1) == MSN_SEC_WPA_AES && msn_sec_next(99, -1) == MSN_SEC_WPA_TKIP);

    /* serialize: only row 1 changes, round trip parses back */
    make(blk); memcpy(old, blk, sizeof old);
    row_init(&w, 0, "Cafe Wifi", MSN_SEC_WPA2_AES, "correct horse");
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &w) == MSN_OK);
    CHECK(!msn_row_equal(old, blk, MSN_CFG_LEN, 1));
    CHECK(msn_rest_equal(old, blk, MSN_CFG_LEN, 1));
    CHECK(memcmp(old + MSN_ROW0 + MSN_ROW_SIZE, blk + MSN_ROW0 + MSN_ROW_SIZE, MSN_CFG_LEN - MSN_ROW0 - MSN_ROW_SIZE) == 0 && memcmp(old, blk, MSN_ROW0) == 0);
    CHECK(msn_parse(blk, MSN_CFG_LEN, 1, &r) == MSN_OK && strcmp(r.ssid, "Cafe Wifi") == 0 && r.ssid_len == 9 && strcmp(r.key, "correct horse") == 0 && r.key_len == 13 && r.sec == 5);
    CHECK(r.flags == 0xA6 && r.mtu == 1500 && r.rateset == 0x000F);                                                       /* flags, mtu, rateset untouched */
    CHECK(blk[MSN_ROW0 + MSN_O_KEY + 13] == 0 && blk[MSN_ROW0 + MSN_O_SSID + 9] == 0 && blk[MSN_ROW0 + MSN_O_SSID + 31] == 0);   /* zero padded, no old tail */
    CHECK(msn_row_stored(blk, MSN_CFG_LEN, 1, &w));
    row_init(&w, 0, "Cafe Wifi", MSN_SEC_WPA2_AES, "correct horsf"); CHECK(!msn_row_stored(blk, MSN_CFG_LEN, 1, &w));
    row_init(&w, 0, "Cafe Wiff", MSN_SEC_WPA2_AES, "correct horse"); CHECK(!msn_row_stored(blk, MSN_CFG_LEN, 1, &w));
    row_init(&w, 1, "", 0, ""); CHECK(!msn_row_stored(blk, MSN_CFG_LEN, 1, &w));
    /* a shorter name over a longer one leaves no tail */
    row_init(&w, 0, "A", MSN_SEC_OPEN, "");
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &w) == MSN_OK && blk[MSN_ROW0 + MSN_O_SSIDLEN] == 1 && blk[MSN_ROW0 + MSN_O_SSID + 1] == 0 && blk[MSN_ROW0 + MSN_O_KEYLEN] == 0);
    for (i = 0; i < 64; i++) CHECK(blk[MSN_ROW0 + MSN_O_KEY + i] == 0);
    CHECK(blk[MSN_ROW0 + MSN_O_UNK] == 0);
    /* TKIP sets the 0x7EE byte, same type again keeps whatever it is */
    row_init(&w, 0, "A", MSN_SEC_WPA_TKIP, "12345678"); CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &w) == MSN_OK && blk[MSN_ROW0 + MSN_O_UNK] == 1);
    blk[MSN_ROW0 + MSN_O_UNK] = 0;
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &w) == MSN_OK && blk[MSN_ROW0 + MSN_O_UNK] == 0);
    /* WEP: key stored four times, key_length 0 */
    row_init(&w, 0, "W", MSN_SEC_WEP64, "abcde");
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &w) == MSN_OK && blk[MSN_ROW0 + MSN_O_KEYLEN] == 0 && memcmp(blk + MSN_ROW0 + MSN_O_KEY, "abcdeabcdeabcdeabcde", 20) == 0 && blk[MSN_ROW0 + MSN_O_KEY + 20] == 0);
    CHECK(msn_parse(blk, MSN_CFG_LEN, 1, &r) == MSN_OK && r.key_len == 5 && strcmp(r.key, "abcde") == 0);
    CHECK(msn_row_stored(blk, MSN_CFG_LEN, 1, &w));
    row_init(&w, 0, "W", MSN_SEC_WEP128, "abcdefghijklm");
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &w) == MSN_OK && memcmp(blk + MSN_ROW0 + MSN_O_KEY, "abcdefghijklmabcdefghijklmabcdefghijklmabcdefghijklm", 52) == 0 && blk[MSN_ROW0 + MSN_O_KEY + 52] == 0);
    CHECK(msn_parse(blk, MSN_CFG_LEN, 1, &r) == MSN_OK && r.key_len == 13 && strcmp(r.key, "abcdefghijklm") == 0);
    /* invalid input never touches the block */
    make(blk); memcpy(old, blk, sizeof old);
    row_init(&w, 0, "Net", MSN_SEC_WPA2_AES, "short");
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &w) == MSN_E_KEY_SHORT && memcmp(old, blk, sizeof old) == 0);
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 0, &w) == MSN_E_PROFILE && msn_serialize(blk, 100, 1, &w) == MSN_E_SHORT && memcmp(old, blk, sizeof old) == 0);

    /* cable: zeroes the wireless part of that row only, sets bit 0, keeps address/proxy/mtu */
    make(blk); memcpy(old, blk, sizeof old);
    row_init(&w, 1, "", 0, "");
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &w) == MSN_OK);
    CHECK(msn_parse(blk, MSN_CFG_LEN, 1, &r) == MSN_OK && r.wired && r.ssid_len == 0 && r.key_len == 0 && r.sec == 0 && r.mtu == 1500 && r.flags == 0xA7);
    CHECK(msn_rest_equal(old, blk, MSN_CFG_LEN, 1));
    CHECK(memcmp(old + MSN_ROW0 + MSN_O_PROXY, blk + MSN_ROW0 + MSN_O_PROXY, MSN_O_RATESET - MSN_O_PROXY) == 0);
    CHECK(memcmp(old + MSN_ROW0 + MSN_O_WIFI_END, blk + MSN_ROW0 + MSN_O_WIFI_END, MSN_ROW_SIZE - MSN_O_WIFI_END) == 0);
    /* wired back to wireless */
    row_init(&w, 0, "Back", MSN_SEC_WPA2_AES, "abcdefgh");
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &w) == MSN_OK && msn_parse(blk, MSN_CFG_LEN, 1, &r) == MSN_OK && !r.wired && strcmp(r.ssid, "Back") == 0);

    /* an empty row gets DHCP, nothing else besides what was written */
    make(blk); row_init(&w, 0, "New", MSN_SEC_WPA2_AES, "abcdefgh");
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 3, &w) == MSN_OK && msn_parse(blk, MSN_CFG_LEN, 3, &r) == MSN_OK && r.flags == 0x06 && r.ip_dhcp && r.dns_dhcp && strcmp(r.ssid, "New") == 0);

    /* select == Nintendont's edit, for every profile and every start state */
    for (i = 1; i <= 3; i++) {
        unsigned a;
        for (a = 0; a < 8; a++) {
            unsigned char x[MSN_CFG_LEN], y[MSN_CFG_LEN];
            make(x);
            x[0x8] = (unsigned char)(((a & 1) ? 0x80 : 0) | ((a >> 2) & 1) | 0x04); x[0x924] = (unsigned char)((a & 2) ? 0xA7 : 0x26); x[0x1240] = (unsigned char)((a & 4) ? 0x81 : 0x00);
            memcpy(y, x, sizeof x);
            nin_select(x, (int)i);
            CHECK(msn_select(y, MSN_CFG_LEN, i) == MSN_OK);
            CHECK(memcmp(x, y, sizeof x) == 0);
        }
    }
    CHECK(msn_select(blk, MSN_CFG_LEN, 0) == MSN_E_PROFILE && msn_select(blk, 10, 1) == MSN_E_SHORT);

    /* restore: edit + select, then put back onto a fresh read that came back identical to the old one except the edit */
    make(old); make(blk);
    row_init(&w, 0, "Other", MSN_SEC_WPA2_AES, "abcdefgh");
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 3, &w) == MSN_OK && msn_select(blk, MSN_CFG_LEN, 3) == MSN_OK);
    CHECK(blk[MSN_HDR_SEL] == 1 && (blk[0x8] & 0x80) == 0 && (blk[0x1240] & 0x80));
    CHECK(msn_rest_equal(old, blk, MSN_CFG_LEN, 3));                                                                  /* the selection is the only change outside the row */
    CHECK(msn_restore(blk, MSN_CFG_LEN, 3, old) == MSN_OK && memcmp(blk, old, sizeof old) == 0);
    /* restore also when row 2 was cable and selected */
    make(old); old[0x8] &= 0x7F; old[0x924] |= 0xA0; old[MSN_HDR_SEL] = 2; memcpy(blk, old, sizeof blk);
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &w) == MSN_OK && msn_select(blk, MSN_CFG_LEN, 1) == MSN_OK && blk[MSN_HDR_SEL] == 1);
    CHECK(msn_restore(blk, MSN_CFG_LEN, 1, old) == MSN_OK && memcmp(blk, old, sizeof old) == 0);
    /* the put-back rule of the Network screen: after a FAILED save that was kept, a later put-back must return to the settings of the screen's OPENING, not to the kept one */
    {
        msn_u8 entry[MSN_CFG_LEN], cur[MSN_CFG_LEN], fresh[MSN_CFG_LEN];
        struct msn_row bad1, bad2;
        make(entry); memcpy(cur, entry, sizeof cur);                                                                  /* the config as the screen opens: snapshot = entry */
        row_init(&bad1, 0, "WrongNet", MSN_SEC_WPA2_AES, "wrongpass1");
        CHECK(msn_serialize(cur, MSN_CFG_LEN, 1, &bad1) == MSN_OK && msn_select(cur, MSN_CFG_LEN, 1) == MSN_OK);       /* save 1 fails its test and is KEPT */
        CHECK(!msn_row_equal(entry, cur, MSN_CFG_LEN, 1));
        row_init(&bad2, 0, "WrongNet", MSN_SEC_WPA2_AES, "wrongpass2");
        memcpy(fresh, cur, sizeof fresh);
        CHECK(msn_serialize(cur, MSN_CFG_LEN, 1, &bad2) == MSN_OK && msn_select(cur, MSN_CFG_LEN, 1) == MSN_OK);      /* save 2 fails too; the put-back reads fresh = the kept state */
        memcpy(fresh, cur, sizeof fresh);
        CHECK(msn_restore(fresh, MSN_CFG_LEN, 1, entry) == MSN_OK && memcmp(fresh, entry, sizeof entry) == 0);        /* from the ENTRY snapshot: the original password is back */
        memcpy(fresh, cur, sizeof fresh);
        {   /* the old behaviour for contrast: a snapshot taken at save 2 restores save 1's wrong row */
            msn_u8 per_save[MSN_CFG_LEN];
            memcpy(per_save, entry, sizeof per_save);
            CHECK(msn_serialize(per_save, MSN_CFG_LEN, 1, &bad1) == MSN_OK);
            CHECK(msn_restore(fresh, MSN_CFG_LEN, 1, per_save) == MSN_OK && !msn_row_equal(entry, fresh, MSN_CFG_LEN, 1));
        }
    }
    /* a stray change outside the row is caught */
    make(old); make(blk); blk[0x1000] ^= 0xFF;
    CHECK(!msn_rest_equal(old, blk, MSN_CFG_LEN, 1));
    make(blk); blk[0x924] ^= 0x01;
    CHECK(!msn_rest_equal(old, blk, MSN_CFG_LEN, 1));                                                                 /* another row's non-selection bit */
    make(blk); blk[0x924] ^= 0x80;
    CHECK(msn_rest_equal(old, blk, MSN_CFG_LEN, 1));

    /* describe: lengths and flags, never characters */
    make(blk);
    CHECK(msn_describe(blk, MSN_CFG_LEN, 1, &in) == MSN_OK && in.ssid_len == 7 && in.key_len == 14 && in.key_nonzero && in.selected && in.tested && !in.wired && in.enc == 5 && in.hdr_sel == 1);
    CHECK(msn_info_line(line, sizeof line, 1, &in) > 0);
    CHECK(strstr(line, "HomeNet") == NULL && strstr(line, "hunter") == NULL && strstr(line, "Home") == NULL && strstr(line, "hunt") == NULL);
    CHECK(strstr(line, "name 7") && strstr(line, "key 14+") && strstr(line, "fl A6") && strstr(line, "sec 5") && strstr(line, "rate 000F") && strstr(line, "dhcp") && strstr(line, "mtu 1500"));
    CHECK(msn_describe(blk, MSN_CFG_LEN, 2, &in) == MSN_OK && in.wired && msn_info_line(line, sizeof line, 2, &in) > 0 && strstr(line, "cable") && !strstr(line, "name"));
    CHECK(msn_describe(blk, MSN_CFG_LEN, 3, &in) == MSN_OK && msn_info_line(line, sizeof line, 3, &in) > 0 && strstr(line, "3: empty"));
    CHECK(msn_describe(blk, MSN_CFG_LEN, 5, &in) == MSN_E_PROFILE);
    { char tiny[8]; msn_describe(blk, MSN_CFG_LEN, 1, &in); rc = msn_info_line(tiny, sizeof tiny, 1, &in); CHECK(rc >= 0 && rc < 8 && tiny[rc] == 0); }     /* a small buffer truncates, never overflows (ASan) */

    /* Type row logic (a Wi-Fi row was saved as "kind cable"): bit 0 of the flag byte = wired, for parse, serialize and the boot check (msp_cfg_kind) alike */
    for (i = 1; i <= 3; i++) {
        unsigned f;
        for (f = 0; f < 256; f++) {
            int link = 0;
            make(blk); blk[msn_row_off(i)] = (unsigned char)f;
            CHECK(msn_parse(blk, MSN_CFG_LEN, i, &r) == MSN_OK && r.wired == (int)(f & 1));
            CHECK(msp_cfg_kind(blk, MSN_CFG_LEN, i, &link) == ((f & 1) ? MSP_NET_WIRED : MSP_NET_WIRELESS));
        }
    }
    make(blk); blk[MSN_ROW0] = 0xA6;                                                   /* the exact Wi-Fi row of the synthetic console: selected, tested, DHCP, interface bit 0 clear */
    CHECK(msn_parse(blk, MSN_CFG_LEN, 1, &r) == MSN_OK && !r.wired);
    memcpy(old, blk, sizeof old);
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &r) == MSN_OK && memcmp(old, blk, sizeof old) == 0);     /* writing back what was read changes nothing (a Wi-Fi row stays Wi-Fi) */
    r.wired = 1; CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &r) == MSN_OK && blk[MSN_ROW0] == 0xA7);    /* only the Type flip sets bit 0 */
    memcpy(blk, old, sizeof blk); msn_parse(blk, MSN_CFG_LEN, 1, &r); r.wired = 0; r.sec = MSN_SEC_WPA_AES;
    CHECK(msn_serialize(blk, MSN_CFG_LEN, 1, &r) == MSN_OK && blk[MSN_ROW0] == 0xA6);

    /* wipe */
    strcpy(r.key, "secretsecret"); msn_wipe(&r, sizeof r); CHECK(r.key[0] == 0 && r.key[5] == 0);

    /* the Network Profile byte of nincfg.bin (byte 543, low 2 bits); never another byte, never a file that is not a settings file */
    {
        static unsigned char cfg[600], ref[600];
        unsigned v, p2, j;
        char ln[64];
        for (j = 0; j < sizeof cfg; j++) cfg[j] = (unsigned char)(j * 7 + 3);
        cfg[0] = 0x01; cfg[1] = 0x07; cfg[2] = 0x0C; cfg[3] = 0xF6;
        for (v = 0; v < 256; v++) for (p2 = 1; p2 <= 3; p2++) {
            int rc2;
            cfg[543] = (unsigned char)v; memcpy(ref, cfg, sizeof cfg);
            rc2 = msn_nincfg_set_profile(cfg, 548, p2);
            CHECK(rc2 == ((v & 3u) == p2 ? 0 : 1));
            CHECK(cfg[543] == (unsigned char)((v & 0xFCu) | p2));                  /* the other 6 bits of the byte stay */
            CHECK(memcmp(cfg, ref, 543) == 0 && memcmp(cfg + 544, ref + 544, sizeof cfg - 544) == 0);   /* every other byte stays */
            CHECK(msn_nincfg_profile(cfg, 548) == (int)p2);
            memcpy(cfg, ref, sizeof cfg);
        }
        cfg[543] = 0x40; memcpy(ref, cfg, sizeof cfg);
        CHECK(msn_nincfg_set_profile(cfg, 548, 4) == -1 && msn_nincfg_set_profile(cfg, 548, 0xFFFFFFFFu) == -1);      /* out of range: never written */
        CHECK(msn_nincfg_set_profile(cfg, 547, 2) == -1 && msn_nincfg_profile(cfg, 547) == -1);             /* too short */
        cfg[2] = 0x0D; CHECK(msn_nincfg_set_profile(cfg, 548, 2) == -1 && msn_nincfg_profile(cfg, 548) == -1); cfg[2] = 0x0C;   /* not the magic */
        CHECK(msn_nincfg_set_profile(NULL, 548, 2) == -1);
        CHECK(memcmp(cfg, ref, sizeof cfg) == 0);                                                          /* a refusal changes nothing */
        CHECK(msn_nincfg_set_profile(cfg, 600, 2) == 1 && cfg[543] == 0x42 && memcmp(cfg + 544, ref + 544, 56) == 0);   /* a longer file: only byte 543 */
        CHECK(msn_nincfg_set_profile(cfg, 548, 0) == 1 && cfg[543] == 0x40 && memcmp(cfg, ref, sizeof cfg) == 0);      /* a put-back to Auto: the byte is as it was */
        CHECK(msn_start_line(ln, sizeof ln, 2, 1) > 0 && strcmp(ln, "Used at start: connection 2 (cable)") == 0);
        CHECK(msn_start_line(ln, sizeof ln, 1, 0) > 0 && strcmp(ln, "Used at start: connection 1 (Wi-Fi)") == 0);
        CHECK(msn_start_line(ln, sizeof ln, 3, -1) > 0 && strcmp(ln, "Used at start: connection 3") == 0);
        CHECK(msn_start_line(ln, sizeof ln, 0, 1) > 0 && strcmp(ln, "Used at start: Auto (the Wii's own choice)") == 0);
        { char tiny[10]; int k2 = msn_start_line(tiny, sizeof tiny, 2, 1); CHECK(k2 == 9 && tiny[9] == 0); }   /* truncates, never overflows (ASan) */
    }

    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
