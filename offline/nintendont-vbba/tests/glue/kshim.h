/* SPDX-License-Identifier: GPL-2.0-only (host test shim for the Nintendont kernel API used by kernel/vbba_glue.c) */
/* A minimal stand-in for the Nintendont kernel headers (global.h, common.h, alloc.h, Config.h, debug.h, EXI.h) so the
   glue file of the patch (kernel/vbba_glue.c, compiled unmodified) can run on the host against tests/glue/kmock.c. */
#ifndef KSHIM_H
#define KSHIM_H
#include <stdint.h>
#include <stddef.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef int32_t s32; typedef int bool;
typedef volatile uint32_t vu32;
#define true 1
#define false 0
#define UINT_MAX 0xFFFFFFFFu

struct ipcmessage { unsigned int command, result, fd; union { struct { int offset, origin; } seek; unsigned int pad[5]; }; };
typedef struct iovec { void *data; u32 len; } ioctlv;

#define HW_TIMER 0x0D800010u
#define EXI_BASE 0x13026800u
#define INT_BASE 0x13026500u                  /* b22a (VBBA_ENGSTAT): the interrupt words the PowerPC's handler reads */
#define RSW_INT (INT_BASE + 0x00)
#define DI_INT (INT_BASE + 0x20)
#define SI_INT (INT_BASE + 0x40)
#define EXI_INT (INT_BASE + 0x60)
#define HW_PPCIRQFLAG 0x0D800030u
#define HW_PPCIRQMASK 0x0D800034u
extern volatile u32 eng_fake[];               /* stand-in for PowerPC MEM1 0x80460000..0x804FC000 (the game words of b22b and the engine status blocks) */
#define ENG_FAKE_BASE 0x80460000u
#define VBBA_PPCMEM(a) ((const volatile u32 *)(eng_fake + (((a) - ENG_FAKE_BASE) >> 2)))
#define DI_BASE 0x13026000u                    /* b22b (VBBA_SCENESTAT): DI registers as the PowerPC wrote them */
#define EXI_CMD_0 (EXI_BASE + 0x00)
#define EXI_CMD_1 (EXI_BASE + 0x04)

u32 read32(u32 addr);
void write32(u32 addr, u32 v);
void sync_before_read(void *p, int len);
void sync_after_write(void *p, int len);
static inline void sync_before_read_align32(void *p, uint32_t len) { sync_before_read(p, (int)len); }   /* kernel/global.h: whole 32-byte lines */
static inline void sync_after_write_align32(void *p, uint32_t len) { sync_after_write(p, (int)len); }
void udelay(int us);
int thread_set_priority(int id, int prio);
int dbgprintf(const char *fmt, ...);

u32 thread_create(u32 (*proc)(void *arg), void *arg, u32 *stack, u32 stacksize, u8 priority, bool detach);
int thread_cancel(int id, int when);
int thread_continue(int id);
int mqueue_create(void *ptr, int n);
int mqueue_recv(int q, struct ipcmessage **m, int flags);
void mqueue_ack(struct ipcmessage *m, int retval);
int mqueue_send_now(int q, struct ipcmessage *m, int flags);
int IOS_Ioctlv(int fd, int request, int nin, int nio, void *vec);
int heap_create(void *base, int size);
void heap_destroy(int heap);
void *heap_alloc_aligned(int heap, int size, int align);
void heap_free(int heap, void *p);
int IOS_Open(const char *device, int mode);
void IOS_Close(int fd);
int IOS_IoctlAsync(int fd, int request, void *in, int lin, void *io, int lio, int queue, struct ipcmessage *message);
int IOS_IoctlvAsync(int fd, int request, int nin, int nio, void *vec, int queue, void *message);

extern unsigned char heap_blk[];            /* the test's MEM2 stand-ins (VBBA_HEAP_BASE / VBBA_STATUS_ADDR overrides) */
extern volatile u32 status_blk[];
extern volatile u32 pub_blk[];

/* provided by the test: the kernel's EXI dispatcher stand-in (kernel/EXI.h of the patch: one pass under the single EXI lock; 0 = the lock was held elsewhere) */
int EXIServiceStep(void);
int EXILockTry(void);
void EXIUnlock(void);
/* kernel/EXI.c diagnostics of the patch (0005): counters and the event ring behind the glue's DIAG lines */
extern u32 exiDiagCmds, exiDiagLastTick, exiDiagBlocked, exiDiagSlowTicks, exiDiagSlowW0;
u32 EXIDiagCount(void);
void EXIDiagEvent(u32 back, u32 now, char *out);
void EXIDiagState(u32 *o);
static inline void mdelay(int ms) { udelay(ms * 1000); }     /* Nintendont's mdelay: a sleep of ms milliseconds */
#endif
