// SPDX-License-Identifier: GPL-3.0-or-later
// PokoMac has no host redirection (AnySwitch's net/redirect.h: a title's [net.redirect] table sending named hosts to replacement servers).
// This header keeps the one question AnySwitch's services ask of it, so they build here unchanged: no title is ever redirected, so the
// account service never reports a network service licence and nothing reaches a server through it.
#pragma once

namespace nxrt::redirect {

inline bool active() { return false; }

}  // namespace nxrt::redirect
