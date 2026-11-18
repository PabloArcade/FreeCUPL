/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecupl - fuse-level PLD simulator.  DARKSOFT.
   Fuse r*ncols+c, 0 = connected; line k drives columns 2k and 2k+1.
   Registers hold Q, the array sees /Q. */
#include "sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int F(const Sim *s, int a) { return a >= 0 ? s->fuse[a] : 0; }

const char *sim_mode_name(const Sim *s)
{
    return s->d->modes[s->mode].name;
}

int sim_init(Sim *s, const Device *d, const unsigned char *fuse, char *err, size_t errlen)
{
    int r, c, i;
    memset(s, 0, sizeof *s);
    s->d = d;
    s->fuse = fuse;
    if (d->npins > SIM_MAXPINS) { strncpy(err, "device has too many pins for simulator", errlen); return -1; }
    s->mode = dev_mode_of(d, fuse);
    if (s->mode < 0) {
        if (err) { strncpy(err, "configuration fuses select no valid mode", errlen - 1); err[errlen - 1] = 0; }
        return -1;
    }
    s->lines = dev_lines(d, s->mode);
    s->nw = (d->ncols + 63) / 64;
    s->rowmask = (Word *)xcalloc((size_t)d->nrows * (size_t)s->nw, sizeof(Word));
    s->live = (unsigned char *)xcalloc((size_t)d->nrows, 1);
    s->rowv = (unsigned char *)xcalloc((size_t)d->nrows, 1);
    s->colv = (Word *)xcalloc((size_t)s->nw, sizeof(Word));
    s->mc = (McCfg *)xcalloc((size_t)d->nmc + 1, sizeof(McCfg));
    s->q = (unsigned char *)xcalloc((size_t)d->nmc + 1, 1);
    s->selmask = (Word *)xcalloc((size_t)d->nrows * (size_t)s->nw, sizeof(Word));
    {
        /* select columns (GAL 'selc'/'selr') are not array inputs */
        unsigned char *issel = (unsigned char *)xcalloc((size_t)d->ncols + 1, 1);
        for (i = 0; i < d->nmc; i++) {
            if (d->mc[i].sel_comb >= 0 && d->mc[i].sel_comb < d->ncols) issel[d->mc[i].sel_comb] = 1;
            if (d->mc[i].sel_reg >= 0 && d->mc[i].sel_reg < d->ncols) issel[d->mc[i].sel_reg] = 1;
        }
        for (r = 0; r < d->nrows; r++) {
            int live = 1;
            for (c = 0; c < d->ncols; c++) {
                int a = dev_fuse(d, r, c);
                if (a >= 0 && fuse[a] == 0)
                    (issel[c] ? s->selmask : s->rowmask)[r * s->nw + c / 64] |= (Word)1 << (c % 64);
            }
            for (c = 0; c < d->ncols; c += 2) {
                int a = dev_fuse(d, r, c), b = dev_fuse(d, r, c + 1);
                if (!issel[c] && !issel[c + 1] && a >= 0 && b >= 0 && fuse[a] == 0 && fuse[b] == 0) live = 0;
            }
            if (d->ptd_base >= 0 && fuse[d->ptd_base + r] == 0) live = 0;
            s->live[r] = (unsigned char)live;
        }
        free(issel);
    }
    for (i = 0; i < d->nmc; i++) {
        const Macrocell *k = &d->mc[i];
        McCfg *m = &s->mc[i];
        m->sum0 = k->sum0;
        m->sum1 = k->sum1;
        m->oe_row = k->oe_row;
        m->oe_kind = k->oe_row >= 0 ? OE_ROW : OE_ALWAYS;
        m->xs0 = 0;
        m->xs1 = -1;
        m->ck_row = m->ar_row = m->ap_row = -1;
        m->fb = GF_NQ;
        m->node = k->node;
        m->rs0 = m->rs1 = -1;
        m->sp_row = -1;
        m->sel = -1;
        m->steer = k->steer_fuse;
        m->steer_step = k->steer_step;
        m->steer_other = -1;
        if (k->steer_fuse >= 0 && d->steer_share) {
            int j2;
            for (j2 = 0; j2 < d->nmc; j2++)
                if (j2 != i && d->mc[j2].steer_fuse >= 0 && d->mc[j2].sum0 == k->sum0 && d->mc[j2].sum1 == k->sum1) m->steer_other = d->mc[j2].steer_fuse;
        }
        switch (k->type) {
        case MC_GEN: {
            int inv = k->pol_fuse >= 0 ? (F(s, k->pol_fuse) != k->pol_hi) : !k->pol;
            int reg = k->reg_kind == GR_REG || k->reg_kind == GR_ARAP ||
                      (k->reg_kind == GR_FUSE && F(s, k->reg_fuse) == k->reg_on);
            m->kind = reg ? MK_REG : MK_COMB;
            m->hasreg = k->alwaysreg;
            m->out_inv = inv;
            m->d_inv = 0;
            if (reg && k->pol_d == 1) { m->d_inv = !inv; m->out_inv = 1; }
            if (reg && k->pol_d == 2) { m->d_inv = inv; m->out_inv = 0; }
            if (k->xsum0 >= 0) { m->xs0 = k->xsum0; m->xs1 = k->xsum1; }
            if (k->split_fuse >= 0 && F(s, k->split_fuse) == k->split_val) { m->sum0 = k->half0; m->sum1 = k->half1; }
            if (k->qsplit_fuse >= 0 && F(s, k->qsplit_fuse)) {
                /* the pin shows the first sum, the register loads another one */
                m->sum0 = k->qhalf0; m->sum1 = k->qhalf1;
                m->rs0 = k->qsum0; m->rs1 = k->qsum1;
            }
            m->sp_row = k->sp_row;
            /* clock pin with the CK row as clock enable: in this simulator a
               product-term clock and a clock enable behave alike (the
               register loads at a clock step while the row is true) */
            m->ce = k->ce_fuse >= 0 && F(s, k->ce_fuse);
            if (k->cksync_fuse >= 0) m->ce = F(s, k->cksync_fuse);
            m->ck_pin = k->ck_pin;
            if (k->cjoin_fuse >= 0 && !F(s, k->cjoin_fuse)) m->sum1 = k->sumc - 1;
            m->ck_row = k->ck_row;
            if (k->ckmux_fuse >= 0 && F(s, k->ckmux_fuse)) m->ck_row = -1;
            m->ar_row = k->ar_row;
            m->ap_row = k->ap_row;
            m->bypass = k->reg_kind == GR_ARAP;
            m->tff = (reg || k->alwaysreg) && k->t_fuse >= 0 && F(s, k->t_fuse) == k->t_val;   /* .TFB: the hidden register of a combinational output */
            m->fb = k->fb;
            if (k->fb_fuse >= 0 && (F(s, k->fb_fuse) == k->fb_pin) != (k->fb_xor && !reg)) m->fb = GF_PIN;
            switch (k->oe_kind) {
            case GO_ALWAYS: m->oe_kind = OE_ALWAYS; break;
            case GO_ROW: m->oe_kind = OE_ROW; break;
            case GO_PIN: m->oe_kind = OE_PIN; break;
            case GO_ROWPIN: m->oe_kind = OE_ROW; m->oe_pin_too = 1; break;
            }
            if (reg && k->oe_reg_pin) m->oe_kind = OE_PIN;
            if (k->oemux_fuse >= 0) m->oe_kind = reg && F(s, k->oemux_fuse) ? OE_PIN : OE_ROW;
            if (k->xv_fuse >= 0) {
                if (F(s, k->xv_fuse)) {
                    m->oe_kind = OE_ROW;
                    m->oe_row = k->sum0;
                    m->sum0 = k->sum0 + 1;
                } else {
                    m->oe_kind = d->oe_pin ? OE_PIN : OE_ALWAYS;
                    m->sum1 = k->sum0 + 1;
                    m->xs0 = k->sum0 + 2;
                    m->xs1 = k->sum1;
                    m->out_inv = 1;
                    m->d_inv = 0;
                }
            }
            break;
        }
        case MC_COMB:
            m->kind = MK_COMB;
            m->out_inv = !k->pol;
            break;
        case MC_REG:
            m->kind = MK_REG;
            m->out_inv = !k->pol;
            m->oe_kind = d->oe_pin ? OE_PIN : OE_ALWAYS;
            break;
        case MC_V10: {
            int s0 = F(s, k->s0_fuse), s1 = F(s, k->s1_fuse);
            m->kind = s1 ? MK_COMB : MK_REG;
            m->out_inv = !s0;
            break;
        }
        case MC_GAL: {
            int x = F(s, k->xor_fuse), ac1 = F(s, k->ac1_fuse) == k->ac1_val;
            const char *mn = d->modes[s->mode].name;
            if (!strcmp(mn, "simple")) {
                m->kind = ac1 ? MK_INPUT : MK_COMB;
                m->oe_kind = ac1 ? OE_NEVER : OE_ALWAYS;
                m->sum0 = k->sum0;
            } else if (!strcmp(mn, "complex") || ac1) {
                m->kind = MK_COMB;
                m->oe_kind = OE_ROW;
                m->oe_row = k->sum0;
                m->sum0 = k->sum0 + 1;
            } else {
                m->kind = MK_REG;
                m->oe_kind = OE_PIN;
                m->sum0 = k->sum0;
            }
            if (m->kind == MK_REG && k->xorr_fuse >= 0) x = F(s, k->xorr_fuse) == k->xorr_val;
            m->out_inv = !x;
            m->d_inv = x;
            if (m->kind == MK_REG) m->out_inv = 1;
            m->sel = m->kind == MK_REG ? k->sel_reg : k->sel_comb;
            m->tff = m->kind == MK_REG && k->t_fuse >= 0 && F(s, k->t_fuse) == k->t_val;
            m->fb = k->fb;
            break;
        }
        }
    }
    for (i = 1; i <= d->npins; i++) s->ext[i] = 0;
    return 0;
}

