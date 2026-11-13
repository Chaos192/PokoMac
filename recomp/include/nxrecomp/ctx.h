/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Portions Copyright (c) 2026 Chaos192
 * Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
 *
 * nxrecomp: the ABI between lifted code and the runtime.
 *
 *   - the guest CPU context (Ctx) and the 128-bit SIMD&FP register (V128);
 *   - the tables a lifted module hands the runtime (NxModule and the rows it points to);
 *   - the functions the runtime provides to lifted code (C linkage);
 *   - the small inline helpers lifted statements call: memory accesses, the exclusive monitor, ordered accesses, flags, bit
 *     operations, float-to-integer saturation, the spin-wait hint; fpexact.h (exact floating point) and neon.h (Advanced SIMD)
 *     follow at the end.
 *
 * Guest addresses are host addresses: every guest mapping sits at its guest address in the host process, and the host is
 * little-endian like the guest. Lifted code is built with -fno-strict-aliasing -ffp-contract=off (recomp/cmake/NxRecomp.cmake,
 * game-build/release_b/pkp_build.py); nothing here assumes otherwise. Valid GNU C11 and C++17 / C++20 (clang, clang-cl, zig c++).
 * The architectural behaviour each helper implements is named after the Arm Architecture Reference Manual (DDI 0487) pseudocode.
 */
#ifndef NXRECOMP_CTX_H
#define NXRECOMP_CTX_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

/* ---- versions and import ids ------------------------------------------------------------------------------------------------- */

#define NXRECOMP_ABI_VERSION 4      /* what nxrt checks (runtime/include/nxrt/abi.h) */
#define NXRECOMP_ABI_LIVE 5         /* ABI 4 lifted with --live-patch: every function entry checks Ctx.ov (NX_OV) */

/* An import: the module index in bits 31..20, the symbol index in bits 19..0. */
#define NX_IMPORT_ID(module_index, sym) ((((uint32_t)(module_index)) << 20) | ((uint32_t)(sym)))
#define NX_IMPORT_MODULE(id) (((uint32_t)(id)) >> 20)
#define NX_IMPORT_SYM(id) (((uint32_t)(id)) & 0xfffffu)

#define NX_ENTRY_CATCHER 1u         /* NxEntryFn.flags: the function is entered through nx_catch_call */

#if defined(__aarch64__) || defined(_M_ARM64)
#define NX_HOST_ARM64 1
#else
#define NX_HOST_ARM64 0
#endif
#if defined(__x86_64__) || defined(_M_X64)
#define NX_HOST_X64 1
#else
#define NX_HOST_X64 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the guest registers ----------------------------------------------------------------------------------------------------- */

/* One SIMD&FP register. Every view starts at byte 0; element 0 is the lowest address. b/h/w/d/f/df are the tier-1 names, u8..f64
 * the tier-2 ones. */
typedef union V128 {
    uint8_t b[16];
    uint8_t u8[16];
    int8_t s8[16];
    uint16_t h[8];
    uint16_t u16[8];
    int16_t s16[8];
    uint32_t w[4];
    uint32_t u32[4];
    int32_t s32[4];
    uint64_t d[2];
    uint64_t u64[2];
    int64_t s64[2];
    float f[4];
    float f32[4];
    double df[2];
    double f64[2];
} __attribute__((aligned(16))) V128;   /* the union type itself is 16-aligned */

/* The guest thread's CPU state. The layout is ABI (880 bytes, 16-aligned): the runtime, the generated tables and the units recompiled
 * on demand use these offsets. */
