// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Settings services (S3c): set (nn::settings::ISettingsServer) and set:sys (nn::settings::ISystemSettingsServer), answered from the title
// profile (services::profile(): language, region, firmware, nickname) and the host keyboard's key code map; plus host answers for the
// SDK's two nn::settings::operator== (Language against LanguageCode).
// Command ids, the Language order and codes, RegionCode, KeyCodeMap (0x1000), DeviceNickName (0x80), SerialNumber (0x18), ProductModel and
// ColorSetId: https://switchbrew.org/wiki/Settings_services. The firmware version struct: https://switchbrew.org/wiki/System_Version_Title.
// Result module 105 (625 LanguageOutOfRange, 808 NullDeviceNicknameBuffer): https://switchbrew.org/wiki/Error_codes. The settings
// items set:sys answers (GetSettingsItemValue) are values of Ryujinx (MIT) src/Ryujinx.HLE/HOS/Services/Settings/NxSettings.cs.
//
// The compare hooks: a game made with an older SDK may compare the language it wants with Language::Chinese (zh-CN) while the console
// gives it zh-Hans (Super Mario Odyssey 1.4.1 then fell back to Japanese), so either spelling of one Chinese script counts as equal there
// (nxnative services.md "Chinese language aliases in settings comparisons"). The codes the game is handed stay as they are.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/dispatch.h"
#include "nxrt/key_code_map.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/nxmodule.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

const uint32_t kLanguageOutOfRange = res(105, 625);
const uint32_t kNullNicknameBuffer = res(105, 808);
const uint32_t kNoKeyCodeMapBuffer = res(105, 1261);

constexpr uint32_t kLanguages = static_cast<uint32_t>(Language::Count);   // 18: Japanese .. BrazilianPortuguese
constexpr uint32_t kLanguagesV1 = 15;                                      // GetAvailableLanguageCodes / Count: the first 15
constexpr uint64_t kNicknameBytes = 0x80;
constexpr size_t kItemTextMax = 0x47;                                      // a settings name or key, without its NUL

std::atomic<int32_t> g_color_set{0};    // set:sys ColorSetId (0 Basic White)
std::atomic<uint8_t> g_lock_screen{0};

// ---- languages --------------------------------------------------------------------------------------------------------------
// GetAvailableLanguageCodes(2): the codes of languages 0.. as 8-byte strings, as many as fit and `max` allows; the reply is the count.
uint32_t write_language_codes(const ipc::Request& req, ipc::Response& rsp, uint32_t max) {
    const ipc::Buffer b = req.out_buf(0);
    uint32_t n = 0;
    if (b.addr) {
        n = static_cast<uint32_t>(std::min<uint64_t>(b.size / 8, max));
        std::vector<uint8_t> codes(8 * n);
        for (uint32_t i = 0; i < n; ++i) {
            const uint64_t c = language_code_u64(static_cast<Language>(i));
            std::memcpy(codes.data() + 8 * i, &c, 8);
        }
        buf_write(b, codes.data(), codes.size());
    }
    rsp.push<int32_t>(static_cast<int32_t>(n));
    return 0;
}

// operator==(Language, LanguageCode): the language's own code, or the other spelling of the same Chinese script.
bool language_matches(uint64_t lang_ptr, uint64_t code_ptr) {
    uint32_t lang = 0xFFFFFFFFu;
    if (lang_ptr) std::memcpy(&lang, reinterpret_cast<const void*>(static_cast<uintptr_t>(lang_ptr)), 4);
    if (lang >= kLanguages || !code_ptr) return false;
    char c8[9] = {};
    std::memcpy(c8, reinterpret_cast<const void*>(static_cast<uintptr_t>(code_ptr)), 8);
    const std::string code(c8);
    const Language l = static_cast<Language>(lang);
    if (code == language_code(l)) return true;
    switch (l) {
        case Language::Chinese: return code == "zh-Hans";
        case Language::SimplifiedChinese: return code == "zh-CN";
        case Language::Taiwanese: return code == "zh-Hant";
        case Language::TraditionalChinese: return code == "zh-TW";
        default: return false;
    }
}

constexpr const char* kEqLanguageCode = "_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE";   // (const Language&, const LanguageCode&)
constexpr const char* kEqCodeLanguage = "_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE";   // (const LanguageCode&, const Language&)

void eq_language_code(::Ctx* c) { c->x[0] = language_matches(c->x[0], c->x[1]) ? 1 : 0; }
void eq_code_language(::Ctx* c) { c->x[0] = language_matches(c->x[1], c->x[0]) ? 1 : 0; }

