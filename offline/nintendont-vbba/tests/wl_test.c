/* SPDX-License-Identifier: GPL-2.0-only (host test) */
/* core/vbba_wl.h: the Wi-Fi driver command buffer has the layout WD uses (node at 0, physical data pointer at +0x10, length at +0x16, command at
   data+0, payload length at data+8, name at data+0x18, SET value after the name), bad names are refused, the read-back value is the payload's first word. */
#include <stdio.h>
#include <string.h>
#include "../core/vbba_wl.h"
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)
int main(void)
{
	static vbba_u8 io[VBBA_WL_IO_SIZE] __attribute__((aligned(32)));
	vbba_u32 n;
	n = vbba_wl_build(io, 0x13456720u, VBBA_WLC_GET_VAR, "roam_off", 0);
	CHECK(n == VBBA_WL_IO_SIZE);
	CHECK(vbba_wl_get32(io + 0x00) == 0);                                    /* next */
	CHECK(vbba_wl_get32(io + 0x10) == 0x13456740u);                          /* data, physical */
	CHECK(io[0x16] == 0 && io[0x17] == 0x18 + 0x20);                         /* node data length */
	CHECK(vbba_wl_get32(io + 0x20) == 0x106u && vbba_wl_get32(io + 0x28) == 0x20u);
	CHECK(!strcmp((const char *)io + 0x20 + 0x18, "roam_off"));
	n = vbba_wl_build(io, 0x13456720u, VBBA_WLC_SET_VAR, "roam_off", 1);
	CHECK(n == VBBA_WL_IO_SIZE && vbba_wl_get32(io + 0x20) == 0x107u);
	CHECK(vbba_wl_get32(io + 0x28) == 8 + 1 + 4 && io[0x17] == 0x18 + 13);   /* name + NUL + value */
	CHECK(vbba_wl_get32(io + 0x20 + 0x18 + 9) == 1u);                        /* value right after the NUL */
	CHECK(vbba_wl_build(io, 0, VBBA_WLC_GET_VAR, "", 0) == 0);
	CHECK(vbba_wl_build(io, 0, VBBA_WLC_GET_VAR, "0123456789abcdef", 0) == 0);  /* 16 characters */
	CHECK(vbba_wl_build(io, 0, VBBA_WLC_GET_VAR, "0123456789abcde", 0) == VBBA_WL_IO_SIZE);   /* 15 fit */
	vbba_wl_put32(io + 0x20 + 0x18, 1u);                                     /* the module's answer */
	CHECK(vbba_wl_value(io) == 1u);
	vbba_wl_put32(io + 0x20 + 0x18, 0u);
	CHECK(vbba_wl_value(io) == 0u);
	/* power save (plain WLC command, 4-byte payload) */
	n = vbba_wl_build_cmd(io, 0x13456720u, VBBA_WLC_GET_PM, 0x5A5A5A5Au);
	CHECK(n == VBBA_WL_IO_SIZE && vbba_wl_get32(io + 0x10) == 0x13456740u && io[0x16] == 0 && io[0x17] == 0x18 + 4);
	CHECK(vbba_wl_get32(io + 0x20) == 85u && vbba_wl_get32(io + 0x28) == 4u && vbba_wl_get32(io + 0x20 + 0x18) == 0x5A5A5A5Au);
	vbba_wl_put32(io + 0x20 + 0x18, 2u);                                     /* the module's answer: PM_FAST */
	CHECK(vbba_wl_value(io) == 2u);
	n = vbba_wl_build_cmd(io, 0x13456720u, VBBA_WLC_SET_PM, 0u);
	CHECK(n == VBBA_WL_IO_SIZE && vbba_wl_get32(io + 0x20) == 86u && vbba_wl_get32(io + 0x20 + 0x18) == 0u && vbba_wl_get32(io + 0x20 + 0x1C) == 0u);
	printf("wl_test: %d checks, %d failures\n", checks, fails);
	return fails != 0;
}
