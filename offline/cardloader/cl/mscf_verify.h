/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont loader) */
/* MSCF signature check shared by the card loader (cl_loader.c, CL_VERIFY builds) and the update installer (ota/ota_core.c): ONE definition of the
 * signed message so the two can never drift.  Freestanding, needs mw2_crypto.h (SHA-512, Ed25519 verify).
 *
 *   message = "MSCFSIGv1\0" || header[0x00..0x40) || SHA-512(tables) || SHA-512(image[0..image_len))        = 10 + 64 + 64 + 64 = 202 bytes
 *   signature = header[0x40..0x80)  (Ed25519 R||S);  tables = segment list then patch list at header+0x80.
 *
 * COMPRESSED files (MSCF v2, F_COMP): the signature covers the STORED stream, so it is verified BEFORE the decoder runs:
 *   message = "MSCFSIGz1\0" || header[0x00..0x40) || SHA-512(tables) || SHA-512(stream[0..z_len))        202 bytes (same length, different tag: domain separation)
 * The header holds image_len, image_crc, z_len, z_crc, so the decoded image is a deterministic, length- and CRC-checked function of signed bytes.
 * Returns 1 = signed by a pinned key, 2 = unsigned file accepted (allow_unsigned, dev loaders only), 0 = refuse. */
#ifndef MSCF_VERIFY_H
#define MSCF_VERIFY_H
#include "mw2_crypto.h"

#define MSCF_SIGMSG_LEN 202u

/* h = the 0x80-byte header, tabs/ntab = the tables (inside the same block, right after h), img/len = the image (comp: the stored MSLZ stream and its z_len) */
static unsigned mscf_verify_sig(const mw2_u8* h, const mw2_u8* tabs, mw2_u32 ntab, const mw2_u8* img, mw2_u32 len,
                                const mw2_u8 (*pubs)[32], unsigned npub, int allow_unsigned, int comp)
{
    static const mw2_u8 tag[10] = { 'M', 'S', 'C', 'F', 'S', 'I', 'G', 'v', '1', 0 };
    mw2_u8 m[MSCF_SIGMSG_LEN];
    unsigned i;
    for (i = 0; i < 10; i++)
        m[i] = tag[i];
    if (comp)
        m[7] = 'z';                    /* "MSCFSIGz1" */
    for (i = 0; i < 0x40; i++)
        m[10 + i] = h[i];
    mw2_sha512(tabs, ntab, m + 10 + 0x40);
    mw2_sha512(img, len, m + 10 + 0x40 + 64);
    for (i = 0; i < npub; i++)
        if (mw2_ed25519_verify(pubs[i], m, MSCF_SIGMSG_LEN, h + 0x40))
            return 1;
    if (allow_unsigned) {
        mw2_u8 z = 0;
        for (i = 0; i < 64; i++)
            z = (mw2_u8) (z | h[0x40 + i]);
        if (z == 0)
            return 2;
    }
    return 0;
}
#endif
