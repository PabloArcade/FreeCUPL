/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - three-valued (0/1/X, Z) simulator with CSIM semantics
   (docs/SIMFMT.txt).  DARKSOFT. */
#include "sim3.h"
#include <stdlib.h>
#include <string.h>

#define MAXPASS 42

/* the register value after a reset: 0; on devices whose registered pins
   start high in the reference simulator ('powerup pinhigh') the reference
   also resets them to a high pin and presets them to a low one */
#define RV(t, i) ((t)->d->sim_powerup == PU_PINHIGH ? ((t)->s.mc[i].out_inv ? V0 : V1) : V0)

static int F(const Sim3 *t, int a) { return a >= 0 ? t->s.fuse[a] : 0; }
static int NOT3(int a) { return a == VX ? VX : !a; }
static int XOR3(int a, int b) { return (a == VX || b == VX) ? VX : (a ^ b); }
static int AND3(int a, int b) { if (a == V0 || b == V0) return V0; if (a == VX || b == VX) return VX; return V1; }
static int OR3(int a, int b) { if (a == V1 || b == V1) return V1; if (a == VX || b == VX) return VX; return V0; }

/* PINNODE number naming the register of an output macrocell ('nodealias') */
static int alias_mc(const Sim3 *t, int pin)
{
    int i;
    if (pin <= t->d->npins) return -1;
    for (i = 0; i < t->d->nnodealias; i++)
        if (t->d->nodealias_node[i] == pin) return dev_mc_at(t->d, t->d->nodealias_pin[i]);
    return -1;
}

int sim3_mc_of(const Sim3 *t, int pin)
{
    int m;
    if (pin < 1 || pin > SIM_MAXPINS) return -1;
    m = dev_mc_at(t->d, pin);
    return m >= 0 ? m : alias_mc(t, pin);
}

int sim3_is_reg(const Sim3 *t, int mc)
{
    return mc >= 0 && t->s.mc[mc].kind == MK_REG;
}

int sim3_is_output(const Sim3 *t, int pin)
{
    int m = sim3_mc_of(t, pin);
    if (m < 0) return 0;
    if (t->s.mc[m].node) return 1;
    if (alias_mc(t, pin) >= 0) return SIM_HASREG(&t->s.mc[m]);
    return t->s.mc[m].kind == MK_COMB || t->s.mc[m].kind == MK_REG;
}

static int row_on3(const Sim3 *t, int r) { return r >= 0 ? t->rowv[r] : V0; }

static int bypassed3(const Sim3 *t, int m)
{
    const McCfg *c = &t->s.mc[m];
    return c->bypass && row_on3(t, c->ar_row) == V1 && row_on3(t, c->ap_row) == V1;
}

static int line_raw3(const Sim3 *t, int k);

static int line_value3(const Sim3 *t, int k)
{
    LineSrc src = t->s.lines[k];
    int v = line_raw3(t, k), m;
    if (v != VX && (src.kind == SRC_PIN || src.kind == SRC_FB) && (m = dev_mc_at(t->d, src.pin)) >= 0 && t->fbinv[m])
        return NOT3(v);
    return v;
}

static int line_raw3(const Sim3 *t, int k)
{
    LineSrc src = t->s.lines[k];
    if (t->fbx && (src.kind == SRC_PIN || src.kind == SRC_FB)) {
        /* first evaluation of a vector: combinational loops unknown */
        int m = dev_mc_at(t->d, src.pin);
        if (m >= 0 && t->cyc[m] && !(src.kind == SRC_FB && SIM_HASREG(&t->s.mc[m]) && t->s.mc[m].fb != GF_PIN)) return VX;
    }
    if (src.kind == SRC_PIN) return t->inmode[src.pin] ? t->inq[src.pin] : t->pin[src.pin];
    if (src.kind == SRC_FB) {
        int m = dev_mc_at(t->d, src.pin);
        const McCfg *c = &t->s.mc[m];
        if (t->fbunk[m]) return VX;
        if (SIM_HASREG(c) && !bypassed3(t, m)) {
            int q = t->q[m];
            if (c->fb == GF_Q) return q;
            if (c->fb == GF_XQ) return NOT3(XOR3(q, c->out_inv));
            if (c->fb == GF_XP) return XOR3(q, c->out_inv);
            if (c->fb == GF_NQ) return NOT3(q);
        }
        return t->pin[src.pin];
    }
    return V0;
}

