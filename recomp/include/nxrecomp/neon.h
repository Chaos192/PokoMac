/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
 *
 * nxrecomp: Advanced SIMD for the lifter's tier-1 statements (included last by ctx.h, after fpexact.h).
 *
 * Element sizes are log2 of the element's bytes (0 = 8-bit .. 3 = 64-bit); lane i of size sz is at byte i << sz. Every helper reads all
 * of its operands before it writes its destination, so the destination may be any of the sources (the statements pass &c->v[n] for
 * each operand). A result is built in a zeroed register, so whatever a form does not write (the upper half of a 64-bit form, the
 * lanes above a scalar) is zero, as a write to a SIMD&FP register clears it. Floating point goes through fpexact.h; FPSR (QC and the
 * cumulative flags) is never written. Behaviour follows the A64 instruction pages of the Arm Architecture Reference Manual.
 * Valid GNU C11 and C++17 / C++20.
 */
#ifndef NXRECOMP_NEON_H
#define NXRECOMP_NEON_H

/* ---- lanes ------------------------------------------------------------------------------------------------------------------- */

static inline uint64_t vget(const V128 *v, int sz, int i) {
    switch (sz) {
    case 0: return v->b[i];
    case 1: return v->h[i];
    case 2: return v->w[i];
    default: return v->d[i];
    }
}

static inline void vset(V128 *v, int sz, int i, uint64_t x) {
    switch (sz) {
    case 0: v->b[i] = (uint8_t)x; break;
    case 1: v->h[i] = (uint16_t)x; break;
    case 2: v->w[i] = (uint32_t)x; break;
    default: v->d[i] = x; break;
    }
}

static inline int64_t vsext(uint64_t x, int sz) {
    switch (sz) {
    case 0: return (int8_t)x;
    case 1: return (int16_t)x;
    case 2: return (int32_t)x;
    default: return (int64_t)x;
    }
}

static inline V128 nx_vzeroed_(void) {
    V128 r;
    memset(&r, 0, sizeof r);
    return r;
}

/* ---- saturation (SignedSatQ / UnsignedSatQ): the element's bits of the clamped value; QC is not set --------------------------- */

static inline uint64_t v_sat_s(__int128 v, int sz) {
    const int bits = 8 << sz;
    const __int128 hi = bits >= 64 ? (__int128)INT64_MAX : (((__int128)1 << (bits - 1)) - 1);
    const __int128 lo = -hi - 1;
    if (v > hi) v = hi;
    if (v < lo) v = lo;
    return bits >= 64 ? (uint64_t)v : (uint64_t)v & ((1ull << bits) - 1);
}

static inline uint64_t v_sat_u(__int128 v, int sz) {
    const int bits = 8 << sz;
    const __int128 top = bits >= 64 ? (__int128)UINT64_MAX : (((__int128)1 << bits) - 1);
    const __int128 clamped = v < 0 ? (__int128)0 : v > top ? top : v;
    return (uint64_t)clamped;
}

/* ---- floating point, three registers ------------------------------------------------------------------------------------------- */

enum { F_ADD, F_SUB, F_MUL, F_DIV, F_MLA, F_MLS, F_MAX, F_MIN, F_MAXNM, F_MINNM, F_ABD, F_CMEQ, F_CMGE, F_CMGT, F_RECPS, F_RSQRTS,
       F_ADDP };

/* One lane: the value, or for a compare a mask (*is_mask = 1). acc is the accumulator of FMLA / FMLS. */
static inline float f3s(int op, float acc, float a, float b, uint32_t *mask, int *is_mask) {
    switch (op) {
    case F_ADD: return fp_add32(a, b);
    case F_SUB: return fp_sub32(a, b);
    case F_MUL: return fp_mul32(a, b);
    case F_DIV: return fp_div32(a, b);
    case F_MLA: return fp_fma32(a, b, acc);
    case F_MLS: return fp_fma32(fp_neg32(a), b, acc);
    case F_MAX: return fp_maxmin32(a, b, 0, 0);
    case F_MIN: return fp_maxmin32(a, b, 1, 0);
    case F_MAXNM: return fp_maxmin32(a, b, 0, 1);
    case F_MINNM: return fp_maxmin32(a, b, 1, 1);
    case F_ABD: return fp_abs32(fp_sub32(a, b));
    case F_RECPS: return fp_recps32(a, b);
    case F_RSQRTS: return fp_rsqrts32(a, b);
    case F_CMEQ: *is_mask = 1; *mask = a == b ? 0xffffffffu : 0u; return 0.0f;
    case F_CMGE: *is_mask = 1; *mask = a >= b ? 0xffffffffu : 0u; return 0.0f;
    case F_CMGT: *is_mask = 1; *mask = a > b ? 0xffffffffu : 0u; return 0.0f;
    default: return 0.0f;
    }
}

