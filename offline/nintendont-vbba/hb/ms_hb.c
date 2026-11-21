/* Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only (dual licence: also offered as GPL-2.0-only for the build that links into FIX94's Nintendont kernel) */
/* q33-diag: the live heartbeat of the Nintendont kernel (build flag NIN_MS_HB=1 of offline/nintendont-vbba/build_wii.sh).
 *
 * Why: the Wii hangs (kernel q32) left nothing after 'Jumping to': the freeze watch runs inside the main loop and the dump is written by the main loop, so a
 * wedged main loop (or a wedged SD card) writes nothing, and the power button (polled by the main loop) does nothing.  This file keeps a trail that does not depend on the main loop:
 *
 *  - every IOS call (IOS_Open/Close/Read/Write/Seek/Ioctl/Ioctlv) and every FatFs call of the kernel goes through a --wrap'ed stub that notes, per thread, the call in progress
 *    and its start tick (a slot table of 8 threads), and the last call entered by anyone ('last_op');
 *  - the two SD sector functions (disk_read_sd / disk_write_sd, diskio.c) pass a gate that notes the sector op in progress and lets the heartbeat writer own the card for one write;
 *  - a SAMPLER thread (priority 0x7C, above every kernel thread; it never touches the SD card) polls the power button every 100 ms and once a second appends a 128-byte record
 *    (main loop passes and stage, the main thread's call in progress, the SD gate, EXI served counts, the game's retrace counter, the engine breadcrumb, the scene word, the PowerPC
 *    dispatch count, vBBA state, writer state) to a 28-record RAM ring; it decides the snapshots: retrace counter still for 6 / 36 / 66 s, main loop pass counter still for 5 s,
 *    power button pressed;
 *  - a WRITER thread (priority 0x7A) writes the 4 KB live block every 2 s and each snapshot at once, as ONE raw 8-sector write at a sector fixed at boot (no FatFs, no FAT or
 *    directory update, nothing grows), only while no other SD sector op runs (it waits up to 50 ms; an SD READ stuck for more than 4 s does not block it, a stuck write does);
 *  - Shutdown writes one last live block of kind SHUTDOWN from the calling thread.
 * /hb.log is created once per kernel start by the main thread (FatFs, before the game runs) at 20 KB, its sectors are checked to be contiguous and read back raw before the
 * first raw write: a mismatch switches the SD mirror off (the RAM ring stays).  Format: ms_hb_fmt.h; decoder: the project's other tooling. */
#include "global.h"
#include "common.h"
#include "string.h"
#include "debug.h"
#include "ff_utf8.h"
#include "SDI.h"
#include "diskio.h"
#include "vbba_stall.h"
#include "ms_hb_fmt.h"

extern u32 exiServed[4][3];
extern vu32 vbbaSdOps, vbbaSdErrN, vbbaSdWrFail;
extern void vgHbInfo(u32 *o);                      /* appended to vbba_glue.c by build_wii.sh (NIN_MS_HB) */
extern DRESULT disk_read_sd_hbraw(BYTE pdrv, BYTE *buff, DWORD sector, UINT count);
extern DRESULT disk_write_sd_hbraw(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count);

#define HB_PRIO_SAMPLER 0x7C
#define HB_PRIO_WRITER  0x7A
#define HB_PRIO_FALLBACK 0x78                      /* the priority of the stock DI / HID / socket threads: proven on the Wii */
#define HB_STK 0x1000

vu32 hbMain;                                       /* main loop passes (main.c, top of the loop) */
static volatile u32 hbSlot[HB_SLOTS][HB_SLOT_WORDS];
static volatile u32 hbLop, hbLopTid, hbLopT;
static volatile u32 hbFd[HB_FDS][4];
static volatile u32 hbFdN;
static volatile u32 hbSdIn, hbSdOp, hbSdSec, hbSdCnt, hbSdT0, hbSdTid, hbSdN, hbSdOwn;
static volatile u32 hbWr[8];                       /* HB_H_WR.. */
static volatile u32 hbOn, hbStop, hbWriting, hbFlags, hbSnaps, hbSnapMask, hbSnapUsed;
static u32 hbLba, hbCsize, hbBoot, hbTid[8], hbPrio;
static hb_u32 hbRing[HB_RECS * HB_REC_WORDS];
static volatile u32 hbTotal, hbSeq;
static hb_u32 hbHdr[HB_HDR_WORDS];
static u8 hbBuf[HB_BLOCK] __attribute__((aligned(32)));
static u32 hbStkS[HB_STK / 4] __attribute__((aligned(32)));
static u32 hbStkW[HB_STK / 4] __attribute__((aligned(32)));

