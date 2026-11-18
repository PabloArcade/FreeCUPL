/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - fuse-level PLD simulator.  DARKSOFT. */
#ifndef FREECUPL_SIM_H
#define FREECUPL_SIM_H
#include "device.h"
#include "util.h"

#define SIM_MAXPINS 160
#define SIM_HASREG(m) ((m)->kind == MK_REG || (m)->hasreg)

enum { MK_NONE = 0, MK_COMB, MK_REG, MK_INPUT };
enum { OE_ALWAYS = 0, OE_NEVER, OE_ROW, OE_PIN };

typedef struct {
    int kind;          /* MK_* */
    int sum0, sum1;    /* rows ORed together (inclusive), sum0 > sum1: empty */
    int oe_kind, oe_row;
    int out_inv;       /* comb: pin = sum ^ out_inv ; reg: pin = Q ^ out_inv */
    int d_inv;         /* reg: D = sum ^ d_inv */
    int xs0, xs1;      /* second sum XORed into D / the output (xs0 > xs1: none) */
    int ck_row, ar_row, ap_row; /* per-macrocell clock / reset / preset rows (-1 none) */
    int node;          /* buried register */
    int hasreg;        /* register present (clocked, read through its feedback line) even when the pin shows the sum */
    int tff;           /* T flip-flop: the sum toggles the register */
    int bypass;        /* register bypassed while AR and AP are both true */
    int fb;            /* GF_*: what a registered macrocell feeds back */
    int oe_pin_too;    /* OE row also gated by the device OE pin */
    int rs0, rs1;      /* rows of the register's D input when they differ from the pin's sum (rs0 < 0: same) */
    int sp_row;        /* per-macrocell synchronous preset row (-1 none) */
    int ce;            /* clocked by the clock pin, ck_row is a clock enable */
    int ck_pin;        /* that clock pin when it is not the device's (0) */
    int steer, steer_step; /* steering fuses of the sum rows (-1: none) */
    int steer_other;   /* steershare: the partner's steering fuses (-1: none) */
    int sel;           /* select column: only rows connected there are ORed (-1: all rows) */
} McCfg;

typedef unsigned long long Word;

typedef struct {
    const Device *d;
    const unsigned char *fuse;
    int mode;
    const LineSrc *lines;
    int nw;
    Word *rowmask;           /* nrows * nw, bit set = column connected */
    unsigned char *live;     /* row usable (not disabled, not self-contradictory) */
    Word *selmask;           /* nrows * nw, connected select columns (not array inputs) */
    McCfg *mc;
    unsigned char ext[SIM_MAXPINS + 1];   /* externally applied value */
    unsigned char pin[SIM_MAXPINS + 1];   /* resolved pin value */
    unsigned char drv[SIM_MAXPINS + 1];   /* pin driven by device */
    unsigned char *q;                     /* register Q per macrocell */
    unsigned char *rowv;
    Word *colv;
    int unstable;            /* last settle did not converge */
} Sim;

/* Decode a fuse map for device d.  Returns 0 or -1 with message in err. */
int  sim_init(Sim *s, const Device *d, const unsigned char *fuse, char *err, size_t errlen);
void sim_free(Sim *s);
void sim_set_input(Sim *s, int pin, int v);
void sim_settle(Sim *s);
/* one clock pulse on the clock pin (low-high-low) with current inputs */
void sim_clock(Sim *s);
int  sim_has_regs(const Sim *s);
/* pin-level value of a register (what the pin shows when enabled) */
int  sim_get_state(const Sim *s, int mc);
void sim_set_state(Sim *s, int mc, int v);
/* is pin an input for the tester (dedicated input or I/O)? */
int  sim_pin_is_input(const Sim *s, int pin);
/* print decoded configuration as equations */
void sim_describe(const Sim *s, Buf *out);
const char *sim_mode_name(const Sim *s);
/* row is live and connects at least the given column? helper for relevance */
int  sim_row_uses_line(const Sim *s, int row, int line);
int  sim_row_true(const Sim *s, int row);

#endif
