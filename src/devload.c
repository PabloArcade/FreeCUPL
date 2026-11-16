/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - helpers shared by the command-line tools.  DARKSOFT. */
#include "devload.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>

void load_devfile(const char *path)
{
    size_t len;
    char *t = read_file(path, &len);
    if (!t) die("cannot read %s", path);
    if (dev_parse(t, path) < 0) die("errors in %s", path);
    free(t);
}

const Device *pick_device(const char *name, const Jed *j, const char *jedpath)
{
    const Device *d;
    const char *n = name ? name : j->device;
    if (!n) die("%s: no device given (-d) and none in the JED header", jedpath);
    const DevAlias *al = NULL;
    d = dev_find(n, &al);
    if (!d) die("unknown device '%s'", n);
    /* an automatic mnemonic that selects other descriptions: the JED header
       names the one that was used */
    if (al && al->via && j->device && dev_find(j->device, NULL)) d = dev_find(j->device, NULL);
    if (j->nfuses != d->nfuses)
        die("%s: fuse count %d does not match device %s (%d)", jedpath, j->nfuses, n, d->nfuses);
    return d;
}

void must_read_jed(const char *path, Jed *j, int quiet)
{
    char err[256];
    if (jed_read_file(path, j, err, sizeof err) < 0) die("%s: %s", path, err);
    if (quiet) return;
    if (j->has_fcsum && j->fcsum != jed_fuse_checksum(j->fuse, j->nfuses))
        fprintf(stderr, "%s: warning: fuse checksum C%04X, computed %04X\n", path, j->fcsum, jed_fuse_checksum(j->fuse, j->nfuses));
    if (j->has_xcsum && j->xcsum != 0 && j->xcsum != j->xcsum_calc)
        fprintf(stderr, "%s: warning: transmission checksum %04X, computed %04X\n", path, j->xcsum, j->xcsum_calc);
}