/* ---- per-thread call tracking (the --wrap stubs) ---- */
static __attribute__((noinline)) volatile u32 *hb_slot(void)
{
	u32 id = thread_get_id() + 1u, i;
	for (i = 0; i < HB_SLOTS; i++) if (hbSlot[i][HB_S_TID] == id) return hbSlot[i];
	for (i = 0; i < HB_SLOTS; i++) if (!hbSlot[i][HB_S_TID]) { hbSlot[i][HB_S_TID] = id; return hbSlot[i]; }
	return hbSlot[HB_SLOTS - 1u];
}
static __attribute__((noinline)) u32 hb_enter(volatile u32 *s, u32 op, u32 fat)
{
	u32 t0 = read32(HW_TIMER);
	if (fat) { s[HB_S_FOP] = op; s[HB_S_FT0] = t0; } else { s[HB_S_IOP] = op; s[HB_S_IT0] = t0; }
	s[HB_S_NOPS]++;
	hbLop = op; hbLopTid = s[HB_S_TID]; hbLopT = t0;
	return t0;
}
static __attribute__((noinline)) void hb_leave(volatile u32 *s, u32 op, u32 t0, u32 fat)
{
	if (fat) s[HB_S_FOP] = 0; else s[HB_S_IOP] = 0;
	s[HB_S_LAST] = op; s[HB_S_LDUR] = read32(HW_TIMER) - t0;
}

s32 __real_IOS_Open(const char *device, int mode);
void __real_IOS_Close(int fd);
s32 __real_IOS_Read(s32 fd, void *buffer, u32 length);
s32 __real_IOS_Write(s32 fd, void *buffer, u32 length);
s32 __real_IOS_Seek(u32 fd, u32 where, u32 whence);
s32 __real_IOS_Ioctl(s32 fd, s32 request, void *buffer_in, s32 length_in, void *buffer_io, s32 length_io);
s32 __real_IOS_Ioctlv(s32 fd, s32 request, s32 InCount, s32 OutCont, void *vec);
FRESULT __real_f_open_char(FIL *fp, const char *path, BYTE mode);
FRESULT __real_f_close(FIL *fp);
FRESULT __real_f_read(FIL *fp, void *buff, UINT btr, UINT *br);
FRESULT __real_f_write(FIL *fp, const void *buff, UINT btw, UINT *bw);
FRESULT __real_f_lseek(FIL *fp, FSIZE_t ofs);
FRESULT __real_f_sync(FIL *fp);

