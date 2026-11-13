// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Camera aim (pkp_aim.cpp): ability and tool targets are aimed where the camera looks, not where the character faces. [pkp] camera_aim,
// on by default.
#pragma once
#include <cstdint>

namespace nxrt::games::pkp {

void aim_install(uint64_t base);

}  // namespace nxrt::games::pkp
