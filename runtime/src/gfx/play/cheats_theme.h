// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// The cheats overlay's look, in one place: the design tokens (colour, type, space, radius, sizes), the ImGui style made from them, the
// fonts, the icons and the few widgets every screen shares (buttons, switches, choice cards, chips, section titles, empty states, the
// button-hint glyphs). The colours come from a theme (a JSON file: a built-in one, runtime/assets/cheats/themes, the launcher's own tones
// with Sunrise the default, or the player's own; cheats_theme_file.cpp), so the overlay and the app read as one product, and the player can
// restyle it. Screens never pick a colour, a size or a font of their own: they ask for a token here.
#pragma once
#include <imgui.h>

#include <cstdint>
#include <string>

namespace nxrt::cheats::ui::theme {

// ---- colour: the palette in force (the theme's, with the player's accent and opacity) -------------------------------------------------
struct Palette {
    ImU32 scrim_top, scrim_bottom;   // the backdrop over the game while the overlay is open (the game stays visible through it)
    ImU32 panel;                     // the main panel (the opacity applied; glass: the tint over the blurred game)
    ImU32 solid;                     // popups, dialogs, toasts, the keyboard
    ImU32 card;                      // a card on the panel
    ImU32 surface1, surface2, surface3;   // hover / a control / pressed or selected
    ImU32 sunken, sunken_hover;      // a well: text field, track
    ImU32 line, line_strong;
    ImU32 text, text2, text3;        // primary, secondary (72 %), tertiary (50 %)
    ImU32 accent, accent_hover, accent_press, on_accent, accent_tint;
    ImU32 ok, warn, danger, danger_solid, on_danger;
    ImU32 ok_tint, warn_tint, danger_tint;
    ImU32 control_off;               // a switch's track when off
    ImU32 row_alt;                   // every other table row
    ImU32 cap, on_cap;               // a controller button's key cap (the text colour) and its letter (the panel's)
    ImU32 shadow;                    // a key cap's lip, a knob's shadow
    ImU32 modal_dim;                 // behind a dialog
    bool dark;                       // a dark theme (the panel is dark)
};
const Palette& pal();

// ---- themes (cheats_theme_file.cpp; docs/CHEATS.md "Themes") ------------------------------------------------------------------------
// A theme as read from its JSON file: the whole palette, the corner radius, the panel's style and opacity and, optionally, a font.
struct ThemeSpec {
    std::string name = "Sunrise";
    std::string source = "builtin:sunrise";   // "builtin:<name>" or the file it came from
    bool glass = false;              // style: solid or glass
    float opacity = 0.97f;           // the panel's opacity (solid), 0.3 .. 1
    float blur = 0.6f;               // glass: how strong the blur of the game behind the panel is, 0 .. 1
    float tint = 0.55f;              // glass: the tint's opacity over the blurred game, 0 .. 1
    float radius = 20.f;             // the panel's corner radius at UI scale 1 (the others in proportion)
    ImU32 accent = IM_COL32(255, 194, 61, 255);
    ImU32 panel = 0, dialog = 0, surface = 0, card = 0, well = 0, text = 0, text2 = 0, text3 = 0, line = 0, line_strong = 0, on_accent = 0;
    ImU32 ok = 0, warn = 0, danger = 0, danger_solid = 0, scrim = 0;   // 0 where the file left it out: derived (cheats_theme.cpp)
    std::string font;                // a TrueType / OpenType file instead of Inter (absolute), or empty
    float font_size = 16.f;          // the body size with that font, 12 .. 22
};
// "sunrise" (the default), "dusk", "midnight", "daylight", "paper", "frost", or a theme file's path. On any error the default theme, with
// *err saying why (the caller logs it once).
ThemeSpec load_theme(const std::string& name_or_path, std::string* err);
// "#rgb", "#rrggbb", "#rrggbbaa", "rgb(r, g, b)", "rgba(r, g, b, a)" or "r, g, b": a colour (the player's accent, the theme files).
bool parse_colour(const std::string& text, ImU32* out);

// The look settings' defaults and ranges: runtime/assets/cheats/look.json, the one source the launcher shares.
struct Range { int def = -1, min = 0, max = 100; };     // def -1: the theme's own value
struct LookSpec {
    std::string theme = "sunrise", accent = "theme", dock = "centre";
    Range opacity{-1, 30, 100}, blur{-1, 0, 100}, tint{-1, 0, 100}, scale{100, 50, 200}, hud_opacity{85, 30, 100};
    bool animations = true;
};
const LookSpec& look_spec();

// The player's look ([cheats] accent, style, opacity, blur, tint, scale; the Settings tool) over the theme.
struct Look {
    std::string accent = "theme";    // "theme" (the theme's), "auto" (from the game's picture), a preset, or a colour (parse_colour)
    int style = -1;                  // -1 the theme's, 0 solid, 1 glass
    float opacity = -1.f;            // 0.3 .. 1, < 0: the theme's
    float blur = -1.f, tint = -1.f;  // 0 .. 1, < 0: the theme's
    float scale = 1.f;               // the player's size, on top of the DPI scale
};
const ThemeSpec& theme();
void set_theme(const ThemeSpec& t);
// The style in force after apply(): glass (with its blur strength and tint) or solid. Glass needs the backdrop blur (overlay.cpp); without
// it (an HDR display, a backend that has none) the panel is drawn solid.
bool glass();
float glass_blur();
float glass_tint();

// ---- space, radius, sizes (unscaled px; px() applies the UI scale: the DPI scale times the player's [cheats] scale) -------------------
float k();
inline float px(float v) { return v * k(); }
constexpr int S1 = 4, S2 = 8, S3 = 12, S4 = 16, S5 = 20, S6 = 24, S7 = 32, S8 = 40, S9 = 56;   // the launcher's --space-1 .. 9
inline float sp(int s) { return px(static_cast<float>(s)); }
constexpr float kRadiusXs = 6.f, kRadiusSm = 10.f, kRadiusMd = 14.f, kRadiusLg = 20.f;
constexpr float kControlH = 38.f, kControlSm = 30.f, kControlLg = 48.f, kRowH = 46.f;

// ---- type ------------------------------------------------------------------------------------------------------------------------------
// Body 16 px (the launcher's 1rem), Small 13.5, Strong (semibold body), Title 20, Display 28, Mono (ImGui's own vector font: hex and code).
enum class Type : uint8_t { Body, Small, Strong, Title, Display, Mono };
// Adds the overlay's faces to the shared atlas (overlay.cpp, once, after its default font, which stays the dev menu's and the Mono face).
void load_fonts(ImFontAtlas* atlas);
void push(Type t);
void push(Type t, float scale);   // the type at `scale` times its size (a pin's own size, pin_board.cpp)
void pop();
float size_of(Type t);

// ---- the ImGui style ----------------------------------------------------------------------------------------------------------------
// The palette and the style from the theme and the player's look; `frame_rgba` is the game frame's standing-out colour for accent "auto"
// (null: slate). `blur_ok`: the backdrop blur can be drawn this frame (else glass falls back to solid). Rebuilt from ImGui's unscaled
// defaults every time, so nothing is scaled twice.
void apply(const Look& look, const unsigned char* frame_rgba, bool blur_ok);
// The accent a setting names: the theme's ("theme" or empty), "auto", a preset or a colour.
ImU32 accent_colour(const char* accent, const unsigned char* frame_rgba);
inline ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
ImU32 with_alpha(ImU32 c, float a);

// ---- icons: vector, drawn with the draw list in the launcher's icon style (Lucide: 24-unit grid, round 2-unit strokes) ----------------
enum class Icon : uint8_t {
    None, Cheats, Search, Table, Tools, Close, Check, Snow, Keyboard, Plus, Trash, Undo, Edit, Play, Pause, Gear, Info, Warn, Up, Down,
    Changed, Same, Folder, Code, Eye, Bolt, Pad, Chevron, Back, Export, Import, Refresh, Graph, Memory, Pointer, Thread, Breakpoint, Help,
    Text, Number, Decimal, Question, Dots, Star, Map, Camera, Speed, Tag, Layers, Popout, DockLeft, DockCentre, DockRight, Pin,
    Monitor, Volume, Expand,   // the F1 settings menu's pages (play_ui.cpp)
};
void icon(ImDrawList* dl, Icon i, ImVec2 centre, float size, ImU32 col);
void icon_item(Icon i, float size = 0.f, ImU32 col = 0);   // an icon as a layout item (size 0: the text height)

// ---- widgets ------------------------------------------------------------------------------------------------------------------------
enum class Btn : uint8_t { Secondary, Primary, Quiet, Danger };
// A pill button with an optional leading icon (launcher .btn). width 0: fits the label; < 0: fills to that much from the right edge.
bool button(const char* label, Icon icon = Icon::None, Btn kind = Btn::Secondary, float width = 0.f, float height = 0.f);
bool icon_button(const char* id, Icon icon, const char* tooltip, Btn kind = Btn::Quiet, float size = 0.f);
// The launcher's switch: a pill track (accent when on) with a white knob. Returns true when it was flipped.
bool toggle(const char* id, bool* v);
// A large selectable card: icon, title and a line of explanation (the scan wizard's choices).
bool choice_card(const char* id, Icon icon, const char* title, const char* body, bool selected, ImVec2 size);
// An inline label on a tinted pill.
void chip(const char* text, ImU32 bg, ImU32 fg, Icon icon = Icon::None);
// A section heading with an optional line under it in the secondary colour.
void section(const char* title, const char* sub = nullptr);
// A centred empty state: an icon in a soft circle, a title and an explanation, in the space left (or `height`).
void empty_state(Icon icon, const char* title, const char* body, float height = 0.f);
// Wrapped text in a type and colour.
void text(Type t, ImU32 col, const char* fmt, ...) IM_FMTARGS(3);
void text_wrapped(Type t, ImU32 col, const char* fmt, ...) IM_FMTARGS(3);
// One glyph of the button-hint bar: a controller button ("A", "B", "L", "+") as the launcher's round key cap, or a keyboard key ("Enter")
// as a key cap; then its label. Drawn at the cursor, on one line.
void hint(const char* key, const char* label, bool pad);
float hint_width(const char* key, const char* label, bool pad);
// A big number with a caption under it (the scan's result count).
void stat(const char* value, const char* caption, ImU32 col = 0);
// A key of the on-screen keyboard: a rounded key cap with a label or an icon; `accent` for the confirming key, `on` for a latched one
// (shift). `size` in pixels.
bool key(const char* id, const char* label, Icon icon, bool accent, bool on, ImVec2 size);

}  // namespace nxrt::cheats::ui::theme
