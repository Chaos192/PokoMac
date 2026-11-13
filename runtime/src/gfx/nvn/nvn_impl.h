// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Registration of the NVN implementations (S4d). Each family of entry points lives in its own api_*.cpp and exposes
// one register function, which calls NVN_REG (nvn_abi.h) for every function it implements.
#pragma once
#include "nvn_abi.h"
#include "nvn_decoded.h"

namespace nxrt::nvn {

// Plain-data families (builders, state objects, samplers, texture views): api_*.cpp, no GPU involved.
void register_api_builders();       // DeviceBuilder, QueueBuilder, MemoryPoolBuilder, BufferBuilder, WindowBuilder, EventBuilder
void register_api_sampler();        // SamplerBuilder, Sampler
void register_api_texture_builder();// TextureBuilder, TextureView
void register_api_blend_color();    // BlendState, ColorState, ChannelMaskState
void register_api_vertex();         // VertexAttribState, VertexStreamState (the reference family)
void register_api_depth_poly_ms();  // DepthStencilState, PolygonState, MultisampleState

// Command buffer: lifecycle and memory bookkeeping (api_cmdbuf.cpp), recorders of the state and bind commands (api_cmd_state.cpp)
// and of draws, clears, copies, compute, counters, sync (api_cmd_work.cpp). All of them only record; no GPU involved.
void register_api_cmdbuf();
void register_api_cmd_state();
void register_api_cmd_work();

// Host-side families that talk to the GPU layer only through nvn_gpu.h.
void register_api_device();         // Device, Queue, Sync, Event, Window (api_device.cpp)
void register_api_memory();         // MemoryPool, Buffer, TexturePool, SamplerPool (api_memory.cpp)
void register_api_texture();        // Texture and the builder functions that need the layout (api_texture.cpp)
void register_api_program();        // Program (api_program.cpp)

// Registers every implementation above; called by install_backend(). Idempotent.
void register_all_impls();

}  // namespace nxrt::nvn