static inline double f3d(int op, double acc, double a, double b, uint64_t *mask, int *is_mask) {
    switch (op) {
    case F_ADD: return fp_add64(a, b);
    case F_SUB: return fp_sub64(a, b);
    case F_MUL: return fp_mul64(a, b);
    case F_DIV: return fp_div64(a, b);
    case F_MLA: return fp_fma64(a, b, acc);
    case F_MLS: return fp_fma64(fp_neg64(a), b, acc);
    case F_MAX: return fp_maxmin64(a, b, 0, 0);
    case F_MIN: return fp_maxmin64(a, b, 1, 0);
    case F_MAXNM: return fp_maxmin64(a, b, 0, 1);
    case F_MINNM: return fp_maxmin64(a, b, 1, 1);
    case F_ABD: return fp_abs64(fp_sub64(a, b));
    case F_RECPS: return fp_recps64(a, b);
    case F_RSQRTS: return fp_rsqrts64(a, b);
    case F_CMEQ: *is_mask = 1; *mask = a == b ? ~0ull : 0ull; return 0.0;
    case F_CMGE: *is_mask = 1; *mask = a >= b ? ~0ull : 0ull; return 0.0;
    case F_CMGT: *is_mask = 1; *mask = a > b ? ~0ull : 0ull; return 0.0;
    default: return 0.0;
    }
}

/* n lanes of single (dbl = 0) or double; F_ADDP is the pairwise sum of the concatenation m:n. */
static inline void v_f3v(V128 *d, const V128 *pn, const V128 *pm, int op, int dbl, int n) {
    const V128 x = *pn, y = *pm, acc = *d;
    V128 r = nx_vzeroed_();
    if (op == F_ADDP) {
        if (dbl) {
            r.df[0] = fp_add64(x.df[0], x.df[1]);
            if (n == 2) r.df[1] = fp_add64(y.df[0], y.df[1]);
        } else {
            for (int i = 0; i < n / 2; i++) {
                r.f[i] = fp_add32(x.f[2 * i], x.f[2 * i + 1]);
                r.f[n / 2 + i] = fp_add32(y.f[2 * i], y.f[2 * i + 1]);
            }
        }
    } else {
        for (int i = 0; i != n; ++i) {
            int is_mask = 0;
            if (dbl) {
                uint64_t mask = 0;
                const double v = f3d(op, acc.df[i], x.df[i], y.df[i], &mask, &is_mask);
                if (is_mask) r.d[i] = mask;
                else r.df[i] = v;
            } else {
                uint32_t mask = 0;
                const float v = f3s(op, acc.f[i], x.f[i], y.f[i], &mask, &is_mask);
                if (is_mask) r.w[i] = mask;
                else r.f[i] = v;
            }
        }
    }
    *d = r;
}

/* By element: every lane of the second operand is its element idx. */
static inline void v_f3e(V128 *d, const V128 *pn, const V128 *pm, int idx, int op, int dbl, int n) {
    V128 e;
    if (dbl) {
        const uint64_t v = pm->d[idx];
        e.d[0] = e.d[1] = v;
    } else {
        const uint32_t v = pm->w[idx];
        e.w[0] = e.w[1] = e.w[2] = e.w[3] = v;
    }
    v_f3v(d, pn, &e, op, dbl, n);
}

/* ---- floating point, one register ------------------------------------------------------------------------------------------- */

enum { F1_ABS, F1_NEG, F1_SQRT, F1_RSQRTE, F1_RECPE, F1_SCVTF, F1_UCVTF, F1_CVTZS, F1_CVTZU, F1_CMEQ0, F1_CMGT0, F1_CMGE0, F1_CMLT0,
       F1_CMLE0 };