static void eval_rows3(Sim3 *t)
{
    const Device *d = t->d;
    int nw = t->s.nw, k, r, w;
    Word zero[8], xm[8];
    memset(zero, 0, sizeof zero);
    memset(xm, 0, sizeof xm);
    for (k = 0; k < d->nlines; k++) {
        int v = line_value3(t, k);
        int ct = 2 * k + d->truecol_odd, cf = 2 * k + 1 - d->truecol_odd;
        if (v == VX) {
            xm[ct / 64] |= (Word)1 << (ct % 64);
            xm[cf / 64] |= (Word)1 << (cf % 64);
        } else {
            int c0 = v ? cf : ct;
            zero[c0 / 64] |= (Word)1 << (c0 % 64);
        }
    }
    for (r = 0; r < d->nrows; r++) {
        int v = V0;
        if (t->s.live[r]) {
            const Word *m = &t->s.rowmask[r * nw];
            int z = 0, x = 0;
            for (w = 0; w < nw; w++) {
                if (m[w] & zero[w]) { z = 1; break; }
                if (m[w] & xm[w]) x = 1;
            }
            v = z ? V0 : x ? VX : V1;
        }
        if (t->stuck[r] >= 0) v = t->stuck[r];
        t->rowv[r] = (unsigned char)v;
    }
}

static int row_in_sum(const Sim3 *t, const McCfg *m, int r)
{
    if (m->steer >= 0) {
        int i = r - m->sum0;
        if (m->steer_other >= 0) {
            if (F(t, m->steer_other + i * m->steer_step) && !F(t, m->steer + i * m->steer_step)) return 0;
        } else if (!F(t, m->steer + i * m->steer_step)) return 0;
    }
    if (m->sel >= 0 && !((t->s.selmask[r * t->s.nw + m->sel / 64] >> (m->sel % 64)) & 1)) return 0;
    return 1;
}

static int sum3(const Sim3 *t, const McCfg *m, int s0, int s1)
{
    int r, v = V0, x = V0;
    for (r = s0; r <= s1; r++) if (row_in_sum(t, m, r)) v = OR3(v, t->rowv[r]);
    for (r = m->xs0; r <= m->xs1; r++) x = OR3(x, t->rowv[r]);
    return XOR3(v, x);
}

static int psum3(const Sim3 *t, const McCfg *m) { return sum3(t, m, m->sum0, m->sum1); }

static int dsum3(const Sim3 *t, const McCfg *m)
{
    if (m->rs0 < 0) return psum3(t, m);
    return sum3(t, m, m->rs0, m->rs1);
}

static int oe3(const Sim3 *t, const McCfg *m)
{
    const Device *d = t->d;
    switch (m->oe_kind) {
    case OE_ALWAYS: return V1;
    case OE_NEVER: return V0;
    case OE_ROW: {
        int v = t->rowv[m->oe_row];
        if (m->oe_pin_too && t->oepin && d->sim_oe != 1) v = AND3(v, d->sim_oe == 2 ? t->pin[t->oepin] : NOT3(t->pin[t->oepin]));
        return v;
    }
    case OE_PIN:
        if (!t->oepin || d->sim_oe == 1) return V1;
        return d->sim_oe == 2 ? t->pin[t->oepin] : NOT3(t->pin[t->oepin]);
    }
    return V0;
}

int sim3_clock_of(const Sim3 *t, int mc)
{
    const McCfg *m = &t->s.mc[mc];
    if (m->ck_row >= 0 && !m->ce) return t->rowv[m->ck_row];
    if (m->ce && m->ck_pin) return t->pin[m->ck_pin];
    if (t->d->clk_pin) return t->pin[t->d->clk_pin];
    /* clock enable without a described clock pin (ATV2500B/C): the
       synchronous clock is pin 1 */
    if (m->ce) return t->pin[1];
    return V0;
}

