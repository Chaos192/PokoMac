// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// CPU-side constant setup for AMD FidelityFX FSR 1 (EASU upscale and RCAS sharpen), ported from the FidelityFX SDK v1.1.4
// headers in third_party/ffx_fsr1 (MIT licence, see third_party/ffx_fsr1/LICENSE.txt). The constants are what the GPU
// shaders in extras_shaders.inc read from their push constants (kFsrEasuComp, kFsrRcasComp).
#pragma once

#include <cstdint>

namespace nxrt::extras {

// Port of ffxFsrPopulateEasuConstants (fsr1/ffx_fsr1.h). in_view_* is the source viewport in pixels, in_tex_* the source
// texture size in pixels, out_* the output size in pixels. Each word is the bit pattern of a float, as the SDK stores it.
void fsr1_easu_consts(uint32_t con0[4], uint32_t con1[4], uint32_t con2[4], uint32_t con3[4], float in_view_w, float in_view_h,
                      float in_tex_w, float in_tex_h, float out_w, float out_h);

// Port of FsrRcasCon (fsr1/ffx_fsr1.h). sharpness_stops: 0 = sharpest, larger = softer (each stop halves the sharpening).
// con[0] is the bit pattern of exp2(-sharpness_stops); con[1] packs that value as two binary16 halves; con[2] and con[3] are 0.
void fsr1_rcas_consts(uint32_t con[4], float sharpness_stops);

}  // namespace nxrt::extras