void sim_free(Sim *s)
{
    free(s->rowmask);
    free(s->live);
    free(s->selmask);
    free(s->rowv);
    free(s->colv);
    free(s->mc);
    free(s->q);
    memset(s, 0, sizeof *s);
}

void sim_set_input(Sim *s, int pin, int v)
{
    if (pin >= 1 && pin <= s->d->npins) s->ext[pin] = (unsigned char)(v ? 1 : 0);
}

int sim_has_regs(const Sim *s)
{
    int i;
    for (i = 0; i < s->d->nmc; i++) if (SIM_HASREG(&s->mc[i])) return 1;
    return 0;
}

/* register state at pin level (raw Q for a register the pin does not show) */
int sim_get_state(const Sim *s, int mc)
{
    return s->q[mc] ^ (SIM_HASREG(&s->mc[mc]) ? s->mc[mc].out_inv : 0);
}

void sim_set_state(Sim *s, int mc, int v)
{
    s->q[mc] = (unsigned char)((v ? 1 : 0) ^ (SIM_HASREG(&s->mc[mc]) ? s->mc[mc].out_inv : 0));
}

int sim_pin_is_input(const Sim *s, int pin)
{
    const Device *d = s->d;
    int k = pin <= d->npins ? dev_mc_at(d, pin) : -1;
    if (pin == d->gnd || pin == d->vcc || pin < 1 || pin > d->npins) return 0;
    if (k < 0) return 1;
    if (s->mc[k].kind == MK_INPUT) return 1;
    if (s->mc[k].oe_kind == OE_ALWAYS) return 0;
    return 1; /* tri-statable I/O */
}

