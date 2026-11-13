/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
 *
 * nxrecomp: AArch64 floating point for lifted code, bit-exact where the host's IEEE arithmetic is not the guest's (included by ctx.h).
 *
 * The runtime puts the guest's FPCR on the host thread (runtime/src/core/abi_glue.cpp apply_guest_fpcr: x86-64 MXCSR RC from RMode,
 * FTZ and DAZ from FZ; arm64 FPCR RMode, FZ and DN), so host arithmetic here already rounds and flushes the guest's way. What the host
 * does differently is the NaN a result carries, the estimate instructions, half precision, FMAX/FMIN and the fused operations; those
 * follow the Arm Architecture Reference Manual pseudocode (FPProcessNaNs, FPRecipEstimate, FPConvert, FPMax, FPMulAdd, ...) with
 * FPCR.DN = 0 and AHP = 0. FPSR is never written: cumulative exception flags are not modelled. Valid GNU C11 and C++17 / C++20.
 */
#ifndef NXRECOMP_FPEXACT_H
#define NXRECOMP_FPEXACT_H

#define FP_QBIT32 0x00400000u
#define FP_QBIT64 0x0008000000000000ull
#define FP_SIGN32 0x80000000u
#define FP_SIGN64 0x8000000000000000ull
#define FP_DNAN32 0x7fc00000u            /* FPDefaultNaN: positive, quiet, no payload */
#define FP_DNAN64 0x7ff8000000000000ull

/* ---- bit patterns ------------------------------------------------------------------------------------------------------------ */

static inline uint32_t fp_bits32(float x) { uint32_t b; memcpy(&b, &x, sizeof b); return b; }
static inline float fp_from_bits32(uint32_t b) { float x; memcpy(&x, &b, sizeof x); return x; }
static inline uint64_t fp_bits64(double x) { uint64_t b; memcpy(&b, &x, sizeof b); return b; }
static inline double fp_from_bits64(uint64_t b) { double x; memcpy(&x, &b, sizeof x); return x; }

/* Classes, read with the sign shifted out: b << 1 holds the exponent at the top and the fraction (quiet bit first) under it. */
static inline int fp_nan32b(uint32_t b) { return (uint32_t)(b << 1) > 0xff000000u; }
static inline int fp_snan32b(uint32_t b) {
    const uint32_t m = b << 1;
    return m > 0xff000000u && m < 0xff800000u;                           /* a NaN whose quiet bit is clear */
}
static inline int fp_inf32b(uint32_t b) { return (uint32_t)(b << 1) == 0xff000000u; }
static inline int fp_zero32b(uint32_t b) { return (uint32_t)(b << 1) == 0u; }
static inline int fp_nan64b(uint64_t b) { return (b << 1) > 0xffe0000000000000ull; }
static inline int fp_snan64b(uint64_t b) {
    const uint64_t m = b << 1;
    return m > 0xffe0000000000000ull && m < 0xfff0000000000000ull;
}
static inline int fp_inf64b(uint64_t b) { return (b << 1) == 0xffe0000000000000ull; }
static inline int fp_zero64b(uint64_t b) { return (b << 1) == 0u; }

/* A NaN's pattern with the quiet bit set; sign and payload kept. */
static inline float fp_quiet32(uint32_t b) { return fp_from_bits32(b | FP_QBIT32); }
static inline double fp_quiet64(uint64_t b) { return fp_from_bits64(b | FP_QBIT64); }

/* x86-64's default NaN is negative; the guest's is positive. */
static inline float fnan32(float r) { return fp_bits32(r) == 0xffc00000u ? fp_from_bits32(0x7fc00000u) : r; }
static inline double fnan64(double r) { return fp_bits64(r) == 0xfff8000000000000ull ? fp_from_bits64(0x7ff8000000000000ull) : r; }

/* ---- the NaN of an arithmetic result (FPProcessNaNs / FPProcessNaNs3 with DN = 0) --------------------------------------------- */
/* Of the first n operands: the first signalling NaN, quietened; else the first NaN; else the default NaN (FPDefaultNaN, positive). */

static inline float fp_nan_result32(int n, float x0, float x1, float x2) {
    const uint32_t op[3] = {fp_bits32(x0), fp_bits32(x1), fp_bits32(x2)};
    for (int k = 0; k < n; k++)
        if (fp_snan32b(op[k])) return fp_quiet32(op[k]);
    for (int k = 0; k < n; k++)
        if (fp_nan32b(op[k])) return fp_from_bits32(op[k]);
    return fp_from_bits32(FP_DNAN32);
}

