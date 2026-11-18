/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - CSIM-compatible test-vector simulator (.si -> .so/.wo, JEDEC
   test vectors).  See docs/SIMFMT.txt.  DARKSOFT. */
#ifndef FREECUPL_CSIM_H
#define FREECUPL_CSIM_H
#include "device.h"
#include <stddef.h>

/* header items in the order of the CUPL header */
enum { CH_NAME, CH_PARTNO, CH_REV, CH_DATE, CH_DESIGNER, CH_COMPANY, CH_ASSEMBLY, CH_LOCATION, CH_DEVICE, CH_FORMAT, CH_COUNT };

typedef struct {
    char *name;
    int pin;            /* pin or node number */
    int neg;            /* declared active low */
} CsSym;

typedef struct {
    char *name;
    int n;
    char **mem;         /* member symbol names, in written order */
} CsField;

/* what the simulator needs to know about a compiled design */
typedef struct {
    const Device *dev;
    char mnemonic[32];
    unsigned char *fuse;
    char *hdr[CH_COUNT];   /* NULL: not given */
    CsSym *sym; int nsym;
    CsField *fld; int nfld;
    int reserve[8]; int nreserve;   /* power-down pins (held low in the JED vectors) */
    /* pins / nodes with .AR / .SP equations: the reference simulator applies a
       device-wide reset / preset term only to these registers */
    int arpin[128]; int narpin;
    int sppin[128]; int nsppin;
    int oepin[128]; int noepin;     /* pins with .OE equations */
    int ctlinfo;                    /* arpin / sppin are known */
    /* declared pins without any equation: the reference simulator works
       from the source and treats them as inputs (used = read by the
       design) or leaves their column as written (not read) */
    int noeqn[64]; unsigned char noeqn_used[64]; int nnoeqn;
    /* output pins that other equations read complemented in the reference
       simulator (B040) */
    int fbinv[128]; int nfbinv;
} CsDesign;

typedef struct {
    const char *si_path;   /* simulation input */
    const char *si_name;   /* printed after "LISTING FOR SIMULATION FILE: " */
    const char *so_path;   /* listing (NULL: none) */
    const char *wo_path;   /* waveform file (NULL: none) */
    const char *jed_path;  /* JED to append the vectors to (NULL: none) */
    const char *banner[4]; /* first lines of the listing */
    int verbose;           /* -v: listing also to stdout */
    int simlatch;          /* -simlatch: simulate latched outputs and registered/latched inputs
                              (unverified timing); default as the reference: X */
    int refdefect;         /* -compat-refdefect: reproduce documented defects of the reference
                              simulator (22V10 shared AR/SP only on registers with .AR/.SP
                              equations; 'simdefect' devices) */
    int quiet;             /* no warnings on stdout (internal) */
} CsOpts;

/* N pins of JEDEC test vectors per device description (nctable.c) */
typedef struct { const char *dev; const char *pins; } CsNcEntry;
extern const CsNcEntry cs_nctable[];

/* run the simulation; returns 0 when clean, 1 when errors were reported,
   2 when the input could not be read.  Messages go to the listing. */
int csim_run(const CsDesign *d, const CsOpts *o);

void cs_design_free(CsDesign *d);
/* absolute file (freecupl's own text format, docs/SIMFMT.txt) */
int cs_abs_write(const char *path, const CsDesign *d);
int cs_abs_read(const char *path, CsDesign *d, char *err, size_t errlen);

/* the part model follows the reference simulator and is not verified */
int csim_unverified(const Device *dv);

#endif
