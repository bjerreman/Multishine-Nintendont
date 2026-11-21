/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* Host test of platform/nintendont/vbba_wl.h: the byte layout of the Wi-Fi driver's command buffer.  cc -I core -I platform/nintendont tests/wl_build_test.c */
#include <stdio.h>
#include <string.h>
#include "vbba_wl.h"
static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)
int main(void)
{
    vbba_u8 io[VBBA_WL_IOBUF_SIZE];
    /* SET roam_off = 1 : payload "roam_off\0" (9) + 4 value bytes = 0x0D */
    CHECK(vbba_wl_build(io, 0x13001020u, VBBA_WLC_SET_VAR, 0x0Du, "roam_off", 1) == 0);
    CHECK(vbba_wl_get32(io + 0x00) == 0);                                  /* next */
    CHECK(vbba_wl_get32(io + 0x10) == 0x13001020u);                        /* data pointer (physical) */
    CHECK(io[0x16] == 0 && io[0x17] == 0x18 + 0x0D);                       /* node data length */
    CHECK(vbba_wl_get32(io + 0x20) == 0x107u);                             /* command */
    CHECK(vbba_wl_get32(io + 0x28) == 0x0Du);                              /* payload length */
    CHECK(memcmp(io + 0x20 + 0x18, "roam_off", 9) == 0);                   /* name with its NUL */
    CHECK(io[0x20 + 0x18 + 9] == 1);                                       /* value byte right after the NUL */
    /* GET roam_off */
    CHECK(vbba_wl_build(io, 0x13001020u, VBBA_WLC_GET_VAR, VBBA_WL_GET_INNER, "roam_off", -1) == 0);
    CHECK(vbba_wl_get32(io + 0x20) == 0x106u && vbba_wl_get32(io + 0x28) == 0x20u);
    CHECK(io[0x17] == 0x18 + 0x20 && io[0x20 + 0x18 + 9] == 0);            /* no value written */
    memset(io + 0x20 + 0x18, 0, 8); io[0x20 + 0x18 + 3] = 1;               /* the driver's answer: word 1 at +0x18 */
    CHECK(vbba_wl_value(io) == 1u);
    /* a command with no name, a name that does not fit */
    CHECK(vbba_wl_build(io, 0x1000u, 0, 4u, NULL, -1) == 0 && vbba_wl_get32(io + 0x20) == 0);
    CHECK(vbba_wl_build(io, 0x1000u, VBBA_WLC_SET_VAR, 4u, "this_iovar_name_is_much_too_long_for_the_buffer", 1) == -1);
    { unsigned i; for (i = 0; i < sizeof io; i++) CHECK(io[i] == 0); }      /* left zeroed on refusal */
    printf(bad ? "vbba_wl: %d failures\n" : "vbba_wl: ok\n", bad);
    return bad != 0;
}