static inline double fp_nan_result64(int n, double x0, double x1, double x2) {
    const uint64_t op[3] = {fp_bits64(x0), fp_bits64(x1), fp_bits64(x2)};
    for (int k = 0; k < n; k++)
        if (fp_snan64b(op[k])) return fp_quiet64(op[k]);
    for (int k = 0; k < n; k++)
        if (fp_nan64b(op[k])) return fp_from_bits64(op[k]);
    return fp_from_bits64(FP_DNAN64);
}

/* ---- FADD, FSUB, FMUL, FDIV, FSQRT, FNEG, FABS --------------------------------------------------------------------------------- */
/* The host computes the value (guest rounding mode and flush, through the thread's FP environment); only a NaN result is replaced by
 * the one Arm chooses. FPCR.DN = 1 is not followed: an operand's NaN still propagates. */

#define NX_FP_BINARY_(name, T, W, op)                                                                                               \
    static inline T name(T a, T b) {                                                                                                \
        T r = a op b;                                                                                                               \
        return r != r ? fp_nan_result##W(2, a, b, (T)0) : r;                                                                        \
    }
NX_FP_BINARY_(fp_add32, float, 32, +)
NX_FP_BINARY_(fp_sub32, float, 32, -)
NX_FP_BINARY_(fp_mul32, float, 32, *)
NX_FP_BINARY_(fp_div32, float, 32, /)
NX_FP_BINARY_(fp_add64, double, 64, +)
NX_FP_BINARY_(fp_sub64, double, 64, -)
NX_FP_BINARY_(fp_mul64, double, 64, *)
NX_FP_BINARY_(fp_div64, double, 64, /)
#undef NX_FP_BINARY_

static inline float fp_sqrt32(float a) {
    float r = sqrtf(a);
    return r != r ? fp_nan_result32(1, a, 0.0f, 0.0f) : r;
}
static inline double fp_sqrt64(double a) {
    double r = sqrt(a);
    return r != r ? fp_nan_result64(1, a, 0.0, 0.0) : r;
}

/* The sign bit flipped or cleared, nothing else: a NaN keeps its payload, nothing is flushed. */
static inline float fp_neg32(float a) { return fp_from_bits32(fp_bits32(a) ^ FP_SIGN32); }
static inline float fp_abs32(float a) { return fp_from_bits32(fp_bits32(a) & ~FP_SIGN32); }
static inline double fp_neg64(double a) { return fp_from_bits64(fp_bits64(a) ^ FP_SIGN64); }
static inline double fp_abs64(double a) { return fp_from_bits64(fp_bits64(a) & ~FP_SIGN64); }

/* ---- estimates: FRECPE, FRSQRTE, URECPE, URSQRTE ------------------------------------------------------------------------------- */

/* RecipEstimate: a = 256..511 (1.xxxxxxxx), result 256..511. */
static inline uint32_t recip_estimate(uint32_t a) {
    uint32_t den = 2u * a + 1u;
    uint32_t q = (1u << 19) / den;
    return (q + 1u) / 2u;
}