// ---- the firmware version -----------------------------------------------------------------------------------------------------
// nn::settings::system::FirmwareVersion (0x100 bytes) for profile().firmware: "major.minor.micro" (missing parts 0); a string that does not
// start with a number is taken as 12.1.0.
std::vector<uint8_t> firmware_struct() {
    std::string ver = profile().firmware;
    uint32_t part[3] = {0, 0, 0};
    bool valid = true;
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        uint64_t v = 0;
        size_t digits = 0;
        while (pos < ver.size() && ver[pos] >= '0' && ver[pos] <= '9' && v <= 0xFFFFFFFFull) {
            v = v * 10 + static_cast<uint64_t>(ver[pos] - '0');
            ++pos;
            ++digits;
        }
        if (digits == 0 || v > 0xFFFFFFFFull) {
            if (i == 0) valid = false;
            break;
        }
        part[i] = static_cast<uint32_t>(v);
        if (i < 2) {
            if (pos < ver.size() && ver[pos] == '.') ++pos;
            else break;
        }
    }
    if (!valid) {
        ver = "12.1.0";
        part[0] = 12;
        part[1] = 1;
        part[2] = 0;
    }
    std::vector<uint8_t> fw(0x100, 0);
    fw[0] = static_cast<uint8_t>(part[0]);
    fw[1] = static_cast<uint8_t>(part[1]);
    fw[2] = static_cast<uint8_t>(part[2]);
    fw[4] = 1;   // revision 1.0
    std::memcpy(fw.data() + 0x08, "NX", 2);
    if (ver == "12.1.0") std::memcpy(fw.data() + 0x28, "76b10c2dab7d3aa73fc162f8dff1655e6a21caf4", 40);   // 12.1.0's version hash
    std::memcpy(fw.data() + 0x68, ver.data(), std::min<size_t>(ver.size(), 0x17));
    const std::string title = "NintendoSDK Firmware for NX " + ver + "-1.0";
    std::memcpy(fw.data() + 0x80, title.data(), std::min<size_t>(title.size(), 0x7F));
    return fw;
}

// ---- other answers ------------------------------------------------------------------------------------------------------------
// The device nickname, NUL-terminated within the first 0x80 bytes of the buffer.
uint32_t write_nickname(const ipc::Request& req) {
    const ipc::Buffer b = req.out_buf(0);
    if (!b.addr) return kNullNicknameBuffer;
    const uint64_t m = std::min<uint64_t>(b.size, kNicknameBytes);
    if (m == 0) return 0;
    std::vector<uint8_t> out(m, 0);
    const std::string& device_name = profile().device_nickname;
    std::memcpy(out.data(), device_name.data(), std::min<uint64_t>(device_name.size(), m - 1));
    buf_write(b, out.data(), m);
    return 0;
}

// GetKeyCodeMap / 2 / ByPort: the host keyboard's map (every port the same). A request without a buffer fails (the SDK aborts then).
uint32_t write_key_code_map(const ipc::Request& req) {
    const ipc::Buffer b = req.out_buf(0);
    if (b.size == 0) return kNoKeyCodeMapBuffer;
    std::vector<uint8_t> map(hid::kKeyCodeMapBytes);
    hid::current_key_code_map(map.data());
    buf_write(b, map.data(), map.size());
    return 0;
}

// set:sys GetSettingsItemValue(Size): the few settings items games read.
struct Item {
    const char* name;
    const char* key;
    std::vector<uint8_t> value;
};
const std::vector<Item>& items() {
    static const std::vector<Item> v = {
        {"account", "na_required_for_network_service", {1}},
        {"audio", "uac_speaker_enabled", {0}},
        {"idle", "dim_level_percent_lcd", {10, 0, 0, 0}},
        {"capsrv", "screenshot_layerstack", {'s', 'c', 'r', 'e', 'e', 'n', 's', 'h', 'o', 't', 0}},
    };
    return v;
}
std::string item_text(const ipc::Buffer& b) {
    char t[kItemTextMax + 1] = {};
    buf_read(b, t, kItemTextMax);
    return std::string(t);
}
const Item* find_item(const ipc::Request& req, std::string* name, std::string* key) {
    *name = item_text(req.in_buf(0));
    *key = item_text(req.in_buf(1));
    for (const Item& i : items())
        if (*name == i.name && *key == i.key) return &i;
    return nullptr;
}