typedef struct Ctx {
    uint64_t x[32];             /*   0: X0..X30, and x[31] = SP (XZR is never stored) */
    V128 v[32];                 /* 256: V0..V31 */
    uint32_t N, Z, C, V;        /* 768: PSTATE.NZCV, each 0 or 1 */
    uint64_t fpcr;              /* 784: FPCR as the guest last wrote it */
    uint64_t fpsr;              /* 792: FPSR as the guest last wrote it (exception bits and QC are not accumulated) */
    uint64_t tpidr;             /* 800: TPIDR_EL0 */
    uint64_t tpidrro;           /* 808: TPIDRRO_EL0 (the thread's TLS block) */
    uint64_t ex_addr;           /* 816: the exclusive monitor's address, ~0 when it is closed */
    uint64_t ex_val;            /* 824: what the load-exclusive read (zero-extended; the first register of an LDXP) */
    uint64_t ex_val_hi;         /* 832: the second register of a 64-bit LDXP */
    uint64_t ex_fail;           /* 840: failed store-exclusives in a row (the runtime's stx_diag counts them) */
    uint64_t fn;                /* 848: entry of the lifted function entered last (crash reports) */
    const char *in_import;      /* 856: the host import running on this context, or NULL (runtime) */
    uint64_t import_from;       /* 864: that import call's guest return address (runtime) */
    const void *ov;             /* 872: the live-patch table (NX_OV), NULL while there is none. Stays the last member. */
} Ctx;

/* ---- module tables (written by the lifter, read by the runtime) -------------------------------------------------------------- */

typedef struct {
    uint64_t addr;              /* guest address of a lifted function */
    void (*fn)(Ctx *);
} FnEntry;

typedef struct {
    const char *name;
    uint64_t value;             /* a defined symbol's guest address (TLS and absolute symbols: the raw st_value); 0 if undefined */
    int defined;
} DynSym;

typedef struct {
    uint64_t pad;               /* a landing pad */
    void (*fn)(Ctx *, uint64_t); /* resuming there is fn(c, pad) */
} NxResume;

typedef struct {
    uint64_t start, end;        /* the function's range [start, end) */
    void (*fn)(Ctx *, uint64_t); /* fn(c, addr) enters it at addr */
    uint32_t flags;             /* NX_ENTRY_CATCHER */
} NxEntryFn;

typedef struct {
    uint64_t pc;                /* valid from pc up to the FDE's next row */
    uint8_t cfa_reg;            /* 29 (FP) or 31 (SP) */
    uint8_t nsaved;             /* saved registers of this row */
    int32_t cfa_off;            /* CFA = x[cfa_reg] + cfa_off */
    uint32_t saved_first;       /* index of the row's first NxCfiSaved */
} NxCfiRow;

typedef struct {
    uint8_t reg;                /* 0..30 x0..x30, 64..95 the low half of v0..v31 */
    int32_t off;                /* saved at CFA + off */
} NxCfiSaved;

typedef struct {
    uint64_t start, end;        /* the function [start, end) */
    uint32_t row_first, row_count;
    uint32_t site_first, site_count;
    uint32_t type_first;
} NxFde;

typedef struct {
    uint64_t start, len;        /* the call site [start, start + len) */
    uint64_t pad;               /* landing pad, 0 = none */
    uint32_t action;            /* 0 = cleanup only, else 1 + index into NxEh.actions */
} NxCallSite;

typedef struct {
    int32_t filter;             /* > 0 a catch (types[type_first + filter - 1]), 0 a cleanup, < 0 a spec list (specs[-filter - 1]) */
    uint32_t next;              /* 1 + index of the next action, 0 = last */
} NxAction;

typedef struct {
    const NxFde *fdes;
    int nfdes;
    const NxCfiRow *rows;
    const NxCfiSaved *saved;
    const NxCallSite *sites;
    const NxAction *actions;
    const uint64_t *types;      /* guest typeinfo addresses; 0 = catch (...) */
    const int32_t *type_syms;   /* per type: -1 final, else the NxModule.syms index it is imported by */
    const uint32_t *specs;      /* 0-terminated exception-specification lists */
} NxEh;

typedef struct NxModule NxModule;
struct NxModule {
    const char *name;
    uint32_t index;             /* position in the program's module list (the module half of an import id) */
    uint64_t base;              /* load address */
    uint64_t text_end;          /* one past the module's last code byte */
    const FnEntry *fns;         /* sorted by addr */
    int nfns;
    const DynSym *syms;
    int nsyms;
    const NxResume *resume;     /* sorted by pad */
    int nresume;
    const NxEh *eh;             /* NULL: no unwind data */
    const NxEntryFn *entries;   /* ABI 4: mid-function entries, sorted by start */
    int nentries;
};

/* ---- provided by the runtime ------------------------------------------------------------------------------------------------- */

