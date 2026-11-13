// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// HDR patch of the tone-map fragment shader (hdr_tonemap_patch.h).
#include "extras/hdr_tonemap_patch.h"

#include <cstring>
#include <unordered_map>

namespace nxrt::extras {
namespace {

enum : uint32_t {
    kOpExtInstImport = 11, kOpExtInst = 12, kOpEntryPoint = 15, kOpTypeFloat = 22, kOpTypeVector = 23, kOpTypeImage = 25, kOpTypeSampledImage = 27,
    kOpConstant = 43, kOpFunction = 54, kOpLoad = 61, kOpStore = 62, kOpVectorShuffle = 79, kOpCompositeConstruct = 80, kOpImageSampleImplicitLod = 87,
    kOpFMul = 133,
    kGlslFma = 50, kGlslFMax = 40,
    kExecFragment = 4, kDim3D = 2,
};

struct Inst {
    size_t pos;       // index of the first word
    uint32_t op, len;
};

uint32_t float_bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

}  // namespace

bool spirv_hdr_tonemap(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 8 || in[0] != 0x07230203u) return false;
    std::vector<Inst> insts;
    std::unordered_map<uint32_t, size_t> def;   // result id -> index into insts
    for (size_t i = 5; i < in.size();) {
        const uint32_t op = in[i] & 0xffff, len = in[i] >> 16;
        if (len == 0 || i + len > in.size()) return false;
        insts.push_back({i, op, len});
        // result id position: types and constants have it at word 1; most value instructions at word 2
        auto result_at = [&](uint32_t idx) { if (idx < len) def[in[i + idx]] = insts.size() - 1; };
        switch (op) {
            case kOpExtInstImport: case kOpTypeFloat: case kOpTypeVector: case kOpTypeImage: case kOpTypeSampledImage: result_at(1); break;
            case kOpConstant: case kOpLoad: case kOpVectorShuffle: case kOpCompositeConstruct: case kOpImageSampleImplicitLod: case kOpExtInst: case kOpFMul: result_at(2); break;
            default: break;
        }
        i += len;
    }
    auto get = [&](uint32_t id) -> const Inst* {
        auto it = def.find(id);
        return it == def.end() ? nullptr : &insts[it->second];
    };

    // fragment entry point
    bool fragment = false;
    uint32_t glsl = 0;
    for (const Inst& n : insts) {
        if (n.op == kOpEntryPoint && in[n.pos + 1] == kExecFragment) fragment = true;
        if (n.op == kOpExtInstImport && std::memcmp(&in[n.pos + 2], "GLSL.std.450", 12) == 0) glsl = in[n.pos + 1];
    }
    if (!fragment || !glsl) return false;

    // types: 32-bit float, vec3 of it, the constant 1.0
    uint32_t t_float = 0, t_v3 = 0, c_one = 0;
    for (const Inst& n : insts) if (n.op == kOpTypeFloat && in[n.pos + 2] == 32) t_float = in[n.pos + 1];
    if (!t_float) return false;
    for (const Inst& n : insts) if (n.op == kOpTypeVector && in[n.pos + 2] == t_float && in[n.pos + 3] == 3) t_v3 = in[n.pos + 1];
    for (const Inst& n : insts) if (n.op == kOpConstant && in[n.pos + 1] == t_float && in[n.pos + 3] == float_bits(1.0f)) c_one = in[n.pos + 2];
    if (!t_v3) return false;

    // the 3D samples in program order
    struct Sample { size_t idx; uint32_t result, coord; };
    std::vector<Sample> s3;
    for (size_t k = 0; k < insts.size(); ++k) {
        const Inst& n = insts[k];
        if (n.op != kOpImageSampleImplicitLod) continue;
        const Inst* si = get(in[n.pos + 3]);                 // the sampled image value: an OpLoad of a variable
        if (!si || si->op != kOpLoad) continue;
        const Inst* sty = get(in[si->pos + 1]);              // its type: OpTypeSampledImage
        if (!sty || sty->op != kOpTypeSampledImage) continue;
        const Inst* img = get(in[sty->pos + 2]);             // OpTypeImage: result, sampled type, dim
        if (!img || img->op != kOpTypeImage || in[img->pos + 3] != kDim3D) continue;
        s3.push_back({k, in[n.pos + 2], in[n.pos + 4]});
    }
    if (s3.size() != 2) return false;

