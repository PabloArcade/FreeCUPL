/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - registry of the WinCUPL 5.0a bugs (docs/BUGS.txt).  DARKSOFT.
   Corrected by default since 2.0.0; -compat-refdefect reproduces them.
   FREECUPL_FIX_DEFAULT=0 builds the 1.0 behaviour (bugs by default). */
#ifndef FREECUPL_BUGS_H
#define FREECUPL_BUGS_H
#include <stddef.h>

#ifndef FREECUPL_FIX_DEFAULT
#define FREECUPL_FIX_DEFAULT 1
#endif

/* bug numbers: B001.. in docs/BUGS.txt (the order of this list) */
enum {
    BUG_NONE = 0,
    BUG_KB_POLARITY = 1,         /* B001 */
    BUG_KB_FEEDBACK,             /* B002 */
    BUG_NOT_NUMBER,              /* B003 */
    BUG_POL_CONST,               /* B004 */
    BUG_C1_SECOND,               /* B005 */
    BUG_DEFINE_LF,               /* B006 */
    BUG_DEFINE_FORGET,           /* B007 */
    BUG_HEADER_LATE,             /* B008 */
    BUG_MIN_CRASH,               /* B009 */
    BUG_GAL_COMBREG,             /* B010 */
    BUG_G24_PIN19,               /* B011 */
    BUG_COMBFULL,                /* B012 */
    BUG_PEEL_OE,                 /* B013 */
    BUG_TWO_NAMES,               /* B014 */
    BUG_NODENEG,                 /* B015 */
    BUG_NODECOMB,                /* B016 */
    BUG_NOJOIN,                  /* B017 */
    BUG_ALIASREAD,               /* B018 */
    BUG_ALIASCOMB,               /* B019 */
    BUG_MEGA_BANK,               /* B020 */
    BUG_MEGA_COMBD,              /* B021 */
    BUG_IOADDR,                  /* B022 */
    BUG_INREG,                   /* B023 */
    BUG_TFB_D,                   /* B024 */
    BUG_SIM_SHARED_ARSP,         /* B025 */
    BUG_SIM_PEEL_OE,             /* B026 */
    BUG_SIM_REGZERO,             /* B027 */
    BUG_SIM_VP8_OEPIN,           /* B028 */
    BUG_ATV2500_ALIAS8,          /* B029 */
    BUG_SIM_NOCE,                /* B030 */
    BUG_PROP_CRASH,              /* B031 */
    BUG_HDR_ALIAS,               /* B032 */
    BUG_BARE_EQN,                /* B033 */
    BUG_V5K_XQUAD,               /* B034 */
    BUG_V5K_CJOIN,               /* B035 */
    BUG_V5K_CLOCK,               /* B036 */
    BUG_ROOTNOT_COMBD,           /* B037 */
    BUG_V5K_IOL25,               /* B038 */
    BUG_SIM_V5K_INLEVEL,         /* B039 */
    BUG_SIM_V5K_READ,            /* B040 */
    BUG_COUNT
};

/* 1: correct behaviour (-fix), 0: reproduce the bugs (default of version 1) */
extern int g_fix;

/* "WinCUPL bug #B008: WHAT; use -fix for the corrected result", or with
   -fix "deviates from WinCUPL (bug #B008 fixed): WHAT" */
void bug_text(char *buf, size_t n, int bug, const char *what);
const char *bug_title(int bug);

#endif