s32 __wrap_IOS_Open(const char *device, int mode)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_OPEN, 0, mode), t0 = hb_enter(s, op, 0);
	s32 r = __real_IOS_Open(device, mode);
	hb_leave(s, op, t0, 0);
	if (r >= 0 && device) {                         /* remember fd -> the last 12 characters of the device name without '/dev/' (the decoder prints them) */
		const char *d = strncmp(device, "/dev/", 5) ? device : device + 5;
		u32 n = strlen(d), k = hbFdN++ % HB_FDS, i;
		const char *t = n > 12u ? d + n - 12u : d;
		u8 nm[12];
		for (i = 0; i < 12u; i++) nm[i] = (u8)(i < n ? t[i] : 0);
		hbFd[k][0] = (u32)r;
		for (i = 0; i < 3u; i++) hbFd[k][1 + i] = ((u32)nm[4 * i] << 24) | ((u32)nm[4 * i + 1] << 16) | ((u32)nm[4 * i + 2] << 8) | nm[4 * i + 3];
	}
	return r;
}
void __wrap_IOS_Close(int fd)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_CLOSE, fd, 0), t0 = hb_enter(s, op, 0);
	__real_IOS_Close(fd);
	hb_leave(s, op, t0, 0);
}
s32 __wrap_IOS_Read(s32 fd, void *buffer, u32 length)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_READ, fd, length >> 9), t0 = hb_enter(s, op, 0);
	s32 r = __real_IOS_Read(fd, buffer, length);
	hb_leave(s, op, t0, 0);
	return r;
}
s32 __wrap_IOS_Write(s32 fd, void *buffer, u32 length)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_WRITE, fd, length >> 9), t0 = hb_enter(s, op, 0);
	s32 r = __real_IOS_Write(fd, buffer, length);
	hb_leave(s, op, t0, 0);
	return r;
}
s32 __wrap_IOS_Seek(u32 fd, u32 where, u32 whence)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_SEEK, fd, where >> 9), t0 = hb_enter(s, op, 0);
	s32 r = __real_IOS_Seek(fd, where, whence);
	hb_leave(s, op, t0, 0);
	return r;
}
s32 __wrap_IOS_Ioctl(s32 fd, s32 request, void *buffer_in, s32 length_in, void *buffer_io, s32 length_io)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_IOCTL, fd, request), t0 = hb_enter(s, op, 0);
	s32 r = __real_IOS_Ioctl(fd, request, buffer_in, length_in, buffer_io, length_io);
	hb_leave(s, op, t0, 0);
	return r;
}
s32 __wrap_IOS_Ioctlv(s32 fd, s32 request, s32 InCount, s32 OutCont, void *vec)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_IOCTLV, fd, request), t0 = hb_enter(s, op, 0);
	s32 r = __real_IOS_Ioctlv(fd, request, InCount, OutCont, vec);
	hb_leave(s, op, t0, 0);
	return r;
}
FRESULT __wrap_f_open_char(FIL *fp, const char *path, BYTE mode)
{
	u32 h = 0; const char *p = path;
	volatile u32 *s; u32 op, t0; FRESULT r;
	while (p && *p) h = h * 31u + (u8)*p++;          /* a 16-bit hash of the path: the decoder knows the kernel's file names */
	s = hb_slot(); op = HB_OPID(HB_OP_FOPEN, mode, h); t0 = hb_enter(s, op, 1);
	r = __real_f_open_char(fp, path, mode);
	hb_leave(s, op, t0, 1);
	return r;
}
FRESULT __wrap_f_close(FIL *fp)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_FCLOSE, 0, 0), t0 = hb_enter(s, op, 1);
	FRESULT r = __real_f_close(fp);
	hb_leave(s, op, t0, 1);
	return r;
}
FRESULT __wrap_f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_FREAD, 0, btr >> 9), t0 = hb_enter(s, op, 1);
	FRESULT r = __real_f_read(fp, buff, btr, br);
	hb_leave(s, op, t0, 1);
	return r;
}
FRESULT __wrap_f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_FWRITE, 0, btw >> 9), t0 = hb_enter(s, op, 1);
	FRESULT r = __real_f_write(fp, buff, btw, bw);
	hb_leave(s, op, t0, 1);
	return r;
}
FRESULT __wrap_f_lseek(FIL *fp, FSIZE_t ofs)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_FSEEK, 0, (u32)(ofs >> 9)), t0 = hb_enter(s, op, 1);
	FRESULT r = __real_f_lseek(fp, ofs);
	hb_leave(s, op, t0, 1);
	return r;
}
FRESULT __wrap_f_sync(FIL *fp)
{
	volatile u32 *s = hb_slot(); u32 op = HB_OPID(HB_OP_FSYNC, 0, 0), t0 = hb_enter(s, op, 1);
	FRESULT r = __real_f_sync(fp);
	hb_leave(s, op, t0, 1);
	return r;
}

/* ---- the SD gate (diskio.c: disk_read_sd / disk_write_sd are these; the originals are renamed *_hbraw) ---- */
static void hb_gate_in(u32 op, u32 sec, u32 cnt)
{
	u32 w = 0;
	for (;;) {                                      /* single core: In is raised BEFORE Own is tested, so the writer either sees In or this thread sees Own */
		hbSdIn++;
		if (!hbSdOwn || w >= 2000u) break;          /* never wait more than ~2 s for the writer */
		hbSdIn--;
		if (++w == 500u) hbWr[7]++;
		udelay(1000);
	}
	hbSdOp = op; hbSdSec = sec; hbSdCnt = cnt; hbSdT0 = read32(HW_TIMER); hbSdTid = thread_get_id() + 1u; hbSdN++;
}
static void hb_gate_out(void)
{
	if (hbSdIn) hbSdIn--;
	if (!hbSdIn) hbSdOp = 0;
}
DRESULT disk_read_sd(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
	DRESULT r;
	hb_gate_in(1, sector, count);
	r = disk_read_sd_hbraw(pdrv, buff, sector, count);
	hb_gate_out();
	return r;
}
DRESULT disk_write_sd(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
	DRESULT r;
	hb_gate_in(2, sector, count);
	r = disk_write_sd_hbraw(pdrv, buff, sector, count);
	hb_gate_out();
	return r;
}

