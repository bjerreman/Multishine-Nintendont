/* SPDX-License-Identifier: GPL-2.0-only (host test) */
/* Host test of the rotation logic of the "auto" app (loader/source/vbba_sweep.c from patches/0001-multishine-vbba.patch, compiled unmodified): a mock SD
   card with /vbba_sweep.txt and /vbba.cfg.  Missing and corrupt state files, the order and the wrap, a write that fails, /vbba.cfg overriding, a double
   commit, the title line and the word handed to the kernel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vbba_sweep.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct sd { int has_state; char state[64]; int state_len; int has_cfg; int write_fails; int unreadable; int writes; int reads; };
static int m_cfg(void *c) { return ((struct sd *)c)->has_cfg; }
static int m_read(void *c, char *buf, unsigned cap)
{
    struct sd *s = c;
    unsigned n;
    s->reads++;
    if (!s->has_state || s->unreadable) return -1;
    n = (unsigned)s->state_len < cap ? (unsigned)s->state_len : cap;
    memcpy(buf, s->state, n);
    return (int)n;
}
static int m_write(void *c, const char *t)
{
    struct sd *s = c;
    if (s->write_fails) return -1;
    s->writes++; s->has_state = 1; s->state_len = (int)strlen(t); memcpy(s->state, t, (size_t)s->state_len + 1);
    return 0;
}
static struct vbba_sweep_io io_for(struct sd *s) { struct vbba_sweep_io io = { s, m_cfg, m_read, m_write }; return io; }
static void set_state(struct sd *s, const char *t) { s->has_state = 1; s->state_len = (int)strlen(t); memcpy(s->state, t, (size_t)s->state_len + 1); }

/* one launch: plan, then commit; returns the preset handed to the kernel */
static int launch(struct sd *s, struct vbba_sweep *w)
{
    struct vbba_sweep_io io = io_for(s);
    vbba_sweep_plan(&io, w);
    vbba_sweep_commit(&io, w);
    return w->baked;
}

