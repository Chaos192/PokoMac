// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NVNvertexAttribState and NVNvertexStreamState: plain data in the guest object. The bit layout follows the one that
// smo-recomp runtime/gfx/nvn_gl.c (GPL-3.0) uses for the same objects (checked there against a real game): attribute word =
// format in bits 0..7, stream index in bits 8..12, relative offset from bit 14; stream = stride (u32) then divisor (u32).
// This file is also the reference for the other plain-data families: layout comment, NVN_REG list, decode().
#include <cstring>

#include "nvn_impl.h"

namespace nxrt::nvn {
namespace {

inline uint32_t rd32(const void* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline void wr32(void* p, uint32_t v) { std::memcpy(p, &v, 4); }

struct StreamData { uint32_t stride, divisor; };
static_assert(sizeof(StreamData) <= sizeof(NVNvertexStreamState), "vertex stream layout fits the NVN object");
static_assert(sizeof(uint32_t) <= sizeof(NVNvertexAttribState), "vertex attribute layout fits the NVN object");

void nvnVertexAttribStateSetDefaults(NVNvertexAttribState* s) { wr32(s, 0); }
void nvnVertexAttribStateSetFormat(NVNvertexAttribState* s, NVNformat format, ptrdiff_t relative_offset) {
    const uint32_t keep = rd32(s) & 0x1f00u;   // the stream index
    wr32(s, keep | (static_cast<uint32_t>(format) & 0xffu) | (static_cast<uint32_t>(relative_offset) << 14));
}
void nvnVertexAttribStateSetStreamIndex(NVNvertexAttribState* s, int stream) {
    wr32(s, (rd32(s) & ~0x1f00u) | ((static_cast<uint32_t>(stream) & 31u) << 8));
}
void nvnVertexAttribStateGetFormat(const NVNvertexAttribState* s, NVNformat* format, ptrdiff_t* relative_offset) {
    const uint32_t w = rd32(s);
    if (format) *format = static_cast<NVNformat>(w & 0xffu);
    if (relative_offset) *relative_offset = static_cast<ptrdiff_t>(w >> 14);
}
int nvnVertexAttribStateGetStreamIndex(const NVNvertexAttribState* s) { return static_cast<int>((rd32(s) >> 8) & 31u); }

void nvnVertexStreamStateSetDefaults(NVNvertexStreamState* s) { std::memset(s, 0, sizeof(StreamData)); }
void nvnVertexStreamStateSetStride(NVNvertexStreamState* s, ptrdiff_t stride) { wr32(reinterpret_cast<uint8_t*>(s), static_cast<uint32_t>(stride)); }
void nvnVertexStreamStateSetDivisor(NVNvertexStreamState* s, int divisor) { wr32(reinterpret_cast<uint8_t*>(s) + 4, static_cast<uint32_t>(divisor)); }
ptrdiff_t nvnVertexStreamStateGetStride(const NVNvertexStreamState* s) { return static_cast<ptrdiff_t>(rd32(s)); }
int nvnVertexStreamStateGetDivisor(const NVNvertexStreamState* s) { return static_cast<int>(rd32(reinterpret_cast<const uint8_t*>(s) + 4)); }

}  // namespace

VertexAttribInfo decode(const NVNvertexAttribState* s) {
    const uint32_t w = rd32(s);
    VertexAttribInfo i;
    i.format = static_cast<int>(w & 0xffu);
    i.stream = static_cast<int>((w >> 8) & 31u);
    i.relative_offset = static_cast<ptrdiff_t>(w >> 14);
    return i;
}

VertexStreamInfo decode(const NVNvertexStreamState* s) {
    VertexStreamInfo i;
    i.stride = static_cast<ptrdiff_t>(rd32(s));
    i.divisor = static_cast<int>(rd32(reinterpret_cast<const uint8_t*>(s) + 4));
    return i;
}

void register_api_vertex() {
    NVN_REG(nvnVertexAttribStateSetDefaults);
    NVN_REG(nvnVertexAttribStateSetFormat);
    NVN_REG(nvnVertexAttribStateSetStreamIndex);
    NVN_REG(nvnVertexAttribStateGetFormat);
    NVN_REG(nvnVertexAttribStateGetStreamIndex);
    NVN_REG(nvnVertexStreamStateSetDefaults);
    NVN_REG(nvnVertexStreamStateSetStride);
    NVN_REG(nvnVertexStreamStateSetDivisor);
    NVN_REG(nvnVertexStreamStateGetStride);
    NVN_REG(nvnVertexStreamStateGetDivisor);
}

}  // namespace nxrt::nvn
