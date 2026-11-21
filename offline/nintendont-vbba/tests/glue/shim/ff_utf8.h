/* SPDX-License-Identifier: GPL-2.0-only (host test shim) */
/* The three FatFs calls kernel/vbba_glue.c uses to read /vbba.cfg (fatfs/ff_utf8.h in Nintendont), backed by tests/glue/kmock.c. */
#include "kshim.h"
typedef struct { char pad[600]; } FIL;
typedef int FRESULT;
#define FR_OK 0
#define FA_READ 1
#define FA_OPEN_EXISTING 0
#define FA_WRITE 2
#define FA_CREATE_ALWAYS 8
FRESULT f_write(FIL *fp, const void *buff, u32 btw, u32 *bw);
FRESULT f_open_char(FIL *fp, const char *path, unsigned char mode);
FRESULT f_read(FIL *fp, void *buff, u32 btr, u32 *br);
FRESULT f_close(FIL *fp);