static int row_on(const Sim *s, int r) { return r >= 0 && s->rowv[r]; }

/* RA-type register bypassed (AR and AP both true) */
static int bypassed(const Sim *s, int m)
{
    const McCfg *c = &s->mc[m];
    return c->bypass && row_on(s, c->ar_row) && row_on(s, c->ap_row);
}

static int line_value(const Sim *s, int k)
{
    LineSrc src = s->lines[k];
    if (src.kind == SRC_PIN) return s->pin[src.pin];
    if (src.kind == SRC_FB) {
        int m = dev_mc_at(s->d, src.pin);
        if (SIM_HASREG(&s->mc[m]) && !bypassed(s, m)) {
            if (s->mc[m].fb == GF_Q) return s->q[m];
            if (s->mc[m].fb == GF_XQ) return !(s->q[m] ^ s->mc[m].out_inv);
            if (s->mc[m].fb == GF_XP) return s->q[m] ^ s->mc[m].out_inv;
            if (s->mc[m].fb == GF_NQ) return !s->q[m];
        }
        return s->pin[src.pin];
    }
    return 0;
}

static void eval_rows(Sim *s)
{
    const Device *d = s->d;
    int k, r, w;
    memset(s->colv, 0, sizeof(Word) * (size_t)s->nw);
    for (k = 0; k < d->nlines; k++) {
        int v = line_value(s, k);
        int c = 2 * k + ((v ? 0 : 1) ^ d->truecol_odd);
        s->colv[c / 64] |= (Word)1 << (c % 64);
    }
    for (r = 0; r < d->nrows; r++) {
        int t = s->live[r];
        if (t) {
            const Word *m = &s->rowmask[r * s->nw];
            for (w = 0; w < s->nw; w++) if (m[w] & ~s->colv[w]) { t = 0; break; }
        }
        s->rowv[r] = (unsigned char)t;
    }
}

