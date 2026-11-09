// SPDX-License-Identifier: GPL-3.0-or-later
// Placeholder until the AVFoundation movie:: implementation lands: no host movie library, so the game's own (lifted) subsdk0 library
// stays in place.
#include "nxrt/movie.h"

namespace nxrt::media {
bool install_movie_hle() { return false; }
const char* const* movie_import_names(size_t* count) {
    if (count) *count = 0;
    return nullptr;
}
MovieStats movie_stats() { return {}; }
}  // namespace nxrt::media