/* the writer owns the card: Own first, then wait for the running sector op to end (wait_ms), or go ahead past a READ stuck for more than 4 s */
static u32 hb_claim(u32 wait_ms)
{
	u32 w;
	hbSdOwn = 1;
	for (w = 0; hbSdIn; w++) {
		if (hbSdOp == 1u && (u32)(read32(HW_TIMER) - hbSdT0) > 4u * HB_TICKS_S) { hbWr[6]++; return 1; }
		if (w >= wait_ms) { hbSdOwn = 0; return 0; }
		udelay(1000);
	}
	return 1;
}

/* ---- records and blocks ---- */
static void hb_hdr_fill(u32 kind)
{
	u32 i, j;
	memset(hbHdr, 0, sizeof hbHdr);
	hbHdr[HB_H_MAGIC] = HB_MAGIC_BLK; hbHdr[HB_H_VERSION] = HB_VERSION; hbHdr[HB_H_KIND] = kind; hbHdr[HB_H_WSEQ] = hbWr[0];
	hbHdr[HB_H_TICK] = read32(HW_TIMER); hbHdr[HB_H_BOOT] = hbBoot; hbHdr[HB_H_RECSZ] = HB_REC_WORDS * 4u; hbHdr[HB_H_RECCAP] = HB_RECS;
	hbHdr[HB_H_LBA] = hbLba; hbHdr[HB_H_CSIZE] = hbCsize;
	for (i = 0; i < 8u; i++) { hbHdr[HB_H_TID + i] = hbTid[i]; hbHdr[HB_H_WR + i] = hbWr[i]; }
	hbHdr[HB_H_SNAPS] = hbSnaps; hbHdr[HB_H_FLAGS] = hbFlags | (hbStop ? 1u : 0u); hbHdr[HB_H_PRIO] = hbPrio;
	{	/* stack high-water marks: the stacks were filled with 0xA5A5A5A5 */
		u32 us = 0, uw = 0;
		while (us < HB_STK / 4u && hbStkS[us] == 0xA5A5A5A5u) us++;
		while (uw < HB_STK / 4u && hbStkW[uw] == 0xA5A5A5A5u) uw++;
		hbHdr[HB_H_STK] = ((HB_STK - us * 4u) << 16) | (HB_STK - uw * 4u);
	}
	for (i = 0; i < HB_SLOTS; i++) for (j = 0; j < HB_SLOT_WORDS; j++) hbHdr[HB_H_SLOTS + i * HB_SLOT_WORDS + j] = hbSlot[i][j];
	for (i = 0; i < HB_FDS; i++) for (j = 0; j < 4u; j++) hbHdr[HB_H_FDS + i * 4u + j] = hbFd[i][j];
	hbHdr[HB_H_SD + 0] = hbSdIn; hbHdr[HB_H_SD + 1] = hbSdOp; hbHdr[HB_H_SD + 2] = hbSdSec; hbHdr[HB_H_SD + 3] = hbSdCnt;
	hbHdr[HB_H_SD + 4] = hbSdT0; hbHdr[HB_H_SD + 5] = hbSdTid; hbHdr[HB_H_SD + 6] = hbSdN;
}

static volatile u32 *hb_slot_of(u32 tid1)
{
	u32 i;
	if (!tid1) return 0;
	for (i = 0; i < HB_SLOTS; i++) if (hbSlot[i][HB_S_TID] == tid1) return hbSlot[i];
	return 0;
}

