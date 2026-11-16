/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - data-driven device descriptions.  DARKSOFT.
   See docs/DEVFMT.txt for the text format. */
#ifndef FREECUPL_DEVICE_H
#define FREECUPL_DEVICE_H

#define DEV_MAXMODES 4
#define DEV_MAXFUSENAMES 8
#define DEV_MAXCOND 4

enum { SRC_NONE = 0, SRC_PIN, SRC_FB };
typedef struct { unsigned char kind; unsigned char pin; } LineSrc;

enum { MC_COMB = 1, MC_REG, MC_GAL, MC_V10, MC_GEN };

#define MC_MAXCFG 8
/* configuration roles of a generic macrocell */
enum { CR_UNUSED = 0, CR_INPUT, CR_COMB, CR_COMBN, CR_REG, CR_REGN, CR_COMBFB, CR_OEONLY, CR_COUNT };

enum { GR_COMB = 0, GR_REG, GR_FUSE, GR_ARAP };
/* MC_GEN register feedback into the array */
enum { GF_NQ = 0, GF_Q, GF_PIN, GF_XQ, GF_XP };   /* GF_XP: the register after the polarity (= pin while driven) */   /* GF_XQ: complement of the register after the polarity (= !pin while driven) */
enum { GO_ALWAYS = 0, GO_ROW, GO_PIN, GO_ROWPIN };

typedef struct {
    int pin;
    int type;
    int sum0, sum1;     /* inclusive row range of the OR (comb/reg/v10); GAL: all 8 rows */
    int oe_row;         /* -1: none */
    int pol;            /* fixed polarity (comb/reg): 1 = active high */
    int xor_fuse;       /* GAL */
    int ac1_fuse;       /* GAL */
    int s0_fuse, s1_fuse; /* 22V10-style: polarity, comb/reg */
    /* MC_GEN (generic PAL macrocell, see docs/DEVFMT.txt) */
    int xsum0, xsum1;   /* second sum XORed with the first (-1: none) */
    int oe_kind;        /* GO_* */
    int pol_fuse, pol_hi; /* programmable polarity: fuse == pol_hi -> active high */
    int reg_kind;       /* GR_* */
    int reg_fuse, reg_on; /* GR_FUSE: registered when fuse == reg_on */
    int ck_row, ar_row, ap_row; /* per-macrocell clock / async reset / async preset */
    int fb;             /* GF_* */
    int oe_reg_pin;     /* registered configuration: enabled by the device OE pin instead of the OE row */
    int fb_fuse, fb_pin; /* feedback reads the pin when fuse fb_fuse == fb_pin (-1: none) */
    int pol_d;          /* 1: polarity applied before the register, register output inverted; 2: polarity before the register, pin = Q */
    int t_fuse, t_val;
    int node;           /* buried register (PINNODE number in 'pin'): drives no pin */
    int ckmux_fuse;     /* when 1 the register is clocked by the clock pin instead of its CK term (-1: none) */
    int alwaysreg;      /* the register always exists; the reg fuse only selects what drives the pin */
    int split_fuse;     /* when this fuse is 1 the sum is only half0..half1 (-1: none) */
    int half0, half1;
    int split_val;      /* value of split_fuse that selects the split (default 1) */
    int split_out;      /* the split value is also written when the pin is not an output or the register has its own sum */
    int qsplit_fuse;    /* when this fuse is 1 the pin sum is qhalf and the register's D input is qsum (-1: none) */
    int qhalf0, qhalf1, qsum0, qsum1;
    int sp_row;         /* per-macrocell synchronous preset term (may be shared by several macrocells) */
    int reg_idle;       /* value of reg_fuse for an unused pin (-1: 0, and for input/OE-only pins the combinational value) */
    int split_need;     /* the unsplit value is written only when the sum needs rows beyond the half */
    int oemux_fuse;     /* OE multiplexer: 1 = the device OE pin (registered output without .OE), 0 = the OE term (-1: none) */
    int io_bad, io_off; /* 'refdefect ioaddr OFF PIN...': the reference writes the feedback select requested by .io at fbfuse + OFF instead of fbfuse */
    int combfull;       /* 'refdefect combfull PIN...': a combinational output needing all rows next to its OE term (registered mode) is accepted by the reference, which then writes no such output */
    int fb_xor;         /* the feedback select means 'the other source': pin for a register, register for a combinational output */
    int fb_fuse2;       /* a second feedback-select fuse, written like fb_fuse (-1: none) */
    int oe0;            /* 'refdefect oe0 PIN': the reference leaves this output's OE term false without .OE */
    int latch;          /* the register is a latch written with .L (the simulator loads it at the enable step) */
    int xone;           /* XOR sums, PAL22RX8: the second sum takes the first operand (as written, complement included) when it fits, else the second operand; an equation without $ goes there when it fits */
    int xswap;          /* XOR sums: the operand with fewer terms (an equation without $: when it fits) goes into xsum, as the reference does */
    int fb_reg;         /* the pin can only be read back while registered: reading a combinational output is rejected (0014cb) */
    int alias_inv;      /* 'refdefect aliasread': the reference reads this register through its node alias with the other polarity */
    int ce_fuse;        /* when 1 the register is clocked by the clock pin and its CK row is a clock enable (.CE) (-1: none) */
    int steer_fuse, steer_step; /* product-term steering: sum row r reaches this output only when fuse steer_fuse + (r - sum0) * steer_step is 1 (-1: none); macrocells with the same sum rows share them */
    int sel_comb, sel_reg; /* GAL: select columns - a row reaches the combinational output / the register only when its fuse in that column is 0 (-1: none) */
    int ac1_val;        /* GAL: value of the ac1 fuse that means 'input' (simple) / 'combinational' (registered) */
    int xorr_fuse, xorr_val; /* GAL: separate polarity fuse of a register, active high when == xorr_val (-1: none) */
    /* configuration fuses written exactly as the reference compiler does,
       per role (CR_*); values are bit strings over cfg_fuse[] */
    int ncfg;
    int cfg_fuse[MC_MAXCFG];
    signed char cfg_val[CR_COUNT][MC_MAXCFG];   /* -1: role not given */
} Macrocell;

