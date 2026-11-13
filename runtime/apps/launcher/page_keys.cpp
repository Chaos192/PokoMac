// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Keyboard tab: every action of the game with its two keys. Double-click a key to change it. Delete (or the clear button) removes
// the selected key. The menus keep their own keys. The list rows and the edits are in bindings_edit.cpp.
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "bindings_edit.h"
#include "pages.h"

#pragma comment(lib, "uxtheme.lib")

namespace mprl {

namespace {

using nxrt::play::Act;
using nxrt::play::InputId;
using nxrt::play::Settings;

constexpr int kListHeight = 340;   // logical px
constexpr int kMargin = 16;        // logical px, the page margin
constexpr int kButtonW = 190;      // logical px

class KeysPage : public Page {
public:
    const wchar_t* title() const override { return L"Keyboard"; }

    void build(Form& f) override {
        const Env& env = host()->env();
        const RECT area = f.custom_row(nullptr, kListHeight);   // takes the rows' height; the list is widened to the page below
        const int x = env.S(kMargin);
        const int w = f.width() - 2 * env.S(kMargin);
        list_ = f.create(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, x, area.top, w,
                         area.bottom - area.top, &list_id_, WS_EX_CLIENTEDGE);
        SendMessageW(list_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        SetWindowTheme(list_, L"Explorer", nullptr);
        add_columns(w - env.S(24));   // the scroll bar and the frame take the rest

        f.hint(L"Double-click a key to change it. Delete clears the selected key. Menus keep their own keys.", 1, true);
        f.columns(2);
        reset_id_ = f.button(L"Reset keys to defaults", kButtonW);
        clear_id_ = f.button(L"Clear selected key", kButtonW);
    }

    void load(const Settings& s) override {
        edit_ = s;
        Page::load(s);
        refresh();
    }

    void store(Settings* s) override {
        Page::store(s);
        s->bindings = edit_.bindings;
    }

    bool command(int id, int code, HWND) override {
        if (code != BN_CLICKED) return false;
        if (id == reset_id_) {
            if (message(host()->window(), L"Reset the keys on this tab to the factory settings? Only the keys shown here are reset. "
                                          L"The menu, map, gameplay, morph ball and scan keys are not touched.",
                        MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
                return true;
            reset_shown_bindings(edit_);
            refresh();
            host()->changed();
            return true;
        }
        if (id == clear_id_) {
            const int row = selected_row();
            if (row >= 0) clear_key(row);
            return true;
        }
        return false;
    }

    LRESULT notify(NMHDR* hdr, bool* handled) override {
        if (!list_ || hdr->hwndFrom != list_) return 0;
        switch (hdr->code) {
            case NM_CLICK: {
                // Remembers the column, so Delete knows which key of the row to clear.
                const NMITEMACTIVATE* ia = reinterpret_cast<const NMITEMACTIVATE*>(hdr);
                int col = 0;
                if (hit(ia->ptAction, &col) >= 0) last_col_ = col;
                return 0;
            }
            case NM_DBLCLK: {
                const NMITEMACTIVATE* ia = reinterpret_cast<const NMITEMACTIVATE*>(hdr);
                int col = 0;
                const int row = hit(ia->ptAction, &col);
                if (row < 0) return 0;
                *handled = true;
                change_key(row, col == 2 ? 1 : 0);
                return 0;
            }
            case LVN_KEYDOWN: {
                const NMLVKEYDOWN* kd = reinterpret_cast<const NMLVKEYDOWN*>(hdr);
                if (kd->wVKey != VK_DELETE) return 0;
                const int row = selected_row();
                if (row < 0) return 0;
                *handled = true;
                clear_key(row);
                return 0;
            }
        }
        return 0;
    }

private:
    // Columns: 0 action, 1 first key, 2 second key.
    void add_columns(int list_w) {
        const int c0 = list_w * 40 / 100;
        const int c1 = (list_w - c0) / 2;
        const int c2 = list_w - c0 - c1;
        insert_column(0, L"Action", c0);
        insert_column(1, L"Key 1", c1);
        insert_column(2, L"Key 2", c2);
    }

    void insert_column(int index, const wchar_t* text, int width) {
        std::wstring t = text;
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = t.data();
        col.cx = width;
        col.iSubItem = index;
        SendMessageW(list_, LVM_INSERTCOLUMNW, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&col));
    }

    void set_text(int row, int sub, const std::wstring& text) {
        LVITEMW item{};
        item.iSubItem = sub;
        item.pszText = const_cast<wchar_t*>(text.c_str());
        SendMessageW(list_, LVM_SETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
    }

    // Rebuilds the rows from edit_. The selected row stays selected.
    void refresh(int select_row = -1) {
        if (!list_) return;
        if (select_row < 0) select_row = selected_row();
        SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list_, LVM_DELETEALLITEMS, 0, 0);
        const std::vector<KeyRow>& rows = key_rows();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            std::wstring name = std::wstring(rows[static_cast<std::size_t>(i)].group) + L": " + rows[static_cast<std::size_t>(i)].label;
            LVITEMW item{};
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = name.data();
            SendMessageW(list_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));

            const std::vector<InputId> keys = inputs_for(edit_, rows[static_cast<std::size_t>(i)].act);
            set_text(i, 1, keys.size() > 0 ? input_label(keys[0]) : std::wstring());
            set_text(i, 2, keys.size() > 1 ? input_label(keys[1]) : std::wstring());
        }
        if (select_row >= 0 && select_row < static_cast<int>(rows.size())) {
            LVITEMW st{};
            st.state = LVIS_SELECTED | LVIS_FOCUSED;
            st.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
            SendMessageW(list_, LVM_SETITEMSTATE, static_cast<WPARAM>(select_row), reinterpret_cast<LPARAM>(&st));
            SendMessageW(list_, LVM_ENSUREVISIBLE, static_cast<WPARAM>(select_row), FALSE);
        }
        SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(list_, nullptr, TRUE);
    }

