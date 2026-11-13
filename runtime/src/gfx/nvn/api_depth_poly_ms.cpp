// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NVNdepthStencilState (8 bytes), NVNpolygonState (4 bytes) and NVNmultisampleState (0x18 bytes): plain data in the guest
// object. Bit layouts and defaults follow smo-recomp runtime/gfx/nvn_gl.c (GPL-3.0), layout/defaults checked, no code copied.
//
// Depth-stencil state, one 64-bit word:
//   bit 0 depth test, bit 1 depth write, bit 2 stencil test, bits 4..7 depth func,
//   bits 8..11 stencil func front, bits 12..15 stencil func back,
//   bits 16..27 stencil ops front (stencil fail 16..19, depth fail 20..23, depth pass 24..27), bits 28..39 the same for back.
// Polygon state, four bytes: 0 cull face, 1 front face, 2 polygon mode, 3 polygon offset enables.
// Multisample state, 0x18 bytes: 0 multisample enable, 1 alpha to coverage, 2 alpha to coverage dither, 3 flags (bit 0
// coverage to colour, bit 1 sample locations, bit 2 sample locations grid, bits 3..4 coverage modulation mode, bits 5..7
// coverage to colour output), 4 samples, 5 raster samples, 8..23 sample locations: one byte per sample, x in the low nibble
// and y in the high nibble, in sixteenths of a pixel.
#include <cstring>

#include "nvn_impl.h"