/* combinational dependency loops between macrocells */
static void find_loops(Sim3 *t)
{
    const Device *d = t->d;
    int n = d->nmc, i, j, k, r;
    unsigned char *adj = (unsigned char *)xcalloc((size_t)n * (size_t)n + 1, 1);
    for (i = 0; i < n; i++) {
        const McCfg *m = &t->s.mc[i];
        if (m->kind != MK_COMB) continue;
        for (k = 0; k < d->nlines; k++) {
            LineSrc src = t->s.lines[k];
            int b, uses = 0;
            if (src.kind != SRC_PIN && src.kind != SRC_FB) continue;
            b = dev_mc_at(d, src.pin);
            if (b < 0 || t->s.mc[b].kind != MK_COMB) continue;
            if (src.kind == SRC_FB && SIM_HASREG(&t->s.mc[b]) && t->s.mc[b].fb != GF_PIN) continue;
            for (r = m->sum0; r <= m->sum1 && !uses; r++) if (sim_row_uses_line(&t->s, r, k)) uses = 1;
            for (r = m->xs0; r <= m->xs1 && !uses; r++) if (sim_row_uses_line(&t->s, r, k)) uses = 1;
            if (!uses && m->oe_kind == OE_ROW && sim_row_uses_line(&t->s, m->oe_row, k)) uses = 1;
            if (uses) adj[i * n + b] = 1;
        }
    }
    for (k = 0; k < n; k++)
        for (i = 0; i < n; i++)
            if (adj[i * n + k])
                for (j = 0; j < n; j++)
                    if (adj[k * n + j]) adj[i * n + j] = 1;
    for (i = 0; i < n; i++) t->cyc[i] = adj[i * n + i];
    free(adj);
}

int sim3_init(Sim3 *t, const Device *d, const unsigned char *fuse, char *err, size_t errlen)
{
    int i;
    memset(t, 0, sizeof *t);
    t->only = -1;
    t->oepin = d->oe_pin;
    if (sim_init(&t->s, d, fuse, err, errlen) != 0) return -1;
    t->d = d;
    if (t->s.nw > 8) {
        if (err) { strncpy(err, "array too wide for the simulator", errlen - 1); err[errlen - 1] = 0; }
        sim_free(&t->s);
        return -1;
    }
    t->q = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->ck = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->oe = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->sum = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->dsv = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->env = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->cyc = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->gar = (unsigned char *)xmalloc((size_t)d->nmc + 1);
    t->gsp = (unsigned char *)xmalloc((size_t)d->nmc + 1);
    memset(t->gar, 1, (size_t)d->nmc + 1);
    memset(t->gsp, 1, (size_t)d->nmc + 1);
    t->unst = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->oeforce = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->chg = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->fbunk = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->fbinv = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    t->rowv = (unsigned char *)xcalloc((size_t)d->nrows + 1, 1);
    t->stuck = (signed char *)xmalloc((size_t)d->nrows + 1);
    memset(t->stuck, -1, (size_t)d->nrows + 1);
    {
        /* power-up: registers with a reset (GAL, 22V10, AR terms) start reset,
           the others unknown (as the reference simulator) */
        int pu = 0;
        for (i = 0; i < d->nmc; i++) {
            const Macrocell *k = &d->mc[i];
            if (k->type == MC_GAL || k->type == MC_V10 || (k->type == MC_GEN && k->ar_row >= 0 && k->reg_kind != GR_ARAP)) pu = 1;
        }
        if (d->ar_row >= 0) pu = 1;
        if (d->sim_powerup == PU_RESET || d->sim_powerup == PU_PINHIGH) pu = 1;
        else if (d->sim_powerup == PU_X) pu = 0;
        t->powerup = pu ? V0 : VX;
    }
    for (i = 0; i < d->nmc; i++) { t->q[i] = (unsigned char)t->powerup; t->ck[i] = V0; t->sum[i] = VX; t->oe[i] = VX; t->dsv[i] = VX; t->env[i] = VX; }
    if (d->sim_powerup == PU_PINHIGH)   /* every registered pin starts high */
        for (i = 0; i < d->nmc; i++) if (t->s.mc[i].kind == MK_REG) t->q[i] = (unsigned char)(t->s.mc[i].out_inv ? V0 : V1);
    for (i = 0; i <= SIM_MAXPINS; i++) { t->drv[i] = VNONE; t->pin[i] = VX; t->out[i] = VZ; }
    if (d->sim_comb0)   /* combinational outputs start low */
        for (i = 0; i < d->nmc; i++) if (t->s.mc[i].kind == MK_COMB && !d->mc[i].node && d->mc[i].pin <= SIM_MAXPINS) { t->out[d->mc[i].pin] = V0; t->pin[d->mc[i].pin] = V0; }
    t->npos = d->npins + 1;
    for (i = 0; i < d->nmc; i++) if (d->mc[i].pin + 1 > t->npos) t->npos = d->mc[i].pin + 1;
    /* input registers / latches: 'inflag' pins whose fuse is 0 */
    for (i = 0; i < d->ninflag; i++) {
        int p = d->inflag_pin[i];
        if (p > 0 && p <= SIM_MAXPINS && fuse[d->inflag_fuse[i]] == 0) {
            t->inmode[p] = (unsigned char)(d->inflag_dq ? 1 : 2);
            t->inq[p] = VX;
            if (d->inflag_le[i]) { t->inle[p] = (unsigned char)d->inflag_le[i]; t->inq[p] = V0; }
        }
    }
    t->inck = V0;
    find_loops(t);
    return 0;
}

