// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Windows display HDR query (win_display.cpp), see win_display.h. Each call reads everything fresh:
//   1. HMONITOR and MONITORINFOEXW (MonitorFromWindow / MonitorFromPoint, GetMonitorInfoW) give the GDI device name, e.g. \\.\DISPLAY1.
//   2. CCD (GetDisplayConfigBufferSizes + QueryDisplayConfig, active paths) finds the path whose source GDI name is that device, then
//      DisplayConfigGetDeviceInfo gives the advanced-colour state and DISPLAYCONFIG_SDR_WHITE_LEVEL for its target.
//   3. A new DXGI factory (CreateDXGIFactory1) finds the output whose DXGI_OUTPUT_DESC1::Monitor is the HMONITOR and reads its
//      luminance values and colour space through IDXGIOutput6::GetDesc1.
// hdr_enabled needs both the CCD advanced-colour flags and the HDR10 colour space: DXGI cannot tell HDR from auto-colour SDR by itself,
// and the CCD flags alone also cover the SDR wide-colour case.
#include <windows.h>
#include <wingdi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cwchar>
#include <vector>

#include "extras/win_display.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

namespace nxrt::extras {
namespace {

using Microsoft::WRL::ComPtr;

// Writes text into a fixed buffer, always NUL-terminated.
template <size_t N>
void put(char (&dst)[N], const char* text) {
    std::snprintf(dst, N, "%s", text);
}

// Narrows a wide name (device names are ASCII in practice; anything else becomes '?'). Stops at NUL or max_len.
template <size_t N>
void narrow(const wchar_t* src, size_t max_len, char (&dst)[N]) {
    size_t i = 0;
    for (; i < max_len && src[i] != L'\0' && i + 1 < N; ++i) dst[i] = src[i] < 128 ? static_cast<char>(src[i]) : '?';
    dst[i] = '\0';
}

// Finds the active CCD path whose source GDI device name equals gdi_name. Returns its target adapter id and target id.
bool find_active_target(const wchar_t* gdi_name, LUID* adapter, UINT32* target_id, char* note, size_t cap) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    bool sized = false;
    // The buffer sizes can change between the two calls, so retry on ERROR_INSUFFICIENT_BUFFER (documented pattern).
    for (int attempt = 0; attempt < 3 && !sized; ++attempt) {
        UINT32 path_count = 0, mode_count = 0;
        LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (rc != ERROR_SUCCESS) {
            std::snprintf(note, cap, "CCD: GetDisplayConfigBufferSizes %ld", rc);
            return false;
        }
        paths.assign(path_count, DISPLAYCONFIG_PATH_INFO{});
        modes.assign(mode_count, DISPLAYCONFIG_MODE_INFO{});
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
        if (rc == ERROR_INSUFFICIENT_BUFFER) continue;
        if (rc != ERROR_SUCCESS) {
            std::snprintf(note, cap, "CCD: QueryDisplayConfig %ld", rc);
            return false;
        }
        paths.resize(path_count);
        sized = true;
    }
    if (!sized) {
        std::snprintf(note, cap, "CCD: QueryDisplayConfig kept changing size");
        return false;
    }

