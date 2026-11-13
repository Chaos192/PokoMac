// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pass-through tessellation control shader for NVN programs that have an evaluation shader only. NVN then runs the fixed-function
// patch pass-through with the levels of nvnCommandBufferSetInner/OuterTessellationLevels; Vulkan always needs a control shader.
//
// The generated shader (SPIR-V 1.3, written directly as words):
//   - copies, per control point, every per-vertex input of the evaluation shader that the vertex shader writes (same Location and
//     Component), converting the vertex shader's type to the evaluation shader's (component count, int/float bits); an evaluation input
//     the vertex shader does not write gets zero (the console leaves it undefined),
//   - copies gl_Position (the translator's gl_PerVertex block, as the vertex and evaluation shaders declare it), and gl_ClipDistance /
//     gl_CullDistance (whole arrays) when asked (the vertex shader writes them and the evaluation shader reads gl_in[].gl_*Distance),
//   - writes zero to the evaluation shader's per-patch inputs,
//   - sets gl_TessLevelOuter / gl_TessLevelInner from a push constant block (TessPush, offset 0, kPushBytes, tessellation control
//     stage), so a level change between draws needs no new shader or pipeline.
// One module per (vertex shader outputs, evaluation shader inputs, patch size). Header-only, no dependencies, so a standalone test can
// validate the words with spirv-val.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <vector>

namespace nxrt::nvn::be::tessgen {

enum class Kind : uint8_t { Float, Sint, Uint };

// One Location-decorated interface variable (not an array; the per-vertex arrayness of tessellation interfaces is implied).
struct Io {
    uint32_t location = 0, component = 0;
    uint32_t components = 4;   // 1..4
    Kind kind = Kind::Float;
};

// A per-vertex input of the evaluation shader (dst) and the vertex shader output that feeds it (src), if any.
struct Copy {
    Io dst;
    bool has_src = false;
    Io src;
};

// The push constants the generated shader reads (std430-like offsets, as the SPIR-V declares them).
struct TessPush {
    float outer[4];
    float inner[2];
    float pad[2];
};
constexpr uint32_t kPushBytes = sizeof(TessPush);
static_assert(kPushBytes == 32, "outer vec4 at 0, inner vec2 at 16");

// Pairs each per-vertex evaluation input with the vertex output at the same Location and Component.
inline std::vector<Copy> match(const std::vector<Io>& vs_outputs, const std::vector<Io>& tes_per_vertex) {
    std::vector<Copy> out;
    for (const Io& d : tes_per_vertex) {
        Copy c;
        c.dst = d;
        for (const Io& s : vs_outputs)
            if (s.location == d.location && s.component == d.component) { c.has_src = true; c.src = s; break; }
        out.push_back(c);
    }
    return out;
}

// vertices: output patch size (clamped to 1..32); the input arrays have 32 (gl_MaxPatchVertices) elements. The gl_PerVertex block
// matches the translator's ({vec4 Position, float PointSize, float ClipDistance[clip_count], float CullDistance[cull_count]}).
// copy_clip / copy_cull: gl_out[id].gl_ClipDistance = gl_in[id].gl_ClipDistance (and the cull distances); otherwise they are not written.
inline std::vector<uint32_t> passthrough_tcs(const std::vector<Copy>& per_vertex, const std::vector<Io>& per_patch, uint32_t vertices,
                                             uint32_t clip_count = 7, uint32_t cull_count = 1, bool copy_clip = false, bool copy_cull = false) {
    enum : uint32_t {
        kCapability = 17, kMemoryModel = 14, kEntryPoint = 15, kExecutionMode = 16, kDecorate = 71, kMemberDecorate = 72, kTypeVoid = 19,
        kTypeInt = 21, kTypeFloat = 22, kTypeVector = 23, kTypeArray = 28, kTypeStruct = 30, kTypePointer = 32, kTypeFunction = 33,
        kConstant = 43, kConstantNull = 46, kFunction = 54, kFunctionEnd = 56, kVariable = 59, kLoad = 61, kStore = 62, kAccessChain = 65,
        kCompositeConstruct = 80, kCompositeExtract = 81, kBitcast = 124, kLabel = 248, kReturn = 253,
        kInput = 1, kOutput = 3, kPushConstant = 9,
        kBlock = 2, kBuiltIn = 11, kPatch = 15, kLocation = 30, kComponent = 31, kOffset = 35,
        kPosition = 0, kPointSize = 1, kClipDistance = 3, kCullDistance = 4, kInvocationId = 8, kTessLevelOuter = 11, kTessLevelInner = 12,
        kOutputVertices = 26,
    };
    if (vertices < 1) vertices = 1;
    if (vertices > 32) vertices = 32;

    // sections, in module order
    std::vector<uint32_t> ann, glob, code;
    uint32_t next = 1;
    auto id = [&] { return next++; };
    auto op = [](std::vector<uint32_t>& s, uint32_t code_, std::initializer_list<uint32_t> args) {
        s.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | code_);
        s.insert(s.end(), args.begin(), args.end());
    };