    // the curve outputs: the coordinate of the first sample is (Fma(y_r, s, b), Fma(y_g, s, b), Fma(y_b, s, b))
    const Inst* cc = get(s3[0].coord);
    if (!cc || cc->op != kOpCompositeConstruct || cc->len != 6) return false;
    uint32_t y[3];
    for (int c = 0; c < 3; ++c) {
        const Inst* f = get(in[cc->pos + 3 + c]);
        if (!f || f->op != kOpExtInst || in[f->pos + 3] != glsl || in[f->pos + 4] != kGlslFma || f->len != 8) return false;
        y[c] = in[f->pos + 5];
    }

    // the graded colour: Shuffle(sample B) -> vec3 -> Store
    const uint32_t b_res = s3[1].result;
    const Inst* shuf = nullptr;
    for (const Inst& n : insts)
        if (n.op == kOpVectorShuffle && n.len == 8 && in[n.pos + 3] == b_res && in[n.pos + 4] == b_res && in[n.pos + 5] == 0 && in[n.pos + 6] == 1 && in[n.pos + 7] == 2 && in[n.pos + 1] == t_v3) shuf = &n;
    if (!shuf) return false;
    const uint32_t v3_id = in[shuf->pos + 2];
    const Inst* store = nullptr;
    for (size_t k = 0; k < insts.size(); ++k)
        if (insts[k].op == kOpStore && insts[k].pos > shuf->pos && in[insts[k].pos + 2] == v3_id) { store = &insts[k]; break; }
    if (!store) return false;

    // build the new module
    out.clear();
    out.reserve(in.size() + 40);
    uint32_t bound = in[3];
    const uint32_t id_one = c_one ? c_one : bound++;
    uint32_t m[3] = {bound++, bound++, bound++};
    const uint32_t id_vec = bound++, id_mul = bound++;
    out.insert(out.end(), in.begin(), in.begin() + 5);
    out[3] = bound;
    bool const_added = c_one != 0;
    for (const Inst& n : insts) {
        if (!const_added && n.op == kOpFunction) {       // the 1.0 constant goes at the end of the global section, before the first function
            out.push_back((4u << 16) | kOpConstant);
            out.push_back(t_float);
            out.push_back(id_one);
            out.push_back(float_bits(1.0f));
            const_added = true;
        }
        if (&n == store) {
            for (int c = 0; c < 3; ++c) {                // m_c = max(y_c, 1)
                out.push_back((7u << 16) | kOpExtInst);
                out.push_back(t_float);
                out.push_back(m[c]);
                out.push_back(glsl);
                out.push_back(kGlslFMax);
                out.push_back(y[c]);
                out.push_back(id_one);
            }
            out.push_back((6u << 16) | kOpCompositeConstruct);
            out.push_back(t_v3);
            out.push_back(id_vec);
            out.push_back(m[0]);
            out.push_back(m[1]);
            out.push_back(m[2]);
            out.push_back((5u << 16) | kOpFMul);
            out.push_back(t_v3);
            out.push_back(id_mul);
            out.push_back(v3_id);
            out.push_back(id_vec);
            out.push_back(in[n.pos]);                    // the store itself, with the new value
            out.push_back(in[n.pos + 1]);
            out.push_back(id_mul);
            for (uint32_t w = 3; w < n.len; ++w) out.push_back(in[n.pos + w]);
            continue;
        }
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.pos), in.begin() + static_cast<std::ptrdiff_t>(n.pos + n.len));
    }
    return const_added;
}