void sim3_free(Sim3 *t)
{
    sim_free(&t->s);
    free(t->q); free(t->ck); free(t->oe); free(t->sum); free(t->dsv); free(t->env); free(t->cyc); free(t->gar); free(t->gsp); free(t->unst); free(t->oeforce); free(t->chg); free(t->fbunk); free(t->fbinv);
    free(t->rowv); free(t->stuck);
    memset(t, 0, sizeof *t);
}

void sim3_drive(Sim3 *t, int pin, int v)
{
    if (pin >= 1 && pin <= SIM_MAXPINS) t->drv[pin] = (unsigned char)v;
}

int sim3_stuck(Sim3 *t, int addr, int v)
{
    int r;
    for (r = 0; r < t->d->nrows; r++)
        if (dev_row_addr(t->d, r) == addr) { t->stuck[r] = (signed char)(v ? V1 : V0); return 0; }
    return -1;
}

void sim3_preload(Sim3 *t, int mc, int v)
{
    if (mc < 0 || mc >= t->d->nmc || !SIM_HASREG(&t->s.mc[mc])) return;
    if (v == VX) t->q[mc] = VX;
    else if (t->d->sim_preload_pin) t->q[mc] = (unsigned char)(v ^ t->s.mc[mc].out_inv);   /* the pin shows the value */
    else t->q[mc] = (unsigned char)!v;
}