    // types and constants, made once each (key: opcode and operands)
    std::map<std::vector<uint32_t>, uint32_t> made;
    auto type = [&](uint32_t code_, std::initializer_list<uint32_t> args) {
        std::vector<uint32_t> k = {code_};
        k.insert(k.end(), args.begin(), args.end());
        auto it = made.find(k);
        if (it != made.end()) return it->second;
        const uint32_t r = id();
        glob.push_back((static_cast<uint32_t>(args.size() + 2) << 16) | code_);
        glob.push_back(r);
        glob.insert(glob.end(), args.begin(), args.end());
        made.emplace(std::move(k), r);
        return r;
    };
    auto constant = [&](uint32_t t, uint32_t bits) {   // OpConstant has the result type first
        std::vector<uint32_t> k = {kConstant, t, bits};
        auto it = made.find(k);
        if (it != made.end()) return it->second;
        const uint32_t r = id();
        op(glob, kConstant, {t, r, bits});
        made.emplace(std::move(k), r);
        return r;
    };
    auto null_of = [&](uint32_t t) {
        std::vector<uint32_t> k = {kConstantNull, t};
        auto it = made.find(k);
        if (it != made.end()) return it->second;
        const uint32_t r = id();
        op(glob, kConstantNull, {t, r});
        made.emplace(std::move(k), r);
        return r;
    };

    const uint32_t t_void = type(kTypeVoid, {});
    const uint32_t t_fn = type(kTypeFunction, {t_void});
    const uint32_t t_float = type(kTypeFloat, {32});
    const uint32_t t_int = type(kTypeInt, {32, 1});
    const uint32_t t_uint = type(kTypeInt, {32, 0});
    auto scalar = [&](Kind k) { return k == Kind::Float ? t_float : k == Kind::Sint ? t_int : t_uint; };
    auto value_type = [&](const Io& io) {
        const uint32_t s = scalar(io.kind);
        return io.components <= 1 ? s : type(kTypeVector, {s, io.components > 4 ? 4u : io.components});
    };
    auto uconst = [&](uint32_t v) { return constant(t_uint, v); };
    auto iconst = [&](int32_t v) { return constant(t_int, static_cast<uint32_t>(v)); };
    auto array = [&](uint32_t elem, uint32_t n) { return type(kTypeArray, {elem, uconst(n)}); };
    auto ptr = [&](uint32_t storage, uint32_t t) { return type(kTypePointer, {storage, t}); };
    auto variable = [&](uint32_t pointer_type, uint32_t storage) {
        const uint32_t r = id();
        op(glob, kVariable, {pointer_type, r, storage});
        return r;
    };
    const uint32_t t_v4 = type(kTypeVector, {t_float, 4});
    const uint32_t t_v2 = type(kTypeVector, {t_float, 2});

    std::vector<uint32_t> iface;   // the entry point's Input/Output variables

