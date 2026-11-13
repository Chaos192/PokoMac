// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NVNtextureBuilder and NVNtextureView: plain data in the guest objects (family texture_builder). The builder keeps a
// TexBuilderData (112 bytes incl. padding) inside its 0x80-byte object; the view keeps a ViewData (36 bytes) inside its
// 0x28-byte object. Both are copied in and out with memcpy, so no typed pointer into guest memory is ever used. Guest
// pointers (device, pool, packaged data) are stored and handed back, never dereferenced, except nvnTextureBuilderGetMemoryPool,
// which copies the pool object it names.
// Defaults: builder zeroed, then target 2D, levels 1, swizzle R G B A. View zeroed (no field set).
// smo-recomp runtime/gfx/nvn_gl.c (GPL-3.0), layout/defaults checked, no code copied. Also runtime/gfx/nvn_front.h (the
// TexBuilder and View structs), layout checked, no code copied.
// Not here: nvnTextureBuilderGetStorageSize, GetStorageAlignment, GetStorageClass, GetSparseTileLayout and
// GetZCullStorageSize (the manager). nvnTextureBuilderGetDevice, GetPackagedTextureLayout and GetRawStorageClass have no NVN1
// signature (the table says '?'); they follow the NVN2 driver (sdk 0x5EE2A0, 0x5EE380, 0x5EE9A0), see each.
#include <cstring>

#include "nvn_impl.h"