static inline void v_f1v(V128 *d, const V128 *pn, int op, int dbl, int n) {
    const V128 x = *pn;
    V128 r = nx_vzeroed_();
    for (int i = 0; i < n; i++) {
        if (dbl) {
            const double v = x.df[i];
            switch (op) {
            case F1_ABS: r.df[i] = fp_abs64(v); break;
            case F1_NEG: r.df[i] = fp_neg64(v); break;
            case F1_SQRT: r.df[i] = fp_sqrt64(v); break;
            case F1_RSQRTE: r.df[i] = rsqrte_d(v); break;
            case F1_RECPE: r.df[i] = recpe_d(v); break;
            case F1_SCVTF: r.df[i] = (double)(int64_t)x.d[i]; break;
            case F1_UCVTF: r.df[i] = (double)x.d[i]; break;
            case F1_CVTZS: r.d[i] = f2s64(trunc(v)); break;
            case F1_CVTZU: r.d[i] = f2u64(trunc(v)); break;
            case F1_CMEQ0: r.d[i] = v == 0.0 ? ~0ull : 0; break;
            case F1_CMGT0: r.d[i] = v > 0.0 ? ~0ull : 0; break;
            case F1_CMGE0: r.d[i] = v >= 0.0 ? ~0ull : 0; break;
            case F1_CMLT0: r.d[i] = v < 0.0 ? ~0ull : 0; break;
            case F1_CMLE0: r.d[i] = v <= 0.0 ? ~0ull : 0; break;
            default: break;
            }
        } else {
            const float v = x.f[i];
            switch (op) {
            case F1_ABS: r.f[i] = fp_abs32(v); break;
            case F1_NEG: r.f[i] = fp_neg32(v); break;
            case F1_SQRT: r.f[i] = fp_sqrt32(v); break;
            case F1_RSQRTE: r.f[i] = rsqrte_f(v); break;
            case F1_RECPE: r.f[i] = recpe_f(v); break;
            case F1_SCVTF: r.f[i] = (float)(int32_t)x.w[i]; break;
            case F1_UCVTF: r.f[i] = (float)x.w[i]; break;
            case F1_CVTZS: r.w[i] = f2s32(trunc((double)v)); break;
            case F1_CVTZU: r.w[i] = f2u32(trunc((double)v)); break;
            case F1_CMEQ0: r.w[i] = v == 0.0f ? 0xffffffffu : 0; break;
            case F1_CMGT0: r.w[i] = v > 0.0f ? 0xffffffffu : 0; break;
            case F1_CMGE0: r.w[i] = v >= 0.0f ? 0xffffffffu : 0; break;
            case F1_CMLT0: r.w[i] = v < 0.0f ? 0xffffffffu : 0; break;
            case F1_CMLE0: r.w[i] = v <= 0.0f ? 0xffffffffu : 0; break;
            default: break;
            }
        }
    }
    *d = r;
}

/* FRINT<mode> per lane (fpexact.h fp_frint). */
static inline void v_frint(V128 *d, const V128 *pn, int mode, int dbl, int n) {
    const V128 x = *pn;
    V128 r = nx_vzeroed_();
    for (int i = 0; i < n; i++) {
        if (dbl) r.df[i] = fp_frint64(x.df[i], mode);
        else r.f[i] = fp_frint32(x.f[i], mode);
    }
    *d = r;
}

/* FMAXV / FMINV / FMAXNMV / FMINNMV of four singles: the pairwise tree, lower pair first. */
static inline void v_fred32(V128 *d, const V128 *pn, int is_min, int num) {
    const uint32_t w0 = pn->w[0], w1 = pn->w[1], w2 = pn->w[2], w3 = pn->w[3];
    const uint32_t lo = fp_maxmin32_bits(w0, w1, is_min, num), hi = fp_maxmin32_bits(w2, w3, is_min, num);
    V128 r = nx_vzeroed_();
    r.w[0] = fp_maxmin32_bits(lo, hi, is_min, num);
    *d = r;
}

/* ---- integer, three registers ------------------------------------------------------------------------------------------------- */

enum { I_ADD, I_SUB, I_MUL, I_CMEQ, I_CMGT, I_CMHI, I_CMGE, I_CMHS, I_CMTST, I_SQADD, I_UQADD, I_SQSUB, I_UQSUB, I_MLA };

static inline uint64_t nx_ones_(int sz) { return sz >= 3 ? ~0ull : (1ull << (8 << sz)) - 1; }