    // built-ins
    const uint32_t v_invocation = variable(ptr(kInput, t_int), kInput);
    op(ann, kDecorate, {v_invocation, kBuiltIn, kInvocationId});
    iface.push_back(v_invocation);
    // gl_PerVertex: one struct type per direction (as the translator and glslang declare them)
    uint32_t pv[2];
    for (uint32_t& s : pv) {
        s = id();
        op(glob, kTypeStruct, {s, t_v4, t_float, array(t_float, clip_count ? clip_count : 1), array(t_float, cull_count ? cull_count : 1)});
        op(ann, kMemberDecorate, {s, 0, kBuiltIn, kPosition});
        op(ann, kMemberDecorate, {s, 1, kBuiltIn, kPointSize});
        op(ann, kMemberDecorate, {s, 2, kBuiltIn, kClipDistance});
        op(ann, kMemberDecorate, {s, 3, kBuiltIn, kCullDistance});
        op(ann, kDecorate, {s, kBlock});
    }
    const uint32_t v_gl_in = variable(ptr(kInput, array(pv[0], 32)), kInput);
    const uint32_t v_gl_out = variable(ptr(kOutput, array(pv[1], vertices)), kOutput);
    const uint32_t v_outer = variable(ptr(kOutput, array(t_float, 4)), kOutput);
    const uint32_t v_inner = variable(ptr(kOutput, array(t_float, 2)), kOutput);
    op(ann, kDecorate, {v_outer, kBuiltIn, kTessLevelOuter});
    op(ann, kDecorate, {v_outer, kPatch});
    op(ann, kDecorate, {v_inner, kBuiltIn, kTessLevelInner});
    op(ann, kDecorate, {v_inner, kPatch});
    iface.insert(iface.end(), {v_gl_in, v_gl_out, v_outer, v_inner});

    // push constants: { vec4 outer; vec2 inner; }
    const uint32_t t_push = id();
    op(glob, kTypeStruct, {t_push, t_v4, t_v2});
    op(ann, kDecorate, {t_push, kBlock});
    op(ann, kMemberDecorate, {t_push, 0, kOffset, 0});
    op(ann, kMemberDecorate, {t_push, 1, kOffset, 16});
    const uint32_t v_push = variable(ptr(kPushConstant, t_push), kPushConstant);   // not in the 1.3 interface list (Input/Output only)

    auto decorate_io = [&](uint32_t v, const Io& io) {
        op(ann, kDecorate, {v, kLocation, io.location});
        if (io.component) op(ann, kDecorate, {v, kComponent, io.component});
    };
    struct Var { uint32_t in = 0, out = 0; };
    std::vector<Var> pv_vars(per_vertex.size());
    for (size_t i = 0; i < per_vertex.size(); ++i) {
        const Copy& c = per_vertex[i];
        if (c.has_src) {
            pv_vars[i].in = variable(ptr(kInput, array(value_type(c.src), 32)), kInput);
            decorate_io(pv_vars[i].in, c.src);
            iface.push_back(pv_vars[i].in);
        }
        pv_vars[i].out = variable(ptr(kOutput, array(value_type(c.dst), vertices)), kOutput);
        decorate_io(pv_vars[i].out, c.dst);
        iface.push_back(pv_vars[i].out);
    }
    std::vector<uint32_t> patch_vars;
    for (const Io& p : per_patch) {
        const uint32_t v = variable(ptr(kOutput, value_type(p)), kOutput);
        decorate_io(v, p);
        op(ann, kDecorate, {v, kPatch});
        iface.push_back(v);
        patch_vars.push_back(v);
    }

    // main
    const uint32_t f_main = id();
    op(code, kFunction, {t_void, f_main, 0, t_fn});
    op(code, kLabel, {id()});
    const uint32_t inv = id();
    op(code, kLoad, {t_int, inv, v_invocation});

