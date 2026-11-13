// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// macOS side of src/gfx/extras: DLSS (Streamline) and FidelityFX are Windows / PC-Vulkan SDKs and are not available here, so those
// backends report "not available"; the temporal upscaler on macOS is MetalFX (temporal_metalfx.mm), with FSR 1 as its fallback. The HDR
// query is in display_mac.mm.
#include "extras/temporal.h"

namespace nxrt::extras {

TemporalBackend* temporal_ffx(void*) { return nullptr; }
TemporalBackend* temporal_dlss(void*, bool) { return nullptr; }
bool temporal_dlss_started() { return false; }
PFN_vkGetInstanceProcAddr streamline_preinit() { return nullptr; }

}  // namespace nxrt::extras