/* RecipSqrtEstimate (the integer form of the pre-2020 manuals): a = 128..511, result 256..511. */
static inline uint32_t recip_sqrt_estimate(uint32_t a) {
    uint64_t s = a < 256u ? 2u * (uint64_t)a + 1u : 2u * ((uint64_t)(a & ~1u) + 1u);
    uint64_t b = 512;
    while (s * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (uint32_t)((b + 1) / 2);
}

/* The two formats' fields: single (dbl = 0) E 8, F 23; double E 11, F 52. The fraction is worked on left-aligned to 52 bits. */
#define NX_FRAC52_ 0x000fffffffffffffull

/* FPRecipEstimate with DN = 0, FZ = 0, nearest. A denormal too small to have a finite reciprocal gives infinity in every mode. */
static inline uint64_t fp_recpe_bits(uint64_t bits, int dbl) {
    const int E = dbl ? 11 : 8, F = dbl ? 52 : 23;
    const uint64_t emax = (1ull << E) - 1;
    const uint64_t sign = (bits >> (E + F)) & 1u, exp = (bits >> F) & emax, frac = bits & ((1ull << F) - 1);
    const uint64_t sbit = sign << (E + F), inf = emax << F;
    if (exp == emax) return frac ? bits | (1ull << (F - 1)) : sbit;      /* NaN: quietened; infinity: zero */
    if (exp == 0 && frac < (1ull << (F - 2))) return sbit | inf;          /* zero, or |x| < 2^-(2^(E-1)): infinity */
    uint64_t f = frac << (52 - F);
    int e = (int)exp;
    if (e == 0) {                                                        /* a denormal: normalise by one or two places */
        if (!(f >> 51)) {
            e = -1;
            f = (f << 2) & NX_FRAC52_;
        } else {
            f = (f << 1) & NX_FRAC52_;
        }
    }
    const uint32_t est = recip_estimate(256u + (uint32_t)((f >> 44) & 0xffu));
    int re = (dbl ? 2045 : 253) - e;
    f = (uint64_t)(est & 0xffu) << 44;
    if (re == 0) {
        f = (1ull << 51) | (f >> 1);
    } else if (re == -1) {
        f = (1ull << 50) | (f >> 2);
        re = 0;
    }
    return sbit | ((uint64_t)re << F) | (f >> (52 - F));
}

/* FPRSqrtEstimate with DN = 0, FZ = 0. */
static inline uint64_t fp_rsqrte_bits(uint64_t bits, int dbl) {
    const int E = dbl ? 11 : 8, F = dbl ? 52 : 23;
    const uint64_t emax = (1ull << E) - 1;
    const uint64_t sign = (bits >> (E + F)) & 1u, exp = (bits >> F) & emax, frac = bits & ((1ull << F) - 1);
    const uint64_t inf = emax << F;
    if (exp == emax && frac) return bits | (1ull << (F - 1));           /* NaN: quietened */
    if (exp == 0 && frac == 0) return (sign << (E + F)) | inf;            /* +-0: infinity of that sign */
    if (sign) return inf | (1ull << (F - 1));                             /* negative: the default NaN */
    if (exp == emax) return 0;                                            /* +infinity: +0 */
    uint64_t f = frac << (52 - F);
    int e = (int)exp;
    if (e == 0) {                                                        /* a denormal: normalise, dropping the leading 1 */
        while (!(f >> 51)) {
            f <<= 1;
            e--;
        }
        f = (f << 1) & NX_FRAC52_;
    }
    const uint32_t scaled = (e & 1) ? 128u + (uint32_t)((f >> 45) & 0x7fu) : 256u + (uint32_t)((f >> 44) & 0xffu);
    const uint32_t est = recip_sqrt_estimate(scaled);
    const uint64_t re = (uint64_t)(((dbl ? 3068 : 380) - e) / 2);
    return (re << F) | ((uint64_t)(est & 0xffu) << (F - 8));
}

static inline float recpe_f(float x) { return fp_from_bits32((uint32_t)fp_recpe_bits(fp_bits32(x), 0)); }
static inline double recpe_d(double x) { return fp_from_bits64(fp_recpe_bits(fp_bits64(x), 1)); }
static inline float rsqrte_f(float x) { return fp_from_bits32((uint32_t)fp_rsqrte_bits(fp_bits32(x), 0)); }
static inline double rsqrte_d(double x) { return fp_from_bits64(fp_rsqrte_bits(fp_bits64(x), 1)); }

/* ---- half precision (IEEE binary16, AHP = 0) ----------------------------------------------------------------------------------- */

/* m * 2^-k rounded to nearest, ties to even (k >= 0; m < 2^63). */
static inline uint64_t nx_rne_shift_(uint64_t m, int k) {
    if (k <= 0) return m << -k;
    if (k > 63) return 0;                                                /* below half of the last place: m < 2^63 <= 2^(k-1) */
    const uint64_t q = m >> k, rem = m & ((1ull << k) - 1), half = 1ull << (k - 1);
    return (rem > half || (rem == half && (q & 1u))) ? q + 1 : q;
}

/* Single (dbl = 0) or double -> half: always round to nearest even, whatever FPCR.RMode is; no flush. */
static inline uint16_t fp_to_half_bits(uint64_t bits, int dbl) {
    const int E = dbl ? 11 : 8, F = dbl ? 52 : 23, bias = dbl ? 1023 : 127;
    const uint64_t emax = (1ull << E) - 1;
    const uint16_t sign = (uint16_t)(((bits >> (E + F)) & 1u) << 15);
    const uint64_t exp = (bits >> F) & emax, frac = bits & ((1ull << F) - 1);
    if (exp == emax)                                                      /* NaN (quietened, top payload bits kept) or infinity */
        return frac ? (uint16_t)(sign | 0x7e00u | ((frac >> (F - 10)) & 0x1ffu)) : (uint16_t)(sign | 0x7c00u);
    if ((exp | frac) == 0) return sign;                                  /* +-0 */
    /* the value is m * 2^q, m an integer with its top bit at position top */
    const uint64_t m = exp ? frac | (1ull << F) : frac;
    const int q = (exp ? (int)exp : 1) - bias - F;
    const int top = 63 - __builtin_clzll(m);
    int e = top + q;                                                      /* v = 1.f * 2^e */
    if (e > 15) return (uint16_t)(sign | 0x7c00u);
    if (e >= -14) {
        uint64_t s = nx_rne_shift_(m, top - 10);                         /* 11 significant bits */
        if (s >> 11) {
            s >>= 1;
            e++;
        }
        if (e > 15) return (uint16_t)(sign | 0x7c00u);
        return (uint16_t)(sign | ((uint16_t)(e + 15) << 10) | (uint16_t)(s & 0x3ffu));
    }
    /* subnormal: a multiple of 2^-24 (a carry into 2^-14 is the smallest normal, 0x0400) */
    const int k = -24 - q;
    return (uint16_t)(sign | (uint16_t)nx_rne_shift_(m, k));
}

/* Half -> single (dbl = 0) or double: exact. */
static inline uint64_t fp_from_half_bits(uint16_t h, int dbl) {
    const int E = dbl ? 11 : 8, F = dbl ? 52 : 23, bias = dbl ? 1023 : 127;
    const uint64_t emax = (1ull << E) - 1;
    const uint64_t sign = (uint64_t)(h >> 15) << (E + F);
    const uint32_t he = (h >> 10) & 0x1fu, hf = h & 0x3ffu;
    if (he == 0x1f)
        return hf ? sign | (emax << F) | (1ull << (F - 1)) | ((uint64_t)(hf & 0x1ffu) << (F - 10)) : sign | (emax << F);
    if (he == 0) {
        if (hf == 0) return sign;
        const int p = 31 - __builtin_clz(hf);                              /* hf * 2^-24 = 1.x * 2^(p - 24) */
        return sign | ((uint64_t)(p - 24 + bias) << F) | ((uint64_t)(hf & ((1u << p) - 1)) << (F - p));
    }
    return sign | ((uint64_t)((int)he - 15 + bias) << F) | ((uint64_t)hf << (F - 10));
}

static inline uint16_t f32_to_half(float x) { return fp_to_half_bits(fp_bits32(x), 0); }
static inline uint16_t f64_to_half(double x) { return fp_to_half_bits(fp_bits64(x), 1); }
static inline float half_to_f32(uint16_t h) { return fp_from_bits32((uint32_t)fp_from_half_bits(h, 0)); }
static inline double half_to_f64(uint16_t h) { return fp_from_bits64(fp_from_half_bits(h, 1)); }

/* ---- FMAX, FMIN, FMAXNM, FMINNM (FPMax, FPMin, FPMaxNum, FPMinNum) ------------------------------------------------------------- */
/* is_min: 0 max, 1 min. num: the NM forms (a single quiet NaN is a missing value). The result is always one operand's pattern (or a
 * quietened NaN); the comparison itself is the host's, so it sees the thread's flush. */

static inline uint32_t fp_maxmin32_bits(uint32_t a, uint32_t b, int is_min, int num) {
    const int an = fp_nan32b(a), bn = fp_nan32b(b);
    if (an || bn) {
        if (num && an != bn && !fp_snan32b(a) && !fp_snan32b(b)) return an ? b : a;
        if (fp_snan32b(a)) return a | FP_QBIT32;
        if (fp_snan32b(b)) return b | FP_QBIT32;
        return (an ? a : b) | FP_QBIT32;
    }
    if (fp_zero32b(a) && fp_zero32b(b)) return is_min ? (a | b) : (a & b);
    const float x = fp_from_bits32(a), y = fp_from_bits32(b);
    if (is_min) return x < y ? a : b;
    return x > y ? a : b;
}

static inline uint64_t fp_maxmin64_bits(uint64_t a, uint64_t b, int is_min, int num) {
    const int an = fp_nan64b(a), bn = fp_nan64b(b);
    if (an || bn) {
        if (num && an != bn && !fp_snan64b(a) && !fp_snan64b(b)) return an ? b : a;
        if (fp_snan64b(a)) return a | FP_QBIT64;
        if (fp_snan64b(b)) return b | FP_QBIT64;
        return (an ? a : b) | FP_QBIT64;
    }
    if (fp_zero64b(a) && fp_zero64b(b)) return is_min ? (a | b) : (a & b);
    const double x = fp_from_bits64(a), y = fp_from_bits64(b);
    if (is_min) return x < y ? a : b;
    return x > y ? a : b;
}

static inline float fp_maxmin32(float x, float y, int is_min, int num) {
    return fp_from_bits32(fp_maxmin32_bits(fp_bits32(x), fp_bits32(y), is_min, num));
}
static inline double fp_maxmin64(double x, double y, int is_min, int num) {
    return fp_from_bits64(fp_maxmin64_bits(fp_bits64(x), fp_bits64(y), is_min, num));
}

/* ---- fused multiply-add --------------------------------------------------------------------------------------------------------- */
/* nx_fma32 / nx_fma64: n * m + a with one rounding in the thread's mode and flush. An x86-64 build for the baseline (the game build:
 * no -mfma) asks the CPU once and then runs one VFMADD231 in line, or, without FMA3, a software fma that reads MXCSR. Elsewhere the C
 * library's fmaf / fma, which are the FMADD / VFMADD instruction there. */
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__FMA__)
#define NX_FMA_DISPATCH 1