    // `val` of type src converted to dst: components kept in order (missing ones zero), int/float bits reinterpreted
    auto convert = [&](uint32_t val, const Io& src, const Io& dst) {
        const uint32_t sn = src.components < 1 ? 1 : src.components > 4 ? 4 : src.components;
        const uint32_t dn = dst.components < 1 ? 1 : dst.components > 4 ? 4 : dst.components;
        if (src.kind == dst.kind && sn == dn) return val;
        const uint32_t ss = scalar(src.kind), ds = scalar(dst.kind);
        std::vector<uint32_t> parts;
        for (uint32_t c = 0; c < dn; ++c) {
            if (c >= sn) { parts.push_back(null_of(ds)); continue; }
            uint32_t e = val;
            if (sn > 1) {
                e = id();
                op(code, kCompositeExtract, {ss, e, val, c});
            }
            if (ss != ds) {
                const uint32_t b = id();
                op(code, kBitcast, {ds, b, e});
                e = b;
            }
            parts.push_back(e);
        }
        if (dn == 1) return parts[0];
        const uint32_t r = id();
        std::vector<uint32_t> args = {value_type(dst), r};
        args.insert(args.end(), parts.begin(), parts.end());
        code.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | kCompositeConstruct);
        code.insert(code.end(), args.begin(), args.end());
        return r;
    };

    for (size_t i = 0; i < per_vertex.size(); ++i) {   // out[id] = in[id] (or zero)
        const Copy& c = per_vertex[i];
        const uint32_t dt = value_type(c.dst);
        uint32_t val;
        if (c.has_src) {
            const uint32_t st = value_type(c.src), src = id();
            op(code, kAccessChain, {ptr(kInput, st), src, pv_vars[i].in, inv});
            val = id();
            op(code, kLoad, {st, val, src});
            val = convert(val, c.src, c.dst);
        } else {
            val = null_of(dt);
        }
        const uint32_t dst = id();
        op(code, kAccessChain, {ptr(kOutput, dt), dst, pv_vars[i].out, inv});
        op(code, kStore, {dst, val});
    }
    {   // gl_out[id].gl_Position = gl_in[id].gl_Position
        const uint32_t src = id(), val = id(), dst = id();
        op(code, kAccessChain, {ptr(kInput, t_v4), src, v_gl_in, inv, iconst(0)});
        op(code, kLoad, {t_v4, val, src});
        op(code, kAccessChain, {ptr(kOutput, t_v4), dst, v_gl_out, inv, iconst(0)});
        op(code, kStore, {dst, val});
    }
    for (uint32_t member : {2u, 3u}) {   // gl_out[id].gl_ClipDistance / gl_CullDistance = gl_in[id]'s, the whole array
        if (!(member == 2 ? copy_clip : copy_cull)) continue;
        const uint32_t n = member == 2 ? clip_count : cull_count;
        const uint32_t at = array(t_float, n ? n : 1);
        const uint32_t src = id(), val = id(), dst = id();
        op(code, kAccessChain, {ptr(kInput, at), src, v_gl_in, inv, iconst(static_cast<int32_t>(member))});
        op(code, kLoad, {at, val, src});
        op(code, kAccessChain, {ptr(kOutput, at), dst, v_gl_out, inv, iconst(static_cast<int32_t>(member))});
        op(code, kStore, {dst, val});
    }
    // per-patch values: every invocation writes the same ones (identical values from several invocations are well defined)
    {
        const uint32_t po = id(), outer = id(), pi = id(), inner = id();
        op(code, kAccessChain, {ptr(kPushConstant, t_v4), po, v_push, iconst(0)});
        op(code, kLoad, {t_v4, outer, po});
        op(code, kAccessChain, {ptr(kPushConstant, t_v2), pi, v_push, iconst(1)});
        op(code, kLoad, {t_v2, inner, pi});
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t e = id(), dst = id();
            op(code, kCompositeExtract, {t_float, e, outer, i});
            op(code, kAccessChain, {ptr(kOutput, t_float), dst, v_outer, iconst(static_cast<int32_t>(i))});
            op(code, kStore, {dst, e});
        }
        for (uint32_t i = 0; i < 2; ++i) {
            const uint32_t e = id(), dst = id();
            op(code, kCompositeExtract, {t_float, e, inner, i});
            op(code, kAccessChain, {ptr(kOutput, t_float), dst, v_inner, iconst(static_cast<int32_t>(i))});
            op(code, kStore, {dst, e});
        }
    }
    for (size_t i = 0; i < per_patch.size(); ++i) op(code, kStore, {patch_vars[i], null_of(value_type(per_patch[i]))});
    op(code, kReturn, {});
    op(code, kFunctionEnd, {});

    // assemble
    std::vector<uint32_t> w = {0x07230203u, 0x00010300u, 0u, next, 0u};
    op(w, kCapability, {1});   // Shader
    op(w, kCapability, {3});   // Tessellation
    op(w, kMemoryModel, {0, 1});   // Logical GLSL450
    {
        std::vector<uint32_t> args = {1 /* TessellationControl */, f_main, 0x6e69616du /* "main" */, 0u};
        args.insert(args.end(), iface.begin(), iface.end());
        w.push_back((static_cast<uint32_t>(args.size() + 1) << 16) | kEntryPoint);
        w.insert(w.end(), args.begin(), args.end());
    }
    op(w, kExecutionMode, {f_main, kOutputVertices, vertices});
    w.insert(w.end(), ann.begin(), ann.end());
    w.insert(w.end(), glob.begin(), glob.end());
    w.insert(w.end(), code.begin(), code.end());
    w[3] = next;   // the bound: ids made while assembling are none, but keep it last
    return w;
}

}  // namespace nxrt::nvn::be::tessgen