static int pass3(Sim3 *t)
{
    const Device *d = t->d;
    int i, changed = 0;
    for (i = 1; i <= d->npins; i++) {
        int m = dev_mc_at(d, i);
        if (m < 0 || t->s.mc[m].kind == MK_NONE || t->s.mc[m].kind == MK_INPUT) {
            int v = t->drv[i] == VNONE ? VX : t->drv[i];
            if (t->pin[i] != v) { t->pin[i] = (unsigned char)v; changed = 1; }
            if (m >= 0) t->out[i] = VZ;
        }
    }
    for (i = 1; i <= d->npins; i++) {
        if (t->inmode[i] == 2 && ((t->simlatch && d->clk_pin) || (!t->simlatch && (d->sim_le || t->inle[i])))) {
            /* input latch: enable is the 'simle' pin, or the clock pin with
               -simlatch (unverified) */
            int en = t->pin[t->simlatch ? d->clk_pin : t->inle[i] ? t->inle[i] : d->sim_le], v = t->pin[i], nq = t->inq[i];
            if (en == V1) nq = v;
            else if (en == VX && nq != v && t->simlatch) nq = VX;   /* the reference holds on an X enable */
            if (nq != t->inq[i]) { t->inq[i] = (unsigned char)nq; changed = 1; }
        }
    }
    eval_rows3(t);
    /* asynchronous reset / preset (X does nothing) */
    for (i = 0; i < d->nmc; i++) {
        if (!SIM_HASREG(&t->s.mc[i]) || t->noasync) continue;
        if (d->ar_row >= 0 && t->rowv[d->ar_row] == V1 && t->gar[i]) {
            if (t->q[i] != RV(t, i)) { t->q[i] = (unsigned char)RV(t, i); changed = 1; }
        } else if (d->ap_row >= 0 && t->rowv[d->ap_row] == V1 && !(d->ar_row >= 0 && t->rowv[d->ar_row] == V1)) {
            if (t->q[i] != !RV(t, i)) { t->q[i] = (unsigned char)!RV(t, i); changed = 1; }
        }
    }
    for (i = 0; i < d->nmc; i++) {
        const McCfg *m = &t->s.mc[i];
        int nq;
        if (!SIM_HASREG(m) || bypassed3(t, i)) continue;
        nq = t->q[i];
        if (t->noasync) continue;
        if (row_on3(t, m->ap_row) == V1) nq = !RV(t, i);
        if (row_on3(t, m->ar_row) == V1) nq = RV(t, i);
        if (d->mc[i].latch && t->simlatch) {
            int en = sim3_clock_of(t, i), dv = XOR3(dsum3(t, m), m->d_inv);
            if (row_on3(t, m->ap_row) != V1 && row_on3(t, m->ar_row) != V1) {
                if (en == V1) nq = dv;
                else if (en == VX && nq != dv) nq = VX;
            }
        }
        if (nq != t->q[i]) { t->q[i] = (unsigned char)nq; changed = 1; }
    }
    for (i = 0; i < d->nmc; i++) {
        const McCfg *m = &t->s.mc[i];
        int p = d->mc[i].pin, oe, v, o, rd;
        if (m->kind == MK_NONE || m->kind == MK_INPUT) continue;
        if (t->only != -1 && m->kind == MK_COMB && i != t->only) continue;
        t->sum[i] = (unsigned char)psum3(t, m);
        if (m->kind == MK_REG && !bypassed3(t, i)) v = XOR3(t->q[i], m->out_inv);
        else v = XOR3(t->sum[i], m->out_inv);
        if (m->node) {
            if (t->pin[p] != v || t->out[p] != v) changed = 1;
            t->pin[p] = (unsigned char)v;
            t->out[p] = (unsigned char)v;
            t->oe[i] = V1;
            continue;
        }
        oe = t->oeforce[i] ? V1 : oe3(t, m);
        o = oe == V1 ? v : oe == V0 ? VZ : VX;
        rd = t->drv[p] != VNONE ? t->drv[p] : (o == VZ ? VX : o);
        if (oe == VX && t->drv[p] != VNONE && t->drv[p] != v) rd = VX;
        if (t->out[p] != o || t->pin[p] != rd || t->oe[i] != oe) changed = 1;
        t->out[p] = (unsigned char)o;
        t->pin[p] = (unsigned char)rd;
        t->oe[i] = (unsigned char)oe;
    }
    return changed;
}

/* passes spent on a loop before it is declared unstable: 100 on devices
   (or GAL modes) with registers, 40 on combinational ones (observed with
   the original simulator); 'simpasses N' overrides */
static int loop_budget(const Sim3 *t)
{
    const Device *d = t->d;
    int i;
    if (d->sim_passes > 0) return d->sim_passes;
    if (d->nmodes >= 1) {
        const char *mn = d->modes[t->s.mode].name;
        if (!strcmp(mn, "registered")) return 100;
        if (!strcmp(mn, "simple") || !strcmp(mn, "complex")) return 40;
    }
    if (d->clk_pin > 0 || d->ninflag > 0 || d->ar_row >= 0) return 100;
    for (i = 0; i < d->nmc; i++) {
        const Macrocell *k = &d->mc[i];
        if (k->type == MC_REG || k->type == MC_V10 || k->type == MC_GAL) return 100;
        if (k->type == MC_GEN && (k->reg_kind != GR_COMB || k->ck_row >= 0 || k->latch)) return 100;
    }
    return 40;
}

/* count (and mark) the outputs left X by an X circulating in a loop;
   an X from X inputs only is stable, as in the original */
