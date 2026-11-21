/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont kernel) */
/* The live heartbeat of the q33-diag Nintendont kernel (NIN_MS_HB=1): the on-card format of SD:/hb.log and its
 * encoder.  Pure C89, no kernel or libc dependency: compiled into the kernel (kernel/ms_hb.c) and into the host test (hb/tests/hb_fmt_test.c), decoded by the project's other tooling.
 *
 * /hb.log = HB_BLOCKS blocks of HB_BLOCK (4096) bytes, every word big-endian:
 *   block 0       the LIVE block, rewritten in place every 2 s (and at a snapshot, the power button, Shutdown())
 *   blocks 1..4   SNAPSHOTS, each written once when its trigger fires (retrace stalled 6 / 36 / 66 s, main loop stalled 5 s, power button seen); when all four are used the
 *                 last one is overwritten by later snapshots (the first three are kept)
 * A block = a 512-byte header (HB_HDR_WORDS words) + HB_RECS records of 128 bytes (HB_REC_WORDS words), oldest first.  Word HB_H_SUM of the header is the 32-bit sum of every
 * other word of the block (a torn write shows as a bad sum).  Records are sampled once a second by the sampler thread. */
#ifndef MS_HB_FMT_H
#define MS_HB_FMT_H

typedef unsigned int hb_u32;               /* 32 bits on the Starlet and on LP64 hosts */
#define HB_BLOCK      4096u
#define HB_BLOCKS     5u
#define HB_HDR_WORDS  128u
#define HB_REC_WORDS  32u
#define HB_RECS       28u                 /* (4096 - 512) / 128 */
#define HB_MAGIC_BLK  0x4D534842u         /* 'MSHB' */
#define HB_MAGIC_REC  0x48425231u         /* 'HBR1' */
#define HB_VERSION    1u
#define HB_TICKS_S    1898000u            /* HW_TIMER ticks per second (the kernel's own constant) */

/* kinds: a record's word HB_R_KIND and a block's word HB_H_KIND */
#define HB_K_PERIODIC 0u
#define HB_K_RETR6    1u                  /* the game's retrace counter has not moved for 6 s */
#define HB_K_RETR36   2u
#define HB_K_RETR66   3u
#define HB_K_MAIN5    4u                  /* the kernel main loop pass counter has not moved for 5 s */
#define HB_K_BOOT     5u                  /* the first record (threads started) */
#define HB_K_SHUTDOWN 6u                  /* Shutdown entered (the power press was handled) */
#define HB_K_POWER    7u                  /* the power button GPIO seen pressed by the sampler (whether or not the main loop reacts) */

/* header words */
#define HB_H_MAGIC    0
#define HB_H_VERSION  1
#define HB_H_KIND     2                   /* 0 live, else the snapshot's kind */
#define HB_H_WSEQ     3                   /* SD writes so far (this one included) */
#define HB_H_TICK     4                   /* HW_TIMER when the block was built */
#define HB_H_BOOT     5                   /* HW_TIMER at hb_init */
#define HB_H_NREC     6                   /* valid records in this block */
#define HB_H_TOTAL    7                   /* records sampled since boot */
#define HB_H_RECSZ    8
#define HB_H_RECCAP   9
#define HB_H_LBA      10                  /* first sector of /hb.log */
#define HB_H_CSIZE    11                  /* FAT cluster size in sectors */
#define HB_H_TID      12                  /* 12..19: thread ids: main, DI, vBBA net, vBBA done, vBBA tx, HID, hb sampler, hb writer */
#define HB_H_WR       20                  /* 20..27: writer: started, finished, last start tick, last duration, failures, skipped (SD busy), forced (an SD read stuck > 4 s), gate waits > 500 ms */
#define HB_H_SNAPS    28                  /* snapshots taken */
#define HB_H_FLAGS    29                  /* bit0 stopped, bit1 sampler priority lowered (sleep failed), bit2 writer priority lowered, bit3 power seen */
#define HB_H_PRIO     30                  /* sampler prio << 8 | writer prio */
#define HB_H_STK      31                  /* stack bytes used: sampler << 16 | writer */
#define HB_H_SLOTS    32                  /* 32..95: 8 thread slots of HB_SLOT_WORDS words */
#define HB_SLOT_WORDS 8u
#define HB_SLOTS      8u
#define HB_H_FDS      96                  /* 96..119: 6 opened IOS handles: fd, then 12 bytes of the device name's tail */
#define HB_FDS        6u
#define HB_H_SD       120                 /* 120..126: SD gate: in, op (1 read 2 write), sector, count, start tick, tid, sector ops so far */
#define HB_H_SUM      127

/* thread slot words (relative) */
#define HB_S_TID      0                   /* thread id + 1 (0 = free slot) */
#define HB_S_NOPS     1                   /* IOS + FatFs calls made by the thread */
#define HB_S_FOP      2                   /* FatFs call in progress (op id, 0 = none) */
#define HB_S_FT0      3                   /* its start tick */
#define HB_S_IOP      4                   /* IOS call in progress (op id, 0 = none) */
#define HB_S_IT0      5
#define HB_S_LAST     6                   /* the last call that returned (op id) */
#define HB_S_LDUR     7                   /* its duration in ticks */