static inline uint32_t nx_mxcsr_get_(void) {
    uint32_t v;
    __asm__ volatile("stmxcsr %0" : "=m"(v) : : "memory");
    return v;
}
static inline void nx_mxcsr_set_(uint32_t v) { __asm__ volatile("ldmxcsr %0" : : "m"(v) : "memory"); }

/* 1 when FMA3 code can run: CPUID.1:ECX FMA (bit 12) and OSXSAVE (bit 27), and the OS saves XMM and YMM state (XCR0 bits 1, 2). */
static inline int nx_cpu_fma(void) {
    static int known = -1;
    int v = __atomic_load_n(&known, __ATOMIC_RELAXED);
    if (v < 0) {
        uint32_t eax = 1, ebx, ecx = 0, edx;
        __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
        v = 0;
        if ((ecx & (1u << 12)) && (ecx & (1u << 27))) {
            uint32_t lo, hi;
            __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0u));
            (void)hi;
            v = (lo & 6u) == 6u;
        }
        __atomic_store_n(&known, v, __ATOMIC_RELAXED);
    }
    return v;
}

/* Single: the product of the widened operands is exact in double; their sum, rounded to nearest in double and then made odd when
 * inexact (round to odd: the last bit records that something was lost, in the direction of the loss), rounds once more to float in
 * the caller's mode with its flush, which is then the correctly rounded fused result. */