static int x_cyclic(Sim3 *t, unsigned char *mark)
{
    const Device *d = t->d;
    int n = d->nmc, i, j, k, r, cnt = 0, any = 0;
    unsigned char *adj, *isx;
    for (i = 0; i < n; i++) if (t->s.mc[i].kind == MK_COMB && t->cyc[i] && t->out[d->mc[i].pin] == VX) any = 1;
    if (!any) return 0;
    adj = (unsigned char *)xcalloc((size_t)n * (size_t)n + 1, 1);
    isx = (unsigned char *)xcalloc((size_t)n + 1, 1);
    for (i = 0; i < n; i++) isx[i] = (unsigned char)(t->s.mc[i].kind == MK_COMB && t->out[d->mc[i].pin] == VX);
    for (k = 0; k < d->nlines; k++) {
        LineSrc src = t->s.lines[k];
        if (src.kind != SRC_PIN && src.kind != SRC_FB) continue;
        j = dev_mc_at(d, src.pin);
        if (j < 0 || !isx[j]) continue;
        if (src.kind == SRC_FB && SIM_HASREG(&t->s.mc[j]) && t->s.mc[j].fb != GF_PIN) continue;
        for (i = 0; i < n; i++) {
            const McCfg *m = &t->s.mc[i];
            if (!isx[i] || adj[i * n + j]) continue;
            for (r = m->sum0; r >= 0 && r <= m->sum1; r++)
                if (t->rowv[r] == VX && sim_row_uses_line(&t->s, r, k)) { adj[i * n + j] = 1; break; }
            for (r = m->xs0; r >= 0 && r <= m->xs1 && !adj[i * n + j]; r++)
                if (t->rowv[r] == VX && sim_row_uses_line(&t->s, r, k)) { adj[i * n + j] = 1; break; }
        }
    }
    for (k = 0; k < n; k++)
        for (i = 0; i < n; i++)
            if (adj[i * n + k])
                for (j = 0; j < n; j++)
                    if (adj[k * n + j]) adj[i * n + j] = 1;
    for (i = 0; i < n; i++) {
        int u = 0;
        for (j = 0; j < n && !u; j++) if (adj[i * n + j] && adj[j * n + j]) u = 1;
        if (u) { cnt++; if (mark) mark[i] = 1; }
    }
    free(adj); free(isx);
    return cnt;
}

/* As the original: loops are iterated pass by pass in descending pin
   order from the previous vector's values; after 40 passes (100 with
   registers) outputs still changing are unstable. */
static void snap(const Sim3 *t, unsigned char *b)
{
    memcpy(b, t->out, SIM_MAXPINS + 1);
    memcpy(b + SIM_MAXPINS + 1, t->pin, SIM_MAXPINS + 1);
    memcpy(b + 2 * (SIM_MAXPINS + 1), t->oe, (size_t)t->d->nmc + 1);
}

static void unsnap(Sim3 *t, const unsigned char *b)
{
    memcpy(t->out, b, SIM_MAXPINS + 1);
    memcpy(t->pin, b + SIM_MAXPINS + 1, SIM_MAXPINS + 1);
    memcpy(t->oe, b + 2 * (SIM_MAXPINS + 1), (size_t)t->d->nmc + 1);
}

static void trace_snap(Sim3 *t, const unsigned char *b, int n)
{
    size_t sz = 2 * (SIM_MAXPINS + 1) + (size_t)t->d->nmc + 1;
    unsigned char *cur = (unsigned char *)xmalloc(sz);
    snap(t, cur);
    unsnap(t, b);
    t->trace(t->trace_ctx, n);
    unsnap(t, cur);
    free(cur);
}

