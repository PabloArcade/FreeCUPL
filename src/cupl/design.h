/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - in-memory design (symbols, equations, expressions).  DARKSOFT. */
#ifndef FREECUPL_DESIGN_H
#define FREECUPL_DESIGN_H

/* pin / node extensions */
enum {
    X_NONE = 0, X_D, X_OE, X_AR, X_SP, X_T, X_J, X_K, X_S, X_R, X_CK, X_L, X_LE,
    X_AP, X_SR, X_CE, X_CA, X_DQ, X_IO, X_DFB, X_INT, X_LFB, X_TFB, X_IOD, X_IOL,
    X_LQ, X_IOAR, X_IOAP, X_IOCK, X_IOSP, X_IOSR, X_APMUX, X_ARMUX, X_CKMUX,
    X_OEMUX, X_LEMUX, X_IMUX, X_BYP, X_OBS, X_PR, X_TEC, X_T1, X_T2,
    X_COUNT
};
extern const char *const ext_names[X_COUNT];
int ext_lookup(const char *s);   /* case-insensitive, -1 if unknown */

enum { SK_NONE = 0, SK_PIN, SK_NODE, SK_FIELD, SK_FUNC };

typedef struct Sym Sym;
typedef struct Expr Expr;

struct Sym {
    char *name;
    int kind;
    int pin;            /* pin or node number (0: none) */
    int neg;            /* declared active low */
    int index;          /* trailing index 0..31 or -1 */
    int line;
    int used;           /* referenced in an expression */
    /* field */
    Sym **mem;
    int *mem_ext;
    int nmem;
    /* function */
    char **params;
    int nparams;
    int body0, body1;   /* token range of the body */
    int hidden;         /* function-invocation variable */
    int id;
};

enum { E_CONST = 0, E_VAR, E_NOT, E_AND, E_OR, E_XOR };

struct Expr {
    unsigned char op;
    unsigned char neg;  /* E_VAR: complemented literal */
    unsigned char val;  /* E_CONST */
    unsigned char ext;  /* E_VAR: feedback extension (X_NONE, X_IO ...) */
    unsigned char empty;    /* E_CONST: 'name;' without an expression (B033) */
    Sym *sym;
    Expr *a, *b;
};

typedef struct Eqn {
    Sym *sym;
    int ext;
    int neg;            /* '!' on the left-hand side */
    Expr *e;
    int line;
    int assigned;       /* set by '=' (not APPEND) */
    int minlvl;         /* -1: command line level */
    int npieces;
    /* the same function split by origin: the reference minimises (-m1)
       each group separately and only absorbs across groups:
       0 equations/APPEND, 1 TABLE, 2 CONDITION IF and SEQUENCE, 3 DEFAULT OUT */
    Expr *grp[4];
} Eqn;

#define NHDR 10
enum { H_NAME, H_PARTNO, H_REV, H_DATE, H_DESIGNER, H_COMPANY, H_ASSEMBLY, H_LOCATION, H_DEVICE, H_FORMAT };

typedef struct {
    char *hdr[NHDR];
    int hdr_line[NHDR];
    int hdr_missing;            /* a header item was not given */
    Sym **syms; int nsyms, capsyms;
    Eqn **eqns; int neqns, capeqns;
    int fuse_addr[64], fuse_val[64]; int nfuse;
    int nhidden;
    int nseq;           /* SEQUENCE blocks seen */
} Design;

extern Design g_des;

Sym *sym_find(const char *name);
Sym *sym_get(const char *name);       /* find or create (SK_NONE) */
Eqn *eqn_find(const Sym *s, int ext);
Expr *mk_const(int v);
Expr *mk_var(Sym *s, int ext, int neg);
Expr *mk_not(Expr *e);
Expr *mk_bin(int op, Expr *a, Expr *b);
void expr_print(const Expr *e, char *buf, int n);

/* parse the preprocessed text into g_des; returns 0 if no error */
struct PPOut;
int parse_design(struct PPOut *pp);
const char *pp_line_text(int line);

#endif
