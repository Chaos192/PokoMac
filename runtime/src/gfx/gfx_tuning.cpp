// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "nxrt/gfx_tuning.h"

namespace nxrt::gfx {

Tuning& tuning() {
    static Tuning t;
    return t;
}

}  // namespace nxrt::gfx