    for (const DISPLAYCONFIG_PATH_INFO& p : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof src;
        src.header.adapterId = p.sourceInfo.adapterId;
        src.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (std::wcsncmp(src.viewGdiDeviceName, gdi_name, CCHDEVICENAME) == 0) {
            *adapter = p.targetInfo.adapterId;
            *target_id = p.targetInfo.id;
            return true;
        }
    }
    std::snprintf(note, cap, "CCD: no active path for this monitor");
    return false;
}

// Reads the advanced-colour flags and the SDR white level (in nits) for the monitor's active CCD target.
bool read_ccd(const wchar_t* gdi_name, bool* adv_supported, bool* adv_enabled, float* sdr_white_nits, char* note, size_t cap) {
    LUID adapter{};
    UINT32 target_id = 0;
    if (!find_active_target(gdi_name, &adapter, &target_id, note, cap)) return false;

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ac{};
    ac.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    ac.header.size = sizeof ac;
    ac.header.adapterId = adapter;
    ac.header.id = target_id;
    LONG rc = DisplayConfigGetDeviceInfo(&ac.header);
    if (rc != ERROR_SUCCESS) {
        std::snprintf(note, cap, "CCD: advanced colour query %ld", rc);
        return false;
    }
    *adv_supported = ac.advancedColorSupported != 0;
    *adv_enabled = ac.advancedColorEnabled != 0;

    DISPLAYCONFIG_SDR_WHITE_LEVEL sw{};
    sw.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    sw.header.size = sizeof sw;
    sw.header.adapterId = adapter;
    sw.header.id = target_id;
    rc = DisplayConfigGetDeviceInfo(&sw.header);
    if (rc != ERROR_SUCCESS) {
        std::snprintf(note, cap, "CCD: SDR white level query %ld", rc);
        return false;
    }
    *sdr_white_nits = static_cast<float>(sw.SDRWhiteLevel) / 1000.0f * 80.0f;

    std::snprintf(note, cap, "CCD ok: adv sup=%d on=%d", *adv_supported ? 1 : 0, *adv_enabled ? 1 : 0);
    return true;
}

// Finds the DXGI output for hmon (a fresh factory each call, so no cached state) and reads its luminance and colour space.
bool read_dxgi(HMONITOR hmon, DisplayHdrInfo* out, DXGI_COLOR_SPACE_TYPE* color_space, char* note, size_t cap) {
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::snprintf(note, cap, "DXGI: CreateDXGIFactory1 0x%08lx", static_cast<unsigned long>(hr));
        return false;
    }
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(a, adapter.GetAddressOf()))) break;
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(o, output.GetAddressOf()))) break;
            ComPtr<IDXGIOutput6> output6;
            if (FAILED(output.As(&output6))) continue;
            DXGI_OUTPUT_DESC1 desc{};
            if (FAILED(output6->GetDesc1(&desc))) continue;
            if (desc.Monitor != hmon) continue;

            out->max_nits = desc.MaxLuminance;
            out->min_nits = desc.MinLuminance;
            out->max_fullframe_nits = desc.MaxFullFrameLuminance;
            *color_space = desc.ColorSpace;
            std::snprintf(note, cap, "DXGI ok: colour space %d", static_cast<int>(desc.ColorSpace));
            return true;
        }
    }
    std::snprintf(note, cap, "DXGI: no output for this monitor");
    return false;
}

bool run(void* hwnd, DisplayHdrInfo* out) {
    const HMONITOR hmon = hwnd ? MonitorFromWindow(static_cast<HWND>(hwnd), MONITOR_DEFAULTTONEAREST)
                               : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    if (!hmon) {
        put(out->how, "MonitorFromWindow/MonitorFromPoint returned NULL");
        return false;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof info;
    if (!GetMonitorInfoW(hmon, &info)) {
        put(out->how, "GetMonitorInfoW failed");
        return false;
    }
    narrow(info.szDevice, CCHDEVICENAME, out->monitor);

    // Both sources are read even if one fails, so the fields that were found are kept.
    bool adv_supported = false, adv_enabled = false;
    char ccd_note[48] = {};
    const bool ccd_ok = read_ccd(info.szDevice, &adv_supported, &adv_enabled, &out->sdr_white_nits, ccd_note, sizeof ccd_note);

    DXGI_COLOR_SPACE_TYPE color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    char dxgi_note[48] = {};
    const bool dxgi_ok = read_dxgi(hmon, out, &color_space, dxgi_note, sizeof dxgi_note);

    out->valid = ccd_ok && dxgi_ok;
    out->hdr_enabled = out->valid && adv_supported && adv_enabled && color_space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;

    // The notes are short enough for how[96] (two notes of at most 47 characters plus a separator).
    std::snprintf(out->how, sizeof out->how, "%s; %s", ccd_note, dxgi_note);
    return true;
}

}  // namespace

bool query_display_hdr(void* hwnd, DisplayHdrInfo* out) {
    if (!out) return false;
    *out = DisplayHdrInfo{};
    try {
        run(hwnd, out);
    } catch (...) {
        out->valid = false;
        out->hdr_enabled = false;
        put(out->how, "unexpected exception");
    }
    return out->valid;
}

}  // namespace nxrt::extras
