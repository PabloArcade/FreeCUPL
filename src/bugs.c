/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - registry of reproduced WinCUPL bugs (see bugs.h).  DARKSOFT. */
#include <stdio.h>
#include "bugs.h"

int g_fix = FREECUPL_FIX_DEFAULT;

static const char *const titles[BUG_COUNT] = {
    "",
    "-kb complements the sum without changing the output polarity",
    "-kb: a registered output that is not complemented is read back as if it were",
    "'!' directly before a number is not applied to it but to the next operand",
    "a complemented constant keeps its value when an equation is complemented for a fixed-polarity output",
    "PAL16C1/20C1: the terms of both equations of the shared sum are written",
    "$DEFINE with nothing after the name takes the next line as its value",
    "a $DEFINE is forgotten after an $IFDEF/$ELSE block (trigger not found)",
    "a second Name statement loses the target device",
    "the -m3/-m4 minimiser stops with program error 1032cm",
    "GAL registered mode: a combinational and a .D equation on one pin are merged",
    "GAL24V10 registered mode: pin 19 gets a constant-true or misplaced product term",
    "GAL24V10 registered mode: a combinational output needing all its rows corrupts the configuration",
    "PEEL18CV8: an output without .OE is never enabled",
    "a product term reading one signal through two names keeps only one literal",
    "PINNODE declared active low: the equation is not complemented",
    "ATV2500B/ATF2500C: combinational equation on a buried register node",
    "ATF2500C: the third sum term of an output is never joined",
    "ATF750C: a register read through its node alias with the other polarity",
    "ATV750: the equation of an output-register alias is ORed into the combinational pin",
    "MegaPAL: registered outputs of a bank with a combinational output become combinational",
    "MegaPAL: a combinational and a .D equation on one pin are ORed",
    "PAL26V12: the .IO feedback select is written at the wrong address",
    "PAL18V8: an output pin used only as an input stays registered",
    "ATF750C: a D register read through .TFB becomes a T flip-flop",
    "simulator: the shared AR/SP terms act only on registers with an .AR/.SP equation",
    "simulator: PEEL18CV8 output enables of registered outputs ignored",
    "simulator: PAL10016RD4 registers load 0",
    "simulator: GAL16VP8 pin 11 taken as the OE pin",
    "ATV2500B: an output-register alias with more than 4 terms next to a buried register is rejected",
    "simulator: the clock enable (.CE) is ignored",
    "a PROPERTY statement without a name and a { } body, or an empty Device statement, crashes WinCUPL",
    "a header item given under both of its names (REV/REVISION, ASSY/ASSEMBLY, LOC/LOCATION) shifts the JED header fields",
    "a statement without '=' ('name;') is taken as an equation that sets the output true",
};

const char *bug_title(int bug)
{
    return bug > 0 && bug < BUG_COUNT ? titles[bug] : "";
}

void bug_text(char *buf, size_t n, int bug, const char *what)
{
    if (g_fix)
        snprintf(buf, n, "deviates from WinCUPL (bug #B%03d fixed): %s", bug, what);
    else
        snprintf(buf, n, FREECUPL_FIX_DEFAULT ? "WinCUPL bug #B%03d: %s; reproduced for -compat-refdefect"
                                              : "WinCUPL bug #B%03d: %s; use -fix for the corrected result", bug, what);
}
