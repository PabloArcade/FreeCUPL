/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - JEDEC fuse file (JESD3) reader / writer.  DARKSOFT. */
#ifndef FREECUPL_JED_H
#define FREECUPL_JED_H
#include <stddef.h>

typedef struct {
    int nfuses;            /* *QF; 0 if absent */
    int npins;             /* *QP; 0 if absent */
    int nvectors;          /* *QV; 0 if absent */
    unsigned char *fuse;   /* nfuses entries, each 0 or 1 */
    unsigned char *set;    /* nfuses entries: 1 if given explicitly by *L */
    int fdefault;          /* *F value, -1 if absent */
    int security;          /* *G value, -1 if absent */
    int has_fcsum;         /* *C present */
    unsigned fcsum;        /* *C value as read */
    int has_xcsum;         /* 4 hex digits after ETX present */
    unsigned xcsum;        /* transmission checksum as read */
    unsigned xcsum_calc;   /* computed STX..ETX byte sum */
    int has_stx, has_etx;
    char *header;          /* text between STX and the first '*' */
    char *device;          /* "Device <name>" from the header, or NULL */
    int nvec;              /* raw test vectors (*V fields) */
    char **vec;
    int nnotes;
    char **notes;          /* *N fields */
} Jed;

void jed_init(Jed *j);
void jed_free(Jed *j);
/* allocate fuse map of n fuses, all set to v */
void jed_alloc(Jed *j, int n, int v);
/* Parse a JEDEC file.  Returns 0 on success, -1 on error (message in err). */
int jed_parse(const char *buf, size_t len, Jed *j, char *err, size_t errlen);
int jed_read_file(const char *path, Jed *j, char *err, size_t errlen);
/* fuse checksum: 16-bit sum of 8-fuse little-endian bytes */
unsigned jed_fuse_checksum(const unsigned char *fuse, int n);

/* Writer.  header: text lines (no CR/LF) printed after STX.  Fuses are
   written in rows of 32, omitting rows equal to the default (0) value,
   as the reference compiler does. */
typedef struct {
    const char **header;
    int nheader;
    int npins;      /* for *QP; 0 = omit */
    int security;   /* *G value */
    const char **vectors; /* optional *V lines (already formatted, no '*') */
    int nvectors;
} JedWriteOpts;

char *jed_format(const unsigned char *fuse, int nfuses, const JedWriteOpts *o, size_t *len);

/* header block as the original writes it: tool line, Device/Library,
   Created and the eight header fields, labels padded to 16 columns */
#define JED_NFIELDS 8
extern const char *const jed_field_names[JED_NFIELDS];
typedef struct {
    const char *tool;      /* complete first line */
    const char *device, *library, *created;
    const char *field[JED_NFIELDS];
} JedHeader;
/* fills lines[0..10] with malloc'd strings; returns the count (11) */
int jed_std_header(const JedHeader *h, char **lines);

#endif