void sim3_settle(Sim3 *t)
{
    const Device *d = t->d;
    int i, n, anyx = 0, cyc = 0, budget, j, k, best;
    size_t sz = 2 * (SIM_MAXPINS + 1) + (size_t)d->nmc + 1;
    unsigned char *prev, *last, *sp, *done;
    memset(t->unst, 0, (size_t)d->nmc + 1);
    for (i = 0; i < d->nmc; i++) if (t->s.mc[i].kind == MK_COMB && t->cyc[i]) cyc = 1;
    if (!cyc) {
        for (n = 0; n < MAXPASS && pass3(t); n++) ;
        t->passes = n;
        return;
    }
    prev = (unsigned char *)xmalloc((size_t)d->nmc + 1);
    last = (unsigned char *)xmalloc((size_t)d->nmc + 1);
    sp = (unsigned char *)xmalloc(sz);
    done = (unsigned char *)xmalloc((size_t)d->nmc + 1);
    for (i = 0; i < d->nmc; i++) prev[i] = t->out[d->mc[i].pin];
    t->fbx = 1;
    for (n = 0; n < MAXPASS && pass3(t); n++) ;
    t->fbx = 0;
    t->passes = n;
    for (i = 0; i < d->nmc; i++)
        if (t->s.mc[i].kind == MK_COMB && t->cyc[i] && t->out[d->mc[i].pin] == VX) anyx = 1;
    for (i = 0; i < d->nmc; i++) {
        int p = d->mc[i].pin;
        last[i] = prev[i];
        if (t->s.mc[i].kind == MK_COMB) {
            t->out[p] = prev[i];
            t->pin[p] = t->drv[p] != VNONE ? t->drv[p] : (prev[i] == VZ ? VX : prev[i]);
        }
    }
    budget = loop_budget(t);
    t->budget = budget;
    for (n = 1; n <= budget; n++) {
        int changed = 0;
        memset(done, 0, (size_t)d->nmc + 1);
        for (k = 0; k < d->nmc; k++) {
            best = -1;
            for (j = 0; j < d->nmc; j++)
                if (!done[j] && t->s.mc[j].kind == MK_COMB && (best < 0 || d->mc[j].pin > d->mc[best].pin)) best = j;
            if (best < 0) break;
            done[best] = 1;
            t->only = best;
            pass3(t);
        }
        t->only = -1;
        for (i = 0; i < d->nmc; i++) {
            int o = t->out[d->mc[i].pin];
            if (t->s.mc[i].kind != MK_COMB) continue;
            t->chg[i] = (unsigned char)(o != last[i]);
            if (o != last[i]) changed = 1;
            last[i] = (unsigned char)o;
        }
        if (!changed && !x_cyclic(t, NULL)) break;
        if (n > 1 && t->trace) trace_snap(t, sp, n - 1);
        snap(t, sp);
    }
    if (n > budget) {
        if (t->trace) trace_snap(t, sp, budget);
        x_cyclic(t, t->unst);
        for (i = 0; i < d->nmc; i++)
            if (t->cyc[i] && t->chg[i]) t->unst[i] = 1;   /* still changing */
    } else if (n == 1 && anyx && t->trace) {
        t->trace(t->trace_ctx, 1);   /* a loop holding its value */
    }
    /* registers, latches and outputs that do not take part */
    for (k = 0; k < MAXPASS; k++) {
        t->only = -3;
        if (!pass3(t)) break;
    }
    t->only = -1;
    t->passes += n;
    free(prev); free(last); free(sp); free(done);
}

/* the value a register loads at its clock */
static int next_q(const Sim3 *t, int i, int sp)
{
    const McCfg *m = &t->s.mc[i];
    int dv = dsum3(t, m), nq;
    if (m->tff) nq = XOR3(t->q[i], dv);
    else nq = XOR3(dv, m->d_inv);
    /* a synchronous preset acts when 1; X does nothing, like the
       asynchronous terms (observed with the original simulator) */
    if (sp == V1) nq = !RV(t, i);
    if (t->regzero) nq = m->out_inv ? V1 : V0;
    return nq;
}