int main(void)
{
    struct sd sd;
    struct vbba_sweep w;
    struct vbba_sweep_io io;
    char title[96];
    int i;
    static const int order[6] = { 0, 4, 5, 3, 2, 1 };
    printf("== loader sweep rotation (vbba_sweep.c unmodified)\n");

    /* the order, a missing state file, and the wrap: 8 launches d0 d4 d5 d3 d2 d1 d0 d4 */
    memset(&sd, 0, sizeof sd);
    for (i = 0; i < 8; i++) {
        int got = launch(&sd, &w);
        CHECK(got == order[i % 6] && w.index == (unsigned)(i % 6) && w.duty == (unsigned)order[i % 6]);
        CHECK(!(w.flags & (VBBA_SWEEP_WRITEFAIL | VBBA_SWEEP_CFG)));
        CHECK(((w.flags & VBBA_SWEEP_NOSTATE) != 0) == (i == 0));                    /* only the very first launch has no state file */
        CHECK(sd.has_state && atoi(sd.state) == (i + 1) % 6);                         /* the file holds the NEXT index */
    }
    CHECK(sd.writes == 8);
    printf("   order d0 d4 d5 d3 d2 d1 d0 d4 over 8 launches, state file after each: next index, wraps 5 -> 0\n");

    /* plan alone writes nothing (our may leave the app without starting a game); commit twice advances once */
    memset(&sd, 0, sizeof sd); set_state(&sd, "2\n");
    io = io_for(&sd);
    vbba_sweep_plan(&io, &w);
    CHECK(sd.writes == 0 && w.baked == 5 && w.index == 2 && !w.flags);
    vbba_sweep_commit(&io, &w); vbba_sweep_commit(&io, &w);
    CHECK(sd.writes == 1 && atoi(sd.state) == 3 && w.baked == 5);

    /* damaged state files: all start at d0 and are flagged, and the file is repaired by the write */
    {
        static const char *bad[] = { "", "   ", "abc", "7", "6", "-1", "+2", "3x", "3 4", "99999999999", "4.5", "\xff\xfe\x01", "0x2", "1000" };
        for (i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
            memset(&sd, 0, sizeof sd); set_state(&sd, bad[i]);
            CHECK(launch(&sd, &w) == 0 && w.index == 0 && (w.flags & VBBA_SWEEP_NOSTATE) && !(w.flags & VBBA_SWEEP_WRITEFAIL));
            CHECK(atoi(sd.state) == 1);
        }
        CHECK(vbba_sweep_parse("  4\r\n", 6) == 4 && vbba_sweep_parse("0", 1) == 0 && vbba_sweep_parse("5", 1) == 5 && vbba_sweep_parse("05", 2) == 5 && vbba_sweep_parse("3\n7", 3) == -1);
        CHECK(vbba_sweep_parse("4\n", 2) == 4 && vbba_sweep_parse("", 0) == -1 && vbba_sweep_parse("12", 2) == -1);
        memset(&sd, 0, sizeof sd); set_state(&sd, "4"); sd.unreadable = 1;                      /* present but cannot be read */
        CHECK(launch(&sd, &w) == 0 && (w.flags & VBBA_SWEEP_NOSTATE));
    }
    printf("   missing, empty, garbage, out of range, unreadable state files: d0, flagged, repaired by the write\n");

    /* the write fails: the baseline d0 is used (logged by the loader), the state is left alone; once it works again the sweep resumes where it was */
    memset(&sd, 0, sizeof sd); set_state(&sd, "3\n");
    sd.write_fails = 1;
    CHECK(launch(&sd, &w) == 3 && (w.flags & VBBA_SWEEP_WRITEFAIL) && w.index == 3 && w.duty == 3 && atoi(sd.state) == 3 && sd.writes == 0);
    CHECK(launch(&sd, &w) == 3 && (w.flags & VBBA_SWEEP_WRITEFAIL) && atoi(sd.state) == 3);       /* still failing: d0 again, never a stuck setting */
    sd.write_fails = 0;
    CHECK(launch(&sd, &w) == 3 && !(w.flags & VBBA_SWEEP_WRITEFAIL) && atoi(sd.state) == 4);
    memset(&sd, 0, sizeof sd); sd.write_fails = 1;                                       /* no state file and no write: d0, both flags */
    CHECK(launch(&sd, &w) == 0 && (w.flags & (VBBA_SWEEP_NOSTATE | VBBA_SWEEP_WRITEFAIL)) == (VBBA_SWEEP_NOSTATE | VBBA_SWEEP_WRITEFAIL));
    { struct vbba_sweep_io nowrite = { &sd, m_cfg, m_read, NULL }; vbba_sweep_plan(&nowrite, &w); vbba_sweep_commit(&nowrite, &w); CHECK(w.baked == 0 && (w.flags & VBBA_SWEEP_WRITEFAIL)); }                 /* (state missing: d0 is the planned preset anyway) */
    printf("   write failure: baseline d0, state untouched, flag set; the sweep resumes when the card writes again\n");

    /* /vbba.cfg on the card decides: no preset from the sweep, no write, the position is reported */
    memset(&sd, 0, sizeof sd); set_state(&sd, "2\n"); sd.has_cfg = 1;
    CHECK(launch(&sd, &w) == -1 && (w.flags & VBBA_SWEEP_CFG) && w.index == 2 && sd.writes == 0 && atoi(sd.state) == 2);
    CHECK(launch(&sd, &w) == -1 && sd.writes == 0 && atoi(sd.state) == 2);                  /* any number of launches: the sweep stays where it was */
    sd.has_cfg = 0;
    CHECK(launch(&sd, &w) == 5 && atoi(sd.state) == 3);                                    /* file removed: the sweep goes on from its position */
    printf("   /vbba.cfg present: no preset, no write, position kept; removed: the sweep resumes\n");

    /* title line and kernel word */
    memset(&sd, 0, sizeof sd); set_state(&sd, "1\n");
    io = io_for(&sd); vbba_sweep_plan(&io, &w);
    vbba_sweep_title(&w, title, sizeof title);
    CHECK(!strcmp(title, " duty 4 (auto 2/6)") && vbba_sweep_info(&w) == ((0u << 16) | (2u << 8) | 6u));
    memset(&sd, 0, sizeof sd);
    io = io_for(&sd); vbba_sweep_plan(&io, &w); vbba_sweep_title(&w, title, sizeof title);
    CHECK(!strcmp(title, " duty 0 (auto 1/6) new") && vbba_sweep_info(&w) == ((1u << 16) | (1u << 8) | 6u));
    sd.write_fails = 1; vbba_sweep_commit(&io, &w); vbba_sweep_title(&w, title, sizeof title);
    CHECK(strstr(title, "WRITE FAILED") && (vbba_sweep_info(&w) >> 16) == (VBBA_SWEEP_NOSTATE | VBBA_SWEEP_WRITEFAIL));
    memset(&sd, 0, sizeof sd); set_state(&sd, "5\n"); sd.has_cfg = 1;
    io = io_for(&sd); vbba_sweep_plan(&io, &w); vbba_sweep_title(&w, title, sizeof title);
    CHECK(strstr(title, "/vbba.cfg") && strstr(title, "6/6") && (vbba_sweep_info(&w) >> 16) == VBBA_SWEEP_CFG);
    CHECK(strlen(title) < 56);                                                                  /* fits the loader's title buffer */
    printf("   title \"%s\", kernel word %08x\n", title, vbba_sweep_info(&w));

    printf("sweep host test: %s (%d checks)\n", fails ? "FAIL" : "PASS", checks);
    return fails != 0;
}