    int selected_row() const {
        if (!list_) return -1;
        return static_cast<int>(SendMessageW(list_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
    }

    // The row under a point (client coordinates) and its column. Returns -1 when no row is there.
    int hit(POINT pt, int* col) {
        LVHITTESTINFO hti{};
        hti.pt = pt;
        const int row = static_cast<int>(SendMessageW(list_, LVM_SUBITEMHITTEST, 0, reinterpret_cast<LPARAM>(&hti)));
        *col = hti.iSubItem;
        return row;
    }

    bool row_action(int row, Act* out) const {
        const std::vector<KeyRow>& rows = key_rows();
        if (row < 0 || row >= static_cast<int>(rows.size())) return false;
        *out = rows[static_cast<std::size_t>(row)].act;
        return true;
    }

    // Double-click: ask for the new key, then put it in the slot of the column that was clicked.
    void change_key(int row, int slot) {
        Act act = Act::None;
        if (!row_action(row, &act)) return;
        InputId in;
        if (!capture_input(host()->window(), host()->env(), act_label(act), &in)) return;

        Act displaced = Act::None;
        bind_slot(edit_, act, slot, in, &displaced);
        refresh(row);
        host()->changed();
        if (displaced != Act::None)
            message(host()->window(), input_label(in) + L" was used for " + act_label(displaced) + L"; that action now has one key less.");
    }

    // Delete / clear button: removes the key of the last clicked column (the first key when no click came first).
    void clear_key(int row) {
        Act act = Act::None;
        if (!row_action(row, &act)) return;
        clear_slot(edit_, act, last_col_ == 2 ? 1 : 0);
        refresh(row);
        host()->changed();
    }

    HWND list_ = nullptr;
    int list_id_ = 0;
    int reset_id_ = 0;
    int clear_id_ = 0;
    int last_col_ = 1;   // column of the last click: 2 is the second key, anything else the first
    Settings edit_;      // the page's working copy; store() writes its bindings back
};

}  // namespace

std::unique_ptr<Page> make_keys_page() { return std::make_unique<KeysPage>(); }

}  // namespace mprl