int sim_row_true(const Sim *s, int row) { return s->rowv[row]; }

int sim_row_uses_line(const Sim *s, int row, int line)
{
    const Word *m = &s->rowmask[row * s->nw];
    int c = 2 * line;
    if (!s->live[row]) return 0;
    return ((m[c / 64] >> (c % 64)) & 1) || ((m[(c + 1) / 64] >> ((c + 1) % 64)) & 1);
}

static int sum_of(const Sim *s, const McCfg *m)
{
    int r, v = 0, x = 0;
    for (r = m->sum0; r <= m->sum1; r++)
        if (s->rowv[r] && (m->steer < 0 || (m->steer_other >= 0 ? !(F(s, m->steer_other + (r - m->sum0) * m->steer_step) && !F(s, m->steer + (r - m->sum0) * m->steer_step))
                                                               : F(s, m->steer + (r - m->sum0) * m->steer_step))) && (m->sel < 0 || ((s->selmask[r * s->nw + m->sel / 64] >> (m->sel % 64)) & 1))) { v = 1; break; }
    for (r = m->xs0; r <= m->xs1; r++) if (s->rowv[r]) { x = 1; break; }
    return v ^ x;
}

static int dsum_of(const Sim *s, const McCfg *m)
{
    McCfg t;
    if (m->rs0 < 0) return sum_of(s, m);
    t = *m;
    t.sum0 = m->rs0;
    t.sum1 = m->rs1;
    return sum_of(s, &t);
}

static int oe_of(const Sim *s, const McCfg *m)
{
    switch (m->oe_kind) {
    case OE_ALWAYS: return 1;
    case OE_NEVER: return 0;
    case OE_ROW: return s->rowv[m->oe_row] && !(m->oe_pin_too && s->d->oe_pin && s->pin[s->d->oe_pin]);
    case OE_PIN: return s->d->oe_pin ? !s->pin[s->d->oe_pin] : 1;
    }
    return 0;
}

