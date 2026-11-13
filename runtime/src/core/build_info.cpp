// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "nxrt/abi.h"
namespace nxrt {
#define NXRT_STR2(x) #x
#define NXRT_STR(x) NXRT_STR2(x)
const char* abi_source() { return "recomp/include/nxrecomp/ctx.h, ABI version " NXRT_STR(NXRECOMP_ABI_VERSION); }
int abi_version() { return NXRECOMP_ABI_VERSION; }
}  // namespace nxrt