/* op ids: class << 24 | (fd & 0xFF) << 16 | (request or argument & 0xFFFF) */
#define HB_OP_OPEN    0x01u
#define HB_OP_CLOSE   0x02u
#define HB_OP_READ    0x03u
#define HB_OP_WRITE   0x04u
#define HB_OP_SEEK    0x05u
#define HB_OP_IOCTL   0x06u
#define HB_OP_IOCTLV  0x07u
#define HB_OP_FOPEN   0x10u
#define HB_OP_FCLOSE  0x11u
#define HB_OP_FREAD   0x12u
#define HB_OP_FWRITE  0x13u
#define HB_OP_FSEEK   0x14u
#define HB_OP_FSYNC   0x15u
#define HB_OPID(cls, fd, arg) (((hb_u32)(cls) << 24) | (((hb_u32)(fd) & 0xFFu) << 16) | ((hb_u32)(arg) & 0xFFFFu))

/* record words */
#define HB_R_MAGIC    0
#define HB_R_SEQ      1
#define HB_R_TICK     2
#define HB_R_KIND     3
#define HB_R_MAIN     4                   /* kernel main loop passes */
#define HB_R_STAGE    5                   /* msCrumb.stage (MSS_*: where the main loop is) */
#define HB_R_STAGE_T  6                   /* HW_TIMER at its entry */
#define HB_R_CRUMBS   7                   /* msCrumb.n */
#define HB_R_MOP      8                   /* main thread: the call in progress (IOS if any, else FatFs), 0 = none */
#define HB_R_MOP_T0   9
#define HB_R_LOP      10                  /* the last call entered by any thread */
#define HB_R_LOP_TID  11
#define HB_R_LOP_T    12
#define HB_R_SD       13                  /* SD gate: in << 24 | op << 16 | count */
#define HB_R_SD_T0    14
#define HB_R_SD_SEC   15
#define HB_R_SDOPS    16                  /* SD commands (vbbaSdOps) */
#define HB_R_SDERR    17                  /* failed SD commands << 16 | sector writes failed after retries */
#define HB_R_EXI0     18                  /* EXI commands served: channel 0 (memory card A) */
#define HB_R_EXI1     19                  /* channel 1 (slot B) */
#define HB_R_EXI2     20                  /* channel 2 (the BBA) */
#define HB_R_RETRACE  21                  /* the game's VI retrace counter (0x804D7420) */
#define HB_R_BC       22                  /* engine breadcrumb (FZ4 word 0xD30267D0) */
#define HB_R_BC_TBL   23                  /* its time base (0xD30267D4) */
#define HB_R_SCENE    24                  /* Melee scene word (0x80479D30) */
#define HB_R_DISP     25                  /* PowerPC interrupt dispatches (FZ2 0xD30267A0) */
#define HB_R_VBBA     26                  /* vBBA: started << 31 | net released << 30 | (main_beat & 0x3FFFFFFF) */
#define HB_R_VQ       27                  /* vBBA request slots in use << 16 | send queue now */
#define HB_R_VTX      28                  /* sendto in IOS now << 16 | IOS timeouts */
#define HB_R_DI_OPS   29                  /* DI thread calls (activity) */
#define HB_R_NET_OPS  30                  /* vBBA net thread calls */
#define HB_R_WR       31                  /* writer: started << 16 | finished (low 16 bits each) */

static __inline__ void hb_put32(unsigned char *p, hb_u32 v)
{
	p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

/* Build one 4096-byte block: hdr = HB_HDR_WORDS words (HB_H_NREC, HB_H_TOTAL and HB_H_SUM are filled here), ring = HB_RECS records of HB_REC_WORDS words written
 * circularly (record k of all time sits at ring[(k % HB_RECS) * HB_REC_WORDS]), total = records written so far.  The newest min(total, HB_RECS) go out, oldest first. */
static __inline__ void hb_block_build(unsigned char *out, hb_u32 *hdr, const volatile hb_u32 *ring, hb_u32 total)
{
	hb_u32 n = total < HB_RECS ? total : HB_RECS, first = total - n, sum = 0, i, j, x;
	unsigned char *p;
	for (i = 0; i < HB_BLOCK; i++) out[i] = 0;
	hdr[HB_H_NREC] = n; hdr[HB_H_TOTAL] = total; hdr[HB_H_SUM] = 0;
	for (i = 0; i < n; i++) {
		const volatile hb_u32 *r = ring + ((first + i) % HB_RECS) * HB_REC_WORDS;
		p = out + HB_HDR_WORDS * 4u + i * HB_REC_WORDS * 4u;
		for (j = 0; j < HB_REC_WORDS; j++) { x = r[j]; hb_put32(p + j * 4u, x); sum += x; }   /* one read per word: the sampler may refresh the ring meanwhile */
	}
	for (i = 0; i < HB_HDR_WORDS; i++) if (i != HB_H_SUM) sum += hdr[i];
	hdr[HB_H_SUM] = sum;
	for (i = 0; i < HB_HDR_WORDS; i++) hb_put32(out + i * 4u, hdr[i]);
}

#endif