std::shared_ptr<ipc::Service> make_set() {
    return make_iface("set", [](ipc::Service& s) {
        s.add_cmd(0, "GetLanguageCode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(language_code_u64(profile().language));
            return 0;
        });
        s.add_cmd(1, "GetAvailableLanguageCodes", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            return write_language_codes(req, rsp, kLanguagesV1);
        });
        s.add_cmd(2, "MakeLanguageCode", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t l = req.arg<uint32_t>(0);
            if (l >= kLanguages) {
                NXLOG_W("settings", "MakeLanguageCode: language %u is out of range", l);
                return kLanguageOutOfRange;
            }
            rsp.push<uint64_t>(language_code_u64(static_cast<Language>(l)));
            return 0;
        });
        s.add_cmd(3, "GetAvailableLanguageCodeCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<int32_t>(static_cast<int32_t>(kLanguagesV1));
            return 0;
        });
        s.add_cmd(4, "GetRegionCode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const int region_code = profile().region;
            rsp.push<int32_t>(region_code >= 0 && region_code <= 5 ? region_code : 1);   // out of range: Usa
            return 0;
        });
        s.add_cmd(5, "GetAvailableLanguageCodes2", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            return write_language_codes(req, rsp, kLanguages);
        });
        s.add_cmd(6, "GetAvailableLanguageCodeCount2", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<int32_t>(static_cast<int32_t>(kLanguages));
            return 0;
        });
        s.add_cmd(7, "GetKeyCodeMap", [](ipc::Request& req, ipc::Response&) -> uint32_t { return write_key_code_map(req); });
        s.add_cmd(8, "GetQuestFlag", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);   // not a kiosk unit
            return 0;
        });
        s.add_cmd(9, "GetKeyCodeMap2", [](ipc::Request& req, ipc::Response&) -> uint32_t { return write_key_code_map(req); });
        s.add_cmd(10, "GetFirmwareVersionForDebug", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const std::vector<uint8_t> fw = firmware_struct();
            buf_write(req.out_buf(0), fw.data() + 0x80, 0x80);
            return 0;
        });
        s.add_cmd(11, "GetDeviceNickName", [](ipc::Request& req, ipc::Response&) -> uint32_t { return write_nickname(req); });
        s.add_cmd(12, "GetKeyCodeMapByPort", [](ipc::Request& req, ipc::Response&) -> uint32_t { return write_key_code_map(req); });
    });
}

std::shared_ptr<ipc::Service> make_set_sys() {
    return make_iface("set:sys", [](ipc::Service& s) {
        const auto firmware = [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const std::vector<uint8_t> fw = firmware_struct();
            buf_write(req.out_buf(0), fw.data(), fw.size());
            return 0;
        };
        s.add_cmd(3, "GetFirmwareVersion", firmware);
        s.add_cmd(4, "GetFirmwareVersion2", firmware);
        s.add_cmd(7, "GetLockScreenFlag", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(g_lock_screen.load());
            return 0;
        });
        s.add_cmd(8, "SetLockScreenFlag", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            g_lock_screen.store(req.arg<uint8_t>(0) ? 1 : 0);
            return 0;
        });
        s.add_cmd(23, "GetColorSetId", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<int32_t>(g_color_set.load());
            return 0;
        });
        s.add_cmd(24, "SetColorSetId", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            g_color_set.store(req.arg<int32_t>(0));
            return 0;
        });
        s.add_cmd(37, "GetSettingsItemValueSize", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            std::string name, key;
            const Item* it = find_item(req, &name, &key);
            rsp.push<uint64_t>(it ? it->value.size() : 0);
            return 0;
        });
        s.add_cmd(38, "GetSettingsItemValue", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            std::string name, key;
            const Item* it = find_item(req, &name, &key);
            const ipc::Buffer out = req.out_buf(0);
            uint64_t n = 0;
            if (it) {
                n = buf_write(out, it->value.data(), it->value.size());
            } else {
                NXLOG_I("settings", "GetSettingsItemValue(%s, %s): no such setting", name.c_str(), key.c_str());
                if (out.addr && out.size) std::memset(buf_mut(out), 0, static_cast<size_t>(out.size));
            }
            rsp.push<uint64_t>(n);
            return 0;
        });
        s.add_cmd(68, "GetSerialNumber", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            uint8_t serial[0x18] = {};
            std::memcpy(serial, "NXRT0000000000", 14);
            rsp.out.insert(rsp.out.end(), serial, serial + sizeof serial);
            return 0;
        });
        s.add_cmd(77, "GetDeviceNickName", [](ipc::Request& req, ipc::Response&) -> uint32_t { return write_nickname(req); });
        s.add_cmd(79, "GetProductModel", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<int32_t>(1);   // Nx
            return 0;
        });
    });
}

}  // namespace

void install_language_compare_hooks() {
    const std::pair<const char*, HleFn> hooks[2] = {{kEqLanguageCode, eq_language_code}, {kEqCodeLanguage, eq_code_language}};
    for (const auto& h : hooks) {
        hle_register(h.first, h.second);
        module_override_symbol(h.first);       // the game's imports
        module_override_definition(h.first);   // the SDK's own definition, reached by its address
    }
}

void register_settings() {
    add("set", [] {
        install_language_compare_hooks();
        return make_set();
    });
    add("set:sys", [] { return make_set_sys(); });
}

void settings_reset() {
    g_color_set.store(0);
    g_lock_screen.store(0);
}

}  // namespace nxrt::services