static inline float nx_soft_fma32(float n, float m, float a) {
    /* volatile: every operation stays between the MXCSR changes that bracket it */
    volatile double vp = (double)n * (double)m, vda = (double)a;
    const uint32_t csr = nx_mxcsr_get_();
    nx_mxcsr_set_(csr & ~0x6000u);                                       /* round to nearest for the sum and its error */
    const double p = vp, da = vda;
    const double s0 = p + da;
    const double bv = s0 - p;
    volatile double vs = s0, verr = (p - (s0 - bv)) + (da - bv);
    nx_mxcsr_set_(csr);
    const double s = vs, err = verr;
    double r = s;
    const uint64_t sb = fp_bits64(s);
    if (err != 0.0 && (sb & 0x7ff0000000000000ull) != 0x7ff0000000000000ull && !(sb & 1u)) {
        const int up = (err > 0.0) == !(sb >> 63);                          /* the exact sum is further from zero than s */
        r = fp_from_bits64(up ? sb + 1 : sb - 1);
    }
    volatile double vr = r;
    return (float)vr;
}

/* DAZ on one operand: a denormal (exponent 0) keeps only its sign. */
static inline uint64_t nx_daz64_(uint64_t b) { return (b & 0x7ff0000000000000ull) ? b : (b & FP_SIGN64); }

