/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FreeCUPL - the file name of the JEDEC file.  One definition for the
   compiler, the simulator and the front ends, which look for the file.
   DARKSOFT. */
#ifndef FREECUPL_NAMEBASE_H
#define FREECUPL_NAMEBASE_H
#include <string.h>

/* File name (without extension) of the JEDEC file for a Name header
   item, formed as WinCUPL's cupl.exe forms it: the item up to the first
   blank, without a directory or drive part, at most 28 characters.
   Returns 0, or -1 when the result is empty or is no file name on
   Windows (one of * ? " < > |, a control character, or a device name
   such as CON or LPT1); out then has the name for the message. */
static int name_file_base(const char *name, char *out, size_t n)
{
    static const char *const dev[] = { "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4",
        "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6",
        "lpt7", "lpt8", "lpt9" };
    const char *p, *b = name;
    size_t len, i, k;
    int bad = 0;
    for (p = name; *p && *p != ' ' && *p != '\t'; p++)
        if (*p == '/' || *p == '\\' || *p == ':') b = p + 1;
    len = (size_t)(p - b);
    if (len > 28) len = 28;
    if (len >= n) len = n - 1;
    memcpy(out, b, len);
    out[len] = 0;
    if (len == 0) return -1;
    for (i = 0; i < len; i++)
        if ((unsigned char)out[i] < 32 || strchr("*?\"<>|", out[i])) bad = 1;
    /* a device name, alone or before a dot, with blanks after it */
    for (k = 0; k < sizeof dev / sizeof dev[0]; k++) {
        size_t dl = strlen(dev[k]);
        if (len < dl) continue;
        for (i = 0; i < dl; i++) {
            int c = (unsigned char)out[i];
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (c != dev[k][i]) break;
        }
        if (i < dl) continue;
        while (i < len && out[i] == ' ') i++;
        if (i == len || out[i] == '.') bad = 1;
    }
    return bad ? -1 : 0;
}

#endif
