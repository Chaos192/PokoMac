// SPDX-License-Identifier: GPL-3.0-or-later
// macOS side of src/gfx/extras: DLSS (Streamline) and FidelityFX are Windows / PC-Vulkan SDKs and are not available here, so those
// backends report "not available" and the upscaler settings fall back to the built-in ones (FSR 1, NIS). The HDR query is in display_mac.mm.
#include "extras/temporal.h"

namespace nxrt::extras {

TemporalBackend* temporal_ffx(void*) { return nullptr; }
TemporalBackend* temporal_dlss(void*, bool) { return nullptr; }
bool temporal_dlss_started() { return false; }
PFN_vkGetInstanceProcAddr streamline_preinit() { return nullptr; }

}  // namespace nxrt::extras
