// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPIR-V rewrites the back end applies to the translated shaders when it builds a pipeline (render thread). The offline translator
// knows one shader at a time; the console's rules are looser than Vulkan's about what the stages of a pipeline have to agree on.
//
//   spirv_add_outputs   a fragment shader reads varyings (or an indexed array of them) that the stage before it never declared. The
//                       console hands it undefined values; Vulkan wants an output in every slot (VUID-RuntimeSpirv-OpEntryPoint-08743).
//                       The producer gets unwritten outputs there.
//   spirv_dual_source_output
//                       a fragment shader of a draw with dual-source blending: the console takes the second blend source from render
//                       target 1's output; Vulkan from the output at Location 0, Index 1 (and allows a single such attachment).
//
//   spirv_add_xfb      a vertex shader that runs with transform feedback: the Location outputs are captured into buffer 0 in Location
//                       order, each narrowed to the components the shader stores (a vec4 whose xyz are written is captured as 12
//                       bytes), tightly packed. The console's layout lives in the driver's compile data; this is the layout the
//                       game's stream-out shaders use (docs/S4e.md).
//
//   spirv_init_outputs  a vertex, tessellation evaluation or geometry shader that writes only some components of a Location output (the
//                       sky dome's vertex shader stores .w of one vec4 and .xyz of others) while the next stage reads all four. The
//                       console (and Ryujinx) start every user output at (0,0,0,1); in Vulkan an unwritten component is undefined: AMD's
//                       Windows driver happens to hand 0, Mesa RADV hands garbage (a NaN that blacks out the HDR frame). The entry point
//                       now stores (0,0,0,1) to every Location output first, and a geometry shader again after each emitted vertex.
//
//   spirv_base_instance_from_c0
//                       a vertex shader that adds NVN's base instance word c[0] + 0x644 to the instance ID (GLSLC's instance
//                       index; the translation reads `gl_InstanceIndex - gl_BaseInstance + c0[100].y`). The word is the draw's
//                       base instance, which for an indirect draw lives in the arguments the GPU may write (and a multi-draw has
//                       one per draw). Each load of c0[100].y becomes a load of the BaseInstance built-in, which Vulkan takes from
//                       the draw itself, direct or indirect.
//
//   spirv_ballot_per_lane
//                       a compute shader's warp-aggregated atomic counter (ballot(true), the highest lane leads, adds the bit count
//                       for all, hands each lane its slot by a shuffle plus its rank) can stop NVIDIA GPUs for good after some lanes
//                       returned (device lost). Each such ballot becomes the lane's own bit: every lane leads itself and adds 1.
//                       Approach from pokopia-native (MIT) windows/docs/ballot-hang.md, vk_shader.c @4a72d23; the matcher is ours.
//
// The idea (pattern-matching on the module's declarations, new ids from the bound) follows SuperMarioWonderRecomp's
// nvn_spirv_patch.cpp (MIT); the code is new.
#pragma once
#include <cstdint>
#include <vector>

