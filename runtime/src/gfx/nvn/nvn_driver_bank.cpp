// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NVN driver constant bank c[0] (declarations in nvn_driver_bank.h).
#include "nvn_driver_bank.h"

#include <algorithm>
#include <cstring>

namespace nxrt::nvn::be {
namespace {

// One table row as GLSLC reads it: 64-bit address, then the 32-bit size (pokopia-native put_range).
void put_range(uint8_t* d, uint32_t off, const DriverRange& r) {
    if (off + 12 > kDriverBankBytes) return;
    const uint32_t size = static_cast<uint32_t>(std::min<uint64_t>(r.size, 0xffffffffu));
    std::memcpy(d + off, &r.addr, 8);
    std::memcpy(d + off + 8, &size, 4);
}

void put_handle(uint8_t* d, uint32_t off, uint64_t h) {
    if (off + 4 > kDriverBankBytes) return;
    const uint32_t lo = static_cast<uint32_t>(h);   // the 32-bit hardware handle (texture id | sampler id << 20)
    std::memcpy(d + off, &lo, 4);
}

}  // namespace

Nvn2ComputeTable nvn2_compute_table(const ShaderControl& c) {
    Nvn2ComputeTable t;
    t.ubo0 = 0x10;
    t.ssbo0 = t.ubo0 + 0x10 * c.c0_ubos;
    t.tex0 = t.ssbo0 + 0x10 * c.c0_ssbos;
    t.img0 = t.tex0 + 8 * c.c0_textures;
    t.end = t.img0 + 8 * c.c0_images;
    return t;
}

uint32_t fill_driver_bank_nvn2_compute(uint8_t* d, const ShaderControl& c, const DriverRange ubo[16], const DriverRange ssbo[16],
                                       const uint64_t tex[64], const uint64_t img[16]) {
    std::memset(d, 0, kDriverBankBytes);
    const Nvn2ComputeTable t = nvn2_compute_table(c);
    for (int n = 0; n < std::min<int>(c.c0_ubos, 16); ++n) put_range(d, t.ubo0 + 0x10 * n, ubo[n]);
    for (int n = 0; n < std::min<int>(c.c0_ssbos, 16); ++n) put_range(d, t.ssbo0 + 0x10 * n, ssbo[n]);
    for (int n = 0; n < std::min<int>(c.c0_textures, 64); ++n) put_handle(d, t.tex0 + 8 * n, tex[n]);
    for (int n = 0; n < std::min<int>(c.c0_images, 16); ++n) put_handle(d, t.img0 + 8 * n, img[n]);
    return static_cast<uint32_t>(std::min<int>(t.end, kDriverBankBytes));
}

uint32_t fill_driver_bank_graphics(uint8_t* d, int nvn_stage, const DriverRange ssbo[16], uint32_t base_instance) {
    std::memset(d, 0, kDriverBankBytes);
    // NVN stage -> hardware stage (Ryujinx's order): vertex 0, tess control 1, tess evaluation 2, geometry 3, fragment 4
    static const int kHw[5] = {0, 4, 3, 1, 2};
    if (nvn_stage >= 0 && nvn_stage < 5)
        for (int n = 0; n < 16; ++n) put_range(d, 0x110 + 0x100 * kHw[nvn_stage] + 0x10 * n, ssbo[n]);
    std::memcpy(d + kDriverBankPointSize, &kDriverBankMaxPointSize, 4);
    std::memcpy(d + kDriverBankBaseInstance, &base_instance, 4);
    return kDriverBankBaseInstance + 4;
}

}  // namespace nxrt::nvn::be