int sim3_clock_step(Sim3 *t)
{
    const Device *d = t->d;
    int i, loaded = 0, gsp = d->sp_row >= 0 ? t->rowv[d->sp_row] : V0;
    if (d->sim_noclock) return 0;   /* the reference simulator never clocks these registers */
    for (i = 0; i < d->nmc; i++) {
        const McCfg *m = &t->s.mc[i];
        int c;
        if (!SIM_HASREG(m) || d->mc[i].latch) continue;
        c = sim3_clock_of(t, i);
        if (t->pinclk_off && !(m->ck_row >= 0 && !m->ce) && !(m->ck_pin && t->firepins && !t->firepins[m->ck_pin])) { t->ck[i] = (unsigned char)c; continue; }
        if (t->ck[i] == V0 && c == V1) {
            /* rising edge: D as the array computes it with the clock high (CSIM) */
            int nq = next_q(t, i, OR3(t->gsp[i] ? gsp : V0, row_on3(t, m->sp_row))), en = m->ce && !t->noce ? t->rowv[m->ck_row] : V1, old = t->q[i];
            if (en == V1) t->q[i] = (unsigned char)nq;
            else if (en == VX && nq != t->q[i]) t->q[i] = VX;
            if (t->q[i] != old) loaded = 1;
        }
        t->ck[i] = (unsigned char)c;
    }
    if (d->clk_pin && !t->pinclk_off) {
        /* input registers: loaded at a rising edge of the clock pin */
        int ck = t->pin[d->clk_pin], p;
        for (p = 1; p <= d->npins; p++) {
            if (t->inmode[p] != 1 || !t->simlatch || !d->clk_pin) continue;
            if (t->inck == V0 && ck == V1) {
                if (t->inq[p] != t->insv[p]) loaded = 1;
                t->inq[p] = t->insv[p];
            }
            t->insv[p] = t->pin[p];
        }
        t->inck = (unsigned char)ck;
    }
    for (i = 0; i < d->nmc; i++) {
        const McCfg *m = &t->s.mc[i];
        if (!SIM_HASREG(m) || d->mc[i].latch) continue;
        t->dsv[i] = (unsigned char)next_q(t, i, OR3(t->gsp[i] ? gsp : V0, row_on3(t, m->sp_row)));
        t->env[i] = (unsigned char)(m->ce && !t->noce ? t->rowv[m->ck_row] : V1);
    }
    return loaded;
}

/* K on the clock pin: the registers clocked by the clock pin load now,
   with the array evaluated at the current (low) clock level */
void sim3_clock_force(Sim3 *t)
{
    const Device *d = t->d;
    int i, gsp = d->sp_row >= 0 ? t->rowv[d->sp_row] : V0;
    if (d->sim_noclock) return;
    for (i = 0; i < d->nmc; i++) {
        const McCfg *m = &t->s.mc[i];
        int nq, en;
        if (!SIM_HASREG(m) || d->mc[i].latch || (m->ck_row >= 0 && !m->ce)) continue;
        if (m->ck_pin && t->firepins && !t->firepins[m->ck_pin]) continue;
        nq = next_q(t, i, OR3(t->gsp[i] ? gsp : V0, row_on3(t, m->sp_row)));
        en = m->ce && !t->noce ? t->rowv[m->ck_row] : V1;
        if (en == V1) t->q[i] = (unsigned char)nq;
        else if (en == VX && nq != t->q[i]) t->q[i] = VX;
    }
    if (d->clk_pin)
        for (i = 1; i <= d->npins; i++) if (t->inmode[i] == 1 && t->simlatch && d->clk_pin) t->inq[i] = t->pin[i];
}

/* take the current clock levels as the reference for edge detection */
void sim3_clock_sync(Sim3 *t)
{
    int i;
    for (i = 0; i < t->d->nmc; i++) t->ck[i] = (unsigned char)sim3_clock_of(t, i);
    if (t->d->clk_pin) t->inck = t->pin[t->d->clk_pin];
}

int sim3_value(const Sim3 *t, int pin)
{
    int m;
    if (pin < 1 || pin > SIM_MAXPINS) return VX;
    if ((m = alias_mc(t, pin)) >= 0) {
        /* the register as the array reads it through its feedback line */
        const McCfg *c = &t->s.mc[m];
        if (c->fb == GF_NQ) return NOT3(t->q[m]);
        if (c->fb == GF_XQ) return NOT3(XOR3(t->q[m], c->out_inv));
        if (c->fb == GF_XP) return XOR3(t->q[m], c->out_inv);
        return t->q[m];
    }
    return t->out[pin];
}

int sim3_prebuffer(const Sim3 *t, int pin)
{
    int m = sim3_mc_of(t, pin);
    if (m < 0) return VX;
    if (t->s.mc[m].kind == MK_REG) return t->q[m];
    return t->sum[m];
}
