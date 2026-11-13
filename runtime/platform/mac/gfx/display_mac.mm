// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// macOS: the HDR capability of the screen that shows the window (or the main screen), from NSScreen's extended dynamic range values.
// EDR headroom is relative to SDR white: max_nits = headroom * sdr_white_nits, with SDR white taken as the 100-nit reference of the
// macOS reference modes (an XDR display reports a potential headroom of 16 = 1600 nits).
#include "extras/win_display.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstdio>

namespace nxrt::extras {

bool query_display_hdr(void* hwnd, DisplayHdrInfo* out) {
    if (!out) return false;
    *out = DisplayHdrInfo{};
    @autoreleasepool {
        NSScreen* screen = nil;
        if (hwnd) {
            id obj = (__bridge id)hwnd;
            if ([obj isKindOfClass:[NSWindow class]]) screen = ((NSWindow*)obj).screen;
        }
        if (!screen) screen = NSScreen.mainScreen;
        if (!screen) {
            std::snprintf(out->how, sizeof out->how, "no screen");
            return false;
        }
        const double potential = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
        const double now = screen.maximumExtendedDynamicRangeColorComponentValue;
        out->valid = true;
        out->sdr_white_nits = 100.0f;
        out->hdr_enabled = potential > 1.0;
        out->max_nits = static_cast<float>(potential * 100.0);
        out->max_fullframe_nits = static_cast<float>(std::min(potential, 5.0) * 100.0);
        out->min_nits = 0.0005f;
        const char* name = screen.localizedName.UTF8String;
        std::snprintf(out->monitor, sizeof out->monitor, "%s", name ? name : "screen");
        std::snprintf(out->how, sizeof out->how, "NSScreen EDR potential %.2f, now %.2f", potential, now);
    }
    return out->valid;
}

}  // namespace nxrt::extras
