// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NVNsamplerBuilder and NVNsampler through the typed thunks: plain data in the guest objects, no host resource.
//
// Layout. The parameter block (SamplerData, 48 bytes) is the SamplerParams layout of smo-recomp runtime/gfx/nvn_front.h, used
// by runtime/gfx/nvn_gl.c (GPL-3.0). A sampler holds the block at offset 0 (48 = NVNsampler 0x30). A builder holds its device
// pointer at offset 0 and the block at offset 8 (56 bytes of its 0x60). nvnSamplerInitialize copies the block out of the builder.
// Border colour: the four 32-bit words are stored as given; border_kind says how to read them (0 float, 1 signed integer,
// 2 unsigned integer, the numbering of SamplerInfo in nvn_decoded.h).
// Defaults (nvnSamplerBuilderSetDefaults): min and mag LINEAR, wrap CLAMP_TO_EDGE on s, t, r, lod clamp 0..1000, lod bias 0,
// compare mode NONE with compare func LESS, max anisotropy 1, reduction AVERAGE, border 0 0 0 0, lod snap 0. The device pointer
// is not a sampler parameter, so SetDefaults leaves it alone.
// smo-recomp runtime/gfx/nvn_gl.c (GPL-3.0), layout/defaults checked, no code copied
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "nvn_impl.h"
#include "nxrt/gauge.h"

