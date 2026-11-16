/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - design to device fitting and fuse map generation.  DARKSOFT. */
#ifndef FREECUPL_FIT_H
#define FREECUPL_FIT_H
#include "device.h"
#include "logic.h"
#include "design.h"
#include "util.h"

typedef struct {
    int minlevel;       /* -m0..-m4, default 1 */
    int security;       /* -g */
    int kb;             /* -kb: choose the output polarity needing fewer terms */
} FitOpts;

/* documentation entry for one fitted function */
typedef struct {
    Sym *sym;
    int ext;
    int inv;            /* written as "!name" (complemented sum) */
    Cover f;            /* in variable indices, see fit_var_name */
    int used, max;      /* product terms used / available */
    int level;
} DocEq;

typedef struct {
    const Device *dev;
    char resolved[32];  /* mnemonic as printed in the JED header */
    int mode;
    unsigned char *fuse;
    DocEq *doc; int ndoc;
} FitResult;

int fit_design(const char *mnemonic, const FitOpts *o, FitResult *r);
/* name of a cover variable (after fit_design) */
const char *fit_var_name(int v, int *ext);

#endif
