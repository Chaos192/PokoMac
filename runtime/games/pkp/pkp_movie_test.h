// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Test hook: plays a movie through the game's own player and saves decoded frames ([test] movie = MV02). See pkp_movie_test.cpp.
#pragma once
#include "nxrt/abi.h"

namespace nxrt::games::pkp {

void movie_test_frame(::Ctx* c);   // the game thread, once a frame (pkp_play.cpp frame hook)

}  // namespace nxrt::games::pkp