void call_indirect(Ctx *c, uint64_t addr);
void host_import(Ctx *c, uint32_t import_id);
void guest_trap(const char *what, uint64_t addr);
void guest_unimpl(uint64_t pc, uint32_t insn, const char *what);
void guest_svc(Ctx *c, uint32_t number);
uint64_t guest_mrs(Ctx *c, uint32_t sysreg);
void guest_msr(Ctx *c, uint32_t sysreg, uint64_t value);
void nx_site_inline(Ctx *c, uint64_t addr);
int nx_site_midhook(Ctx *c, uint64_t addr);
int nx_hook_entry(Ctx *c, uint64_t addr);
int nx_hook_resume(Ctx *c, uint64_t entry, uint64_t pc);
int nx_rule_site(Ctx *c, uint64_t addr, const char *rule);
void nx_stack_switch(Ctx *c, uint32_t reg, void (*tail)(Ctx *));
void stx_diag(Ctx *c, uint64_t a, uint64_t expected, uint64_t seen, uint64_t v, int bits);
void nx_no_entry(Ctx *c, uint64_t addr);
void nx_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t));
void nx_longjmp(Ctx *c, uint64_t target);
void nx_frame_call(Ctx *c, void (*body)(Ctx *, uint64_t), uint64_t rp);
void nx_stack_unwind(Ctx *c, uint64_t resume, void (*body)(Ctx *, uint64_t));

/* ---- inlining ---------------------------------------------------------------------------------------------------------------- */
/* Every helper from here on (and neon.h's, and fpexact.h's arithmetic, declared below) stands for one guest instruction on the
 * lifted code's hot path, so it is inlined at every call. Left to its cost model, clang at the game build's -O1 keeps some out of
 * line in blocks it judges cold (a lifted function has thousands of blocks), and then a flag-setting compare is a call that passes
 * the flags through memory (Pokopia's game build, 2026-11-13: adds32 / adds64 and the FP arithmetic out of line). */
#if defined(__clang__)
#pragma clang attribute push(__attribute__((always_inline)), apply_to = function)
#endif

/* ---- plain memory accesses --------------------------------------------------------------------------------------------------- */
/* LDR / STR: any alignment (a memcpy), one host access of that width each, freely optimised like any non-volatile access. */

static inline uint8_t rd8(uint64_t a) { uint8_t v; memcpy(&v, (const void *)(uintptr_t)a, sizeof v); return v; }
static inline uint16_t rd16(uint64_t a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, sizeof v); return v; }
static inline uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, sizeof v); return v; }
static inline uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, sizeof v); return v; }
static inline void wr8(uint64_t a, uint8_t v) { memcpy((void *)(uintptr_t)a, &v, sizeof v); }
static inline void wr16(uint64_t a, uint16_t v) { memcpy((void *)(uintptr_t)a, &v, sizeof v); }
static inline void wr32(uint64_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, sizeof v); }
static inline void wr64(uint64_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, sizeof v); }

/* SIMD&FP loads and stores of n = 1, 2, 4, 8 or 16 bytes: a load zeroes the bytes of the register above n. */
static inline V128 rdv(uint64_t a, int n) {
    V128 r;
    memset(&r, 0, sizeof r);
    memcpy(&r, (const void *)(uintptr_t)a, (size_t)n);
    return r;
}
static inline void wrv(uint64_t a, const V128 *v, int n) {
    void *dst = (void *)(uintptr_t)a;
    memcpy(dst, v->b, (size_t)n);
}
static inline void vzero(V128 *v) { memset(v, 0, sizeof *v); }

/* ---- poll loads (the plain loads of a busy-wait loop, rewritten by nxrecomp/spin.py) ------------------------------------------ */
/* Each is one relaxed atomic load, so it is really repeated on every pass of the loop around it. */

static inline uint8_t rdp8(uint64_t a) { return __atomic_load_n((const uint8_t *)(uintptr_t)a, __ATOMIC_RELAXED); }
static inline uint16_t rdp16(uint64_t a) { return __atomic_load_n((const uint16_t *)(uintptr_t)a, __ATOMIC_RELAXED); }
static inline uint32_t rdp32(uint64_t a) { return __atomic_load_n((const uint32_t *)(uintptr_t)a, __ATOMIC_RELAXED); }
static inline uint64_t rdp64(uint64_t a) { return __atomic_load_n((const uint64_t *)(uintptr_t)a, __ATOMIC_RELAXED); }