bool spirv_hdr_tonemap_lut(const std::vector<uint32_t>& in, float peak_ratio, std::vector<uint32_t>& out) {
    enum : uint32_t {
        kOpTypeInt = 21, kOpTypePointer = 32, kOpFunctionEnd = 56, kOpVariable = 59, kOpAccessChain = 65, kOpDecorate = 71, kOpFSub = 131,
        kOpReturn = 253, kDecLocation = 30, kScOutput = 3, kScPrivate = 6, kGlslExp = 27, kGlslSmoothStep = 49,
    };
    if (!(peak_ratio > 1.001f) || in.size() < 8 || in[0] != 0x07230203u) return false;
    std::vector<Inst> insts;
    insts.reserve(in.size() / 4);
    for (size_t i = 5; i < in.size();) {
        const uint32_t op = in[i] & 0xffff, len = in[i] >> 16;
        if (len == 0 || i + len > in.size()) return false;
        insts.push_back({i, op, len});
        i += len;
    }

    uint32_t glsl = 0, entry = 0, t_float = 0, t_int = 0, t_v4 = 0, c_a = 0, c_b = 0;
    size_t entry_inst = SIZE_MAX;
    for (size_t k = 0; k < insts.size(); ++k) {
        const Inst& n = insts[k];
        const uint32_t* w = &in[n.pos];
        if (n.op == kOpEntryPoint && n.len >= 3 && w[1] == kExecFragment) { entry = w[2]; entry_inst = k; }
        if (n.op == kOpExtInstImport && n.len >= 5 && std::memcmp(&w[2], "GLSL.std.450", 12) == 0) glsl = w[1];
        if (n.op == kOpTypeFloat && n.len == 3 && w[2] == 32) t_float = w[1];
    }
    if (!entry || !glsl || !t_float) return false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.pos];
        if (n.op == kOpTypeInt && n.len == 4 && w[2] == 32 && w[3] == 1) t_int = w[1];
        if (n.op == kOpTypeVector && n.len == 4 && w[2] == t_float && w[3] == 4) t_v4 = w[1];
        if (n.op == kOpConstant && n.len == 4 && w[1] == t_float && w[3] == float_bits(5.55555582f)) c_a = w[2];
        if (n.op == kOpConstant && n.len == 4 && w[1] == t_float && w[3] == float_bits(0.0479959995f)) c_b = w[2];
    }
    if (!t_int || !t_v4 || !c_a || !c_b) return false;

    // the output: an Output vec4 variable at Location 0, and the Output float pointer type its components are written through
    std::unordered_map<uint32_t, uint32_t> location;          // id -> Location
    std::unordered_map<uint32_t, uint32_t> int_value;         // int constant id -> value
    uint32_t p_out_v4 = 0, p_out_f = 0, out_var = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.pos];
        if (n.op == kOpDecorate && n.len == 4 && w[2] == kDecLocation) location[w[1]] = w[3];
        if (n.op == kOpConstant && n.len == 4 && w[1] == t_int) int_value[w[2]] = w[3];
        if (n.op == kOpTypePointer && n.len == 4 && w[2] == kScOutput && w[3] == t_v4) p_out_v4 = w[1];
        if (n.op == kOpTypePointer && n.len == 4 && w[2] == kScOutput && w[3] == t_float) p_out_f = w[1];
    }
    if (!p_out_v4 || !p_out_f) return false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.pos];
        if (n.op == kOpVariable && n.len >= 4 && w[1] == p_out_v4 && w[3] == kScOutput) {
            auto it = location.find(w[2]);
            if (it != location.end() && it->second == 0) out_var = w[2];
        }
    }
    if (!out_var) return false;
    uint32_t idx[3] = {0, 0, 0};                               // int constants 0, 1, 2 used to address the output's components
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.pos];
        if (n.op != kOpAccessChain || n.len != 5 || w[1] != p_out_f || w[3] != out_var) continue;
        auto it = int_value.find(w[4]);
        if (it != int_value.end() && it->second < 3) idx[it->second] = w[4];
    }
    if (!idx[0] || !idx[1] || !idx[2]) return false;

    // the shaper: exactly three Fma(x, 5.5555558, 0.047996), one per channel, all in the entry function
    std::vector<size_t> shaper;
    size_t fn_begin = SIZE_MAX, fn_end = SIZE_MAX;
    for (size_t k = 0; k < insts.size(); ++k) {
        const Inst& n = insts[k];
        const uint32_t* w = &in[n.pos];
        if (n.op == kOpFunction && n.len >= 3 && w[2] == entry) fn_begin = k;
        if (n.op == kOpFunctionEnd && fn_begin != SIZE_MAX && fn_end == SIZE_MAX && k > fn_begin) fn_end = k;
        if (n.op == kOpExtInst && n.len == 8 && w[1] == t_float && w[3] == glsl && w[4] == kGlslFma && w[6] == c_a && w[7] == c_b) shaper.push_back(k);
    }
    if (shaper.size() != 3 || fn_begin == SIZE_MAX || fn_end == SIZE_MAX) return false;
    for (size_t k : shaper)
        if (k < fn_begin || k > fn_end) return false;
    size_t first_fn = SIZE_MAX;
    for (size_t k = 0; k < insts.size() && first_fn == SIZE_MAX; ++k)
        if (insts[k].op == kOpFunction) first_fn = k;

    // new ids
    uint32_t bound = in[3];
    const uint32_t p_priv = bound++;
    const uint32_t var[3] = {bound++, bound++, bound++};
    const float a = peak_ratio - 1.0f;
    const uint32_t c0 = bound++, c1 = bound++, c09 = bound++, cA = bound++, cNegInvA = bound++;
    const struct { uint32_t id; float v; } consts[] = {{c0, 0.0f}, {c1, 1.0f}, {c09, 0.9f}, {cA, a}, {cNegInvA, -1.0f / a}};

    out.clear();
    out.reserve(in.size() + 160);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    auto emit = [&](std::initializer_list<uint32_t> words, uint32_t op) {
        out.push_back((static_cast<uint32_t>(words.size() + 1) << 16) | op);
        out.insert(out.end(), words.begin(), words.end());
    };
    auto ext = [&](uint32_t res, uint32_t fn, std::initializer_list<uint32_t> args) {
        out.push_back((static_cast<uint32_t>(5 + args.size()) << 16) | kOpExtInst);
        out.push_back(t_float);
        out.push_back(res);
        out.push_back(glsl);
        out.push_back(fn);
        out.insert(out.end(), args.begin(), args.end());
    };
    const bool interface_all = in[1] >= 0x00010400u;            // SPIR-V 1.4+: every global the entry point uses is in its interface
    for (size_t k = 0; k < insts.size(); ++k) {
        const Inst& n = insts[k];
        const auto copy = [&] { out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.pos), in.begin() + static_cast<std::ptrdiff_t>(n.pos + n.len)); };
        if (k == entry_inst && interface_all) {
            out.push_back(((n.len + 3) << 16) | n.op);
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.pos + 1), in.begin() + static_cast<std::ptrdiff_t>(n.pos + n.len));
            out.insert(out.end(), {var[0], var[1], var[2]});
            continue;
        }
        if (k == first_fn) {                                     // globals: the Private float pointer, the constants, the three channel variables
            emit({p_priv, kScPrivate, t_float}, kOpTypePointer);
            for (const auto& c : consts) emit({t_float, c.id, float_bits(c.v)}, kOpConstant);
            for (uint32_t v : var) emit({p_priv, v, kScPrivate, c0}, kOpVariable);
        }
        if (n.op == kOpReturn && k > fn_begin && k < fn_end) {   // before each return of the entry function: scale the written colour
            const uint32_t x0 = bound++, x1 = bound++, x2 = bound++, m01 = bound++, m = bound++, d = bound++, e = bound++, t = bound++, ex = bound++,
                           om = bound++, f1 = bound++, gate = bound++, ff = bound++, g01 = bound++, gm = bound++;
            uint32_t ptr[3], col[3], res[3];
            for (int c = 0; c < 3; ++c) { ptr[c] = bound++; col[c] = bound++; res[c] = bound++; }
            emit({t_float, x0, var[0]}, kOpLoad);
            emit({t_float, x1, var[1]}, kOpLoad);
            emit({t_float, x2, var[2]}, kOpLoad);
            ext(m01, kGlslFMax, {x0, x1});
            ext(m, kGlslFMax, {m01, x2});
            emit({t_float, d, m, c1}, kOpFSub);                  // the knee at scene 1.0
            ext(e, kGlslFMax, {d, c0});
            emit({t_float, t, e, cNegInvA}, kOpFMul);
            ext(ex, kGlslExp, {t});
            emit({t_float, om, c1, ex}, kOpFSub);
            emit({t_float, f1, om, cA}, kOpFMul);                // F - 1 = (R - 1) * (1 - exp(-e / (R - 1)))
            for (int c = 0; c < 3; ++c) {
                emit({p_out_f, ptr[c], out_var, idx[c]}, kOpAccessChain);
                emit({t_float, col[c], ptr[c]}, kOpLoad);
            }
            ext(g01, kGlslFMax, {col[0], col[1]});
            ext(gm, kGlslFMax, {g01, col[2]});
            ext(gate, kGlslSmoothStep, {c09, c1, gm});
            ext(ff, kGlslFma, {f1, gate, c1});
            for (int c = 0; c < 3; ++c) {
                emit({t_float, res[c], col[c], ff}, kOpFMul);
                emit({ptr[c], res[c]}, kOpStore);
            }
            copy();
            continue;
        }
        copy();
        for (int c = 0; c < 3; ++c)                              // after the shaper's Fma: keep its scene input for the end
            if (k == shaper[c]) emit({var[c], in[n.pos + 5]}, kOpStore);
    }
    out[3] = bound;
    return true;
}

}  // namespace nxrt::extras
