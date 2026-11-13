/* SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
 *
 * nxrecomp: code recompiled on demand (nxrecomp/ondemand.py; docs/ARCHITECTURE.md, "Code the lift did not see").
 *
 * When the dispatcher meets a guest address that no table of the build covers, the runtime lifts the function holding it with the same
 * lifter and the same lift map as the build, compiles it with the build's toolchain into a small shared library (a "unit") and loads it.
 * A unit links against nothing: the runtime functions its code calls (ctx.h "runtime-provided") are defined in the unit as forwards
 * through the NxOdApi table the runtime hands to the unit's one export, so the same unit loads into any build model (the release's
 * pkp_rt library, a statically linked development exe, a test) on Windows, macOS and Linux.
 *
 *   const NxOdUnit *nxod_unit(const NxOdApi *api);     exported as NXOD_ENTRY; null when the api is older than the unit needs
 *
 * Valid C11 and C++17.
 */
#ifndef NXRECOMP_ONDEMAND_H
#define NXRECOMP_ONDEMAND_H

#include "ctx.h"

#define NXOD_VERSION 1          /* bump for any change of NxOdApi or NxOdUnit */
#define NXOD_ENTRY "nxod_unit"

#ifdef __cplusplus
extern "C" {
#endif

/* What the runtime gives a unit: its own runtime-provided functions (ctx.h). */
typedef struct NxOdApi {
    uint32_t size;                                   /* sizeof(NxOdApi) of the runtime */
    uint32_t abi;                                    /* NXRECOMP_ABI_VERSION of the runtime */
    void (*call_indirect)(Ctx *, uint64_t);
    void (*host_import)(Ctx *, uint32_t);
    void (*guest_trap)(const char *, uint64_t);
    void (*guest_unimpl)(uint64_t, uint32_t, const char *);
    void (*guest_svc)(Ctx *, uint32_t);
    uint64_t (*guest_mrs)(Ctx *, uint32_t);
    void (*guest_msr)(Ctx *, uint32_t, uint64_t);
    void (*nx_site_inline)(Ctx *, uint64_t);
    int (*nx_site_midhook)(Ctx *, uint64_t);
    int (*nx_hook_entry)(Ctx *, uint64_t);
    void (*stx_diag)(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
    void (*nx_catch_call)(Ctx *, void (*)(Ctx *, uint64_t));
    void (*nx_no_entry)(Ctx *, uint64_t);
    void (*spin_hook)(int);                          /* the runtime's nx_spin_hook when the unit is loaded (may be null) */
} NxOdApi;

/* What a unit gives the runtime. */
typedef struct NxOdUnit {
    uint32_t version;                                /* NXOD_VERSION */
    uint32_t abi;                                    /* NXRECOMP_ABI_VERSION the unit was built with */
    const char *module;                              /* the module it belongs to ("" for code made at run time) */
    uint64_t base;                                   /* that module's base when it was lifted */
    uint64_t addr;                                   /* the address the unit was made for */
    uint64_t start, end;                             /* the guest range it lifted */
    const FnEntry *fns; int nfns;                    /* addresses that are now functions (fn_register) */
    const NxEntryFn *entries; int nentries;          /* the function and its mid-function entries (every instruction) */
} NxOdUnit;

typedef const NxOdUnit *(*NxOdEntryFn)(const NxOdApi *api);

/* A module unit: a whole module the game loads while it runs (an NRO: the jit service's plugin) lifted on demand, as the build lifts its
 * own modules (discovery, unwind tables, jump tables). Its one export, nxod_module, binds the unit's runtime calls like nxod_unit and
 * returns the module's NxModule, which the runtime registers as it registers the build's (nxmodule_register).
 *   const NxOdModuleUnit *nxod_module(const NxOdApi *api);     exported as NXOD_MODULE_ENTRY */
#define NXOD_MODULE_ENTRY "nxod_module"
typedef struct NxOdModuleUnit {
    uint32_t version;                                /* NXOD_VERSION */
    uint32_t abi;                                    /* NXRECOMP_ABI_VERSION the module was lifted and built with */
    const NxModule *module;
} NxOdModuleUnit;
typedef const NxOdModuleUnit *(*NxOdModuleEntryFn)(const NxOdApi *api);

#ifdef __cplusplus
}
#endif

#endif /* NXRECOMP_ONDEMAND_H */