/* The vector form: lane 0 for 1..8 bytes; 16 bytes are two 64-bit loads in address order (a 16-byte atomic load would be a
 * cmpxchg16b on x86-64, which writes). */
static inline V128 rdvp(uint64_t a, int n) {
    V128 r;
    memset(&r, 0, sizeof r);
    switch (n) {
    case 1: r.b[0] = rdp8(a); break;
    case 2: r.h[0] = rdp16(a); break;
    case 4: r.w[0] = rdp32(a); break;
    case 8: r.d[0] = rdp64(a); break;
    default:
        r.d[0] = rdp64(a);
        r.d[1] = rdp64(a + 8);
        break;
    }
    return r;
}

/* ---- the exclusive monitor --------------------------------------------------------------------------------------------------- */
/* Modelled by value (Arm ARM B2.9): a store-exclusive succeeds when the monitor is open on that address and memory still holds what
 * the load-exclusive read; it is then one compare-and-swap. The load side (ex_addr / ex_val) is written by the lifted statements. A
 * write of the same value in between is not seen (ABA, recomp/docs/GAPS.md). Results are STXR's status: 0 stored, 1 failed. */

#define NX_STX_BODY(N, T)                                                                                                         \
    T nx_seen_ = (T)c->ex_val;                                                                                                    \
    if (c->ex_addr != a) {                                                                                                        \
        stx_diag(c, a, 0, 0, (uint64_t)v, -(N));                                                                                  \
        return 1;                                                                                                                 \
    }                                                                                                                             \
    c->ex_addr = ~(uint64_t)0;                                                                                                    \
    if (__atomic_compare_exchange_n((T *)(uintptr_t)a, &nx_seen_, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {                    \
        c->ex_fail = 0;                                                                                                           \
        return 0;                                                                                                                 \
    }                                                                                                                             \
    stx_diag(c, a, c->ex_val, (uint64_t)nx_seen_, (uint64_t)v, (N));                                                              \
    return 1;

static inline uint32_t stx8(Ctx *c, uint64_t a, uint8_t v) { NX_STX_BODY(8, uint8_t) }
static inline uint32_t stx16(Ctx *c, uint64_t a, uint16_t v) { NX_STX_BODY(16, uint16_t) }
static inline uint32_t stx32(Ctx *c, uint64_t a, uint32_t v) { NX_STX_BODY(32, uint32_t) }
static inline uint32_t stx64(Ctx *c, uint64_t a, uint64_t v) { NX_STX_BODY(64, uint64_t) }
#undef NX_STX_BODY

/* LDXP / LDAXP of two 64-bit registers: one 16-byte acquire load (on x86-64 a lock cmpxchg16b: the location must be writable). */
static inline void ldxp128(Ctx *c, uint64_t a, uint64_t *lo, uint64_t *hi) {
    unsigned __int128 *p = (unsigned __int128 *)(uintptr_t)a;
    const unsigned __int128 v = __atomic_load_n(p, __ATOMIC_ACQUIRE);
    c->ex_addr = a;
    c->ex_val = (uint64_t)v;
    c->ex_val_hi = (uint64_t)(v >> 64);
    *lo = (uint64_t)v;
    *hi = (uint64_t)(v >> 64);
}

/* STXP of two 32-bit registers: one 64-bit store-exclusive. */
static inline uint32_t stxp32(Ctx *c, uint64_t a, uint32_t lo, uint32_t hi) {
    return stx64(c, a, (uint64_t)lo | ((uint64_t)hi << 32));
}

/* STXP of two 64-bit registers: one 16-byte compare-and-swap. Unlike stx<N>: no stx_diag when only the value differs, and a success
 * leaves ex_fail as it is. */
static inline uint32_t stxp64(Ctx *c, uint64_t a, uint64_t lo, uint64_t hi) {
    if (c->ex_addr != a) {
        stx_diag(c, a, 0, 0, lo, -128);
        return 1;
    }
    c->ex_addr = ~(uint64_t)0;
    unsigned __int128 seen = ((unsigned __int128)c->ex_val_hi << 64) | c->ex_val;
    unsigned __int128 want = ((unsigned __int128)hi << 64) | lo;
    return __atomic_compare_exchange_n((unsigned __int128 *)(uintptr_t)a, &seen, want, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0u : 1u;
}

/* ---- ordered accesses: LDAR and STLR ----------------------------------------------------------------------------------------- */
/* Load-Acquire / Store-Release (Arm ARM B2.3), plus the two orders the console's cores keep beyond them (docs/ENGINES.md "Memory
 * ordering"). NX_ORDER_MODE (the build may set it; tools/lifter-tests/litmus_order.cpp compares the two):
 *   1 (default)  LDAR = a full fence, then a sequentially consistent load. STLR = a sequentially consistent store, and on an arm64
 *                host a full fence after it (x86-64's XCHG is already one).
 *   0            LDAR = an acquire load; STLR = a sequentially consistent store, nothing else. */
#ifndef NX_ORDER_MODE
#define NX_ORDER_MODE 1
#endif

#if NX_ORDER_MODE >= 1
#define NX_LOAD_ACQUIRE_(T, a) (__atomic_thread_fence(__ATOMIC_SEQ_CST), __atomic_load_n((const T *)(uintptr_t)(a), __ATOMIC_SEQ_CST))
#if NX_HOST_ARM64
#define NX_STORE_RELEASE_(T, a, v) (__atomic_store_n((T *)(uintptr_t)(a), (v), __ATOMIC_SEQ_CST), __atomic_thread_fence(__ATOMIC_SEQ_CST))
#else
#define NX_STORE_RELEASE_(T, a, v) __atomic_store_n((T *)(uintptr_t)(a), (v), __ATOMIC_SEQ_CST)
#endif
#else
#define NX_LOAD_ACQUIRE_(T, a) __atomic_load_n((const T *)(uintptr_t)(a), __ATOMIC_ACQUIRE)
#define NX_STORE_RELEASE_(T, a, v) __atomic_store_n((T *)(uintptr_t)(a), (v), __ATOMIC_SEQ_CST)
#endif

static inline uint8_t ldar8(uint64_t a) { return NX_LOAD_ACQUIRE_(uint8_t, a); }
static inline uint16_t ldar16(uint64_t a) { return NX_LOAD_ACQUIRE_(uint16_t, a); }
static inline uint32_t ldar32(uint64_t a) { return NX_LOAD_ACQUIRE_(uint32_t, a); }
static inline uint64_t ldar64(uint64_t a) { return NX_LOAD_ACQUIRE_(uint64_t, a); }
static inline void stlr8(uint64_t a, uint8_t v) { NX_STORE_RELEASE_(uint8_t, a, v); }
static inline void stlr16(uint64_t a, uint16_t v) { NX_STORE_RELEASE_(uint16_t, a, v); }
static inline void stlr32(uint64_t a, uint32_t v) { NX_STORE_RELEASE_(uint32_t, a, v); }
static inline void stlr64(uint64_t a, uint64_t v) { NX_STORE_RELEASE_(uint64_t, a, v); }
#undef NX_LOAD_ACQUIRE_
#undef NX_STORE_RELEASE_

/* ---- live-patch checks (lift --live-patch, ABI 5) ---------------------------------------------------------------------------- */
/* Ctx.ov, when set, is one bitmap pointer per module index; bit km of byte kb stands for the function's first code word. A set bit
 * hands the call to the runtime (nx_hook_entry at a function entry, nx_hook_resume at a loop head), and a nonzero answer means the
 * patched code did the work: the lifted function returns. Nothing but one load and a branch while no patch exists. */
#define NX_OV(c, mi, kb, km, addr)                                                                                                \
    do {                                                                                                                          \
        const uint8_t *const *nx_ov_ = (const uint8_t *const *)(c)->ov;                                                           \
        if (__builtin_expect(nx_ov_ != 0, 0)) {                                                                                   \
            const uint8_t *nx_bits_ = nx_ov_[(mi)];                                                                               \
            if (nx_bits_ && (nx_bits_[(kb)] & (km)) && nx_hook_entry((c), (addr))) return;                                        \
        }                                                                                                                         \
    } while (0)

#define NX_OVL(c, mi, kb, km, entry, pc)                                                                                          \
    do {                                                                                                                          \
        const uint8_t *const *nx_ov_ = (const uint8_t *const *)(c)->ov;                                                           \
        if (__builtin_expect(nx_ov_ != 0, 0)) {                                                                                   \
            const uint8_t *nx_bits_ = nx_ov_[(mi)];                                                                               \
            if (nx_bits_ && (nx_bits_[(kb)] & (km)) && nx_hook_resume((c), (entry), (pc))) return;                                \
        }                                                                                                                         \
    } while (0)

/* ---- flags ------------------------------------------------------------------------------------------------------------------- */

/* On arm64 hosts a compare's operands are hidden from the optimiser, so it cannot conclude "operand == register" from the compare and
 * then address a later load through the other register: that would lose the guest's address dependency (Arm ARM B2.3, dependency
 * ordering; tools/lifter-tests/litmus_dep.py checks it). */
#if NX_HOST_ARM64
#define NX_HIDE_VALUE_(v) __asm__("" : "+r"(v))
#else
#define NX_HIDE_VALUE_(v) ((void)0)
#endif

/* AddWithCarry with NZCV. SUBS / CMP / SBCS pass b = NOT(operand), and cin 1 or C. */
static inline uint64_t adds64(Ctx *c, uint64_t a, uint64_t b, uint32_t cin) {
    NX_HIDE_VALUE_(a);
    NX_HIDE_VALUE_(b);
    unsigned __int128 wide = (unsigned __int128)a + b + cin;
    uint64_t r = (uint64_t)wide;
    c->N = (int64_t)r < 0;
    c->Z = r == 0;
    c->C = (uint32_t)(wide >> 64);
    c->V = (int64_t)((a ^ r) & (b ^ r)) < 0;                 /* both operands differ in sign from the result */
    return r;
}

static inline uint32_t adds32(Ctx *c, uint32_t a, uint32_t b, uint32_t cin) {
    NX_HIDE_VALUE_(a);
    NX_HIDE_VALUE_(b);
    const uint64_t sum = (uint64_t)a + (uint64_t)b + (uint64_t)cin;
    uint32_t r = (uint32_t)sum;
    c->N = r >> 31;
    c->Z = r == 0;
    c->C = sum > 0xffffffffull;
    c->V = (int32_t)((a ^ r) & (b ^ r)) < 0;
    return r;
}
#undef NX_HIDE_VALUE_

/* ANDS / BICS / TST: N and Z from the result, C and V cleared. */
static inline uint64_t logic64(Ctx *c, uint64_t r) {
    c->N = (int64_t)r < 0;
    c->Z = r == 0;
    c->C = 0;
    c->V = 0;
    return r;
}

static inline uint32_t logic32(Ctx *c, uint32_t r) {
    c->N = r >> 31;
    c->Z = r == 0;
    c->C = 0;
    c->V = 0;
    return r;
}

/* NZCV from a 4-bit value (bit 3 N .. bit 0 V); other bits are ignored. */
static inline void set_nzcv(Ctx *c, int f) {
    c->N = (uint32_t)(f >> 3) & 1u;
    c->Z = (uint32_t)(f >> 2) & 1u;
    c->C = (uint32_t)(f >> 1) & 1u;
    c->V = (uint32_t)f & 1u;
}

/* The NZCV system register: N in bit 31 .. V in bit 28. */
static inline uint64_t get_nzcv(const Ctx *c) {
    return ((uint64_t)c->N << 31) | ((uint64_t)c->Z << 30) | ((uint64_t)c->C << 29) | ((uint64_t)c->V << 28);
}

/* ConditionHolds(cc): 1 or 0. */
static inline int cond(const Ctx *c, int cc) {
    int r;
    switch ((cc >> 1) & 7) {
    case 0: r = c->Z == 1; break;                                   /* EQ / NE */
    case 1: r = c->C == 1; break;                                   /* CS / CC */
    case 2: r = c->N == 1; break;                                   /* MI / PL */
    case 3: r = c->V == 1; break;                                   /* VS / VC */
    case 4: r = c->C == 1 && c->Z == 0; break;                      /* HI / LS */
    case 5: r = c->N == c->V; break;                                /* GE / LT */
    case 6: r = c->N == c->V && c->Z == 0; break;                   /* GT / LE */
    default: return 1;                                              /* AL / NV */
    }
    if (cc & 1) r = !r;                                             /* the odd code is the negation */
    return r;
}

/* FCMP / FCMPE as NZCV, by the host's IEEE comparison in the thread's FP environment (on x86-64 DAZ, i.e. the guest's FZ, makes a
 * denormal compare as zero, as FPUnpack does with FZ). Unordered 0011, equal 0110, less 1000, greater 0010. */
static inline void fcmp(Ctx *c, double a, double b) {
    if (a != a || b != b) set_nzcv(c, 0x3);
    else if (a == b) set_nzcv(c, 0x6);
    else if (a < b) set_nzcv(c, 0x8);
    else set_nzcv(c, 0x2);
}

/* ---- bit operations ---------------------------------------------------------------------------------------------------------- */

static inline uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31u;
    return n ? (v >> n) | (v << (32u - n)) : v;
}
static inline uint64_t ror64(uint64_t v, unsigned n) {
    n &= 63u;
    return n ? (v >> n) | (v << (64u - n)) : v;
}
static inline uint32_t bswap32(uint32_t x) { const uint32_t swapped = __builtin_bswap32(x); return swapped; }
static inline uint64_t bswap64(uint64_t x) { const uint64_t swapped = __builtin_bswap64(x); return swapped; }
static inline uint64_t rev32_64(uint64_t v) {
    return (uint64_t)__builtin_bswap32((uint32_t)v) | ((uint64_t)__builtin_bswap32((uint32_t)(v >> 32)) << 32);
}
static inline uint32_t rev16_32(uint32_t v) { return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu); }
static inline uint64_t rev16_64(uint64_t v) {
    return ((v & 0x00ff00ff00ff00ffull) << 8) | ((v >> 8) & 0x00ff00ff00ff00ffull);
}
static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32u; }
static inline uint64_t clz64(uint64_t v) { return v ? (uint64_t)__builtin_clzll(v) : 64u; }
/* CountLeadingSignBits: the bits below the top one that equal it. */
static inline uint32_t cls32(uint32_t v) { return clz32(v ^ (uint32_t)((int32_t)v >> 31)) - 1u; }
static inline uint64_t cls64(uint64_t v) { return clz64(v ^ (uint64_t)((int64_t)v >> 63)) - 1u; }
static inline uint64_t rbit64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0f0f0f0f0f0f0f0full) | ((v & 0x0f0f0f0f0f0f0f0full) << 4);
    return __builtin_bswap64(v);
}
static inline uint32_t rbit32(uint32_t v) { return (uint32_t)(rbit64(v) >> 32); }