static inline void v_i3v(V128 *d, const V128 *pn, const V128 *pm, int op, int sz, int n) {
    const V128 x = *pn, y = *pm, acc = *d;
    const uint64_t ones = nx_ones_(sz);
    V128 r = nx_vzeroed_();
    for (int i = 0; i < n; i++) {
        const uint64_t a = vget(&x, sz, i), b = vget(&y, sz, i);
        const int64_t sa = vsext(a, sz), sb = vsext(b, sz);
        uint64_t v;
        switch (op) {
        case I_ADD: v = a + b; break;
        case I_SUB: v = a - b; break;
        case I_MUL: v = a * b; break;
        case I_CMEQ: v = a == b ? ones : 0; break;
        case I_CMGT: v = sa > sb ? ones : 0; break;
        case I_CMGE: v = sa >= sb ? ones : 0; break;
        case I_CMHI: v = a > b ? ones : 0; break;
        case I_CMHS: v = a >= b ? ones : 0; break;
        case I_CMTST: v = (a & b) ? ones : 0; break;
        case I_SQADD: v = v_sat_s((__int128)sa + sb, sz); break;
        case I_UQADD: v = v_sat_u((__int128)a + b, sz); break;
        case I_SQSUB: v = v_sat_s((__int128)sa - sb, sz); break;
        case I_UQSUB: v = v_sat_u((__int128)a - (__int128)b, sz); break;
        case I_MLA: v = vget(&acc, sz, i) + a * b; break;
        default: v = 0; break;
        }
        vset(&r, sz, i, v);
    }
    *d = r;
}

/* ADDV: the sum of n lanes, modulo the element, in lane 0. */
static inline void v_addv(V128 *d, const V128 *pn, int sz, int n) {
    const V128 x = *pn;
    uint64_t s = 0;
    for (int i = 0; i < n; i++) s += vget(&x, sz, i);
    V128 r = nx_vzeroed_();
    vset(&r, sz, 0, s);
    *d = r;
}

/* SMULL / UMULL and the accumulating forms (acc 1 add, 2 subtract), the "2" forms (upper) and by element (elem >= 0: m whole). */
static inline void v_mull(V128 *d, const V128 *pn, const V128 *pm, int sz, int upper, int is_unsigned, int acc, int elem) {
    const V128 x = *pn, y = *pm, old = *d;
    const int nsrc = 8 >> sz, base = upper ? nsrc : 0;
    V128 r = nx_vzeroed_();
    for (int i = 0; i < nsrc; i++) {
        const uint64_t a = vget(&x, sz, base + i), b = vget(&y, sz, elem >= 0 ? elem : base + i);
        const uint64_t p = is_unsigned ? a * b : (uint64_t)(vsext(a, sz) * vsext(b, sz));
        uint64_t v = p;
        if (acc == 1) v = vget(&old, sz + 1, i) + p;
        else if (acc == 2) v = vget(&old, sz + 1, i) - p;
        vset(&r, sz + 1, i, v);
    }
    *d = r;
}

/* MUL / MLA / MLS (by element). */
static inline void v_mul_elem(V128 *d, const V128 *pn, const V128 *pm, int sz, int elem, int acc, int n) {
    const V128 x = *pn, old = *d;
    const uint64_t b = vget(pm, sz, elem);
    V128 r = nx_vzeroed_();
    for (int i = 0; i < n; i++) {
        const uint64_t p = vget(&x, sz, i) * b;
        uint64_t v = p;
        if (acc == 1) v = vget(&old, sz, i) + p;
        else if (acc == 2) v = vget(&old, sz, i) - p;
        vset(&r, sz, i, v);
    }
    *d = r;
}

/* One element of a saturating shift right narrow: source element of size dsz + 1, optional rounding, floor shift, saturate. */
static inline uint64_t nx_qshrn1_(uint64_t s, int shift, int dsz, int round, int src_signed, int dst_signed) {
    __int128 v = src_signed ? (__int128)vsext(s, dsz + 1) : (__int128)s;
    if (round) {
        const __int128 half = (__int128)1 << (shift - 1);   /* rounding: add half of the last kept bit */
        v += half;
    }
    v >>= shift;
    return dst_signed ? v_sat_s(v, dsz) : v_sat_u(v, dsz);
}