void sim_settle(Sim *s)
{
    const Device *d = s->d;
    int it, i, p;
    /* outputs start from the values they drove (a combinational loop keeps
       its state, as in the device and the reference simulator); the other
       pins from the applied levels */
    for (p = 1; p <= d->npins; p++) {
        if (!s->drv[p]) s->pin[p] = s->ext[p];
        s->drv[p] = 0;
    }
    s->unstable = 1;
    for (it = 0; it < 4 * d->nmc + 8; it++) {
        int changed = 0;
        eval_rows(s);
        if (d->ap_row >= 0 && s->rowv[d->ap_row] && !(d->ar_row >= 0 && s->rowv[d->ar_row])) {
            for (i = 0; i < d->nmc; i++)
                if (SIM_HASREG(&s->mc[i]) && !s->q[i]) { s->q[i] = 1; changed = 1; }
        }
        if (d->ar_row >= 0 && s->rowv[d->ar_row]) {
            for (i = 0; i < d->nmc; i++)
                if (SIM_HASREG(&s->mc[i]) && s->q[i]) { s->q[i] = 0; changed = 1; }
        }
        for (i = 0; i < d->nmc; i++) {
            /* per-macrocell asynchronous reset / preset (reset wins) */
            const McCfg *m = &s->mc[i];
            int nq;
            if (!SIM_HASREG(m) || bypassed(s, i)) continue;
            nq = s->q[i];
            if (row_on(s, m->ap_row)) nq = 1;
            if (row_on(s, m->ar_row)) nq = 0;
            if (nq != s->q[i]) { s->q[i] = (unsigned char)nq; changed = 1; }
        }
        for (i = 0; i < d->nmc; i++) {
            const McCfg *m = &s->mc[i];
            int pinno = d->mc[i].pin, oe, v;
            if (m->kind == MK_NONE || m->kind == MK_INPUT) continue;
            if (m->node) {
                /* buried register: its value is only seen through the array */
                v = m->kind == MK_REG ? s->q[i] ^ m->out_inv : sum_of(s, m) ^ m->out_inv;
                if (s->pin[pinno] != v) changed = 1;
                s->pin[pinno] = (unsigned char)v;
                continue;
            }
            oe = oe_of(s, m);
            if (oe) {
                v = m->kind == MK_REG && !bypassed(s, i) ? (s->q[i] ^ m->out_inv) : (sum_of(s, m) ^ m->out_inv);
            } else v = s->ext[pinno];
            if (s->pin[pinno] != v || s->drv[pinno] != oe) changed = 1;
            s->pin[pinno] = (unsigned char)v;
            s->drv[pinno] = (unsigned char)oe;
        }
        if (!changed) { s->unstable = 0; break; }
    }
}

void sim_clock(Sim *s)
{
    const Device *d = s->d;
    int i, sp;
    unsigned char *nq = (unsigned char *)xmalloc((size_t)d->nmc + 1);
    sim_settle(s);
    sp = d->sp_row >= 0 && s->rowv[d->sp_row];
    for (i = 0; i < d->nmc; i++) {
        const McCfg *m = &s->mc[i];
        nq[i] = s->q[i];
        if (!SIM_HASREG(m)) continue;
        /* a product-term clock pulses only when its term is true */
        if (m->ck_row >= 0 && !row_on(s, m->ck_row)) continue;
        {
            int p = sp || row_on(s, m->sp_row);
            if (m->tff) nq[i] = (unsigned char)(p ? 1 : (s->q[i] ^ dsum_of(s, m)));
            else nq[i] = (unsigned char)(p ? 1 : (dsum_of(s, m) ^ m->d_inv));
        }
    }
    memcpy(s->q, nq, (size_t)d->nmc);
    free(nq);
    for (i = 0, sp = 0; i < d->nmc; i++)
        if (SIM_HASREG(&s->mc[i]) && (s->mc[i].ck_row < 0 || s->mc[i].ce)) sp = 1;
    if (sp && d->clk_pin > 0 && d->clk_pin <= SIM_MAXPINS) {
        /* high phase of the clock pulse: terms reading the pin act
           while it is high */
        unsigned char lvl = s->ext[d->clk_pin];
        s->ext[d->clk_pin] = 1;
        sim_settle(s);
        s->ext[d->clk_pin] = lvl;
    }
    sim_settle(s);
}

static void put_line(const Sim *s, Buf *b, int col)
{
    LineSrc src = s->lines[col / 2];
    int neg = (col & 1) ^ s->d->truecol_odd;
    if (src.kind == SRC_NONE) { buf_printf(b, "%sL%d", neg ? "!" : "", col / 2); return; }
    if (src.kind == SRC_FB) {
        int m = dev_mc_at(s->d, src.pin);
        if (SIM_HASREG(&s->mc[m]) && s->mc[m].fb != GF_PIN) {
            buf_printf(b, "%sQ%d", (neg != 0) == (s->mc[m].fb == GF_Q) ? "!" : "", src.pin);
            return;
        }
    }
    buf_printf(b, "%sp%d", neg ? "!" : "", src.pin);
}