/* Double: the exact n * m + a in 128-bit integers, rounded once by MXCSR.RC, with DAZ on the operands and FTZ on the result. */
static inline double nx_soft_fma64(double x, double y, double z) {
    const uint32_t csr = nx_mxcsr_get_();
    const int rc = (int)((csr >> 13) & 3u);                                 /* 0 nearest, 1 down, 2 up, 3 toward zero */
    uint64_t bx = fp_bits64(x), by = fp_bits64(y), bz = fp_bits64(z);
    if (csr & 0x40u) {                                                   /* DAZ: denormal operands are zeros */
        bx = nx_daz64_(bx);
        by = nx_daz64_(by);
        bz = nx_daz64_(bz);
    }
    const double fx = fp_from_bits64(bx), fy = fp_from_bits64(by), fz = fp_from_bits64(bz);
    const uint64_t EXP = 0x7ff0000000000000ull;
    if ((bx & EXP) == EXP || (by & EXP) == EXP) {                        /* an infinite or NaN factor */
        volatile double pr = fx * fy;
        return pr + fz;
    }
    if ((bz & EXP) == EXP) return fz;                                     /* an infinite or NaN addend, as it is */
    if (fp_zero64b(bx) || fp_zero64b(by)) {                               /* an exact zero product */
        volatile double pr = fx * fy;
        return pr + fz;
    }
    if (fp_zero64b(bz)) return fx * fy;
    /* all three finite and nonzero */
    const uint64_t sp = (bx ^ by) >> 63, sz = bz >> 63;
    const int ex = (int)((bx >> 52) & 0x7ffu), ey = (int)((by >> 52) & 0x7ffu), ez = (int)((bz >> 52) & 0x7ffu);
    const uint64_t mx = (bx & NX_FRAC52_) | (ex ? 1ull << 52 : 0), my = (by & NX_FRAC52_) | (ey ? 1ull << 52 : 0);
    const uint64_t mz = (bz & NX_FRAC52_) | (ez ? 1ull << 52 : 0);
    /* value = mant * 2^q */
    unsigned __int128 A = (unsigned __int128)mx * my, B = mz;
    int qa = (ex ? ex : 1) - 1075 + (ey ? ey : 1) - 1075, qb = (ez ? ez : 1) - 1075;
    uint64_t sa = sp, sbn = sz;
    /* both mantissas with their top bit at 125 */
    {
        const int ta = 127 - ((uint64_t)(A >> 64) ? __builtin_clzll((uint64_t)(A >> 64)) : 64 + __builtin_clzll((uint64_t)A));
        const int tb = 63 - __builtin_clzll((uint64_t)B);
        A <<= 125 - ta;
        qa -= 125 - ta;
        B <<= 125 - tb;
        qb -= 125 - tb;
    }
    if (qa < qb || (qa == qb && A < B)) {                                 /* A the larger magnitude */
        unsigned __int128 t = A; A = B; B = t;
        int tq = qa; qa = qb; qb = tq;
        uint64_t ts = sa; sa = sbn; sbn = ts;
    }
    const int d = qa - qb;
    if (d) {
        if (d >= 127) {
            B = 1;                                                       /* only a sticky bit is left */
        } else {
            const unsigned __int128 lost = B & ((((unsigned __int128)1) << d) - 1);
            B = (B >> d) | (lost != 0);
        }
    }
    unsigned __int128 R = sa == sbn ? A + B : A - B;
    const uint64_t sgn = sa;
    if (R == 0) return fp_from_bits64(rc == 1 ? 0x8000000000000000ull : 0);  /* exact cancellation */
    int q = qa;
    const int top = 127 - ((uint64_t)(R >> 64) ? __builtin_clzll((uint64_t)(R >> 64)) : 64 + __builtin_clzll((uint64_t)R));
    int e = top + q;                                                      /* R * 2^q = 1.f * 2^e */
    const int normal = e >= -1022;
    const int shift = normal ? top - 52 : -1074 - q;                      /* low bits to drop */
    unsigned __int128 m;
    int inexact_up = 0;
    if (shift <= 0) {
        m = R << -shift;
    } else {
        unsigned __int128 rem, half;
        if (shift >= 128) {
            m = 0;
            rem = R;
            half = ~(unsigned __int128)0;                                 /* R is below half of the last place */
        } else {
            m = R >> shift;
            rem = R & ((((unsigned __int128)1) << shift) - 1);
            half = ((unsigned __int128)1) << (shift - 1);
        }
        if (rem) {
            if (rc == 0) inexact_up = rem > half || (rem == half && (m & 1u));
            else if (rc == 1) inexact_up = sgn != 0;
            else if (rc == 2) inexact_up = sgn == 0;
        }
        m += (unsigned)inexact_up;
    }
    uint64_t out;
    if (normal) {
        if (m >> 53) {
            m >>= 1;
            e++;
        }
        if (e > 1023) {                                                  /* overflow: infinity, or the largest finite toward zero */
            const int to_max = rc == 3 || (rc == 1 && !sgn) || (rc == 2 && sgn);
            out = to_max ? 0x7fefffffffffffffull : 0x7ff0000000000000ull;
        } else {
            out = ((uint64_t)(e + 1023) << 52) | ((uint64_t)m & NX_FRAC52_);
        }
    } else {
        out = (uint64_t)m;                                                /* a carry to 2^52 is the smallest normal */
        if ((csr & 0x8000u) && !(out >> 52)) out = 0;                    /* FTZ: a denormal result is a zero */
    }
    return fp_from_bits64(out | (sgn << 63));
}

