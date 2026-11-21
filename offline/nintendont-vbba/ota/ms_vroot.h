/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The virtual memory card files (saves/GALE.raw, GALE_B.raw, its temp and marker) live on the device the game runs from: the kernel opens "<root device>:/multishine/saves/<ID>.raw"
 * (patches/0002-multishine-paths.patch), root device = USB when NIN_CFG_USB is set.  A player who runs from USB (Zell,) had the loader write them to the SD card, where the kernel
 * never looks.  Everything else the loader writes (kernel, shell, update state, log, download caches, markers) stays on the SD card.  Host tests define MS_HOST_TEST and the int ms_vusb. */
#ifndef MS_VROOT_H
#define MS_VROOT_H
#include <stdio.h>
#ifdef MS_HOST_TEST
extern int ms_vusb;
#define MS_VUSB() (ms_vusb)
#else
#define MS_VUSB() (!UseSD)
#endif
/* "sd:/multishine/<rest>" or "usb:/multishine/<rest>"; four rotating buffers (a path is used at once, and a few can be alive in one statement) */
static inline const char *ms_vpath(const char *rest)
{
	static char b[4][96];
	static unsigned i;
	char *o = b[i++ & 3u];
	snprintf(o, 96, "%s:/multishine/%s", MS_VUSB() ? "usb" : "sd", rest);
	return o;
}
#endif
