/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* The time rules of a download (header only, no libc): the TLS fetch (ms_tlsget.c), the plain HTTP fetch and the update steps of the loader (ms_ota_loader.c) and the host test share them.
 * A shell of about 1.9 MB did not arrive within a fixed 25.8 s on a slow Wi-Fi link while its bytes were still coming in, and the fallbacks after it had no time left.  So:
 *  - the budget of a download follows the size the server announced: MSDL_BASE_MS plus the time at MSDL_FLOOR_BPMS bytes per millisecond (about 40 KB/s), never below the caller's floor,
 *    never above MSDL_CAP_MS;
 *  - a download whose body is still arriving is not cut at its deadline: it goes on while the last bytes came less than MSDL_STALL_MS ago, up to twice the deadline;
 *  - before the body flows (connect, handshake, the head) the deadline is hard. */
#ifndef MS_DLRULE_H
#define MS_DLRULE_H

#define MSDL_BASE_MS     8000u
#define MSDL_FLOOR_BPMS  40u
#define MSDL_CAP_MS      120000u
#define MSDL_STALL_MS    4000u

/* size = the announced size in bytes (0 = unknown: floor_ms) */
static inline unsigned int msdl_budget(unsigned int size, unsigned int floor_ms)
{
    unsigned int b;
    if (!size) return floor_ms;
    b = MSDL_BASE_MS + size / MSDL_FLOOR_BPMS;
    if (b < floor_ms) b = floor_ms;
    if (b > MSDL_CAP_MS) b = MSDL_CAP_MS;
    return b;
}

/* elapsed = ms since the call started, idle = ms since the last body bytes arrived, flowing = body bytes have arrived.  1 = give up now */
static inline int msdl_expired(unsigned int elapsed, unsigned int idle, unsigned int deadline, int flowing)
{
    if (elapsed < deadline) return 0;
    if (!flowing) return 1;
    if (elapsed - deadline >= deadline) return 1;
    return idle >= MSDL_STALL_MS;
}

#endif
