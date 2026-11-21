/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only */
/* NETWORK PROFILE AUTO (loader, NIN_MS_NETAUTO=1; pure, host-tested in ota/tests/netauto_test.c).  With Network Profile 'auto' (0) the loader used to leave the
   Wii's own selection alone: a Wii whose Internet settings select a WIRED connection but has no USB LAN adapter plugged in never got online ( field,
   'connection 2 SELECTED wired', nothing worked until profile 1).  Now, for auto only:
     the selected connection is wired, no USB LAN adapter is attached, and a wireless connection passed the Wii's connection test  -> use that wireless connection;
     the selected connection is wireless, a USB LAN adapter is attached, and a wired connection passed the test                  -> use that wired connection;
     anything else (adapter state unknown, nothing selected, no passed alternative) -> leave the Wii's selection as it is.
   f[] = the flags byte of the 3 connections in the ncd config (+0x8, +0x924, +0x1240): bit 7 selected, bit 5 connection test passed, bit 0 wired (the same bits
   the loader's profile code and the kernel's 'ncd config' log line use).  adapter: 1 attached, 0 none, -1 unknown.  Returns the profile to apply (1..3) or 0. */
#ifndef MS_NETAUTO_H
#define MS_NETAUTO_H
static inline unsigned ms_netauto_pick(const unsigned char f[3], int adapter)
{
    int i, sel = -1;
    for (i = 0; i < 3; i++) if (f[i] & 0x80u) { sel = i; break; }
    if (sel < 0 || adapter < 0) return 0;
    if (!adapter && (f[sel] & 1u)) { for (i = 0; i < 3; i++) if (!(f[i] & 1u) && (f[i] & 0x20u)) return (unsigned)i + 1u; }
    if (adapter && !(f[sel] & 1u)) { for (i = 0; i < 3; i++) if ((f[i] & 1u) && (f[i] & 0x20u)) return (unsigned)i + 1u; }
    return 0;
}
/* is the connection IOS will bring up wireless?  f[] as above, prof = the forced Network Profile (1..3) or 0 (auto: the Wii's selected connection).
   1 wireless, 0 wired, -1 unknown (nothing selected / no such entry). */
static inline int ms_netauto_wireless(const unsigned char f[3], unsigned prof)
{
    int i;
    if (prof >= 1u && prof <= 3u) return (f[prof - 1u] & 1u) ? 0 : 1;
    for (i = 0; i < 3; i++) if (f[i] & 0x80u) return (f[i] & 1u) ? 0 : 1;
    return -1;
}
/* no connection can work at all: the selected one is wired, no USB LAN adapter is attached, and no wireless connection passed the Wii's test (nothing to switch to). */
static inline int ms_netauto_dead(const unsigned char f[3], int adapter)
{
    int i;
    if (adapter != 0) return 0;
    for (i = 0; i < 3; i++) if (f[i] & 0x80u) return (f[i] & 1u) && !ms_netauto_pick(f, adapter);
    return 0;
}
/* the interface the net init will use, only when that is sure: 1 wired (the connection in use is wired AND a USB LAN adapter is attached), 2 Wi-Fi (it is wireless AND
   no adapter is attached, or netauto switched to it); 0 = not sure (adapter state unknown, nothing selected, or a wireless selection beside an attached adapter). */
static inline int ms_netauto_iface(const unsigned char f[3], unsigned p, int adapter)
{
    int w = ms_netauto_wireless(f, p);
    if (adapter < 0 || w < 0) return 0;
    if (w == 0) return adapter == 1 ? 1 : 0;
    return (adapter == 0 || p) ? 2 : 0;
}
/* The shell (ota/ms_ota_loader.c) of this tree does not log the pick into ota.log; the kernel build (build_kernel.sh sets NIN_MS_NETAUTO) still compiles main.c with this call, so it is a no-op here. */
void ms_ota_netauto_note(const unsigned char *f, int lan, unsigned p);
void ms_ota_netauto_note(const unsigned char *f, int lan, unsigned p) { (void)f; (void)lan; (void)p; }
#endif
