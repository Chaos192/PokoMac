// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "nvn_impl.h"
#include "nvn_shaders.h"

#include <mutex>

namespace nxrt::nvn {

void register_all_impls() {
    static std::once_flag once;
    std::call_once(once, [] {
        register_api_builders();
        register_api_sampler();
        register_api_texture_builder();
        register_api_blend_color();
        register_api_vertex();
        register_api_depth_poly_ms();
        register_api_cmdbuf();
        register_api_cmd_state();
        register_api_cmd_work();
        register_api_device();
        register_api_memory();
        register_api_texture();
        register_api_program();
        // Open the shader cache now, at backend install: its recorded-order preload starts reading on background threads
        // before the game builds its first program.
        be::shader_cache();
    });
}

}  // namespace nxrt::nvn
