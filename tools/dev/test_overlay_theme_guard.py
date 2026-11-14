#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Portions Copyright (c) 2026 Chaos192
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""Tests of overlay_theme_guard.py: this checkout is clean, and each rule fires on a made-up tree that breaks it (and stays quiet on the
allowed forms: the marked last-resort font, a NewFrame after frame_style, a CreateContext followed by setup_context)."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import overlay_theme_guard as g  # noqa: E402

FAILS = 0


def ok(what, cond):
    global FAILS
    print('%s %s' % ('ok    ' if cond else 'FAILED', what))
    if not cond:
        FAILS += 1


def tree(files):
    d = tempfile.mkdtemp(prefix='otg_')
    for rel, text in files.items():
        p = os.path.join(d, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8') as f:
            f.write(text)
    return d


def rules(files):
    return sorted({b[0] for b in g.check(tree(files))})


def main():
    bad = g.check(g.ROOT)
    for b in bad[:20]:
        print('   ', b[0], '%s:%d' % (b[1], b[2]), b[3].strip()[:100])
    ok('this checkout: no plain ImGui in the overlay (%d violations)' % len(bad), not bad)

    screen = 'runtime/src/gfx/play/play_ui.cpp'
    ok('stock-style: StyleColorsDark is refused', rules({screen: 'void f() { ImGui::StyleColorsDark(); }\n'}) == ['stock-style'])
    ok('stock-style: also in a test', rules({'runtime/tests/menu_ui/x.cpp': 'ImGui::StyleColorsLight(&s);\n'}) == ['stock-style'])
    ok('default-font: AddFontDefaultVector is refused', rules({screen: 'io.Fonts->AddFontDefaultVector();\n'}) == ['default-font'])
    theme = 'runtime/src/gfx/play/cheats_theme.cpp'
    ok('default-font: the marked last resort in cheats_theme.cpp is allowed',
       rules({theme: '// overlay-theme: last-resort font\nif (!g) g = atlas->AddFontDefaultVector();\n'}) == [])
    ok('default-font: unmarked in cheats_theme.cpp is refused', rules({theme: 'atlas->AddFontDefault();\n'}) == ['default-font'])
    ok('default-font: a second last resort is refused',
       rules({theme: '// overlay-theme: last-resort font\na->AddFontDefault();\n// overlay-theme: last-resort font\na->AddFontDefault();\n'}) == ['default-font'])
    ok('debug-window: ShowDemoWindow is refused', rules({screen: 'ImGui::ShowDemoWindow();\n'}) == ['debug-window'])
    ok('debug-window: ShowMetricsWindow is refused', rules({screen: 'ImGui::ShowMetricsWindow(&open);\n'}) == ['debug-window'])
    for w in ['Checkbox("a", &v)', 'SliderInt("a", &i, 0, 9)', 'BeginCombo("a", "b")', 'Combo("a", &i, items, 3)', 'TreeNode("a")',
              'CollapsingHeader("a")', 'ProgressBar(0.5f)', 'ColorEdit3("a", c)', 'PlotLines("a", v, 3)', 'InputInt("a", &i)',
              'SetTooltip("a")', 'SetItemTooltip("a")', 'RadioButton("a", true)', 'ArrowButton("a", 0)', 'BeginTabBar("a")']:
        ok('stock-widget: ImGui::%s is refused on a screen' % w.split('(')[0], rules({screen: 'ImGui::%s;\n' % w}) == ['stock-widget'])
    ok('stock-widget: allowed inside the theme module', rules({'runtime/src/gfx/play/theme_widgets.cpp': 'ImGui::BeginCombo(l, p, f);\n'}) == [])
    ok('stock-widget: `using namespace ImGui` is refused', rules({screen: 'using namespace ImGui;\n'}) == ['stock-widget'])
    ok('stock-widget: a call in a comment is not one', rules({screen: '// ImGui::Checkbox("a", &v);\n/* ImGui::SliderInt(...) */\n'}) == [])
    ok('frame-style: NewFrame without the theme is refused', rules({screen: 'ImGui_ImplVulkan_NewFrame();\nImGui::NewFrame();\n'}) == ['frame-style'])
    ok('frame-style: NewFrame after frame_style is allowed', rules({screen: 'cheats::ui::frame_style(k, false);\nImGui::NewFrame();\n'}) == [])
    ok('setup-context: CreateContext without the theme is refused', rules({screen: 'ImGui::CreateContext();\nio.Fonts->Build();\n'}) == ['setup-context'])
    ok('setup-context: CreateContext then setup_context is allowed', rules({screen: 'ImGui::CreateContext();\ncheats::ui::setup_context();\n'}) == [])
    ok('window-file: a window outside the screen files is refused', rules({'runtime/src/gfx/extras/new_tool.cpp': 'ImGui::Begin("x");\n'}) == ['window-file'])
    ok('window-file: a popup outside the screen files is refused', rules({'tools/x/y.cpp': 'ImGui::BeginPopupModal("x");\n'}) == ['window-file'])
    ok('window-file: the screen files may open windows', rules({screen: 'ImGui::Begin("x"); ImGui::BeginChild("y");\n'}) == [])
    ok('colour: a hard-coded colour on a screen is refused', rules({screen: 'dl->AddRectFilled(a, b, IM_COL32(20, 20, 24, 200));\n'}) == ['colour'])
    ok('colour: transparent and "theme-ok:" lines are allowed',
       rules({screen: 'c = IM_COL32(0, 0, 0, 0);\nd = IM_COL32(255, 255, 255, 255);   // theme-ok: the pointer\n'}) == [])
    print('test_overlay_theme_guard: %s' % ('FAILED' if FAILS else 'all ok'))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