static void put_row(const Sim *s, Buf *b, int r)
{
    const Device *d = s->d;
    int c, first = 1;
    if (!s->live[r]) { buf_puts(b, "0"); return; }
    for (c = 0; c < d->ncols; c++) {
        int a = dev_fuse(d, r, c);
        if (a >= 0 && s->fuse[a] == 0) {
            if (!first) buf_puts(b, " & ");
            put_line(s, b, c);
            first = 0;
        }
    }
    if (first) buf_puts(b, "1");
}

static void put_sum(const Sim *s, Buf *b, int r0, int r1)
{
    int r, any = 0;
    for (r = r0; r <= r1; r++) {
        if (!s->live[r]) continue;
        buf_puts(b, any ? "\n        # " : "");
        put_row(s, b, r);
        any = 1;
    }
    if (!any) buf_puts(b, "0");
}

void sim_describe(const Sim *s, Buf *b)
{
    const Device *d = s->d;
    int i;
    buf_printf(b, "device %s, mode %s\n", d->name, sim_mode_name(s));
    if (d->ar_row >= 0) { buf_puts(b, "AR = "); put_sum(s, b, d->ar_row, d->ar_row); buf_puts(b, "\n"); }
    if (d->sp_row >= 0) { buf_puts(b, "SP = "); put_sum(s, b, d->sp_row, d->sp_row); buf_puts(b, "\n"); }
    if (d->ap_row >= 0) { buf_puts(b, "AP = "); put_sum(s, b, d->ap_row, d->ap_row); buf_puts(b, "\n"); }
    for (i = 0; i < d->nmc; i++) {
        const McCfg *m = &s->mc[i];
        int p = d->mc[i].pin;
        const char *kind = m->kind == MK_COMB ? "comb" : m->kind == MK_REG ? "reg" : m->kind == MK_INPUT ? "input" : "none";
        buf_printf(b, "pin %d: %s", p, kind);
        if (m->kind == MK_INPUT) { buf_puts(b, "\n"); continue; }
        buf_printf(b, ", active %s", m->out_inv ? "low" : "high");
        if (m->ck_row >= 0) { buf_puts(b, m->ce ? ", CK = clock pin, CE = " : ", CK = "); put_row(s, b, m->ck_row); }
        if (m->ar_row >= 0) { buf_puts(b, ", AR = "); put_row(s, b, m->ar_row); }
        if (m->ap_row >= 0) { buf_puts(b, ", AP = "); put_row(s, b, m->ap_row); }
        if (m->sp_row >= 0) { buf_puts(b, ", SP = "); put_row(s, b, m->sp_row); }
        if (m->oe_kind == OE_ALWAYS) buf_puts(b, ", always enabled");
        else if (m->oe_kind == OE_PIN) buf_printf(b, ", enabled by !p%d", d->oe_pin);
        else if (m->oe_kind == OE_ROW) { buf_puts(b, ", OE = "); put_row(s, b, m->oe_row); }
        buf_puts(b, "\n");
        if (m->kind == MK_REG && m->tff) buf_printf(b, "  Q%d.t = %s", p, "");
        else if (m->kind == MK_REG) buf_printf(b, "  Q%d.d = %s", p, m->d_inv ? "!(" : "");
        else buf_printf(b, "  p%d = %s", p, m->out_inv ? "!(" : "");
        if (m->kind == MK_REG && m->rs0 >= 0) put_sum(s, b, m->rs0, m->rs1);
        else put_sum(s, b, m->sum0, m->sum1);
        if (m->xs0 <= m->xs1) { buf_puts(b, "\n        $ ("); put_sum(s, b, m->xs0, m->xs1); buf_puts(b, ")"); }
        buf_puts(b, (m->kind == MK_REG ? (m->d_inv && !m->tff) : m->out_inv) ? ")\n" : "\n");
        if (m->kind == MK_REG) buf_printf(b, "  p%d = %sQ%d\n", p, m->out_inv ? "!" : "", p);
        else if (m->hasreg && m->rs0 >= 0) {
            buf_printf(b, "  Q%d.d = ", p);
            put_sum(s, b, m->rs0, m->rs1);
            buf_puts(b, "\n");
        }
    }
    return;
}