typedef struct {
    char name[16];
    int ncond;
    int cond_fuse[DEV_MAXCOND];
    int cond_val[DEV_MAXCOND];
    LineSrc *lines;     /* nlines entries, NULL until 'lines' is given */
} DevMode;

typedef struct {
    char name[24];      /* mnemonic */
    char mode[16];      /* forced mode or "auto" or "" */
    char simple[24], complex_[24], registered[24]; /* resolved names for auto */
    int fbquirk;
    int autofb_simple;  /* auto: outputs read back do not force complex mode */
    int via;            /* auto: the resolved names are mnemonics of other descriptions (one per mode) */
    int nreserve;       /* -1: the device's power-down pin; else pins listed by reserve= */
    int reserve[4];        /* simple mode: an output read back becomes a dedicated input (reference quirk) */
    char *partname;     /* 'partname': descriptive part name of this mnemonic (documentation only) */
} DevAlias;

typedef struct Device {
    char name[24];      /* canonical name (first 'device' token) */
    int npins;
    int gnd, vcc;
    int nfuses;
    int ncols, nrows;
    int nlines;         /* ncols / 2 */
    int clk_pin;        /* 0 none */
    int oe_pin;         /* 0 none; active low */
    int ar_row, sp_row; /* -1 none */
    int ap_row;         /* global asynchronous preset row, -1 none */
    int nconst;         /* fuses always programmed to 1 (mode / product-term enables) */
    int *const_fuse;
    int ptd_base;       /* -1 none */
    int ues_base, ues_len;
    int pd_fuse, pd_pin;/* -1/0 none */
    int nmodes;
    DevMode modes[DEV_MAXMODES];
    int nmc;
    Macrocell *mc;
    int nalias;
    DevAlias *alias;
    char fusename[DEV_MAXFUSENAMES][16];
    int fuseaddr[DEV_MAXFUSENAMES];
    int nfusenames;
    char *descr;        /* free text */
    char *partname;     /* 'partname' of a description without aliases (its name is the mnemonic) */
    int truecol_odd;       /* 'truecol odd': line k true at column 2k+1, complement at 2k */
    int nodfb;             /* 'nodfb': the .DFB feedback extension is not available */
    int tfb;               /* 'tfb': .TFB reads the register like .DFB and makes it a T flip-flop */
    int nnodealias;        /* 'nodealias NODE PIN': PINNODE NODE names the register of the macrocell on PIN */
    int nodealias_node[48], nodealias_pin[48];
    int nrowat;            /* 'rowat ROW FUSE': rows not at ROW*ncols */
    int rowat_row[8], rowat_addr[8];
    int inflag_dq;         /* 'inflag dq': the input fuses ('inflag') belong to input registers read through .DQ (instead of latches, .LQ) */
    int refdefect_inreg;   /* 'refdefect inreg': the reference leaves an output pin used only as an input in registered configuration (the array reads the register) */
    int refdefect_combd;   /* 'refdefect combd': the reference ORs a combinational and a .D equation of one pin into a combinational sum; freecupl rejects (0907cb) */
    int steer_share;       /* 'steershare 1': a term in both sums of a steering pair is written once without a steering fuse (a row with neither fuse reaches both outputs) */
    int nodedt_reject;     /* 'nodedt reject': .D and .T on one PINNODE are rejected (0025cb); on a pin they give a T flip-flop */
    int rowfill;           /* 'rowfill 1': AND-array fuses not written are left 1 (default 0) */
    int galreg_unused_ac1; /* registered mode: AC1 written for an unused GAL OLMC (default 1) */
    int ninflag;           /* 'inflag PIN FUSE': fuse set to 1 when the pin is read as an input */
    int inflag_pin[32], inflag_fuse[32];
    int nnolatch;          /* 'nolatch PIN': the pin has an 'inflag' fuse but no input latch (.LQ rejected) */
    int nolatch_pin[4];
    int ntec;              /* 'tec PIN FUSE': fuse cleared by PIN.TEC = 'b'0 (.TEC accepted) */
    int tec_pin[16], tec_fuse[16];
    int tecok;             /* 'tec any': .TEC accepted on outputs, no fuse */
    char refdefect_mode[16]; /* 'refdefect MODE PIN': the reference compiler's */
    int refdefect_pin;       /* fuse map is wrong for this output (freecupl warns 0905cb) */
    int refdefect_ones, refdefect_nones; /* 'refdefect ones ADDR N': with that output in use the reference first writes N ones from ADDR */
    int refdefect_shift;     /* 'refdefect shift N': a combinational sum of that output is written N fuses after its rows */
    int refdefect_oe0;       /* 'refdefect oe0': the reference leaves the OE term of an output without .OE false */
    int refdefect_nodeneg;   /* 'refdefect nodeneg': the reference does not complement the equation of a PINNODE declared active low */
    int refdefect_nodecomb;  /* 'refdefect nodecomb': the reference accepts a combinational equation on a buried register */
    unsigned refdefect_kbnoflip; /* 'refdefect kbnoflip [MODE...]': -kb complements the sum without changing the output polarity (bit per mode) */
    unsigned refdefect_kbnoflip_reg; /* the same for registered outputs only ('refdefect kbnoflip MODE/reg') */
    int refdefect_kbfbinv;   /* 'refdefect kbfbinv': -kb, a registered output not complemented is read back as if it were */
    int refdefect_nojoin;    /* 'refdefect nojoin': the reference never writes the unsplit value of a split fuse */
    int sim_regoe;           /* 'simdefect regoe': the reference simulator ignores the output enable of
                                registered outputs (observed on the PEEL parts) */
    int sim_passes;          /* 'simpasses N': passes of the reference simulator on a loop (default by
                                architecture, sim3.c) */
    int sim_comb0;           /* 'simcombinit low': combinational outputs start low in the reference simulator */
    int sim_noclock;         /* 'simdefect noclock' (1): the reference simulator has no clock pin (C/K/P invalid, registers never load);
                                'simdefect noload' (2): C/K/P valid, the registers never load on the clock */
    int sim_oe;              /* 'simdefect oeignore' (1): the device OE pin enables nothing; 'simdefect oehigh' (2): it is active high */
    int sim_regzero;         /* 'simdefect regzero': the reference simulator loads 0 into the registers and rejects C/K/P */
    int sim_preload_pin;     /* 'simpreload pin': a preload value is the pin level of the register (else !Q) */
    int sim_unverified;      /* 'simnote unverified': the simulation model of the part is copied from the reference simulator */
    int sim_le;              /* 'simle PIN': latch enable of the input latches in the reference simulator */
    int sim_oepin;           /* 'simdefect oepin N': the reference simulator takes the OE of registered outputs from pin N */
    int sim_powerup;         /* 'powerup reset|pinhigh|x': register state at power-up in the
                                reference simulator (found by black-box probing);
                                0: by architecture (sim3.c) */
    int nlibtag;
    char (*libtag)[2][24]; /* resolved mnemonic -> library tag printed in the JED header */
} Device;
enum { PU_ARCH, PU_RESET, PU_PINHIGH, PU_X };

/* Parse description text; appends to the global registry.  Returns number
   of devices added, -1 on error (message printed to stderr). */
int dev_parse(const char *text, const char *srcname);
/* Load the built-in descriptions (once). */
void dev_load_builtin(void);
/* Find by mnemonic (case-insensitive), searching aliases. Returns NULL if
   unknown.  If alias is non-NULL it receives the matching alias entry. */
const Device *dev_find(const char *mnemonic, const DevAlias **alias);
int dev_count(void);
/* library tag for a resolved mnemonic ("" if unknown) */
const char *dev_libtag(const Device *d, const char *resolved);
const Device *dev_get(int i);
/* index of macrocell at pin, -1 if none */
int dev_mc_at(const Device *d, int pin);
/* first fuse of AND-array row r */
int dev_row_addr(const Device *d, int r);
/* determine mode index from fuses; -1 if no mode matches */
int dev_mode_of(const Device *d, const unsigned char *fuse);
const LineSrc *dev_lines(const Device *d, int mode);

#endif