/* SQSHRN / SQRSHRN (1, 1), UQSHRN / UQRSHRN (0, 0), SQSHRUN / SQRSHRUN (1, 0); upper: the "2" forms keep d's lower half. */
static inline void v_qshrn(V128 *d, const V128 *pn, int shift, int dsz, int round, int src_signed, int dst_signed, int upper) {
    const V128 x = *pn;
    const int ndst = 8 >> dsz;
    V128 r = nx_vzeroed_();
    if (upper) r.d[0] = d->d[0];
    for (int i = 0; i < ndst; i++)
        vset(&r, dsz, (upper ? ndst : 0) + i, nx_qshrn1_(vget(&x, dsz + 1, i), shift, dsz, round, src_signed, dst_signed));
    *d = r;
}

/* The scalar form: element 0 only. */
static inline void v_qshrn_s(V128 *d, const V128 *pn, int shift, int dsz, int round, int src_signed, int dst_signed) {
    const uint64_t s = vget(pn, dsz + 1, 0);
    V128 r = nx_vzeroed_();
    vset(&r, dsz, 0, nx_qshrn1_(s, shift, dsz, round, src_signed, dst_signed));
    *d = r;
}

/* ---- integer, one register ---------------------------------------------------------------------------------------------------- */

enum { I1_NOT, I1_NEG, I1_ABS, I1_CMEQ0, I1_REV64, I1_CNT };

static inline void v_i1v(V128 *d, const V128 *pn, int op, int sz, int n) {
    const V128 x = *pn;
    const uint64_t ones = nx_ones_(sz);
    const int k = 8 >> sz;                                                /* lanes per doubleword (REV64) */
    V128 r = nx_vzeroed_();
    for (int i = 0; i < n; i++) {
        const uint64_t a = vget(&x, sz, i);
        switch (op) {
        case I1_NOT: vset(&r, sz, i, ~a); break;
        case I1_NEG: vset(&r, sz, i, 0 - a); break;
        case I1_ABS: vset(&r, sz, i, vsext(a, sz) < 0 ? 0 - a : a); break;
        case I1_CMEQ0: vset(&r, sz, i, a == 0 ? ones : 0); break;
        case I1_CNT: vset(&r, sz, i, (uint64_t)__builtin_popcountll(a)); break;
        case I1_REV64: vset(&r, sz, (i - i % k) + (k - 1 - i % k), a); break;
        default: break;
        }
    }
    *d = r;
}

/* ---- narrow, select, duplicate, extract, table, structures, permutes, shifts --------------------------------------------------- */

/* XTN / XTN2: sz is the destination element size; XTN2 keeps d's lower half. */
static inline void v_xtn(V128 *d, const V128 *pn, int sz, int upper) {
    const V128 x = *pn;
    const int k = 8 >> sz;
    V128 r = nx_vzeroed_();
    if (upper) r.d[0] = d->d[0];
    for (int i = 0; i < k; i++) vset(&r, sz, (upper ? k : 0) + i, vget(&x, sz + 1, i));
    *d = r;
}

/* BSL (kind 1), BIT (2), BIF (anything else), on each doubleword. */
static inline void v_bitsel(V128 *d, const V128 *pn, const V128 *pm, int kind, int q) {
    const V128 x = *d, nn = *pn, mm = *pm;
    V128 r = nx_vzeroed_();
    for (int h = 0; h < (q ? 2 : 1); h++) {
        const uint64_t a = x.d[h], b = nn.d[h], c = mm.d[h];
        if (kind == 1) r.d[h] = (a & b) | (~a & c);
        else if (kind == 2) r.d[h] = (a & ~c) | (b & c);
        else r.d[h] = (a & c) | (b & ~c);
    }
    *d = r;
}

static inline void v_dup(V128 *d, uint64_t value, int sz, int n) {
    V128 r = nx_vzeroed_();
    for (int i = 0; i < n; i++) vset(&r, sz, i, value);
    *d = r;
}

/* EXT: bytes imm.. of the concatenation m:n. */
static inline void v_ext(V128 *d, const V128 *pn, const V128 *pm, int imm, int q) {
    const V128 x = *pn, y = *pm;
    const int nb = q ? 16 : 8;
    V128 r = nx_vzeroed_();
    for (int i = 0; i < nb; i++) r.b[i] = imm + i < nb ? x.b[imm + i] : y.b[imm + i - nb];
    *d = r;
}

