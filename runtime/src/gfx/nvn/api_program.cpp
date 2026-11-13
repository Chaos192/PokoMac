// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NVNprogram: the host Program object (nvn_core.h) behind the 0xC0-byte guest object, and the entry points that set its
// stages. SetShaders takes each NVNshaderData{data, control}: data is the GPU address of the code block (a host pointer into
// the game's pool), control the control section. The shader cache (nvn_shaders.h) gives the ShaderInfo of the block; its
// NVN stage comes from the SPIR-V execution model. A block the cache cannot translate leaves the program unlinked.
//
// Signatures: the five entries in work/nvn/sigs.txt use those types. nvnProgramSetSampleShading has no signature in sigs.txt
// (table entry '?'): assumed void(NVNprogram*, int), the argument order smo-recomp runtime/gfx/nvn_gl.c (GPL-3.0) uses
// (layout/defaults checked, no code copied). nvnProgramSetShadersExt(NVNprogram*, int count, entries): 0x18-byte entries whose first
// two words are NVNshaderData's {data, control}; the third word is not used here. Stride from ACNH 3.0.3 main+0x24d9a8 (the caller
// fills the array with `mov w24, #0x18`), as ACNHRecomp (MIT) found. nvnProgramSetSubroutineLinkage is accepted and ignored: the back end has no subroutine support.
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <cstring>
#include <string>

#include "nvn_core.h"
#include "nvn_gpu.h"
#include "nvn_impl.h"
#include "nvn_shaders.h"
#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"

