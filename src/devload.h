/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - helpers shared by the command-line tools.  DARKSOFT. */
#ifndef FREECUPL_DEVLOAD_H
#define FREECUPL_DEVLOAD_H
#include "device.h"
#include "jed.h"
/* load extra description file (exits on error) */
void load_devfile(const char *path);
/* pick device: explicit name, else JED header; exits with message on failure */
const Device *pick_device(const char *name, const Jed *j, const char *jedpath);
/* read JED or exit; warns about checksum problems unless quiet */
void must_read_jed(const char *path, Jed *j, int quiet);
#endif