namespace nxrt::nvn {
namespace {

inline uint64_t rd64(const void* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline void wr64(void* p, uint64_t v) { std::memcpy(p, &v, 8); }
inline uint8_t rdb(const void* p, size_t off) { return static_cast<const uint8_t*>(p)[off]; }
inline void wrb(void* p, size_t off, uint8_t v) { static_cast<uint8_t*>(p)[off] = v; }

// A field of `width` bits at `shift` in a word.
inline uint64_t get_bits(uint64_t w, unsigned shift, unsigned width) { return (w >> shift) & ((uint64_t{1} << width) - 1); }
inline uint64_t set_bits(uint64_t w, unsigned shift, unsigned width, uint64_t v) {
    const uint64_t mask = (uint64_t{1} << width) - 1;
    return (w & ~(mask << shift)) | ((v & mask) << shift);
}

constexpr unsigned kFront = static_cast<unsigned>(NVN_FACE_FRONT);
constexpr unsigned kBack = static_cast<unsigned>(NVN_FACE_BACK);

// Face selection: FRONT writes the front, BACK the back, FRONT_AND_BACK both. A getter asked for FRONT_AND_BACK reads the front.
inline bool writes_front(NVNface f) { return (static_cast<unsigned>(f) & kFront) != 0; }
inline bool writes_back(NVNface f) { return (static_cast<unsigned>(f) & kBack) != 0; }
inline unsigned read_index(NVNface f) { return (writes_back(f) && !writes_front(f)) ? 1u : 0u; }

// ---- depth-stencil state (smo layout, see the file header)
constexpr uint64_t kDsDefaults = 0x1111118822ull;   // smo ds_defaults
inline unsigned stencil_func_shift(unsigned face) { return 8u + 4u * face; }
inline unsigned stencil_op_shift(unsigned face) { return 16u + 12u * face; }
static_assert(sizeof(uint64_t) <= sizeof(NVNdepthStencilState), "depth-stencil layout fits the NVN object");

void nvnDepthStencilStateSetDefaults(NVNdepthStencilState* s) { wr64(s, kDsDefaults); }
void nvnDepthStencilStateSetDepthTestEnable(NVNdepthStencilState* s, NVNboolean v) { wr64(s, set_bits(rd64(s), 0, 1, v)); }
void nvnDepthStencilStateSetDepthWriteEnable(NVNdepthStencilState* s, NVNboolean v) { wr64(s, set_bits(rd64(s), 1, 1, v)); }
void nvnDepthStencilStateSetDepthFunc(NVNdepthStencilState* s, NVNdepthFunc func) {
    wr64(s, set_bits(rd64(s), 4, 4, static_cast<uint64_t>(static_cast<uint32_t>(func))));
}
void nvnDepthStencilStateSetStencilTestEnable(NVNdepthStencilState* s, NVNboolean v) { wr64(s, set_bits(rd64(s), 2, 1, v)); }
void nvnDepthStencilStateSetStencilFunc(NVNdepthStencilState* s, NVNface face, NVNstencilFunc func) {
    uint64_t w = rd64(s);
    const uint64_t v = static_cast<uint32_t>(func);
    if (writes_front(face)) w = set_bits(w, stencil_func_shift(0), 4, v);
    if (writes_back(face)) w = set_bits(w, stencil_func_shift(1), 4, v);
    wr64(s, w);
}
void nvnDepthStencilStateSetStencilOp(NVNdepthStencilState* s, NVNface face, NVNstencilOp stencil_fail, NVNstencilOp depth_fail,
                                      NVNstencilOp depth_pass) {
    const uint64_t ops = (static_cast<uint64_t>(static_cast<uint32_t>(stencil_fail)) & 15u) |
                         ((static_cast<uint64_t>(static_cast<uint32_t>(depth_fail)) & 15u) << 4) |
                         ((static_cast<uint64_t>(static_cast<uint32_t>(depth_pass)) & 15u) << 8);
    uint64_t w = rd64(s);
    if (writes_front(face)) w = set_bits(w, stencil_op_shift(0), 12, ops);
    if (writes_back(face)) w = set_bits(w, stencil_op_shift(1), 12, ops);
    wr64(s, w);
}
NVNboolean nvnDepthStencilStateGetDepthTestEnable(const NVNdepthStencilState* s) {
    return static_cast<NVNboolean>(get_bits(rd64(s), 0, 1));
}
NVNboolean nvnDepthStencilStateGetDepthWriteEnable(const NVNdepthStencilState* s) {
    return static_cast<NVNboolean>(get_bits(rd64(s), 1, 1));
}
NVNdepthFunc nvnDepthStencilStateGetDepthFunc(const NVNdepthStencilState* s) {
    return static_cast<NVNdepthFunc>(get_bits(rd64(s), 4, 4));
}
NVNboolean nvnDepthStencilStateGetStencilTestEnable(const NVNdepthStencilState* s) {
    return static_cast<NVNboolean>(get_bits(rd64(s), 2, 1));
}
NVNstencilFunc nvnDepthStencilStateGetStencilFunc(const NVNdepthStencilState* s, NVNface face) {
    return static_cast<NVNstencilFunc>(get_bits(rd64(s), stencil_func_shift(read_index(face)), 4));
}
void nvnDepthStencilStateGetStencilOp(const NVNdepthStencilState* s, NVNface face, NVNstencilOp* stencil_fail,
                                      NVNstencilOp* depth_fail, NVNstencilOp* depth_pass) {
    const uint64_t ops = get_bits(rd64(s), stencil_op_shift(read_index(face)), 12);
    if (stencil_fail) *stencil_fail = static_cast<NVNstencilOp>(static_cast<int>(ops & 15u));
    if (depth_fail) *depth_fail = static_cast<NVNstencilOp>(static_cast<int>((ops >> 4) & 15u));
    if (depth_pass) *depth_pass = static_cast<NVNstencilOp>(static_cast<int>((ops >> 8) & 15u));
}

// ---- polygon state (four bytes)
constexpr size_t kPolyCull = 0, kPolyFront = 1, kPolyMode = 2, kPolyOffset = 3;
static_assert(4 <= sizeof(NVNpolygonState), "polygon layout fits the NVN object");

void nvnPolygonStateSetDefaults(NVNpolygonState* s) {
    // smo poly_defaults {0, 1, 2, 0}: NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, no polygon offset
    const uint8_t d[4] = {static_cast<uint8_t>(NVN_FACE_NONE), static_cast<uint8_t>(NVN_FRONT_FACE_CCW),
                          static_cast<uint8_t>(NVN_POLYGON_MODE_FILL), 0};
    std::memcpy(s, d, 4);
}
void nvnPolygonStateSetCullFace(NVNpolygonState* s, NVNface face) { wrb(s, kPolyCull, static_cast<uint8_t>(face)); }
void nvnPolygonStateSetFrontFace(NVNpolygonState* s, NVNfrontFace front) { wrb(s, kPolyFront, static_cast<uint8_t>(front)); }
void nvnPolygonStateSetPolygonMode(NVNpolygonState* s, NVNpolygonMode mode) { wrb(s, kPolyMode, static_cast<uint8_t>(mode)); }
void nvnPolygonStateSetPolygonOffsetEnables(NVNpolygonState* s, int enables) {
    wrb(s, kPolyOffset, static_cast<uint8_t>(enables));
}
NVNface nvnPolygonStateGetCullFace(const NVNpolygonState* s) { return static_cast<NVNface>(rdb(s, kPolyCull)); }
NVNfrontFace nvnPolygonStateGetFrontFace(const NVNpolygonState* s) { return static_cast<NVNfrontFace>(rdb(s, kPolyFront)); }
NVNpolygonMode nvnPolygonStateGetPolygonMode(const NVNpolygonState* s) { return static_cast<NVNpolygonMode>(rdb(s, kPolyMode)); }
NVNpolygonOffsetEnable nvnPolygonStateGetPolygonOffsetEnables(const NVNpolygonState* s) {
    return static_cast<NVNpolygonOffsetEnable>(rdb(s, kPolyOffset));
}

// ---- multisample state (0x18 bytes)
constexpr size_t kMsEnable = 0, kMsA2C = 1, kMsA2CDither = 2, kMsFlags = 3, kMsSamples = 4, kMsRasterSamples = 5;
constexpr size_t kMsLocations = 8;
constexpr unsigned kMsLocationCount = 16;
static_assert(kMsLocations + kMsLocationCount <= sizeof(NVNmultisampleState), "multisample layout fits the NVN object");

// bits of the flags byte
constexpr unsigned kFlagC2C = 0, kFlagLocations = 1, kFlagGrid = 2, kFlagCovMod = 3, kFlagC2COut = 5;

inline unsigned get_flag(const void* s, unsigned shift, unsigned width) {
    return (rdb(s, kMsFlags) >> shift) & ((1u << width) - 1u);
}
inline void set_flag(void* s, unsigned shift, unsigned width, unsigned v) {
    const unsigned mask = (1u << width) - 1u;
    const unsigned b = rdb(s, kMsFlags);
    wrb(s, kMsFlags, static_cast<uint8_t>((b & ~(mask << shift)) | ((v & mask) << shift)));
}

// One sample coordinate in sixteenths of a pixel, clamped to 0..15 (as smo does; NaN and negatives give 0).
inline uint8_t sixteenth(float v) {
    const float t = v * 16.0f;
    if (!(t > 0.0f)) return 0;
    if (t >= 15.0f) return 15;
    return static_cast<uint8_t>(t);
}

void nvnMultisampleStateSetDefaults(NVNmultisampleState* s) {
    // smo ms_defaults: enable on, alpha-to-coverage dither on, every sample at the pixel centre (8/16, 8/16)
    std::memset(s, 0, sizeof(NVNmultisampleState));
    wrb(s, kMsEnable, 1);
    wrb(s, kMsA2CDither, 1);
    std::memset(static_cast<uint8_t*>(static_cast<void*>(s)) + kMsLocations, 0x88, kMsLocationCount);
}
void nvnMultisampleStateSetMultisampleEnable(NVNmultisampleState* s, NVNboolean v) { wrb(s, kMsEnable, v & 1); }
void nvnMultisampleStateSetSamples(NVNmultisampleState* s, int samples) { wrb(s, kMsSamples, static_cast<uint8_t>(samples)); }
void nvnMultisampleStateSetAlphaToCoverageEnable(NVNmultisampleState* s, NVNboolean v) { wrb(s, kMsA2C, v & 1); }
void nvnMultisampleStateSetAlphaToCoverageDither(NVNmultisampleState* s, NVNboolean v) { wrb(s, kMsA2CDither, v & 1); }
NVNboolean nvnMultisampleStateGetMultisampleEnable(const NVNmultisampleState* s) { return rdb(s, kMsEnable) & 1; }
int nvnMultisampleStateGetSamples(const NVNmultisampleState* s) { return static_cast<int>(rdb(s, kMsSamples)); }
NVNboolean nvnMultisampleStateGetAlphaToCoverageEnable(const NVNmultisampleState* s) { return rdb(s, kMsA2C) & 1; }
NVNboolean nvnMultisampleStateGetAlphaToCoverageDither(const NVNmultisampleState* s) { return rdb(s, kMsA2CDither) & 1; }
void nvnMultisampleStateSetRasterSamples(NVNmultisampleState* s, int samples) {
    wrb(s, kMsRasterSamples, static_cast<uint8_t>(samples));
}
int nvnMultisampleStateGetRasterSamples(NVNmultisampleState* s) { return static_cast<int>(rdb(s, kMsRasterSamples)); }
void nvnMultisampleStateSetCoverageModulationMode(NVNmultisampleState* s, NVNcoverageModulationMode mode) {
    set_flag(s, kFlagCovMod, 2, static_cast<unsigned>(static_cast<uint32_t>(mode)));
}
NVNcoverageModulationMode nvnMultisampleStateGetCoverageModulationMode(const NVNmultisampleState* s) {
    return static_cast<NVNcoverageModulationMode>(get_flag(s, kFlagCovMod, 2));
}
void nvnMultisampleStateSetCoverageToColorEnable(NVNmultisampleState* s, NVNboolean v) { set_flag(s, kFlagC2C, 1, v & 1); }
NVNboolean nvnMultisampleStateGetCoverageToColorEnable(const NVNmultisampleState* s) {
    return static_cast<NVNboolean>(get_flag(s, kFlagC2C, 1));
}
void nvnMultisampleStateSetCoverageToColorOutput(NVNmultisampleState* s, int output) {
    set_flag(s, kFlagC2COut, 3, static_cast<unsigned>(output));
}
int nvnMultisampleStateGetCoverageToColorOutput(const NVNmultisampleState* s) {
    return static_cast<int>(get_flag(s, kFlagC2COut, 3));
}
void nvnMultisampleStateSetSampleLocationsEnable(NVNmultisampleState* s, NVNboolean v) { set_flag(s, kFlagLocations, 1, v & 1); }
NVNboolean nvnMultisampleStateGetSampleLocationsEnable(const NVNmultisampleState* s) {
    return static_cast<NVNboolean>(get_flag(s, kFlagLocations, 1));
}
// The grid is the pixel grid the GPU repeats the sample locations over. It is a property of the GPU, which this family does
// not touch, so 1 x 1 is reported (smo reports 1 x 1 when the grid is off, and asks the GL driver when it is on).
void nvnMultisampleStateGetSampleLocationsGrid(NVNmultisampleState* s, int* width, int* height) {
    (void)s;
    if (width) *width = 1;
    if (height) *height = 1;
}
void nvnMultisampleStateSetSampleLocationsGridEnable(NVNmultisampleState* s, NVNboolean v) { set_flag(s, kFlagGrid, 1, v & 1); }
NVNboolean nvnMultisampleStateGetSampleLocationsGridEnable(const NVNmultisampleState* s) {
    return static_cast<NVNboolean>(get_flag(s, kFlagGrid, 1));
}
// (state, start, count, const float* xy pairs in [0, 1)): stores up to 16 locations, 1/16 pixel resolution (smo's packing).
void nvnMultisampleStateSetSampleLocations(NVNmultisampleState* s, int start, int count, const float* values) {
    if (!values || start < 0 || count <= 0 || static_cast<unsigned>(start) >= kMsLocationCount) return;
    for (int i = 0; i < count && static_cast<unsigned>(start + i) < kMsLocationCount; ++i) {
        const uint8_t x = sixteenth(values[2 * i]);
        const uint8_t y = sixteenth(values[2 * i + 1]);
        wrb(s, kMsLocations + static_cast<size_t>(start + i), static_cast<uint8_t>(x | (y << 4)));
    }
}

}  // namespace

DepthStencilInfo decode(const NVNdepthStencilState* s) {
    const uint64_t w = rd64(s);
    DepthStencilInfo i;
    i.depth_test = get_bits(w, 0, 1) != 0;
    i.depth_write = get_bits(w, 1, 1) != 0;
    i.stencil_test = get_bits(w, 2, 1) != 0;
    i.depth_func = static_cast<int>(get_bits(w, 4, 4));
    for (unsigned f = 0; f < 2; ++f) {
        i.stencil_func[f] = static_cast<int>(get_bits(w, stencil_func_shift(f), 4));
        const uint64_t ops = get_bits(w, stencil_op_shift(f), 12);
        i.stencil_fail[f] = static_cast<int>(ops & 15u);
        i.depth_fail[f] = static_cast<int>((ops >> 4) & 15u);
        i.depth_pass[f] = static_cast<int>((ops >> 8) & 15u);
    }
    return i;
}

PolygonInfo decode(const NVNpolygonState* s) {
    PolygonInfo i;
    i.cull_face = rdb(s, kPolyCull);
    i.front_face = rdb(s, kPolyFront);
    i.polygon_mode = rdb(s, kPolyMode);
    i.offset_enables = rdb(s, kPolyOffset);
    return i;
}

MultisampleInfo decode(const NVNmultisampleState* s) {
    MultisampleInfo i;
    i.enable = rdb(s, kMsEnable) != 0;
    i.samples = static_cast<int>(rdb(s, kMsSamples));
    i.raster_samples = static_cast<int>(rdb(s, kMsRasterSamples));
    i.alpha_to_coverage = rdb(s, kMsA2C) != 0;
    i.alpha_to_coverage_dither = rdb(s, kMsA2CDither) != 0;
    i.coverage_modulation_mode = static_cast<int>(get_flag(s, kFlagCovMod, 2));
    i.coverage_to_color = get_flag(s, kFlagC2C, 1) != 0;
    i.coverage_to_color_output = static_cast<int>(get_flag(s, kFlagC2COut, 3));
    i.sample_locations = get_flag(s, kFlagLocations, 1) != 0;
    i.sample_locations_grid = get_flag(s, kFlagGrid, 1) != 0;
    return i;
}

void register_api_depth_poly_ms() {
    // depth-stencil state
    NVN_REG(nvnDepthStencilStateSetDefaults);
    NVN_REG(nvnDepthStencilStateSetDepthTestEnable);
    NVN_REG(nvnDepthStencilStateSetDepthWriteEnable);
    NVN_REG(nvnDepthStencilStateSetDepthFunc);
    NVN_REG(nvnDepthStencilStateSetStencilTestEnable);
    NVN_REG(nvnDepthStencilStateSetStencilFunc);
    NVN_REG(nvnDepthStencilStateSetStencilOp);
    NVN_REG(nvnDepthStencilStateGetDepthTestEnable);
    NVN_REG(nvnDepthStencilStateGetDepthWriteEnable);
    NVN_REG(nvnDepthStencilStateGetDepthFunc);
    NVN_REG(nvnDepthStencilStateGetStencilTestEnable);
    NVN_REG(nvnDepthStencilStateGetStencilFunc);
    NVN_REG(nvnDepthStencilStateGetStencilOp);
    // polygon state
    NVN_REG(nvnPolygonStateSetDefaults);
    NVN_REG(nvnPolygonStateSetCullFace);
    NVN_REG(nvnPolygonStateSetFrontFace);
    NVN_REG(nvnPolygonStateSetPolygonMode);
    NVN_REG(nvnPolygonStateSetPolygonOffsetEnables);
    NVN_REG(nvnPolygonStateGetCullFace);
    NVN_REG(nvnPolygonStateGetFrontFace);
    NVN_REG(nvnPolygonStateGetPolygonMode);
    NVN_REG(nvnPolygonStateGetPolygonOffsetEnables);
    // multisample state
    NVN_REG(nvnMultisampleStateSetDefaults);
    NVN_REG(nvnMultisampleStateSetMultisampleEnable);
    NVN_REG(nvnMultisampleStateSetSamples);
    NVN_REG(nvnMultisampleStateSetAlphaToCoverageEnable);
    NVN_REG(nvnMultisampleStateSetAlphaToCoverageDither);
    NVN_REG(nvnMultisampleStateGetMultisampleEnable);
    NVN_REG(nvnMultisampleStateGetSamples);
    NVN_REG(nvnMultisampleStateGetAlphaToCoverageEnable);
    NVN_REG(nvnMultisampleStateGetAlphaToCoverageDither);
    NVN_REG(nvnMultisampleStateSetRasterSamples);
    NVN_REG(nvnMultisampleStateGetRasterSamples);
    NVN_REG(nvnMultisampleStateSetCoverageModulationMode);
    NVN_REG(nvnMultisampleStateGetCoverageModulationMode);
    NVN_REG(nvnMultisampleStateSetCoverageToColorEnable);
    NVN_REG(nvnMultisampleStateGetCoverageToColorEnable);
    NVN_REG(nvnMultisampleStateSetCoverageToColorOutput);
    NVN_REG(nvnMultisampleStateGetCoverageToColorOutput);
    NVN_REG(nvnMultisampleStateSetSampleLocationsEnable);
    NVN_REG(nvnMultisampleStateGetSampleLocationsEnable);
    NVN_REG(nvnMultisampleStateGetSampleLocationsGrid);
    NVN_REG(nvnMultisampleStateSetSampleLocationsGridEnable);
    NVN_REG(nvnMultisampleStateGetSampleLocationsGridEnable);
    NVN_REG(nvnMultisampleStateSetSampleLocations);
}

}  // namespace nxrt::nvn
