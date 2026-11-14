// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Portions Copyright (c) 2026 Chaos192
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
// Body 16 px (the launcher's 1rem), Small 13.5, Strong (semibold body), Title 20, Display 28, Mono (AnySwitch Mono: hex and code).
enum class Type : uint8_t { Body, Small, Strong, Title, Display, Mono };
// Adds the overlay's faces to the atlas, Inter first, so it is the context's default font (every window, tooltip and draw-list text):
// Inter 4.1 Regular and SemiBold (Latin, Greek, Cyrillic, tabular figures), AnySwitch Mono (Cascadia Mono subset) and, merged into each,
// AnySwitch Symbols (DejaVu Sans' symbol blocks) and the system's CJK faces (theme_fonts.cpp), so game names and cheat text in Chinese,
// Japanese or Korean draw. All three files are compiled in (assets/fonts, OFL 1.1 / Bitstream Vera; licences beside them). ImGui's own
// font is only the last resort of a build without them, never seen otherwise (tools/dev/overlay_theme_guard.py).
void load_fonts(ImFontAtlas* atlas);
// The faces in force (the theme's own font replaces Inter when it brings one).
ImFont* font_of(Type t);
// The system CJK faces found and mapped (theme_fonts.cpp): their paths, for the log and the tests. Empty when none was found.
const char* cjk_fonts();
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
// The guard (tools/dev/overlay_theme_guard.py is its static half): true when the style in force is the theme's, every ImGuiCol set by
// apply() (none left at ImGui's stock dark colours, which ImGuiStyle's constructor puts there) and the default font Inter (or the theme's).
// *why names what is not. Both screenshot tests check it on every screen.
bool style_check(std::string* why);
// ImGui's stock colours (StyleColorsDark / Light / Classic), for the tests' scan of the drawn vertices: true when `c` is one of the
// stock accents no theme colour may equal (the blue family of the stock buttons, headers, check marks, sliders and title bars).
bool is_stock_colour(ImU32 c);
inline ImVec4 v4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }
ImU32 with_alpha(ImU32 c, float a);

// ---- icons: vector, drawn with the draw list in the launcher's icon style (Lucide: 24-unit grid, round 2-unit strokes) ----------------
enum class Icon : uint8_t {
    None, Cheats, Search, Table, Tools, Close, Check, Snow, Keyboard, Plus, Trash, Undo, Edit, Play, Pause, Gear, Info, Warn, Up, Down,
    Changed, Same, Folder, Code, Eye, Bolt, Pad, Chevron, Back, Export, Import, Refresh, Graph, Memory, Pointer, Thread, Breakpoint, Help,
    Text, Number, Decimal, Question, Dots, Star, Map, Camera, Speed, Tag, Layers, Popout, DockLeft, DockCentre, DockRight, Pin,
    Monitor, Volume, Expand,   // the F1 settings menu's pages (play_ui.cpp)
    Minus, ChevronDown, Power, Mark,   // steppers, disclosure / combo chevrons, the quit prompt, the AnySwitch mark (the monoline A and sun)
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

// ---- ImGui's stock widgets, drawn in the theme (theme_widgets.cpp) ----------------------------------------------------------------------
// The same arguments and behaviour as the ImGui call each replaces (mouse, keyboard and pad navigation, Ctrl+click typing on a slider),
// drawn as the launcher draws its controls. Screens call these, never ImGui's (tools/dev/overlay_theme_guard.py).
// A rounded box, filled with the accent and a tick when on, the label after it (ImGui::Checkbox).
bool checkbox(const char* label, bool* v);
// A ring with an accent dot when chosen (ImGui::RadioButton).
bool radio(const char* label, bool active);
bool radio(const char* label, int* v, int value);
// The launcher's slider: a thin track filled with the accent up to a white round knob, the value after it (ImGui::SliderInt/Float).
bool slider_int(const char* label, int* v, int lo, int hi, const char* format = "%d", ImGuiSliderFlags flags = 0);
bool slider_float(const char* label, float* v, float lo, float hi, const char* format = "%.3f", ImGuiSliderFlags flags = 0);
// A select: the field with a chevron, its list in a themed popup (ImGui::BeginCombo; end with end_combo()).
bool begin_combo(const char* label, const char* preview, ImGuiComboFlags flags = 0);
void end_combo();
bool combo(const char* label, int* cur, const char* const items[], int n);
bool combo(const char* label, int* cur, const char* items_zero_separated);   // "a\0b\0c\0"
// A number field with round - and + buttons (ImGui::InputInt; step 0: no buttons).
bool input_int(const char* label, int* v, int step = 1, int step_fast = 100, ImGuiInputTextFlags flags = 0);
// A row that opens and closes what is under it, with a chevron (ImGui::TreeNode: TreePop when open / CollapsingHeader: none).
bool tree_node(const char* label, ImGuiTreeNodeFlags flags = 0);
bool collapsing_header(const char* label, ImGuiTreeNodeFlags flags = 0);
// The launcher's progress bar: a thin pill with the accent part done; `overlay` text after it (ImGui::ProgressBar).
void progress(float fraction, ImVec2 size = ImVec2(-1.f, 0.f), const char* overlay = nullptr);
// A round colour swatch button (ImGui::ColorButton); a colour row: swatch + hex field (ImGui::ColorEdit3, RGB 0..1).
bool colour_swatch(const char* id, ImU32 col, float size = 0.f, bool selected = false);
bool colour_edit(const char* label, float rgb[3]);
// A line graph on a sunken card (ImGui::PlotLines); `offset` is the ring's first value.
void plot_lines(const char* label, const float* values, int n, int offset, const char* overlay, float lo, float hi, ImVec2 size);
// Tooltips: the solid dialog colour, Small type, tight padding (ImGui::SetTooltip / SetItemTooltip).
void tooltip(const char* fmt, ...) IM_FMTARGS(1);
void item_tooltip(const char* fmt, ...) IM_FMTARGS(1);

// ---- windows -----------------------------------------------------------------------------------------------------------------------
// A window with the overlay's header (title in Strong type, a close button when `open` is given) in place of ImGui's title bar; the
// panel colour, radius and padding of the theme. End it with ImGui::End(). Tool pop-outs, Diagnostics, debug panels.
bool begin_window(const char* title, bool* open, ImGuiWindowFlags flags = 0);
// A centred dialog sheet over the dimmed game (the keyboard, the quit and controller prompts): the theme's dialog colour and radius, a
// header with an icon and the title, `width` in pixels (0: the theme's), at the top of the screen when `top`. Returns true when its
// content is to be drawn; end it with end_dialog() either way.
bool begin_dialog(const char* id, const char* title, Icon icon, float width = 0.f, bool top = false);
void end_dialog();
// A notice drawn on the foreground list (no window, no input): the dialog colour at `opacity`, rounded, a coloured stripe on the left
// (`stripe` 0: none), lines of text (the first in Strong type), placed by `anchor` (0..1 of the free space on each axis) inside `area`.
struct Notice {
    const char* lines[4] = {};
    int n = 0;
    ImU32 stripe = 0;
    Icon icon = Icon::None;
    ImU32 icon_col = 0;
    float size = 0.f;                // the body text size in pixels (0: the theme's at the UI scale)
    float opacity = 0.92f;
    bool right_align = false;        // lines right-aligned (a counter in a right corner)
};
ImVec2 notice_size(const Notice& n);
void notice(ImDrawList* dl, const Notice& n, ImVec2 pos);

}  // namespace nxrt::cheats::ui::theme