static void hb_sample(u32 kind)
{
	hb_u32 *r = hbRing + (hbTotal % HB_RECS) * HB_REC_WORDS;
	volatile u32 *m;
	u32 v[6], i;
	memset(r, 0, HB_REC_WORDS * 4u);
	r[HB_R_MAGIC] = HB_MAGIC_REC; r[HB_R_SEQ] = hbSeq++; r[HB_R_TICK] = read32(HW_TIMER); r[HB_R_KIND] = kind;
	r[HB_R_MAIN] = hbMain; r[HB_R_STAGE] = msCrumb.stage; r[HB_R_STAGE_T] = msCrumb.t; r[HB_R_CRUMBS] = msCrumb.n;
	m = hb_slot_of(hbTid[0]);
	if (m) {
		if (m[HB_S_IOP]) { r[HB_R_MOP] = m[HB_S_IOP]; r[HB_R_MOP_T0] = m[HB_S_IT0]; }
		else if (m[HB_S_FOP]) { r[HB_R_MOP] = m[HB_S_FOP]; r[HB_R_MOP_T0] = m[HB_S_FT0]; }
	}
	r[HB_R_LOP] = hbLop; r[HB_R_LOP_TID] = hbLopTid; r[HB_R_LOP_T] = hbLopT;
	r[HB_R_SD] = (hbSdIn << 24) | ((hbSdOp & 0xFFu) << 16) | (hbSdCnt & 0xFFFFu); r[HB_R_SD_T0] = hbSdT0; r[HB_R_SD_SEC] = hbSdSec;
	r[HB_R_SDOPS] = vbbaSdOps; r[HB_R_SDERR] = (vbbaSdErrN << 16) | (vbbaSdWrFail & 0xFFFFu);
	for (i = 0; i < 3u; i++) r[HB_R_EXI0 + i] = exiServed[i][0] + exiServed[i][1] + exiServed[i][2];
	sync_before_read((void *)0x004D7400, 0x40);     /* the game's VI block (retraceCount 0x804D7420), read by the freeze watch too */
	sync_before_read((void *)0x00479D20, 0x20);     /* Melee scene word 0x80479D30 */
	sync_before_read((void *)0x130267A0, 0x40);     /* FZ2 dispatch count 0xD30267A0, FZ4 engine breadcrumb 0xD30267D0 / time base D4 */
	r[HB_R_RETRACE] = read32(0x004D7420); r[HB_R_BC] = read32(0x130267D0); r[HB_R_BC_TBL] = read32(0x130267D4);
	r[HB_R_SCENE] = read32(0x00479D30); r[HB_R_DISP] = read32(0x130267A0);
	vgHbInfo(v);
	r[HB_R_VBBA] = v[0]; r[HB_R_VQ] = v[1]; r[HB_R_VTX] = v[2];
	if (v[0] & 0x80000000u) { hbTid[2] = v[3] + 1u; hbTid[3] = v[4] + 1u; hbTid[4] = v[5] ? v[5] + 1u : 0; }
	m = hb_slot_of(hbTid[1]); r[HB_R_DI_OPS] = m ? m[HB_S_NOPS] : 0;
	m = hb_slot_of(hbTid[2]); r[HB_R_NET_OPS] = m ? m[HB_S_NOPS] : 0;
	r[HB_R_WR] = ((hbWr[0] & 0xFFFFu) << 16) | (hbWr[1] & 0xFFFFu);
	hbTotal++;
}

/* one raw write of block blk (0 live, 1..4 snapshots); the caller is the writer thread or Shutdown */
static u32 hb_write(u32 blk, u32 kind, u32 wait_ms)
{
	u32 t0, ok;
	if (!hbLba) return 0;
	if (!hb_claim(wait_ms)) { hbWr[5]++; return 0; }
	t0 = read32(HW_TIMER);
	hbWr[0]++; hbWr[2] = t0;
	hb_hdr_fill(kind);
	hb_block_build(hbBuf, hbHdr, hbRing, hbTotal);
	ok = sdio_WriteSectors(hbLba + blk * (HB_BLOCK / 512u), HB_BLOCK / 512u, hbBuf) ? 1u : 0u;
	hbSdOwn = 0;
	hbWr[3] = read32(HW_TIMER) - t0;
	if (ok) hbWr[1]++; else hbWr[4]++;
	return ok;
}

static void hb_snap(u32 kind)
{
	hbSnaps++;
	hbSnapMask |= 1u << kind;
}

