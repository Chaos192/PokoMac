// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The launcher's start-up and menu tweaks (S46, src/gfx/play/play_fastboot.h): rows at the end of the Extras tab.
#include <cctype>
#include <string>
#include <vector>

#include "nxrt/game.h"

#include "pages.h"

namespace mprl {

namespace {

// Language names as the settings service reads them (services/common.cpp), in nn::settings::Language order, with the label the drop-down shows.
struct LangRow {
    const char* name;
    const wchar_t* label;
};
constexpr LangRow kLangRows[] = {
    {"Japanese", L"\u65e5\u672c\u8a9e (Japanese)"}, {"AmericanEnglish", L"English (US)"}, {"French", L"Fran\u00e7ais (French)"},
    {"German", L"Deutsch (German)"}, {"Italian", L"Italiano (Italian)"}, {"Spanish", L"Espa\u00f1ol (Spain)"},
    {"Chinese", L"\u4e2d\u6587 (Chinese)"}, {"Korean", L"\ud55c\uad6d\uc5b4 (Korean)"}, {"Dutch", L"Nederlands (Dutch)"},
    {"Portuguese", L"Portugu\u00eas (Portugal)"}, {"Russian", L"\u0420\u0443\u0441\u0441\u043a\u0438\u0439 (Russian)"}, {"Taiwanese", L"\u4e2d\u6587 (Taiwan)"},
    {"BritishEnglish", L"English (UK)"}, {"CanadianFrench", L"Fran\u00e7ais (Canada)"}, {"LatinAmericanSpanish", L"Espa\u00f1ol (Latin America)"},
    {"SimplifiedChinese", L"\u7b80\u4f53\u4e2d\u6587 (Simplified Chinese)"}, {"TraditionalChinese", L"\u7e41\u9ad4\u4e2d\u6587 (Traditional Chinese)"},
    {"BrazilianPortuguese", L"Portugu\u00eas (Brasil)"},
};

// The game's languages (nxrt/game.h Info::languages), as rows of kLangRows; index 0 of the drop-down is "Match Windows".
const std::vector<const LangRow*>& game_langs() {
    static const std::vector<const LangRow*> v = [] {
        std::vector<const LangRow*> out;
        uint32_t mask = nxrt::game::info().languages;
        if (!mask) mask = 1u << 1;   // American English only
        for (uint32_t i = 0; i < sizeof kLangRows / sizeof kLangRows[0]; ++i)
            if ((mask >> i) & 1u) out.push_back(&kLangRows[i]);
        return out;
    }();
    return v;
}

bool same_name(const std::string& a, const char* b) {
    if (a.size() != std::char_traits<char>::length(b)) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

}  // namespace

void add_fastboot_rows(Form& f) {
    f.heading(L"Language");
    {
        std::vector<std::wstring> items{L"Match Windows"};
        for (const LangRow* r : game_langs()) items.push_back(r->label);
        f.combo(
            L"Game language", items,
            [](const Settings& s) {
                const auto& v = game_langs();
                for (size_t i = 0; i < v.size(); ++i)
                    if (same_name(s.fastboot.language, v[i]->name)) return static_cast<int>(i + 1);
                return 0;
            },
            [](Settings& s, int i) {
                const auto& v = game_langs();
                s.fastboot.language = (i >= 1 && static_cast<size_t>(i) <= v.size()) ? v[i - 1]->name : "";
            });
        f.hint(L"Sets the language of the game's text and voices. Match Windows uses your Windows display language when the game has it, else English. Takes effect the next time the game starts.", 2);
    }
    f.heading(L"Start-up and menus");
    f.checkbox(L"Skip the opening cutscene of a new game", [](const Settings& s) { return s.fastboot.skip_intro; },
               [](Settings& s, bool v) { s.fastboot.skip_intro = v; });
    f.checkbox(L"Skip the title intro movie", [](const Settings& s) { return s.fastboot.skip_boot_logos; },
               [](Settings& s, bool v) { s.fastboot.skip_boot_logos = v; });
    f.checkbox(L"Faster menus", [](const Settings& s) { return s.fastboot.fast_menus; }, [](Settings& s, bool v) { s.fastboot.fast_menus = v; });
    f.slider(
        L"Menu speed", 1, 4, [](const Settings& s) { return s.fastboot.menu_speed; }, [](Settings& s, int v) { s.fastboot.menu_speed = v; },
        [](int v) { return std::to_wstring(v) + L"x"; });
    f.hint(L"New Game then starts in the docking hangar, after the game's own cutscene skip. Faster menus speed up the title, main menu and save "
           L"slots (used only with Faster menus on). Everything here is off by default.", 2, true);
}

}  // namespace mprl