/* The in-line VFMADD231: a = n * m + a, in the order of the instruction's operands (NaN choice included). Volatile: the guest may
 * change MXCSR between two calls with the same operands. */
static inline float nx_fma32(float n, float m, float a) {
    if (__builtin_expect(!nx_cpu_fma(), 0)) return nx_soft_fma32(n, m, a);   /* no FMA3 */
    __asm__ volatile("vfmadd231ss %2, %1, %0" : "+x"(a) : "x"(n), "x"(m));
    return a;
}
static inline double nx_fma64(double n, double m, double a) {
    if (__builtin_expect(!nx_cpu_fma(), 0)) return nx_soft_fma64(n, m, a);
    __asm__ volatile("vfmadd231sd %2, %1, %0" : "+x"(a) : "x"(n), "x"(m));
    return a;
}
#else
static inline float nx_fma32(float n, float m, float a) {
    return fmaf(n, m, a);
}
static inline double nx_fma64(double n, double m, double a) {
    return fma(n, m, a);
}
#endif

/* FMADD and friends (FPMulAdd(addend, op1, op2), DN = 0): a + n * m. The statements pass negated operands for the subtracting
 * forms, so a NaN taken from one of them carries the flipped sign, as the pseudocode's FPNeg does. */
/* One factor infinite and the other zero: the invalid product of FPMulAdd, FPRecipStepFused and FPRSqrtStepFused. */
static inline int fp_inf_zero32(uint32_t x, uint32_t y) { return fp_inf32b(x) ? fp_zero32b(y) : fp_zero32b(x) && fp_inf32b(y); }
static inline int fp_inf_zero64(uint64_t x, uint64_t y) { return fp_inf64b(x) ? fp_zero64b(y) : fp_zero64b(x) && fp_inf64b(y); }

static inline float fp_fma32(float n, float m, float a) {
    const float r = nx_fma32(n, m, a);
    if (r == r) return r;
    const uint32_t ba = fp_bits32(a);
    const int quiet_addend = fp_nan32b(ba) && !fp_snan32b(ba);
    /* a quiet NaN addend with inf * 0 gives the default NaN, not the addend; else the operands' NaN, addend first */
    return quiet_addend && fp_inf_zero32(fp_bits32(n), fp_bits32(m)) ? fp_from_bits32(FP_DNAN32) : fp_nan_result32(3, a, n, m);
}
static inline double fp_fma64(double n, double m, double a) {
    const double r = nx_fma64(n, m, a);
    if (r == r) return r;
    const uint64_t ba = fp_bits64(a);
    const int quiet_addend = fp_nan64b(ba) && !fp_snan64b(ba);
    return quiet_addend && fp_inf_zero64(fp_bits64(n), fp_bits64(m)) ? fp_from_bits64(FP_DNAN64) : fp_nan_result64(3, a, n, m);
}

/* FRECPS (FPRecipStepFused): 2 - a * b, rounded once; inf * 0 gives 2. */
static inline float fp_recps32(float a, float b) {
    return fp_inf_zero32(fp_bits32(a), fp_bits32(b)) ? 2.0f : fp_fma32(fp_neg32(a), b, 2.0f);
}
static inline double fp_recps64(double a, double b) {
    return fp_inf_zero64(fp_bits64(a), fp_bits64(b)) ? 2.0 : fp_fma64(fp_neg64(a), b, 2.0);
}

/* FRSQRTS (FPRSqrtStepFused): (3 - a * b) / 2, rounded once; inf * 0 gives 1.5. Halving the larger of the two factors first (exact
 * for |x| >= 2 * the smallest normal) keeps the product from overflowing before the one rounding. */