/* a checked sleep (as the stall watchdog's): udelay returns at once when it cannot create its queue or timer; a thread this high must then not spin */
static void hb_sleep(u32 us, u32 *shortn, u32 flag)
{
	u32 t0 = read32(HW_TIMER);
	udelay((int)us);
	if ((u32)(read32(HW_TIMER) - t0) < us * 949u / 1000u) {       /* less than half the time */
		if (++*shortn >= 8u) { thread_set_priority(0, 0x40); hbFlags |= flag; }
	} else *shortn = 0;
}

static u32 HBSampler(void *arg)
{
	u32 tick = 0, shortn = 0, lastc = 0, lastcT = read32(HW_TIMER), rN = 0, lastm = hbMain, lastmT = lastcT, mStall = 0, pwr = 0, pwrN = 0;
	(void)arg;
	hb_sample(HB_K_BOOT);
	while (!hbStop) {
		u32 now, c, m;
		hb_sleep(100000, &shortn, 2u);
		now = read32(HW_TIMER);
		if (read32(HW_GPIO_IN) & GPIO_POWER) {
			if (!pwr && pwrN < 4u) { pwr = 1; pwrN++; hbFlags |= 8u; hb_sample(HB_K_POWER); hb_snap(HB_K_POWER); }
		} else pwr = 0;
		if (++tick < 10u) continue;
		tick = 0;
		hb_sample(HB_K_PERIODIC);
		sync_before_read((void *)0x004D7400, 0x40);
		c = read32(0x004D7420);
		if (c != lastc) { lastc = c; lastcT = now; rN = 0; }
		else if (c && rN < 3u && (u32)(now - lastcT) > (6u + 30u * rN) * HB_TICKS_S) { hb_sample(HB_K_RETR6 + rN); hb_snap(HB_K_RETR6 + rN); rN++; }
		m = hbMain;
		if (m != lastm) { lastm = m; lastmT = now; mStall = 0; }
		else if (!mStall && (u32)(now - lastmT) > 5u * HB_TICKS_S) { mStall = 1; hb_sample(HB_K_MAIN5); hb_snap(HB_K_MAIN5); }
	}
	return 0;
}

static u32 HBWriter(void *arg)
{
	u32 shortn = 0, last = read32(HW_TIMER) - 2u * HB_TICKS_S;
	(void)arg;
	while (!hbStop) {
		u32 now;
		hb_sleep(100000, &shortn, 4u);
		if (hbStop) break;
		now = read32(HW_TIMER);
		if (hbSnapMask) {
			u32 k = 0, blk;
			while (!(hbSnapMask & (1u << k))) k++;
			hbSnapMask &= ~(1u << k);
			blk = hbSnapUsed < HB_BLOCKS - 1u ? ++hbSnapUsed : HB_BLOCKS - 1u;
			hbWriting = 1; (void)hb_write(blk, k, 50); hbWriting = 0;
			last = now - 2u * HB_TICKS_S;                 /* the live block follows at once */
			continue;
		}
		if ((u32)(now - last) >= 2u * HB_TICKS_S) {
			last = now;
			hbWriting = 1; (void)hb_write(0, HB_K_PERIODIC, 50); hbWriting = 0;
		}
	}
	return 0;
}

