/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - three-valued simulator on the configuration decoded by
   sim_init().  DARKSOFT. */
#ifndef FREECUPL_SIM3_H
#define FREECUPL_SIM3_H
#include "sim.h"

enum { V0 = 0, V1 = 1, VX = 2, VZ = 3, VNONE = 4 };

typedef struct {
    Sim s;                               /* decoded configuration */
    const Device *d;
    int npos;                            /* highest pin / node number + 1 */
    unsigned char drv[SIM_MAXPINS + 1];  /* external drive: V0 V1 VX or VNONE */
    unsigned char pin[SIM_MAXPINS + 1];  /* value seen by the array: V0 V1 VX */
    unsigned char out[SIM_MAXPINS + 1];  /* value the device puts on the pin: V0 V1 VX VZ */
    unsigned char *q;                    /* per macrocell register / latch Q: V0 V1 VX */
    unsigned char *ck;                   /* per macrocell clock signal seen at the last step */
    unsigned char *oe;                   /* per macrocell output enable: V0 V1 VX */
    unsigned char *sum;                  /* per macrocell value of its sum (D input / OR term) */
    unsigned char *dsv, *env;            /* per register: next state and clock enable sampled at the last clock step */
    unsigned char *rowv;                 /* per row: V0 V1 VX */
    signed char *stuck;                  /* per row: -1, 0 or 1 (STUCKL/STUCKH) */
    unsigned char *cyc;                  /* per macrocell: part of a combinational loop */
    unsigned char *chg;                  /* per macrocell: changed in the last loop pass */
    int budget;                          /* passes of the loop iteration (40 / 100) */
    unsigned char *unst;                 /* per macrocell: unstable after the last settle */
    int passes;                          /* passes of the last settle */
    int pinclk_off;                      /* clock steps ignore registers clocked by the clock pin (C/K pulses) */
    unsigned char *gar, *gsp;            /* per macrocell: the device-wide AR / SP term applies (default all) */
    int powerup;                         /* power-up register state (V0 or VX) */
    int fbx;                             /* evaluating with combinational loops unknown */
    unsigned char *fbinv;                /* per macrocell: the array reads the complement (reference simulator bug B040) */
    unsigned char *fbunk;                /* per macrocell: the array reads X (buried node without an equation, as the reference simulator) */
    int oepin;                           /* the device OE pin as the simulator uses it (normally d->oe_pin) */
    int noce;                            /* clock enables ignored (reference simulator bug B030) */
    int regzero;                         /* registers load 0 (reference simulator defect, -compat-refdefect) */
    int noasync;                         /* asynchronous reset / preset do not act (intermediate step of a vector) */
    int only;                            /* pass3: -1 all outputs, >= 0 only this one of the loop outputs, -2 none of them */
    int simlatch;                        /* simulate latched outputs and input registers/latches (else, as the
                                            reference simulator: they never load, input registers read X) */
    unsigned char *oeforce;              /* per macrocell: always enabled (reference simulator defect on devices
                                            with 'simdefect regoe', -compat-refdefect) */
    const unsigned char *firepins;       /* C/K vector: the pins pulsed (NULL: the device clock pin) */
    unsigned char inle[SIM_MAXPINS + 1];   /* latch enable pin of an input latch (0: the device's simle pin) */
    unsigned char inmode[SIM_MAXPINS + 1]; /* 1: the array reads the pin through its input register (.DQ), 2: latch (.LQ) */
    unsigned char inq[SIM_MAXPINS + 1];  /* input register / latch state */
    unsigned char insv[SIM_MAXPINS + 1]; /* pin value sampled at the last clock step */
    unsigned char inck;                  /* clock pin level at the last clock step */
    /* trace hook: called after every repeated evaluation of a combinational loop */
    void (*trace)(void *ctx, int pass);
    void *trace_ctx;
} Sim3;

int  sim3_init(Sim3 *t, const Device *d, const unsigned char *fuse, char *err, size_t errlen);
void sim3_free(Sim3 *t);
/* macrocell index of a pin or node number (-1 none) */
int  sim3_mc_of(const Sim3 *t, int pin);
/* the pin (or node) is an output of the design (a macrocell configured to drive it) */
int  sim3_is_output(const Sim3 *t, int pin);
/* registered (or latched) macrocell */
int  sim3_is_reg(const Sim3 *t, int mc);
void sim3_drive(Sim3 *t, int pin, int v);       /* V0 V1 VX VNONE */
/* evaluate combinational logic until stable (max 1 + 40 passes) */
void sim3_settle(Sim3 *t);
/* load registers whose clock rose since the last step; nonzero if one
   changed */
int  sim3_clock_step(Sim3 *t);
void sim3_clock_force(Sim3 *t);
void sim3_clock_sync(Sim3 *t);
/* value of a pin or node for display: V0 V1 VX VZ */
int  sim3_value(const Sim3 *t, int pin);
/* value before the output buffer (register Q or OR term), for $TRACE 4 */
int  sim3_prebuffer(const Sim3 *t, int pin);
/* preload: the register of macrocell mc gets !Q = v (v V0/V1) */
void sim3_preload(Sim3 *t, int mc, int v);
/* force the product term starting at JEDEC fuse addr; returns -1 if no
   term starts there */
int  sim3_stuck(Sim3 *t, int addr, int v);
/* clock signal of a register (V0 V1 VX) in the current state */
int  sim3_clock_of(const Sim3 *t, int mc);

#endif