/* TBL / TBX: a table of len consecutive registers from rn (wrapping after v31), indexed by the bytes of rm. */
static inline void v_tbl(Ctx *c, int rd, int rn, int len, int rm, int q, int tbx) {
    uint8_t table[64];
    for (int k = 0; k < len; k++) memcpy(table + 16 * k, &c->v[(rn + k) & 31], 16);
    const V128 idx = c->v[rm];
    V128 r = c->v[rd];
    const int nb = q ? 16 : 8;
    for (int i = 0; i < nb; i++) {
        if (idx.b[i] < 16 * len) r.b[i] = table[idx.b[i]];
        else if (!tbx) r.b[i] = 0;
    }
    if (!q) r.d[1] = 0;
    c->v[rd] = r;
}

/* LD2..LD4 (multiple structures): element i of register (rt + k) mod 32 is at a + (i * n + k) * 2^sz. */
static inline void v_ldn(Ctx *c, uint64_t a, int rt, int n, int sz, int q) {
    const int count = (q ? 16 : 8) >> sz, es = 1 << sz;
    V128 r[4];
    for (int k = 0; k < n; k++) r[k] = nx_vzeroed_();
    for (int i = 0; i < count; i++)
        for (int k = 0; k < n; k++) memcpy(&r[k].b[i * es], (const void *)(uintptr_t)(a + (uint64_t)((i * n + k) * es)), (size_t)es);
    for (int k = 0; k < n; k++) c->v[(rt + k) & 31] = r[k];
}

/* ST2..ST4: the inverse, in the same order. */
static inline void v_stn(Ctx *c, uint64_t a, int rt, int n, int sz, int q) {
    const int count = (q ? 16 : 8) >> sz, es = 1 << sz;
    for (int i = 0; i < count; i++)
        for (int k = 0; k < n; k++) memcpy((void *)(uintptr_t)(a + (uint64_t)((i * n + k) * es)), &c->v[(rt + k) & 31].b[i * es], (size_t)es);
}

/* UZP1/2, TRN1/2, ZIP1/2: opcode = instruction bits 14:12 (bit 2: the "2" forms; low bits 1 UZP, 2 TRN, otherwise ZIP). */
static inline void v_perm(V128 *d, const V128 *pn, const V128 *pm, int opcode, int sz, int n) {
    const V128 x = *pn, y = *pm;
    const int part = (opcode >> 2) & 1, kind = opcode & 3, h = n / 2;
    V128 r = nx_vzeroed_();
    for (int i = 0; i < h; i++) {
        if (kind == 1) {
            vset(&r, sz, i, vget(&x, sz, 2 * i + part));
            vset(&r, sz, h + i, vget(&y, sz, 2 * i + part));
        } else if (kind == 2) {
            vset(&r, sz, 2 * i, vget(&x, sz, 2 * i + part));
            vset(&r, sz, 2 * i + 1, vget(&y, sz, 2 * i + part));
        } else {
            vset(&r, sz, 2 * i, vget(&x, sz, part * h + i));
            vset(&r, sz, 2 * i + 1, vget(&y, sz, part * h + i));
        }
    }
    *d = r;
}

/* SSHLL / USHLL (SXTL / UXTL with shift 0), and the "2" forms: widen, then shift left. */
static inline void v_shll(V128 *d, const V128 *pn, int shift, int sz, int is_unsigned, int upper) {
    const V128 x = *pn;
    const int k = 8 >> sz;
    V128 r = nx_vzeroed_();
    for (int i = 0; i < k; i++) {
        const uint64_t a = vget(&x, sz, i + (upper ? k : 0));
        const uint64_t w = is_unsigned ? a : (uint64_t)vsext(a, sz);
        vset(&r, sz + 1, i, w << shift);
    }
    *d = r;
}

/* SHL (kind 0), SSHR (1), USHR (2), by an immediate. */
static inline void v_shift(V128 *d, const V128 *pn, int shift, int sz, int kind, int n) {
    const V128 x = *pn;
    const int bits = 8 << sz;
    V128 r = nx_vzeroed_();
    for (int i = 0; i < n; i++) {
        const uint64_t a = vget(&x, sz, i);
        uint64_t v;
        if (kind == 0) v = a << shift;
        else if (kind == 1) v = (uint64_t)(vsext(a, sz) >> (shift < bits - 1 ? shift : bits - 1));
        else v = shift >= bits ? 0 : a >> shift;
        vset(&r, sz, i, v);
    }
    *d = r;
}

#endif /* NXRECOMP_NEON_H */
