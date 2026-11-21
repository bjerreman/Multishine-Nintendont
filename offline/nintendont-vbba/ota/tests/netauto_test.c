/* SPDX-License-Identifier: GPL-3.0-or-later (host test of ota/ms_netauto.h) */
#include <stdio.h>
#include "../ms_netauto.h"
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
    const unsigned char field[3] = { 0x26, 0xA7, 0x00 };            /* our Wii: 1 wireless passed, 2 SELECTED wired passed, 3 empty */
    const unsigned char wifi_sel[3] = { 0xA6, 0x27, 0x00 };         /* 1 SELECTED wireless, 2 wired passed */
    const unsigned char wired_only[3] = { 0x00, 0xA7, 0x00 };       /* no wireless alternative */
    const unsigned char untested[3] = { 0x06, 0xA7, 0x00 };         /* the wireless one never passed the test */
    const unsigned char none[3] = { 0x26, 0x27, 0x00 };             /* nothing selected */
    CHECK(ms_netauto_pick(field, 0) == 1u);                          /* no adapter: the wireless connection */
    CHECK(ms_netauto_pick(field, 1) == 0u);                          /* adapter plugged in: the wired selection stays (the wired path) */
    CHECK(ms_netauto_pick(field, -1) == 0u);                         /* adapter state unknown: nothing changes */
    CHECK(ms_netauto_pick(wifi_sel, 1) == 2u);                       /* the adapter came back: the wired connection again */
    CHECK(ms_netauto_pick(wifi_sel, 0) == 0u);                       /* Wi-Fi user without an adapter: unchanged */
    CHECK(ms_netauto_pick(wired_only, 0) == 0u);
    CHECK(ms_netauto_pick(untested, 0) == 0u);
    CHECK(ms_netauto_pick(none, 0) == 0u);
    CHECK(ms_netauto_wireless(field, 0) == 0);                       /* our Wii selects the wired connection */
    CHECK(ms_netauto_wireless(field, 1) == 1);                       /* profile 1 forced: the wireless one */
    CHECK(ms_netauto_wireless(wifi_sel, 0) == 1);
    CHECK(ms_netauto_wireless(none, 0) == -1);
    { static const unsigned char g[3] = { 0x26, 0xA7, 0x00 }, d[3] = { 0x06, 0xA7, 0x00 };   /* the shell-109 log; the same with conn 1 untested */
      CHECK(ms_netauto_pick(g, 0) == 1); CHECK(ms_netauto_dead(g, 0) == 0); CHECK(ms_netauto_dead(d, 0) == 1); CHECK(ms_netauto_dead(d, -1) == 0); CHECK(ms_netauto_dead(d, 1) == 0); CHECK(ms_netauto_dead(wifi_sel, 0) == 0); }
    { static const unsigned char wsel[3] = { 0xA6, 0x27, 0x00 }, wdsel[3] = { 0x26, 0xA7, 0x00 };   /* which interface the wait line may name */
      CHECK(ms_netauto_iface(wdsel, 0, 1) == 1);                         /* wired selected, adapter there: wired */
      CHECK(ms_netauto_iface(wdsel, ms_netauto_pick(wdsel, 0), 0) == 2); /* wired selected, no adapter: netauto switches to Wi-Fi */
      CHECK(ms_netauto_iface(wsel, 0, 0) == 2);                          /* Wi-Fi selected, no adapter */
      CHECK(ms_netauto_iface(wsel, ms_netauto_pick(wsel, 1), 1) == 1);   /* Wi-Fi selected, adapter there: netauto switches to the tested wired one */
      CHECK(ms_netauto_iface(wsel, 0, -1) == 0 && ms_netauto_iface(wdsel, 0, -1) == 0);   /* adapter state unknown: plain text */ }
    printf("netauto_test: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
