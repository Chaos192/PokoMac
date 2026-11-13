// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Texture content override (S17): lets the host replace the pixels of a small BC7 texture the game uploads, by its content.
// Used for the button prompt icons (src/gfx/play/prompts.cpp): the game's own 36x36 icons are replaced by Xbox, PlayStation or
// keyboard and mouse icons. The hook runs on the render thread inside the texture upload; it must be quick and thread safe.
#pragma once
#include <cstddef>
#include <cstdint>

namespace nxrt::gfx {

struct TexOverrideReq {
    uint32_t width = 0, height = 0;       // texels; the texture is a single-level, single-layer 2D BC7 sRGB texture of at most 128 x 128
    const uint8_t* tiled = nullptr;       // the texture's storage in the game's memory exactly as the game wrote it (block-linear)
    size_t tiled_size = 0;
};

// Returns true after writing width * height * 4 bytes of straight-alpha sRGB RGBA8 to rgba, in the row order of the texture's storage (the
// game's UI textures are stored upside down: row 0 is the bottom row of the picture); the texture then shows that image (encoded to BC7 at the
// same size). false: leave the game's texture alone.
using TexOverrideFn = bool (*)(const TexOverrideReq& req, uint8_t* rgba, void* user);

void set_texture_override(TexOverrideFn fn, void* user);   // fn == nullptr removes the hook
// Call after the answers of the hook changed: every texture it may serve is uploaded again the next time it is used. Any thread.
void texture_override_changed();

}  // namespace nxrt::gfx