static inline uint64_t umulh(uint64_t a, uint64_t b) {
    const unsigned __int128 product = (unsigned __int128)a * (unsigned __int128)b;
    return (uint64_t)(product >> 64);
}
static inline uint64_t smulh(uint64_t a, uint64_t b) {
    return (uint64_t)(((__int128)(int64_t)a * (__int128)(int64_t)b) >> 64);
}

/* SDIV: a zero divisor gives 0; a divisor of -1 negates with wrap-around, so most negative / -1 is the dividend. */
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (b == 0) return 0;
    if (b == 0xffffffffu) return 0u - a;
    const int32_t q = (int32_t)a / (int32_t)b;
    return (uint32_t)q;
}
static inline uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (b == 0) return 0;
    if (b == ~0ull) return 0ull - a;
    const int64_t q = (int64_t)a / (int64_t)b;
    return (uint64_t)q;
}

/* ---- float to integer, saturating (FPToFixed after its rounding step: the caller has rounded v already) ------------------------ */
/* A NaN gives 0; out-of-range values give the nearest bound. */

static inline uint32_t f2s32(double v) {
    if (v != v) return 0;
    if (v >= 2147483647.0) return 0x7fffffffu;
    if (v <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)v;
}
static inline uint32_t f2u32(double v) {
    if (v != v || v <= 0.0) return 0;
    if (v >= 4294967295.0) return 0xffffffffu;
    return (uint32_t)v;
}
static inline uint64_t f2s64(double v) {
    if (v != v) return 0;
    if (v >= 9223372036854775808.0) return 0x7fffffffffffffffull;
    if (v <= -9223372036854775808.0) return 0x8000000000000000ull;
    return (uint64_t)(int64_t)v;
}
static inline uint64_t f2u64(double v) {
    if (v != v || v <= 0.0) return 0;
    if (v >= 18446744073709551616.0) return ~0ull;
    return (uint64_t)v;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

/* ---- spin-wait hint (WFE / YIELD) -------------------------------------------------------------------------------------------- */
/* kind 1 = WFE, 0 = YIELD. In C++ the runtime may install a hook (runtime/src/kernel/busy_mutex.cpp; units recompiled on demand get
 * the runtime's through NxOdApi): one instance per linked image, read on every call. Without one: a PAUSE on x86-64, nothing
 * elsewhere. Either way the guest state does not change. */
#ifdef __cplusplus
inline void (*nx_spin_hook)(int) {};                 /* none until the runtime sets one */

static inline void nx_spin_hint(int kind) {
    void (*hook)(int) = nx_spin_hook;
    if (hook) {
        hook(kind);
        return;
    }
#if NX_HOST_X64
    __builtin_ia32_pause();
#endif
}
#else
static inline void nx_spin_hint(int unused) { (void)unused; }
#endif

#if defined(__clang__)
#pragma clang attribute pop
#endif

/* fpexact.h's arithmetic, inlined like the helpers above. Left to clang: its cold paths (fp_nan_result*, the software FMA, the FMA3
 * test, the estimate tables) and fp_maxmin* / fp_fma* / nx_fma*, whose results for denormal inputs under FZ change with the code
 * around them when forced inline (the host's flush-to-zero meets the optimiser); out of line they keep the behaviour the
 * differential pins. */
#if defined(__clang__) || defined(__GNUC__)
#define NX_ALWAYS_INLINE_ __attribute__((always_inline))
#else
#define NX_ALWAYS_INLINE_
#endif
static inline float fp_add32(float a, float b) NX_ALWAYS_INLINE_;
static inline float fp_sub32(float a, float b) NX_ALWAYS_INLINE_;
static inline float fp_mul32(float a, float b) NX_ALWAYS_INLINE_;
static inline float fp_div32(float a, float b) NX_ALWAYS_INLINE_;
static inline double fp_add64(double a, double b) NX_ALWAYS_INLINE_;
static inline double fp_sub64(double a, double b) NX_ALWAYS_INLINE_;
static inline double fp_mul64(double a, double b) NX_ALWAYS_INLINE_;
static inline double fp_div64(double a, double b) NX_ALWAYS_INLINE_;
static inline float fp_sqrt32(float a) NX_ALWAYS_INLINE_;
static inline double fp_sqrt64(double a) NX_ALWAYS_INLINE_;
static inline float fp_neg32(float a) NX_ALWAYS_INLINE_;
static inline float fp_abs32(float a) NX_ALWAYS_INLINE_;
static inline double fp_neg64(double a) NX_ALWAYS_INLINE_;
static inline double fp_abs64(double a) NX_ALWAYS_INLINE_;
static inline float fp_recps32(float a, float b) NX_ALWAYS_INLINE_;
static inline double fp_recps64(double a, double b) NX_ALWAYS_INLINE_;
static inline float fp_rsqrts32(float a, float b) NX_ALWAYS_INLINE_;
static inline double fp_rsqrts64(double a, double b) NX_ALWAYS_INLINE_;
static inline float fp_d2s(double x) NX_ALWAYS_INLINE_;
static inline double fp_s2d(float x) NX_ALWAYS_INLINE_;
static inline float fp_rne32(float x) NX_ALWAYS_INLINE_;
static inline double fp_rne64(double x) NX_ALWAYS_INLINE_;
static inline float fp_frint32(float x, int mode) NX_ALWAYS_INLINE_;
static inline double fp_frint64(double x, int mode) NX_ALWAYS_INLINE_;
#undef NX_ALWAYS_INLINE_

#include "fpexact.h"

#if defined(__clang__)
#pragma clang attribute push(__attribute__((always_inline)), apply_to = function)
#endif
#include "neon.h"
#if defined(__clang__)
#pragma clang attribute pop
#endif

#endif /* NXRECOMP_CTX_H */