/* main thread, after the RUNID line (before the game runs, the only FatFs user then): create /hb.log, find and check its sectors */
void hb_init(u32 usb)
{
	FIL f;
	UINT bw;
	u32 b, ok = 1, lba0 = 0, cs = 0, sec;
	hbBoot = read32(HW_TIMER);
	hbTid[0] = thread_get_id() + 1u;
	if (usb) { dbgprintf("HB: heartbeat SD mirror off (USB device), RAM ring only\r\n"); return; }
	if (f_open_char(&f, "/hb.log", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) { dbgprintf("HB: /hb.log open failed, RAM ring only\r\n"); return; }
	for (b = 0; b < HB_BLOCKS && ok; b++) {
		memset(hbHdr, 0, sizeof hbHdr);
		hbHdr[HB_H_MAGIC] = HB_MAGIC_BLK; hbHdr[HB_H_VERSION] = HB_VERSION; hbHdr[HB_H_KIND] = 0x100u + b; hbHdr[HB_H_BOOT] = hbBoot;   /* kind 0x1nn = an unused block nn */
		hb_block_build(hbBuf, hbHdr, hbRing, 0);
		ok = f_write(&f, hbBuf, HB_BLOCK, &bw) == FR_OK && bw == HB_BLOCK;
	}
	if (ok) ok = f_sync(&f) == FR_OK;
	if (ok) {
		FATFS *fs = f.obj.fs;
		cs = fs->csize;
		for (b = 0; b < HB_BLOCKS && ok; b++) {     /* the sector of each block; they must follow each other (one 8-sector write per block, never across a gap) */
			ok = f_lseek(&f, (FSIZE_t)(b * HB_BLOCK + 1u)) == FR_OK && f.clust >= 2u;
			if (!ok) break;
			sec = fs->database + (f.clust - 2u) * cs + ((b * HB_BLOCK) % (cs * 512u)) / 512u;
			if (b == 0) lba0 = sec; else if (sec != lba0 + b * (HB_BLOCK / 512u)) ok = 0;
		}
	}
	f_close(&f);
	for (b = 0; b < HB_BLOCKS && ok; b++) {          /* read each block's first sector back RAW: magic and block number must be what FatFs wrote there */
		ok = sdio_ReadSectors(lba0 + b * (HB_BLOCK / 512u), 1, hbBuf) ? 1u : 0u;
		if (ok) ok = ((u32)hbBuf[0] << 24 | (u32)hbBuf[1] << 16 | (u32)hbBuf[2] << 8 | hbBuf[3]) == HB_MAGIC_BLK && hbBuf[HB_H_KIND * 4 + 2] == 1u && hbBuf[HB_H_KIND * 4 + 3] == b;
	}
	if (!ok || !lba0) { dbgprintf("HB: /hb.log sectors not usable (contiguous / read back failed, cluster %u sectors), RAM ring only\r\n", cs); return; }
	hbLba = lba0; hbCsize = cs;
	dbgprintf("HB: /hb.log at sector %u (cluster %u sectors), %u blocks of %u bytes; sampler 1 s, live block every 2 s\r\n", lba0, cs, HB_BLOCKS, HB_BLOCK);
}

/* main thread, after 'Kernel Start': the two threads */
void hb_start(u32 diThread)
{
	s32 ts, tw;
	u32 i, ps = HB_PRIO_SAMPLER, pw = HB_PRIO_WRITER;
	hbTid[1] = diThread + 1u;
	for (i = 0; i < HB_STK / 4u; i++) { hbStkS[i] = 0xA5A5A5A5u; hbStkW[i] = 0xA5A5A5A5u; }
	ts = (s32)thread_create(HBSampler, NULL, hbStkS + HB_STK / 4u, HB_STK / 4u, (u8)ps, 1);
	if (ts < 0) { ps = HB_PRIO_FALLBACK; ts = (s32)thread_create(HBSampler, NULL, hbStkS + HB_STK / 4u, HB_STK / 4u, (u8)ps, 1); }
	tw = (s32)thread_create(HBWriter, NULL, hbStkW + HB_STK / 4u, HB_STK / 4u, (u8)pw, 1);
	if (tw < 0) { pw = HB_PRIO_FALLBACK; tw = (s32)thread_create(HBWriter, NULL, hbStkW + HB_STK / 4u, HB_STK / 4u, (u8)pw, 1); }
	hbPrio = (ps << 8) | pw;
	hbTid[6] = ts >= 0 ? (u32)ts + 1u : 0; hbTid[7] = tw >= 0 ? (u32)tw + 1u : 0;
	hbOn = 1;
	dbgprintf("HB: sampler thread %d prio %02X, writer thread %d prio %02X, SD mirror %s\r\n", ts, ps, tw, pw, hbLba ? "on" : "off");
	if (ts >= 0) thread_continue(ts);
	if (tw >= 0) thread_continue(tw);
}

/* the main loop leaves (game exit): no more raw writes before the card is unmounted */
void hb_stop(void)
{
	u32 w;
	hbStop = 1;
	for (w = 0; hbWriting && w < 300u; w++) udelay(1000);
}

/* Shutdown entered (the power press was handled): one last live block from this thread */
void hb_shutdown(void)
{
	if (!hbOn) return;
	hb_stop();
	if (hbWriting) return;
	hb_sample(HB_K_SHUTDOWN);
	(void)hb_write(0, HB_K_SHUTDOWN, 300);
}
