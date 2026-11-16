/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - output files (JEDEC, documentation, listing, macro expansion).
   DARKSOFT. */
#ifndef FREECUPL_OUTPUT_H
#define FREECUPL_OUTPUT_H
#include "fit.h"
#include "pp.h"

int write_jed(const char *path, const FitResult *r, int security);
int write_doc(const char *path, const FitResult *r, int fuseplot, int expanded);
int write_lst(const char *path, const char *srcname);
int write_mx(const char *path, const PPOut *pp);
const char *created_string(void);

#endif
