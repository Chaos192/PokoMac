// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Windows display HDR query (win_display.h): whether the monitor under a window has Windows HDR (advanced colour) on, and its
// luminance numbers. Windows only. The implementation reads the GDI device name and the HMONITOR, the CCD database for the
// advanced-colour state and SDR white level, and a fresh DXGI factory for the luminance values. It never throws.
#pragma once

namespace nxrt::extras {

struct DisplayHdrInfo {
    bool valid = false;            // the query found the monitor and answered
    bool hdr_enabled = false;      // Windows advanced colour (HDR) is on for this monitor
    float max_nits = 0;            // peak luminance reported by the display (DXGI_OUTPUT_DESC1::MaxLuminance), 0 when unknown
    float min_nits = 0;            // DXGI_OUTPUT_DESC1::MinLuminance
    float max_fullframe_nits = 0;  // DXGI_OUTPUT_DESC1::MaxFullFrameLuminance
    float sdr_white_nits = 80;     // Windows "SDR content brightness" in nits (80 = slider at minimum); from SDRWhiteLevel/1000*80
    char monitor[64] = {};         // the GDI device name of the monitor, e.g. \\.\DISPLAY1 (narrow chars)
    char how[96] = {};             // a short note on what the answer was based on / what failed, for the log
};

// hwnd == nullptr: the primary monitor. Otherwise the monitor with the largest overlap with the window
// (MonitorFromWindow with MONITOR_DEFAULTTONEAREST). Returns out->valid. out must not be null (returns false if it is).
bool query_display_hdr(void* hwnd, DisplayHdrInfo* out);

}  // namespace nxrt::extras