namespace nxrt::nvn {
namespace {

// ---- builder ---------------------------------------------------------------------------------------------------------------
struct TexBuilderData {
    NVNdevice* device;
    NVNmemoryPool* pool;
    ptrdiff_t offset;
    ptrdiff_t stride;
    const void* packaged;
    uint8_t layout[8];
    uint32_t gl_texture_name;
    int32_t flags, target, width, height, depth, levels, samples, format;
    int32_t swizzle[4];
    int32_t depth_stencil_mode;
    int32_t has_layout;
};
static_assert(sizeof(TexBuilderData) <= sizeof(NVNtextureBuilder), "texture builder layout fits the NVN object");

TexBuilderData load_b(const NVNtextureBuilder* b) { TexBuilderData d; std::memcpy(&d, b, sizeof d); return d; }
void store_b(NVNtextureBuilder* b, const TexBuilderData& d) { std::memcpy(b, &d, sizeof d); }

template <class F>
void edit_b(NVNtextureBuilder* b, F f) {
    TexBuilderData d = load_b(b);
    f(d);
    store_b(b, d);
}

void nvnTextureBuilderSetDevice(NVNtextureBuilder* b, NVNdevice* device) { edit_b(b, [&](TexBuilderData& d) { d.device = device; }); }
void nvnTextureBuilderSetDefaults(NVNtextureBuilder* b) {
    std::memset(b, 0, sizeof(NVNtextureBuilder));
    TexBuilderData d = load_b(b);
    d.target = static_cast<int32_t>(NVN_TEXTURE_TARGET_2D);
    d.levels = 1;
    d.swizzle[0] = static_cast<int32_t>(NVN_TEXTURE_SWIZZLE_R);
    d.swizzle[1] = static_cast<int32_t>(NVN_TEXTURE_SWIZZLE_G);
    d.swizzle[2] = static_cast<int32_t>(NVN_TEXTURE_SWIZZLE_B);
    d.swizzle[3] = static_cast<int32_t>(NVN_TEXTURE_SWIZZLE_A);
    store_b(b, d);
}
void nvnTextureBuilderSetFlags(NVNtextureBuilder* b, int flags) { edit_b(b, [&](TexBuilderData& d) { d.flags = flags; }); }
void nvnTextureBuilderSetTarget(NVNtextureBuilder* b, NVNtextureTarget target) {
    edit_b(b, [&](TexBuilderData& d) { d.target = static_cast<int32_t>(target); });
}
void nvnTextureBuilderSetWidth(NVNtextureBuilder* b, int width) { edit_b(b, [&](TexBuilderData& d) { d.width = width; }); }
void nvnTextureBuilderSetHeight(NVNtextureBuilder* b, int height) { edit_b(b, [&](TexBuilderData& d) { d.height = height; }); }
void nvnTextureBuilderSetDepth(NVNtextureBuilder* b, int depth) { edit_b(b, [&](TexBuilderData& d) { d.depth = depth; }); }
// Sizes as smo: 1D sets height and depth to 1; 2D sets depth to 1; the target is left as it was set.
void nvnTextureBuilderSetSize1D(NVNtextureBuilder* b, int width) {
    edit_b(b, [&](TexBuilderData& d) { d.width = width; d.height = 1; d.depth = 1; });
}
void nvnTextureBuilderSetSize2D(NVNtextureBuilder* b, int width, int height) {
    edit_b(b, [&](TexBuilderData& d) { d.width = width; d.height = height; d.depth = 1; });
}
void nvnTextureBuilderSetSize3D(NVNtextureBuilder* b, int width, int height, int depth) {
    edit_b(b, [&](TexBuilderData& d) { d.width = width; d.height = height; d.depth = depth; });
}
void nvnTextureBuilderSetLevels(NVNtextureBuilder* b, int levels) { edit_b(b, [&](TexBuilderData& d) { d.levels = levels; }); }
void nvnTextureBuilderSetFormat(NVNtextureBuilder* b, NVNformat format) {
    edit_b(b, [&](TexBuilderData& d) { d.format = static_cast<int32_t>(format); });
}
void nvnTextureBuilderSetSamples(NVNtextureBuilder* b, int samples) { edit_b(b, [&](TexBuilderData& d) { d.samples = samples; }); }
void nvnTextureBuilderSetSwizzle(NVNtextureBuilder* b, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl,
                                 NVNtextureSwizzle a) {
    edit_b(b, [&](TexBuilderData& d) {
        d.swizzle[0] = static_cast<int32_t>(r);
        d.swizzle[1] = static_cast<int32_t>(g);
        d.swizzle[2] = static_cast<int32_t>(bl);
        d.swizzle[3] = static_cast<int32_t>(a);
    });
}
void nvnTextureBuilderSetDepthStencilMode(NVNtextureBuilder* b, NVNtextureDepthStencilMode mode) {
    edit_b(b, [&](TexBuilderData& d) { d.depth_stencil_mode = static_cast<int32_t>(mode); });
}
void nvnTextureBuilderSetStorage(NVNtextureBuilder* b, NVNmemoryPool* pool, ptrdiff_t offset) {
    edit_b(b, [&](TexBuilderData& d) { d.pool = pool; d.offset = offset; });
}
void nvnTextureBuilderSetPackagedTextureData(NVNtextureBuilder* b, const void* data) {
    edit_b(b, [&](TexBuilderData& d) { d.packaged = data; });
}
// As smo: a null layout pointer changes nothing.
void nvnTextureBuilderSetPackagedTextureLayout(NVNtextureBuilder* b, const NVNpackagedTextureLayout* layout) {
    if (!layout) return;
    edit_b(b, [&](TexBuilderData& d) {
        std::memcpy(d.layout, layout->layout, sizeof d.layout);
        d.has_layout = 1;
    });
}
void nvnTextureBuilderSetStride(NVNtextureBuilder* b, ptrdiff_t stride) { edit_b(b, [&](TexBuilderData& d) { d.stride = stride; }); }
void nvnTextureBuilderSetGLTextureName(NVNtextureBuilder* b, uint32_t name) {
    edit_b(b, [&](TexBuilderData& d) { d.gl_texture_name = name; });
}

NVNtextureFlags nvnTextureBuilderGetFlags(const NVNtextureBuilder* b) {
    return static_cast<NVNtextureFlags>(load_b(b).flags);
}
NVNtextureTarget nvnTextureBuilderGetTarget(const NVNtextureBuilder* b) {
    return static_cast<NVNtextureTarget>(load_b(b).target);
}
int nvnTextureBuilderGetWidth(const NVNtextureBuilder* b) { return load_b(b).width; }
int nvnTextureBuilderGetHeight(const NVNtextureBuilder* b) { return load_b(b).height; }
int nvnTextureBuilderGetDepth(const NVNtextureBuilder* b) { return load_b(b).depth; }
int nvnTextureBuilderGetLevels(const NVNtextureBuilder* b) { return load_b(b).levels; }
NVNformat nvnTextureBuilderGetFormat(const NVNtextureBuilder* b) { return static_cast<NVNformat>(load_b(b).format); }
// NVN2: NVNdevice* (const NVNtextureBuilder*), the device given to SetDevice.
NVNdevice* nvnTextureBuilderGetDevice(const NVNtextureBuilder* b) { return load_b(b).device; }
// NVN2: NVNboolean (const NVNtextureBuilder*, NVNpackagedTextureLayout* out). The NVN2 driver keeps three 4-bit fields of the layout
// SetPackagedTextureLayout was given (the low 12 bits of its first halfword) and writes them back as a 32-bit word; it answers 1 when a
// layout was set, else 0 with the word zero. Only 4 bytes of `out` are written, as the driver does.
NVNboolean nvnTextureBuilderGetPackagedTextureLayout(const NVNtextureBuilder* b, NVNpackagedTextureLayout* out) {
    const TexBuilderData d = load_b(b);
    uint16_t h = 0;
    std::memcpy(&h, d.layout, sizeof h);
    const uint32_t w = d.has_layout ? (h & 0xfffu) : 0u;
    if (out) std::memcpy(out, &w, sizeof w);
    return d.has_layout ? 1 : 0;
}
// NVN2: the raw storage class (int) of a texture built from this builder (raw_storage_class, nvn_decoded.h).
int nvnTextureBuilderGetRawStorageClass(const NVNtextureBuilder* b) {
    const TexBuilderData d = load_b(b);
    return raw_storage_class(d.flags, d.format);
}
int nvnTextureBuilderGetSamples(const NVNtextureBuilder* b) { return load_b(b).samples; }
void nvnTextureBuilderGetSwizzle(const NVNtextureBuilder* b, NVNtextureSwizzle* r, NVNtextureSwizzle* g, NVNtextureSwizzle* bl,
                                 NVNtextureSwizzle* a) {
    const TexBuilderData d = load_b(b);
    if (r) *r = static_cast<NVNtextureSwizzle>(d.swizzle[0]);
    if (g) *g = static_cast<NVNtextureSwizzle>(d.swizzle[1]);
    if (bl) *bl = static_cast<NVNtextureSwizzle>(d.swizzle[2]);
    if (a) *a = static_cast<NVNtextureSwizzle>(d.swizzle[3]);
}
NVNtextureDepthStencilMode nvnTextureBuilderGetDepthStencilMode(const NVNtextureBuilder* b) {
    return static_cast<NVNtextureDepthStencilMode>(load_b(b).depth_stencil_mode);
}
const void* nvnTextureBuilderGetPackagedTextureData(const NVNtextureBuilder* b) { return load_b(b).packaged; }
ptrdiff_t nvnTextureBuilderGetStride(const NVNtextureBuilder* b) { return load_b(b).stride; }
uint32_t nvnTextureBuilderGetGLTextureName(const NVNtextureBuilder* b) { return load_b(b).gl_texture_name; }
const NVNmemoryPool* nvnTextureBuilderGetMemoryPool(const NVNtextureBuilder* b) { return load_b(b).pool; }
ptrdiff_t nvnTextureBuilderGetMemoryOffset(const NVNtextureBuilder* b) { return load_b(b).offset; }

// ---- view ------------------------------------------------------------------------------------------------------------------
// Which fields were set. smo keeps the same idea (a 'set' bit per field, read back as the Get return value).
enum : uint32_t {
    kSetLevels = 0x1,
    kSetLayers = 0x2,
    kSetFormat = 0x4,
    kSetSwizzle = 0x8,
    kSetDepthStencil = 0x10,
    kSetTarget = 0x20,
};

struct ViewData {
    int32_t min_level, num_levels;
    int32_t min_layer, num_layers;
    int32_t format;
    int32_t target;
    int32_t depth_stencil_mode;
    uint32_t set;
    uint8_t swizzle[4];   // NVN swizzle values are 0..5
};
static_assert(sizeof(ViewData) == 36, "no padding in ViewData");

// What the object really holds: 16 bytes. The game's CGraphicsTextureToken_NVN keeps an NVNtextureView at +0x40 and its own fields at +0x50,
// +0x58 and +0x60 (the descriptor id), so on this NVN version a view is at most 16 bytes wide and a SetDefaults that clears the 0x28 bytes
// of the practice mod's header zeroes the token's id (debug note 121).
struct ViewPacked {
    int16_t min_level, num_levels, min_layer, num_layers;     // 16 bits each: levels and layers are small; -1 round trips
    uint16_t format;
    uint16_t swizzle;        // four 3-bit values (NVN swizzle values are 0..5)
    uint8_t target, depth_stencil_mode, set, pad;
};
static_assert(sizeof(ViewPacked) == 16, "a texture view occupies 16 bytes of the game's object");

ViewData load_v(const NVNtextureView* v) {
    ViewPacked p;
    std::memcpy(&p, v, sizeof p);
    ViewData d{};
    d.min_level = p.min_level; d.num_levels = p.num_levels; d.min_layer = p.min_layer; d.num_layers = p.num_layers;
    d.format = p.format; d.target = p.target; d.depth_stencil_mode = p.depth_stencil_mode; d.set = p.set;
    for (int k = 0; k < 4; ++k) d.swizzle[k] = static_cast<uint8_t>((p.swizzle >> (3 * k)) & 7);
    return d;
}
void store_v(NVNtextureView* v, const ViewData& d) {
    ViewPacked p{};
    p.min_level = static_cast<int16_t>(d.min_level); p.num_levels = static_cast<int16_t>(d.num_levels);
    p.min_layer = static_cast<int16_t>(d.min_layer); p.num_layers = static_cast<int16_t>(d.num_layers);
    p.format = static_cast<uint16_t>(d.format); p.target = static_cast<uint8_t>(d.target);
    p.depth_stencil_mode = static_cast<uint8_t>(d.depth_stencil_mode); p.set = static_cast<uint8_t>(d.set);
    for (int k = 0; k < 4; ++k) p.swizzle = static_cast<uint16_t>(p.swizzle | ((d.swizzle[k] & 7) << (3 * k)));
    std::memcpy(v, &p, sizeof p);
}

template <class F>
void edit_v(NVNtextureView* v, F f) {
    ViewData d = load_v(v);
    f(d);
    store_v(v, d);
}

void nvnTextureViewSetDefaults(NVNtextureView* v) {
    std::memset(v, 0, sizeof(ViewPacked));
}
void nvnTextureViewSetLevels(NVNtextureView* v, int min_level, int num_levels) {
    edit_v(v, [&](ViewData& d) { d.min_level = min_level; d.num_levels = num_levels; d.set |= kSetLevels; });
}
void nvnTextureViewSetLayers(NVNtextureView* v, int min_layer, int num_layers) {
    edit_v(v, [&](ViewData& d) { d.min_layer = min_layer; d.num_layers = num_layers; d.set |= kSetLayers; });
}
void nvnTextureViewSetFormat(NVNtextureView* v, NVNformat format) {
    edit_v(v, [&](ViewData& d) { d.format = static_cast<int32_t>(format); d.set |= kSetFormat; });
}
void nvnTextureViewSetSwizzle(NVNtextureView* v, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl,
                              NVNtextureSwizzle a) {
    edit_v(v, [&](ViewData& d) {
        d.swizzle[0] = static_cast<uint8_t>(r);
        d.swizzle[1] = static_cast<uint8_t>(g);
        d.swizzle[2] = static_cast<uint8_t>(bl);
        d.swizzle[3] = static_cast<uint8_t>(a);
        d.set |= kSetSwizzle;
    });
}
void nvnTextureViewSetDepthStencilMode(NVNtextureView* v, NVNtextureDepthStencilMode mode) {
    edit_v(v, [&](ViewData& d) { d.depth_stencil_mode = static_cast<int32_t>(mode); d.set |= kSetDepthStencil; });
}
void nvnTextureViewSetTarget(NVNtextureView* v, NVNtextureTarget target) {
    edit_v(v, [&](ViewData& d) { d.target = static_cast<int32_t>(target); d.set |= kSetTarget; });
}
// The getters write the stored value (zero for a field never set, as smo) and return whether the field was set.
NVNboolean nvnTextureViewGetLevels(const NVNtextureView* v, int* min_level, int* num_levels) {
    const ViewData d = load_v(v);
    if (min_level) *min_level = d.min_level;
    if (num_levels) *num_levels = d.num_levels;
    return (d.set & kSetLevels) != 0;
}
NVNboolean nvnTextureViewGetLayers(const NVNtextureView* v, int* min_layer, int* num_layers) {
    const ViewData d = load_v(v);
    if (min_layer) *min_layer = d.min_layer;
    if (num_layers) *num_layers = d.num_layers;
    return (d.set & kSetLayers) != 0;
}
NVNboolean nvnTextureViewGetFormat(const NVNtextureView* v, NVNformat* format) {
    const ViewData d = load_v(v);
    if (format) *format = static_cast<NVNformat>(d.format);
    return (d.set & kSetFormat) != 0;
}
NVNboolean nvnTextureViewGetSwizzle(const NVNtextureView* v, NVNtextureSwizzle* r, NVNtextureSwizzle* g, NVNtextureSwizzle* bl,
                                    NVNtextureSwizzle* a) {
    const ViewData d = load_v(v);
    if (r) *r = static_cast<NVNtextureSwizzle>(d.swizzle[0]);
    if (g) *g = static_cast<NVNtextureSwizzle>(d.swizzle[1]);
    if (bl) *bl = static_cast<NVNtextureSwizzle>(d.swizzle[2]);
    if (a) *a = static_cast<NVNtextureSwizzle>(d.swizzle[3]);
    return (d.set & kSetSwizzle) != 0;
}
NVNboolean nvnTextureViewGetDepthStencilMode(const NVNtextureView* v, NVNtextureDepthStencilMode* mode) {
    const ViewData d = load_v(v);
    if (mode) *mode = static_cast<NVNtextureDepthStencilMode>(d.depth_stencil_mode);
    return (d.set & kSetDepthStencil) != 0;
}
NVNboolean nvnTextureViewGetTarget(const NVNtextureView* v, NVNtextureTarget* target) {
    const ViewData d = load_v(v);
    if (target) *target = static_cast<NVNtextureTarget>(d.target);
    return (d.set & kSetTarget) != 0;
}
// Compares every stored byte, the set flags included (smo compares its whole View the same way).
NVNboolean nvnTextureViewCompare(const NVNtextureView* a, const NVNtextureView* b) {
    return std::memcmp(a, b, sizeof(ViewPacked)) == 0 ? 1 : 0;
}

// NVN 55 name the game asks for and never calls (signature assumed, like nvnTextureGetViewOffset): views do not move the storage.
ptrdiff_t nvnTextureBuilderGetViewOffset(const NVNtextureBuilder*, const NVNtextureView*) { return 0; }

}  // namespace

// ---- decode ----------------------------------------------------------------------------------------------------------------
// Copies what the object holds. A zero-filled object decodes to all zero (no defaults are applied here).
TextureBuilderInfo decode(const NVNtextureBuilder* b) {
    const TexBuilderData d = load_b(b);
    TextureBuilderInfo i;
    i.device = d.device;
    i.flags = d.flags;
    i.target = d.target;
    i.width = d.width;
    i.height = d.height;
    i.depth = d.depth;
    i.levels = d.levels;
    i.samples = d.samples;
    i.format = d.format;
    for (int k = 0; k < 4; ++k) i.swizzle[k] = d.swizzle[k];
    i.depth_stencil_mode = d.depth_stencil_mode;
    i.pool = d.pool;
    i.offset = d.offset;
    i.stride = d.stride;
    i.packaged_data = d.packaged;
    std::memcpy(i.packaged_layout, d.layout, sizeof i.packaged_layout);
    i.has_packaged_layout = d.has_layout != 0;
    i.gl_texture_name = d.gl_texture_name;
    return i;
}

TextureViewInfo decode(const NVNtextureView* v) {
    const ViewData d = load_v(v);
    TextureViewInfo i;
    i.has_levels = (d.set & kSetLevels) != 0;
    i.has_layers = (d.set & kSetLayers) != 0;
    i.has_format = (d.set & kSetFormat) != 0;
    i.has_swizzle = (d.set & kSetSwizzle) != 0;
    i.has_depth_stencil_mode = (d.set & kSetDepthStencil) != 0;
    i.has_target = (d.set & kSetTarget) != 0;
    i.min_level = d.min_level;
    i.num_levels = d.num_levels;
    i.min_layer = d.min_layer;
    i.num_layers = d.num_layers;
    i.format = d.format;
    for (int k = 0; k < 4; ++k) i.swizzle[k] = d.swizzle[k];
    i.depth_stencil_mode = d.depth_stencil_mode;
    i.target = d.target;
    return i;
}

void register_api_texture_builder() {
    NVN_REG(nvnTextureBuilderSetDevice);
    NVN_REG(nvnTextureBuilderSetDefaults);
    NVN_REG(nvnTextureBuilderSetFlags);
    NVN_REG(nvnTextureBuilderSetTarget);
    NVN_REG(nvnTextureBuilderSetWidth);
    NVN_REG(nvnTextureBuilderSetHeight);
    NVN_REG(nvnTextureBuilderSetDepth);
    NVN_REG(nvnTextureBuilderSetSize1D);
    NVN_REG(nvnTextureBuilderSetSize2D);
    NVN_REG(nvnTextureBuilderSetSize3D);
    NVN_REG(nvnTextureBuilderSetLevels);
    NVN_REG(nvnTextureBuilderSetFormat);
    NVN_REG(nvnTextureBuilderSetSamples);
    NVN_REG(nvnTextureBuilderSetSwizzle);
    NVN_REG(nvnTextureBuilderSetDepthStencilMode);
    NVN_REG(nvnTextureBuilderSetStorage);
    NVN_REG(nvnTextureBuilderSetPackagedTextureData);
    NVN_REG(nvnTextureBuilderSetPackagedTextureLayout);
    NVN_REG(nvnTextureBuilderSetStride);
    NVN_REG(nvnTextureBuilderSetGLTextureName);
    NVN_REG(nvnTextureBuilderGetFlags);
    NVN_REG(nvnTextureBuilderGetDevice);
    NVN_REG(nvnTextureBuilderGetPackagedTextureLayout);
    NVN_REG(nvnTextureBuilderGetRawStorageClass);
    NVN_REG(nvnTextureBuilderGetTarget);
    NVN_REG(nvnTextureBuilderGetWidth);
    NVN_REG(nvnTextureBuilderGetHeight);
    NVN_REG(nvnTextureBuilderGetDepth);
    NVN_REG(nvnTextureBuilderGetViewOffset);
    NVN_REG(nvnTextureBuilderGetLevels);
    NVN_REG(nvnTextureBuilderGetFormat);
    NVN_REG(nvnTextureBuilderGetSamples);
    NVN_REG(nvnTextureBuilderGetSwizzle);
    NVN_REG(nvnTextureBuilderGetDepthStencilMode);
    NVN_REG(nvnTextureBuilderGetPackagedTextureData);
    NVN_REG(nvnTextureBuilderGetStride);
    NVN_REG(nvnTextureBuilderGetGLTextureName);
    NVN_REG(nvnTextureBuilderGetMemoryPool);
    NVN_REG(nvnTextureBuilderGetMemoryOffset);
    NVN_REG(nvnTextureViewSetDefaults);
    NVN_REG(nvnTextureViewSetLevels);
    NVN_REG(nvnTextureViewSetLayers);
    NVN_REG(nvnTextureViewSetFormat);
    NVN_REG(nvnTextureViewSetSwizzle);
    NVN_REG(nvnTextureViewSetDepthStencilMode);
    NVN_REG(nvnTextureViewSetTarget);
    NVN_REG(nvnTextureViewGetLevels);
    NVN_REG(nvnTextureViewGetLayers);
    NVN_REG(nvnTextureViewGetFormat);
    NVN_REG(nvnTextureViewGetSwizzle);
    NVN_REG(nvnTextureViewGetDepthStencilMode);
    NVN_REG(nvnTextureViewGetTarget);
    NVN_REG(nvnTextureViewCompare);
}

}  // namespace nxrt::nvn
