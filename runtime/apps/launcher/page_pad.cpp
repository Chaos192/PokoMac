// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Controller tab: stick dead zones, look inversion and sensitivity, trigger point, rumble, motion controls, the face button labels, and the button layout (what each
// controller button presses on the Switch pad).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nxrt/input_source.h"
#include "pages.h"

namespace mprl {

namespace {

using nxrt::play::kPadDefault;
using nxrt::play::kPadSrcCount;
namespace hid = nxrt::hid;

// The single Switch buttons a layout combo can press, in the order of the combo.
struct Choice {
    const wchar_t* name;
    hid::NpadButtonBit bit;
};
const Choice kChoices[] = {
    {L"A", hid::kBtnA},
    {L"B", hid::kBtnB},
    {L"X", hid::kBtnX},
    {L"Y", hid::kBtnY},
    {L"L", hid::kBtnL},
    {L"R", hid::kBtnR},
    {L"ZL", hid::kBtnZL},
    {L"ZR", hid::kBtnZR},
    {L"Plus", hid::kBtnPlus},
    {L"Minus", hid::kBtnMinus},
    {L"D-pad up", hid::kBtnUp},
    {L"D-pad down", hid::kBtnDown},
    {L"D-pad left", hid::kBtnLeft},
    {L"D-pad right", hid::kBtnRight},
    {L"L stick click", hid::kBtnStickL},
    {L"R stick click", hid::kBtnStickR},
};
constexpr int kChoiceCount = static_cast<int>(sizeof kChoices / sizeof kChoices[0]);

// Combo items: 0 Default, 1 Nothing, 2.. the single buttons, then Custom (a value the file holds that no single button gives).
constexpr int kNothingItem = 1;
constexpr int kCustomItem = 2 + kChoiceCount;

const wchar_t* const kSourceLabels[kPadSrcCount] = {
    L"A / Cross",   L"B / Circle",     L"X / Square",      L"Y / Triangle",   L"LB / L1",       L"RB / R1",
    L"LT / L2",     L"RT / R2",        L"Start / Menu",  L"Back / View",  L"L stick click", L"R stick click",
    L"D-pad up",    L"D-pad down",     L"D-pad left",      L"D-pad right",
    L"Right grip R4 (upper)", L"Left grip L4 (upper)", L"Right grip R5 (lower)", L"Left grip L5 (lower)",
    L"Touchpad / left trackpad click", L"Right trackpad click",
};

std::vector<std::wstring> layout_items() {
    std::vector<std::wstring> items = {L"Default", L"Nothing"};
    for (const Choice& c : kChoices) items.push_back(c.name);
    items.push_back(L"Custom (from controls.ini)");
    return items;
}

// The value a combo item stores (not for Custom).
uint64_t value_of_item(int item) {
    if (item == 0) return kPadDefault;
    if (item == kNothingItem) return 0;
    return hid::btn(kChoices[item - 2].bit);
}

// The item that shows a stored value, or Custom when no item above holds it.
int item_of_value(uint64_t v) {
    for (int i = 0; i < kCustomItem; ++i)
        if (value_of_item(i) == v) return i;
    return kCustomItem;
}

// The slider position that shows a value (percent), kept inside the slider's range.
int position_of(double v, int lo, int hi) {
    return std::clamp(static_cast<int>(std::lround(v * 100.0)), lo, hi);
}

class PadPage : public Page {
public:
    const wchar_t* title() const override { return L"Controller"; }