namespace nxrt::nvn {
using namespace be;
namespace {

// Live programs (nxrt/gauge.h): from Initialize to Finalize, or to an Initialize over the program.
nxrt::GaugeCounter g_programs("nvn.programs");

static_assert(sizeof(GuestHeader) <= sizeof(NVNprogram), "program header fits the NVN object");

// Blocks larger than this are not hashed for a log key (a corrupt control section must not make us read far).
constexpr uint32_t kMaxLogHashBytes = 16u << 20;

// NVN stage index of a translated stage (NVNshaderStage values); -1 for a stage the back end does not know.
int nvn_stage_of(ShaderStage s) {
    switch (s) {
        case ShaderStage::Vertex: return 0;
        case ShaderStage::Fragment: return 1;
        case ShaderStage::Geometry: return 2;
        case ShaderStage::TessControl: return 3;
        case ShaderStage::TessEval: return 4;
        case ShaderStage::Compute: return 5;
        default: return -1;
    }
}

// The 16 hex digits of a shader hash as a number. 0 if the text is not hex.
uint64_t parse_hash(const std::string& hex) {
    uint64_t v = 0;
    for (char ch : hex) {
        uint64_t d;
        if (ch >= '0' && ch <= '9') d = static_cast<uint64_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') d = static_cast<uint64_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') d = static_cast<uint64_t>(ch - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    return v;
}

// True the first time a key is seen: the log line of a missing shader is written once per hash.
bool first_report(const std::string& key) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lk(mu);
    return seen.insert(key).second;
}

// The key of a block the cache could not load: its hash when the control section is sane, else the control address.
std::string missing_key(const uint8_t* code, const uint8_t* control) {
    const ShaderControl c = read_control(control);
    if (c.valid && c.code_size != 0 && c.code_size <= kMaxLogHashBytes) return shader_hash(code, c.code_size);
    char buf[40];
    std::snprintf(buf, sizeof buf, "control@%p", static_cast<const void*>(control));
    return buf;
}

void report_missing(const uint8_t* code, const uint8_t* control) {
    if (!code || !control) {
        if (first_report("null")) NXLOG_W("nvn", "nvnProgramSetShaders: a stage has no code or control address; program not linked");
        return;
    }
    const std::string key = missing_key(code, control);
    if (first_report(key)) NXLOG_W("nvn", "nvnProgramSetShaders: no translated shader for block %s; program not linked", key.c_str());
}

NVNboolean nvnProgramInitialize(NVNprogram* program, NVNdevice* device) {
    (void)device;   // the program needs no device state of its own
    if (!program) return 0;
    const bool was_live = host_of<Program>(program) != nullptr;   // initialised over a live program: replaced, not added
    bind_guest(program, new Program());
    if (!was_live) g_programs.add(1);
    return 1;
}

void nvnProgramFinalize(NVNprogram* program) {
    if (!host_of<Program>(program)) return;
    g_programs.sub(1);
    unbind_guest(program);
}

void nvnProgramSetDebugLabel(NVNprogram* program, const char* label) {
    if (Program* p = host_of<Program>(program)) p->label = label ? label : "";
}

NVNboolean nvnProgramSetShaders(NVNprogram* program, int count, const NVNshaderData* shaders) {
    Program* p = host_of<Program>(program);
    if (!p) return 0;
    std::lock_guard<std::mutex> lk(p->retry_mu);
    const uint64_t gen = shader_cache().generation();   // before the lookups: a rescan during them makes the next bind look again
    p->pending.clear();
    p->has_pending.store(false, std::memory_order_relaxed);
    for (int s = 0; s < 6; ++s) {
        p->stage[s] = nullptr;
        p->code_hash[s] = 0;
        p->src_code[s] = p->src_control[s] = nullptr;
    }
    p->missing[0] = 0;
    ++p->serial;
    p->linked = false;
    p->gpl_bound.store(0, std::memory_order_relaxed);
    if (count > 0 && !shaders) return 0;

    bool all_found = true;
    bool addresses_ok = true;     // every stage had a code and a control address (the block is just not in the cache)
    for (int k = 0; k < count; ++k) {
        const uint8_t* code = host_ptr(shaders[k].data);
        const uint8_t* control = static_cast<const uint8_t*>(shaders[k].control);
        const ShaderInfo* si = (code && control) ? shader_cache().find(code, control) : nullptr;
        if (!si) {
            report_missing(code, control);
            if (code && control) {   // remembered for the trace of the program's binds and dispatches
                const std::string key = missing_key(code, control);
                const size_t used = std::strlen(p->missing);
                std::snprintf(p->missing + used, sizeof p->missing - used, "%s%s", used ? "," : "", key.c_str());
                p->pending.emplace_back(code, control);
            }
            all_found = false;
            if (!code || !control) addresses_ok = false;
            continue;
        }
        const int stage = nvn_stage_of(si->stage);
        if (stage < 0) {
            if (first_report("stage:" + si->hash))
                NXLOG_W("nvn", "nvnProgramSetShaders: block %s is a stage the back end does not run; program not linked", si->hash.c_str());
            all_found = false;
            continue;
        }
        p->stage[stage] = si;
        p->code_hash[stage] = parse_hash(si->hash);
        p->src_code[stage] = code;
        p->src_control[stage] = control;
    }
    p->linked = all_found;
    p->pending_gen.store(gen, std::memory_order_relaxed);
    p->has_pending.store(!p->pending.empty(), std::memory_order_release);
    if (all_found && gpu::active()) gpu::prewarm_program_shaders(p->stage);
    // A block missing from the shader cache leaves the program unlinked (its draws are skipped). The call still reports success: the game treats a
    // failed SetShaders as "shader not built" and rebuilds it (programs, buffer allocations out of its pool allocator) on every bind, which costs
    // milliseconds per attempt under the allocator's lock and grows without bound (S18). [nvn] accept_unlinked = 0 restores the old return value.
    static const bool accept_unlinked = config_get_bool("nvn", "accept_unlinked", true);
    return (all_found || (accept_unlinked && addresses_ok)) ? 1 : 0;
}

}  // namespace

void be::program_retry_missing(Program* p) {
    if (!p->has_pending.load(std::memory_order_acquire)) return;
    const uint64_t gen = shader_cache().generation();
    if (gen == p->pending_gen.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(p->retry_mu);
    if (!p->has_pending.load(std::memory_order_relaxed) || gen == p->pending_gen.load(std::memory_order_relaxed)) return;
    p->pending_gen.store(gen, std::memory_order_relaxed);
    size_t kept = 0;
    for (const auto& [code, control] : p->pending) {
        const ShaderInfo* si = shader_cache().find(code, control);
        const int stage = si ? nvn_stage_of(si->stage) : -1;
        if (stage < 0 || p->stage[stage]) {   // still missing, a stage the back end does not run, or the slot is taken: stays pending
            p->pending[kept++] = {code, control};
            continue;
        }
        p->code_hash[stage] = parse_hash(si->hash);
        p->src_code[stage] = code;
        p->src_control[stage] = control;
        p->stage[stage] = si;   // last: the replay thread reads stage[] alone, and a slot only ever goes from empty to this shader
        NXLOG_I("nvn", "shader %s arrived: program %p has its %s stage now", si->hash.c_str(), static_cast<void*>(p), stage_name(si->stage));
    }
    p->pending.resize(kept);
    if (kept) return;
    p->missing[0] = 0;
    p->linked = true;
    p->has_pending.store(false, std::memory_order_release);
    if (gpu::active()) gpu::prewarm_program_shaders(p->stage);
}

namespace {

// The Ext entry: NVNshaderData plus one more word (see the header comment).
struct ShaderDataExt {
    NVNshaderData base;
    uint64_t extra;
};
static_assert(sizeof(ShaderDataExt) == 0x18, "nvnProgramSetShadersExt entries are 0x18 bytes");

NVNboolean nvnProgramSetShadersExt(NVNprogram* program, int count, const ShaderDataExt* shaders) {
    if (count > 0 && !shaders) return nvnProgramSetShaders(program, count, nullptr);
    NVNshaderData plain[6]{};
    if (count < 0 || count > 6) {
        NXLOG_W("nvn", "nvnProgramSetShadersExt: %d stages (at most 6)", count);
        return 0;
    }
    for (int k = 0; k < count; ++k) plain[k] = shaders[k].base;
    return nvnProgramSetShaders(program, count, plain);
}

NVNboolean nvnProgramSetSubroutineLinkage(NVNprogram* program, int stage, const NVNsubroutineLinkageMapPtr* map) {
    (void)stage;
    (void)map;
    if (!host_of<Program>(program)) return 0;
    if (first_report("subroutine-linkage")) NXLOG_W("nvn", "nvnProgramSetSubroutineLinkage: subroutines are not supported; the linkage is ignored");
    return 1;
}

// Assumed signature (see the header comment). The back end keeps no sample-shading state yet, so this only validates.
void nvnProgramSetSampleShading(NVNprogram* program, int mode) {
    (void)mode;
    if (!host_of<Program>(program)) return;
}

// NVN 55 name the game asks for and never calls (signature assumed): the sub-tile size is not modelled; it reports 0 x 0.
int nvnProgramGetSubtileSize(const NVNprogram*, int* width, int* height) {
    if (width) *width = 0;
    if (height) *height = 0;
    return 0;
}

}  // namespace

void register_api_program() {
    NVN_REG(nvnProgramGetSubtileSize);
    NVN_REG(nvnProgramInitialize);
    NVN_REG(nvnProgramFinalize);
    NVN_REG(nvnProgramSetDebugLabel);
    NVN_REG(nvnProgramSetShaders);
    NVN_REG(nvnProgramSetShadersExt);
    NVN_REG(nvnProgramSetSubroutineLinkage);
    NVN_REG(nvnProgramSetSampleShading);
}

}  // namespace nxrt::nvn