namespace nxrt::nvn {
namespace {

// Live samplers (nxrt/gauge.h). A sampler has no host object, so the count is the set of guest addresses initialised and not yet finalized.
std::mutex g_live_mu;
std::unordered_set<uintptr_t> g_live;
struct SamplerGauge {
    SamplerGauge() {
        nxrt::gauge_register("nvn.samplers", [] {
            std::lock_guard<std::mutex> lk(g_live_mu);
            return static_cast<int64_t>(g_live.size());
        });
    }
    ~SamplerGauge() { nxrt::gauge_unregister("nvn.samplers"); }
};
SamplerGauge g_sampler_gauge;

// The parameter block: byte for byte the smo SamplerParams layout (48 bytes). Enum fields hold the NVN values (all below 256).
struct SamplerData {
    uint8_t min, mag, ws, wt, wr, cmode, cfunc, reduction;
    float lodmin, lodmax, bias, aniso;
    float border[4];
    uint8_t border_kind, pad[3];
    float lod_snap;
};
static_assert(sizeof(SamplerData) == 48, "sampler parameter block is 48 bytes");
static_assert(sizeof(SamplerData) <= sizeof(NVNsampler), "sampler layout fits the NVN object");
constexpr size_t kBuilderBlock = 8;   // the builder's device pointer comes first
static_assert(sizeof(NVNdevice*) <= kBuilderBlock, "device pointer fits before the block");
static_assert(kBuilderBlock + sizeof(SamplerData) <= sizeof(NVNsamplerBuilder), "builder layout fits the NVN object");

// Where the block of each object lives. Guest memory has byte alignment, so the block is always moved with memcpy.
uint8_t* block(NVNsampler* s) { return reinterpret_cast<uint8_t*>(s); }
const uint8_t* block(const NVNsampler* s) { return reinterpret_cast<const uint8_t*>(s); }
uint8_t* block(NVNsamplerBuilder* b) { return reinterpret_cast<uint8_t*>(b) + kBuilderBlock; }
const uint8_t* block(const NVNsamplerBuilder* b) { return reinterpret_cast<const uint8_t*>(b) + kBuilderBlock; }

template <class O> SamplerData read_params(const O* o) {
    SamplerData d;
    std::memcpy(&d, block(o), sizeof d);
    return d;
}
template <class O> void write_params(O* o, const SamplerData& d) { std::memcpy(block(o), &d, sizeof d); }
template <class O, class F> void edit_params(O* o, F f) {
    SamplerData d = read_params(o);
    f(d);
    write_params(o, d);
}

// The NVN default for every parameter (SetDefaults).
SamplerData default_params() {
    SamplerData d{};   // zero, including the pad bytes
    d.min = NVN_MIN_FILTER_LINEAR;
    d.mag = NVN_MAG_FILTER_LINEAR;
    d.ws = d.wt = d.wr = NVN_WRAP_MODE_CLAMP_TO_EDGE;
    d.cmode = NVN_COMPARE_MODE_NONE;
    d.cfunc = NVN_COMPARE_FUNC_LESS;
    d.reduction = NVN_SAMPLER_REDUCTION_AVERAGE;
    d.lodmin = 0.0f;
    d.lodmax = 1000.0f;
    d.bias = 0.0f;
    d.aniso = 1.0f;
    d.lod_snap = 0.0f;
    return d;
}

// ---- shared bodies: the builder and the sampler entry points call the same code on their own block ------------------------
template <class O> void set_defaults(O* o) { write_params(o, default_params()); }

template <class O> void set_minmag(O* o, NVNminFilter mn, NVNmagFilter mg) {
    edit_params(o, [&](SamplerData& d) { d.min = static_cast<uint8_t>(mn); d.mag = static_cast<uint8_t>(mg); });
}
template <class O> void get_minmag(const O* o, NVNminFilter* mn, NVNmagFilter* mg) {
    const SamplerData d = read_params(o);
    if (mn) *mn = static_cast<NVNminFilter>(d.min);
    if (mg) *mg = static_cast<NVNmagFilter>(d.mag);
}

template <class O> void set_wrap(O* o, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) {
    edit_params(o, [&](SamplerData& d) {
        d.ws = static_cast<uint8_t>(s);
        d.wt = static_cast<uint8_t>(t);
        d.wr = static_cast<uint8_t>(r);
    });
}
template <class O> void get_wrap(const O* o, NVNwrapMode* s, NVNwrapMode* t, NVNwrapMode* r) {
    const SamplerData d = read_params(o);
    if (s) *s = static_cast<NVNwrapMode>(d.ws);
    if (t) *t = static_cast<NVNwrapMode>(d.wt);
    if (r) *r = static_cast<NVNwrapMode>(d.wr);
}

template <class O> void set_lodclamp(O* o, float lo, float hi) {
    edit_params(o, [&](SamplerData& d) { d.lodmin = lo; d.lodmax = hi; });
}
template <class O> void get_lodclamp(const O* o, float* lo, float* hi) {
    const SamplerData d = read_params(o);
    if (lo) *lo = d.lodmin;
    if (hi) *hi = d.lodmax;
}

template <class O> void set_lodbias(O* o, float v) { edit_params(o, [&](SamplerData& d) { d.bias = v; }); }
template <class O> float get_lodbias(const O* o) { return read_params(o).bias; }

template <class O> void set_compare(O* o, NVNcompareMode m, NVNcompareFunc f) {
    edit_params(o, [&](SamplerData& d) { d.cmode = static_cast<uint8_t>(m); d.cfunc = static_cast<uint8_t>(f); });
}
template <class O> void get_compare(const O* o, NVNcompareMode* m, NVNcompareFunc* f) {
    const SamplerData d = read_params(o);
    if (m) *m = static_cast<NVNcompareMode>(d.cmode);
    if (f) *f = static_cast<NVNcompareFunc>(d.cfunc);
}

// kind: 0 float, 1 signed integer, 2 unsigned integer. The four words are stored as they are given.
template <class O> void set_border(O* o, const void* src, uint8_t kind) {
    if (!src) return;
    edit_params(o, [&](SamplerData& d) {
        std::memcpy(d.border, src, sizeof d.border);
        d.border_kind = kind;
    });
}
// The getters hand back the four words as stored, whatever kind they were set with.
template <class O> void get_border(const O* o, void* out) {
    if (!out) return;
    const SamplerData d = read_params(o);
    std::memcpy(out, d.border, sizeof d.border);
}

template <class O> void set_aniso(O* o, float v) { edit_params(o, [&](SamplerData& d) { d.aniso = v; }); }
template <class O> float get_aniso(const O* o) { return read_params(o).aniso; }

template <class O> void set_reduction(O* o, NVNsamplerReduction r) {
    edit_params(o, [&](SamplerData& d) { d.reduction = static_cast<uint8_t>(r); });
}
template <class O> NVNsamplerReduction get_reduction(const O* o) {
    return static_cast<NVNsamplerReduction>(read_params(o).reduction);
}

template <class O> void set_lodsnap(O* o, float v) { edit_params(o, [&](SamplerData& d) { d.lod_snap = v; }); }
template <class O> float get_lodsnap(const O* o) { return read_params(o).lod_snap; }

// Decoded view of a parameter block: every field is filled from the bytes, so a zero block decodes to all zero values.
SamplerInfo to_info(const SamplerData& d) {
    SamplerInfo i;
    i.min_filter = d.min;
    i.mag_filter = d.mag;
    i.wrap_s = d.ws;
    i.wrap_t = d.wt;
    i.wrap_r = d.wr;
    i.lod_min = d.lodmin;
    i.lod_max = d.lodmax;
    i.lod_bias = d.bias;
    i.lod_snap = d.lod_snap;
    i.compare_mode = d.cmode;
    i.compare_func = d.cfunc;
    std::memcpy(i.border_color, d.border, sizeof d.border);
    i.border_kind = d.border_kind;
    i.max_anisotropy = d.aniso;
    i.reduction = d.reduction;
    return i;
}

inline bool same_bits(float a, float b) {
    uint32_t x, y;
    std::memcpy(&x, &a, sizeof x);
    std::memcpy(&y, &b, sizeof y);
    return x == y;
}

}  // namespace

// ---- decode ----------------------------------------------------------------------------------------------------------------
SamplerInfo decode(const NVNsamplerBuilder* b) {
    return to_info(b ? read_params(b) : SamplerData{});
}
SamplerInfo decode(const NVNsampler* s) {
    return to_info(s ? read_params(s) : SamplerData{});
}

bool SamplerInfo::operator==(const SamplerInfo& o) const {
    if (min_filter != o.min_filter || mag_filter != o.mag_filter) return false;
    if (wrap_s != o.wrap_s || wrap_t != o.wrap_t || wrap_r != o.wrap_r) return false;
    if (!same_bits(lod_min, o.lod_min) || !same_bits(lod_max, o.lod_max)) return false;
    if (!same_bits(lod_bias, o.lod_bias) || !same_bits(lod_snap, o.lod_snap)) return false;
    if (compare_mode != o.compare_mode || compare_func != o.compare_func) return false;
    for (int k = 0; k < 4; ++k)
        if (!same_bits(border_color[k], o.border_color[k])) return false;
    if (border_kind != o.border_kind) return false;
    if (!same_bits(max_anisotropy, o.max_anisotropy)) return false;
    return reduction == o.reduction;
}

namespace {

// ---- builder entry points ----------------------------------------------------------------------------------------------------
void nvnSamplerBuilderSetDevice(NVNsamplerBuilder* b, NVNdevice* device) { std::memcpy(b, &device, sizeof device); }
NVNdevice* nvnSamplerBuilderGetDevice(const NVNsamplerBuilder* b) {
    NVNdevice* device = nullptr;
    std::memcpy(&device, b, sizeof device);
    return device;
}
void nvnSamplerBuilderSetDefaults(NVNsamplerBuilder* b) { set_defaults(b); }
void nvnSamplerBuilderSetMinMagFilter(NVNsamplerBuilder* b, NVNminFilter mn, NVNmagFilter mg) { set_minmag(b, mn, mg); }
void nvnSamplerBuilderSetWrapMode(NVNsamplerBuilder* b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) { set_wrap(b, s, t, r); }
void nvnSamplerBuilderSetLodClamp(NVNsamplerBuilder* b, float lo, float hi) { set_lodclamp(b, lo, hi); }
void nvnSamplerBuilderSetLodBias(NVNsamplerBuilder* b, float v) { set_lodbias(b, v); }
void nvnSamplerBuilderSetCompare(NVNsamplerBuilder* b, NVNcompareMode m, NVNcompareFunc f) { set_compare(b, m, f); }
void nvnSamplerBuilderSetBorderColor(NVNsamplerBuilder* b, const float* c) { set_border(b, c, 0); }
void nvnSamplerBuilderSetBorderColori(NVNsamplerBuilder* b, const int* c) { set_border(b, c, 1); }
void nvnSamplerBuilderSetBorderColorui(NVNsamplerBuilder* b, const uint32_t* c) { set_border(b, c, 2); }
void nvnSamplerBuilderSetMaxAnisotropy(NVNsamplerBuilder* b, float v) { set_aniso(b, v); }
void nvnSamplerBuilderSetReductionFilter(NVNsamplerBuilder* b, NVNsamplerReduction r) { set_reduction(b, r); }
void nvnSamplerBuilderSetLodSnap(NVNsamplerBuilder* b, float v) { set_lodsnap(b, v); }

void nvnSamplerBuilderGetMinMagFilter(const NVNsamplerBuilder* b, NVNminFilter* mn, NVNmagFilter* mg) { get_minmag(b, mn, mg); }
void nvnSamplerBuilderGetWrapMode(const NVNsamplerBuilder* b, NVNwrapMode* s, NVNwrapMode* t, NVNwrapMode* r) { get_wrap(b, s, t, r); }
void nvnSamplerBuilderGetLodClamp(const NVNsamplerBuilder* b, float* lo, float* hi) { get_lodclamp(b, lo, hi); }
float nvnSamplerBuilderGetLodBias(const NVNsamplerBuilder* b) { return get_lodbias(b); }
void nvnSamplerBuilderGetCompare(const NVNsamplerBuilder* b, NVNcompareMode* m, NVNcompareFunc* f) { get_compare(b, m, f); }
void nvnSamplerBuilderGetBorderColor(const NVNsamplerBuilder* b, float* out) { get_border(b, out); }
void nvnSamplerBuilderGetBorderColori(const NVNsamplerBuilder* b, int* out) { get_border(b, out); }
void nvnSamplerBuilderGetBorderColorui(const NVNsamplerBuilder* b, uint32_t* out) { get_border(b, out); }
float nvnSamplerBuilderGetMaxAnisotropy(const NVNsamplerBuilder* b) { return get_aniso(b); }
NVNsamplerReduction nvnSamplerBuilderGetReductionFilter(const NVNsamplerBuilder* b) { return get_reduction(b); }
float nvnSamplerBuilderGetLodSnap(const NVNsamplerBuilder* b) { return get_lodsnap(b); }

// ---- sampler entry points ----------------------------------------------------------------------------------------------------
NVNboolean nvnSamplerInitialize(NVNsampler* s, const NVNsamplerBuilder* b) {
    if (!s || !b) return 0;
    write_params(s, read_params(b));
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live.insert(reinterpret_cast<uintptr_t>(s));
    return 1;
}
void nvnSamplerFinalize(NVNsampler* s) {                               // nothing host-side to release; the live count drops
    std::lock_guard<std::mutex> lk(g_live_mu);
    g_live.erase(reinterpret_cast<uintptr_t>(s));
}
void nvnSamplerSetDebugLabel(NVNsampler* s, const char* label) {}      // labels are not kept

void nvnSamplerGetMinMagFilter(const NVNsampler* s, NVNminFilter* mn, NVNmagFilter* mg) { get_minmag(s, mn, mg); }
void nvnSamplerGetWrapMode(const NVNsampler* s, NVNwrapMode* a, NVNwrapMode* t, NVNwrapMode* r) { get_wrap(s, a, t, r); }
void nvnSamplerGetLodClamp(const NVNsampler* s, float* lo, float* hi) { get_lodclamp(s, lo, hi); }
float nvnSamplerGetLodBias(const NVNsampler* s) { return get_lodbias(s); }
void nvnSamplerGetCompare(const NVNsampler* s, NVNcompareMode* m, NVNcompareFunc* f) { get_compare(s, m, f); }
void nvnSamplerGetBorderColor(const NVNsampler* s, float* out) { get_border(s, out); }
void nvnSamplerGetBorderColori(const NVNsampler* s, int* out) { get_border(s, out); }
void nvnSamplerGetBorderColorui(const NVNsampler* s, uint32_t* out) { get_border(s, out); }
float nvnSamplerGetMaxAnisotropy(const NVNsampler* s) { return get_aniso(s); }
NVNsamplerReduction nvnSamplerGetReductionFilter(const NVNsampler* s) { return get_reduction(s); }
NVNboolean nvnSamplerCompare(const NVNsampler* a, const NVNsampler* b) {
    if (!a || !b) return 0;
    return decode(a) == decode(b) ? 1 : 0;   // the decoded state: every field, floats bit for bit
}
uint64_t nvnSamplerGetDebugID(const NVNsampler* s) { return 0; }

}  // namespace

void register_api_sampler() {
    NVN_REG(nvnSamplerBuilderSetDevice);
    NVN_REG(nvnSamplerBuilderGetDevice);
    NVN_REG(nvnSamplerBuilderSetDefaults);
    NVN_REG(nvnSamplerBuilderSetMinMagFilter);
    NVN_REG(nvnSamplerBuilderSetWrapMode);
    NVN_REG(nvnSamplerBuilderSetLodClamp);
    NVN_REG(nvnSamplerBuilderSetLodBias);
    NVN_REG(nvnSamplerBuilderSetCompare);
    NVN_REG(nvnSamplerBuilderSetBorderColor);
    NVN_REG(nvnSamplerBuilderSetBorderColori);
    NVN_REG(nvnSamplerBuilderSetBorderColorui);
    NVN_REG(nvnSamplerBuilderSetMaxAnisotropy);
    NVN_REG(nvnSamplerBuilderSetReductionFilter);
    NVN_REG(nvnSamplerBuilderSetLodSnap);
    NVN_REG(nvnSamplerBuilderGetMinMagFilter);
    NVN_REG(nvnSamplerBuilderGetWrapMode);
    NVN_REG(nvnSamplerBuilderGetLodClamp);
    NVN_REG(nvnSamplerBuilderGetLodBias);
    NVN_REG(nvnSamplerBuilderGetCompare);
    NVN_REG(nvnSamplerBuilderGetBorderColor);
    NVN_REG(nvnSamplerBuilderGetBorderColori);
    NVN_REG(nvnSamplerBuilderGetBorderColorui);
    NVN_REG(nvnSamplerBuilderGetMaxAnisotropy);
    NVN_REG(nvnSamplerBuilderGetReductionFilter);
    NVN_REG(nvnSamplerBuilderGetLodSnap);
    NVN_REG(nvnSamplerInitialize);
    NVN_REG(nvnSamplerFinalize);
    NVN_REG(nvnSamplerSetDebugLabel);
    NVN_REG(nvnSamplerGetMinMagFilter);
    NVN_REG(nvnSamplerGetWrapMode);
    NVN_REG(nvnSamplerGetLodClamp);
    NVN_REG(nvnSamplerGetLodBias);
    NVN_REG(nvnSamplerGetCompare);
    NVN_REG(nvnSamplerGetBorderColor);
    NVN_REG(nvnSamplerGetBorderColori);
    NVN_REG(nvnSamplerGetBorderColorui);
    NVN_REG(nvnSamplerGetMaxAnisotropy);
    NVN_REG(nvnSamplerGetReductionFilter);
    NVN_REG(nvnSamplerCompare);
    NVN_REG(nvnSamplerGetDebugID);
}

}  // namespace nxrt::nvn