    void build(Form& f) override {
        f.heading(L"Controller");
        f.columns(2);
        f.slider(
            L"Left dead zone", 0, 50, [](const Settings& s) { return position_of(s.pad.stick_deadzone, 0, 50); },
            [this](Settings& s, int v) {
                if (v != deadzone_pos_) s.pad.stick_deadzone = v / 100.0;   // the slider does not show every digit: keep the file's value
            },
            [](int v) { return std::to_wstring(v) + L"%"; });
        f.slider(
            L"Right dead zone", 0, 50, [](const Settings& s) { return position_of(s.pad.right_deadzone, 0, 50); },
            [this](Settings& s, int v) {
                if (v != right_deadzone_pos_) s.pad.right_deadzone = v / 100.0;
            },
            [](int v) { return std::to_wstring(v) + L"%"; });
        f.slider(
            L"Trigger point", 5, 100, [](const Settings& s) { return position_of(s.pad.trigger_threshold, 5, 100); },
            [this](Settings& s, int v) {
                if (v != trigger_pos_) s.pad.trigger_threshold = v / 100.0;
            },
            [](int v) { return std::to_wstring(v) + L"%"; });
        f.checkbox(
            L"Invert look (right stick up looks down)", [](const Settings& s) { return s.pad.invert_y; },
            [](Settings& s, bool v) { s.pad.invert_y = v; });
        f.slider(
            L"Look sensitivity", 25, 300, [](const Settings& s) { return position_of(s.pad.look_sens, 25, 300); },
            [this](Settings& s, int v) {
                if (v != look_pos_) s.pad.look_sens = v / 100.0;
            },
            [](int v) { return std::to_wstring(v) + L"%"; });
        f.checkbox(L"Rumble", [](const Settings& s) { return s.pad.rumble; }, [](Settings& s, bool v) { s.pad.rumble = v; });
        f.checkbox(L"Motion controls (gyro)", [](const Settings& s) { return s.pad.gyro; }, [](Settings& s, bool v) { s.pad.gyro = v; });
        f.slider(
            L"Rumble strength", 0, 200, [](const Settings& s) { return position_of(s.pad.rumble_scale, 0, 200); },
            [this](Settings& s, int v) {
                if (v != rumble_pos_) s.pad.rumble_scale = v / 100.0;
            },
            [](int v) { return std::to_wstring(v) + L"%"; });
        f.combo(
            L"Face buttons", {L"Switch layout (right = A, bottom = B)", L"Xbox layout (A = A, B = B)"},
            [](const Settings& s) { return s.pad.label_abxy ? 1 : 0; }, [](Settings& s, int i) { s.pad.label_abxy = (i == 1); });

        f.columns(1);
        f.heading(L"Button layout");
        f.hint(L"What each controller button presses on the Switch pad. Default keeps the built-in layout.", 1, true);
        f.columns(3);
        const std::vector<std::wstring> items = layout_items();
        for (int i = 0; i < kPadSrcCount; ++i) {
            combos_[i] = f.combo(
                kSourceLabels[i], items,
                [i](const Settings& s) { return item_of_value(s.pad.remap[i]); },
                [this, i](Settings& s, int item) { s.pad.remap[i] = item == kCustomItem ? custom_[i] : value_of_item(item); });
        }
        f.columns(2);
        reset_id_ = f.button(L"Reset layout", 150, true);
        find_id_ = f.button(L"Find a button...", 150, true);
        f.columns(1);
    }

    void load(const Settings& s) override {
        deadzone_pos_ = position_of(s.pad.stick_deadzone, 0, 50);
        right_deadzone_pos_ = position_of(s.pad.right_deadzone, 0, 50);
        trigger_pos_ = position_of(s.pad.trigger_threshold, 5, 100);
        look_pos_ = position_of(s.pad.look_sens, 25, 300);
        rumble_pos_ = position_of(s.pad.rumble_scale, 0, 200);
        for (int i = 0; i < kPadSrcCount; ++i) custom_[i] = s.pad.remap[i];
        Page::load(s);
    }

    bool command(int id, int code, HWND) override {
        if (id == find_id_ && code == BN_CLICKED) {
            // The player presses the button on the pad; that row's drop-down gets the focus and opens so it is clear which one it is.
            int row = -1;
            if (capture_pad_button(host()->window(), host()->env(), &row) && row >= 0 && row < kPadSrcCount) {
                SetFocus(combos_[row]);
                SendMessageW(combos_[row], CB_SHOWDROPDOWN, TRUE, 0);
            }
            return true;
        }
        if (id != reset_id_ || code != BN_CLICKED) return false;
        for (HWND c : combos_) SendMessageW(c, CB_SETCURSEL, 0, 0);
        host()->changed();
        return true;
    }

private:
    int deadzone_pos_ = 10;       // slider positions the values read in load()
    int right_deadzone_pos_ = 10;
    int trigger_pos_ = 35;
    int look_pos_ = 100;
    int rumble_pos_ = 100;
    uint64_t custom_[kPadSrcCount] = {};   // the value of each button when the page opened; written back while Custom is selected
    HWND combos_[kPadSrcCount] = {};
    int reset_id_ = 0;
    int find_id_ = 0;
};

}  // namespace

std::unique_ptr<Page> make_pad_page() { return std::make_unique<PadPage>(); }

}  // namespace mprl