namespace nxrt::nvn::be::gpu {

enum class SpvScalar : uint8_t { Float, Sint, Uint };

struct SpvOutputSlot {
    uint32_t location = 0;
    SpvScalar type = SpvScalar::Float;
};

// Adds one never-written vec4 Output variable per slot (a 4-component vector of the slot's scalar type) to the entry point of `in`.
// Returns false (out untouched) when the module cannot be rewritten (malformed, no entry point, a slot that already has an output).
bool spirv_add_outputs(const std::vector<uint32_t>& in, const std::vector<SpvOutputSlot>& slots, std::vector<uint32_t>& out);

// Vertex inputs that carry integers. The console hands a vertex shader the raw bits of an integer vertex attribute (RGBA8UI bone
// indices, say) in the float registers the translation reads, and the translated shader declares every input as a float and
// bitcasts it where it wants an integer. A Vulkan integer format needs an integer input. For each slot the Input variable becomes a
// Private variable and a new Input variable of uvec4 (ivec4) takes its Location; the entry point copies it over with an OpBitcast, so
// the shader reads the same bits as on the console. The slot's input is a vec4 of floats, or an element of an Input array of vec4 (the
// translation of a shader that indexes its attributes by a computed value): then the array becomes Private as a whole, every Location it
// covers gets its own new Input (float, uvec4 or ivec4 by what the draw feeds) and the entry point builds the array from them. False (out
// untouched) otherwise.
struct SpvIntSlot {
    uint32_t location = 0;
    bool is_signed = false;
};
bool spirv_int_attributes(const std::vector<uint32_t>& in, const std::vector<SpvIntSlot>& slots, std::vector<uint32_t>& out);
// The vertex input locations (is_signed = false) a game would have to feed with integer attributes: the vector inputs whose every use, in any
// component, is a bitcast to a 32-bit integer (the translator reads raw integer attribute bits that way). A guess from the code alone, made
// so the patched variant can be compiled before the first draw; a draw whose real integer slots differ builds its own variant.
std::vector<SpvIntSlot> spirv_predict_int_inputs(const std::vector<uint32_t>& in);

// A geometry shader the translator made for a console "passthrough" geometry shader (the hardware copies every attribute of the input
// vertices to the output vertices; the shader code only sets the position and gl_Layer, so its translation writes no Location output at
// all). Adds the outputs for `slots` like spirv_add_outputs and makes the k-th OpEmitVertex copy the input at the same Location of vertex k.
// Needs an Input array [3] of vec4 at each slot's Location and exactly three OpEmitVertex in a module with triangle input. False (out
// untouched) otherwise; the caller then uses spirv_add_outputs.
bool spirv_gs_passthrough(const std::vector<uint32_t>& in, const std::vector<SpvOutputSlot>& slots, std::vector<uint32_t>& out);

// Moves the fragment output at Location 1 to Location 0, Index 1. False (out untouched) when the module has no such output.
bool spirv_dual_source_output(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

// A fragment shader of a pass without colour attachments: every Location output becomes a private variable (the decorations go, and
// the entry point's interface loses them before SPIR-V 1.4). False (out untouched) when no Location output exists or a built-in output
// (FragDepth, SampleMask) does.
bool spirv_outputs_to_private(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

// Stores the console's start value (component 3 = 1, the others 0, counted from the Component decoration; 1.0f for floats, 1 for
// integers) to every Location output a Vertex / TessellationEvaluation / Geometry entry point lists, right after the leading OpVariables
// of its first block, and in a geometry shader again after every OpEmitVertex / OpEmitStreamVertex. Covers scalars and vectors of 32-bit
// float / int and Output arrays of them; built-ins are left alone. False (out untouched) for another stage, an unexpected module or
// nothing to initialise.
bool spirv_init_outputs(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

// [nvn] reduced_precision: the module as the translator would have written it with QueryHostReducedPrecision() = true (Ryujinx's
// HostCapabilities.ReducedPrecision, the mode pokopia-native translates with), without translating again. That mode changes exactly two
// things in SPIR-V (tools/shaders/third_party/ryubing CodeGen/Spirv): no NoContraction on the float results of binary / ternary ops, so the
// driver may fuse a multiply and an add, except on the ops ForcePreciseEnable marks (fragment stage only: the FP32 add of a constant whose
// result is the divisor of 1.0 / x, so a small bias against a division by zero is not rounded away); and Invariant on every BuiltIn Position
// (the variable, or member of a gl_PerVertex block). Here every OpDecorate NoContraction goes except, in a fragment module, the one on an
// OpFAdd whose second operand is a float constant and whose result reaches the divisor of an OpFDiv with constant 1.0 as dividend, directly
// or through a Function variable (stores and loads, bitcasts peeled). That keeps a superset of the translator's precise ops. A BuiltIn
// Position decoration gets an Invariant next to it. False (out untouched) when the module is malformed or nothing changes.
bool spirv_reduced_precision(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

struct SpvXfbOutput {
    uint32_t location, components, offset;   // offset in bytes inside a captured vertex
};
// Adds XfbBuffer / XfbStride / Offset decorations, the TransformFeedback capability and the Xfb execution mode. layout receives the
// captured outputs, stride the bytes of one captured vertex. False (out untouched): no Location outputs or an unexpected module.
bool spirv_add_xfb(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, std::vector<SpvXfbOutput>* layout, uint32_t* stride);

// Replaces every OpLoad of c[0] word 0x644 (an access chain into the variable named "c0" / "<stage>_c0" with the constant indices 0,
// 100, 1) by a load of the module's BaseInstance input, bitcast to the loaded type. Vertex modules that already declare BaseInstance
// only (the translator declares it whenever the code reads the instance ID). False (out untouched) when nothing matches.
bool spirv_base_instance_from_c0(const std::vector<uint32_t>& in, std::vector<uint32_t>& out);

struct BallotRewrite {
    int ballots = 0;       // ballots rewritten
    int groups = 0;        // counters found (a group of ballots joined by what they meet)
    int groups_left = 0;   // of those, left as translated (a use the rewrite would change); then the module is left alone
};
// Each warp-aggregated counter's ballot(true) (the leader's FindUMsb, the count's BitCount added by an OpAtomicIAdd inside the leader
// test, the rank's BitCount of LtMask & ballot added to the result the shuffle helper hands out) becomes uvec4(1 << lane, 0, 0, 0),
// every ballot of a counter or none: each lane leads itself, adds its own share and gets a slot of its own (as many slots, in the same
// range, as before; only which lane takes which within a warp changes). Assumes 32-lane subgroups. Other ballots (REDUX / MATCH masks,
// first-lane broadcasts) are left alone. False (out untouched) when nothing is rewritten; stats filled either way when given.
bool spirv_ballot_per_lane(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, BallotRewrite* stats = nullptr);

}  // namespace nxrt::nvn::be::gpu