static inline float fp_rsqrts32(float a, float b) {
    const uint32_t ba = fp_bits32(a), bb = fp_bits32(b);
    if (fp_inf_zero32(ba, bb)) return 1.5f;
    const float na = fp_neg32(a);
    if (!fp_nan32b(ba) && !fp_nan32b(bb)) {
        if (fabsf(a) >= 0x1p-125f) return nx_fma32(na * 0.5f, b, 1.5f);
        if (fabsf(b) >= 0x1p-125f) return nx_fma32(na, b * 0.5f, 1.5f);
    }
    const float t = fp_fma32(na, b, 3.0f);                                /* a NaN stays that NaN when halved */
    return t * 0.5f;
}
static inline double fp_rsqrts64(double a, double b) {
    const uint64_t ba = fp_bits64(a), bb = fp_bits64(b);
    if (fp_inf_zero64(ba, bb)) return 1.5;
    const double na = fp_neg64(a);
    if (!fp_nan64b(ba) && !fp_nan64b(bb)) {
        if (fabs(a) >= 0x1p-1021) return nx_fma64(na * 0.5, b, 1.5);
        if (fabs(b) >= 0x1p-1021) return nx_fma64(na, b * 0.5, 1.5);
    }
    const double t = fp_fma64(na, b, 3.0);
    return t * 0.5;
}

/* ---- FCVT single <-> double (FPConvert, FPConvertNaN) --------------------------------------------------------------------------- */

static inline float fp_d2s(double x) {
    const uint64_t b = fp_bits64(x);
    if (fp_nan64b(b))
        return fp_from_bits32((uint32_t)((b >> 63) << 31) | 0x7f800000u | FP_QBIT32 | (uint32_t)((b >> 29) & 0x3fffffu));
    return (float)x;
}
static inline double fp_s2d(float x) {
    const uint32_t b = fp_bits32(x);
    if (fp_nan32b(b))
        return fp_from_bits64(((uint64_t)(b >> 31) << 63) | 0x7ff0000000000000ull | FP_QBIT64 | ((uint64_t)(b & 0x3fffffu) << 29));
    return (double)x;
}

/* ---- rounding to integral (FPRoundInt) ---------------------------------------------------------------------------------------- */

/* Nearest, ties to even, whatever the thread's mode (FRINTN, FCVTN*): built from exact operations only. NaN, infinity and values that
 * are integral already (|x| >= 2^23 / 2^52) come back unchanged; a zero result keeps x's sign. */
static inline float fp_rne32(float x) {
    if (!(fabsf(x) < 0x1p23f)) return x;
    const float t = truncf(x), d = x - t;
    if (d > 0.5f || (d == 0.5f && fmodf(t, 2.0f) != 0.0f)) return t + 1.0f;
    if (d < -0.5f || (d == -0.5f && fmodf(t, 2.0f) != 0.0f)) return t - 1.0f;
    return t;
}
static inline double fp_rne64(double x) {
    if (!(fabs(x) < 0x1p52)) return x;
    const double t = trunc(x), d = x - t;
    if (d > 0.5 || (d == 0.5 && fmod(t, 2.0) != 0.0)) return t + 1.0;
    if (d < -0.5 || (d == -0.5 && fmod(t, 2.0) != 0.0)) return t - 1.0;
    return t;
}

/* FRINT<mode>: 1 A (ties away), 2 M (floor), 3 P (ceil), 4 Z (trunc), 5 X / I (the thread's mode); anything else N. A NaN comes
 * back quietened. */
static inline float fp_frint32(float x, int mode) {
    const uint32_t b = fp_bits32(x);
    if (fp_nan32b(b)) return fp_quiet32(b);
    switch (mode) {
    case 1: return roundf(x);
    case 2: return floorf(x);
    case 3: return ceilf(x);
    case 4: return truncf(x);
    case 5: return nearbyintf(x);
    default: return fp_rne32(x);
    }
}
static inline double fp_frint64(double x, int mode) {
    const uint64_t b = fp_bits64(x);
    if (fp_nan64b(b)) return fp_quiet64(b);
    switch (mode) {
    case 1: return round(x);
    case 2: return floor(x);
    case 3: return ceil(x);
    case 4: return trunc(x);
    case 5: return nearbyint(x);
    default: return fp_rne64(x);
    }
}

#undef NX_FRAC52_

#endif /* NXRECOMP_FPEXACT_H */
