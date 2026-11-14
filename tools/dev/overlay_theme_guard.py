#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Portions Copyright (c) 2026 Chaos192
# Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
"""overlay_theme_guard.py: no plain Dear ImGui anywhere in the in-game overlay (owner order 11-14).

Every ImGui surface the runtime draws over a game (the F1 menu, the cheats overlay and its tools, the pins, the counter and notices,
the software keyboard, the prompts, debug windows) wears the overlay's theme (runtime/src/gfx/play/cheats_theme.h). This is the static
half of the guard; the screenshot tests (runtime/tests/cheats_ui, runtime/tests/menu_ui: soft_raster.h theme_guard) are the runtime
half. It fails when, in the runtime's or the tools' C/C++ sources:

  stock-style     ImGui::StyleColorsDark / Light / Classic is called (the theme makes every colour itself);
  default-font    AddFontDefault / AddFontDefaultVector / AddFontDefaultBitmap is called anywhere but the one last-resort fallback in
                  cheats_theme.cpp (marked "overlay-theme: last-resort font"), which a build with the compiled-in fonts never reaches;
  debug-window    one of ImGui's own tool windows is shown (ShowDemoWindow, ShowMetricsWindow, ShowStyleEditor, ...);
  stock-widget    a screen calls an ImGui widget the theme replaces (Checkbox, RadioButton, Slider*, Drag*, Combo / BeginCombo,
                  TreeNode*, CollapsingHeader, ProgressBar, ColorEdit / ColorPicker / ColorButton, PlotLines / PlotHistogram,
                  InputInt / InputFloat with ImGui's +/- buttons, ArrowButton, SetTooltip / SetItemTooltip / BeginTooltip, menu and
                  tab bars) outside the theme module (theme_widgets.cpp), or imports ImGui's names with `using namespace ImGui`;
  frame-style     an ImGui::NewFrame() is not preceded by the theme's base style of the frame (cheats::ui::frame_style), so the
                  frame's windows would start from ImGui's stock style;
  setup-context   an ImGui::CreateContext() is not followed by cheats::ui::setup_context() (the fonts and the theme before the first
                  frame);
  window-file     a file that is not one of the overlay's screen files (OVERLAY_FILES below) opens an ImGui window, child, popup
                  or tooltip: a new screen file is added to the list only once it draws inside the themed frame;
  colour          a hard-coded colour (IM_COL32 with a non-zero alpha) in a screen file outside the theme module, unless the line
                  says why with "theme-ok:" (the mouse pointer, a transparent warm-up pixel).

    python tools/dev/overlay_theme_guard.py [--root <checkout>] [--list]
exit 0: clean; 1: violations (one line each: rule, file:line, the code); CI runs it through tools/dev/test_overlay_theme_guard.py.
"""
import argparse
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
SCAN_DIRS = ['runtime', 'tools', 'game-build', 'recomp']
SKIP_DIRS = {'third_party', 'node_modules', '_deps', '.git', 'build', 'out', 'refs', '__pycache__'}
EXTS = ('.cpp', '.cc', '.cxx', '.c', '.h', '.hpp', '.mm', '.m', '.inl')

# the theme module: the only place ImGui's own widgets, tooltips and the last-resort font may be called
THEME_FILES = {'runtime/src/gfx/play/theme_widgets.cpp', 'runtime/src/gfx/play/cheats_theme.cpp', 'runtime/src/gfx/play/cheats_theme_file.cpp',
               'runtime/src/gfx/play/theme_fonts.cpp'}
# the overlay's screen files: every window they open is drawn inside a themed frame (overlay.cpp draw: frame_style before NewFrame)
OVERLAY_FILES = {
    'runtime/src/gfx/overlay.cpp',
    'runtime/src/gfx/play/cheats_ui.cpp', 'runtime/src/gfx/play/cheats_ui_mem.cpp', 'runtime/src/gfx/play/cheats_ui_osk.cpp',
    'runtime/src/gfx/play/cheats_ui_scan.cpp', 'runtime/src/gfx/play/cheats_ui_tools.cpp', 'runtime/src/gfx/play/cheats_icon.cpp',
    'runtime/src/gfx/play/pin_board.cpp', 'runtime/src/gfx/play/perf_pins.cpp', 'runtime/src/gfx/play/play_ui.cpp',
    'runtime/src/gfx/play/play_ui_about.cpp', 'runtime/src/gfx/play/play_ui_fastboot.cpp', 'runtime/src/gfx/play/overlay_screens.cpp',
    'runtime/src/gfx/play/touch_pad.cpp',
}
TEST_DIRS = ('runtime/tests/cheats_ui/', 'runtime/tests/menu_ui/')

RX_STOCK_STYLE = re.compile(r'\bStyleColors(Dark|Light|Classic)\s*\(')
RX_DEFAULT_FONT = re.compile(r'\bAddFontDefault(Vector|Bitmap)?\s*\(')
RX_DEBUG_WINDOW = re.compile(r'\bShow(Demo|Metrics|DebugLog|IDStackTool|About|Font|Style|User)\w*\s*\(')
RX_STOCK_WIDGET = re.compile(r'\bImGui::(Checkbox|CheckboxFlags|RadioButton|Slider\w*|VSlider\w*|Drag\w*|Combo|BeginCombo|ListBox|BeginListBox|'
                             r'TreeNode\w*|CollapsingHeader|ProgressBar|ColorEdit\d|ColorPicker\d|ColorButton|PlotLines|PlotHistogram|'
                             r'InputInt\d?|InputFloat\d?|InputDouble|ArrowButton|SetTooltip|SetItemTooltip|BeginTooltip|BeginItemTooltip|'
                             r'BeginMenuBar|BeginMainMenuBar|BeginTabBar|BeginTabItem)\s*\(')
RX_USING_IMGUI = re.compile(r'\busing\s+namespace\s+ImGui\b')
RX_NEW_FRAME = re.compile(r'\bImGui::NewFrame\s*\(')
RX_CREATE_CONTEXT = re.compile(r'\bImGui::CreateContext\s*\(')
RX_WINDOW = re.compile(r'\bImGui::(Begin|BeginChild|BeginPopup|BeginPopupModal|BeginPopupContextItem|BeginPopupContextWindow|'
                       r'BeginPopupContextVoid|BeginTooltipEx|OpenPopup)\s*\(')
RX_COLOUR = re.compile(r'\bIM_COL32\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)')
FALLBACK_MARK = 'overlay-theme: last-resort font'


def strip_comments(lines):
    """The lines with // comments and /* */ blocks blanked (string literals kept as they are: none of the patterns sits in one)."""
    out, block = [], False
    for ln in lines:
        s, i, res = ln, 0, ''
        while i < len(s):
            if block:
                j = s.find('*/', i)
                if j < 0:
                    i = len(s)
                    break
                block, i = False, j + 2
                continue
            if s.startswith('//', i):
                break
            if s.startswith('/*', i):
                block, i = True, i + 2
                continue
            if s[i] == '"':   # a string literal: copied as it is
                j = i + 1
                while j < len(s) and s[j] != '"':
                    j += 2 if s[j] == '\\' else 1
                res += s[i:j + 1]
                i = j + 1
                continue
            res += s[i]
            i += 1
        out.append(res)
    return out


def files(root):
    for top in SCAN_DIRS:
        base = os.path.join(root, top)
        if not os.path.isdir(base):
            continue
        for d, dirs, fs in os.walk(base):
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS and not x.startswith('build')]
            for f in fs:
                if f.endswith(EXTS):
                    p = os.path.join(d, f)
                    yield os.path.relpath(p, root).replace(os.sep, '/'), p


def check(root):
    bad = []
    fallbacks = []
    for rel, path in files(root):
        try:
            raw = open(path, encoding='utf-8', errors='replace').read().split('\n')
        except OSError:
            continue
        joined = '\n'.join(raw)
        if not any(k in joined for k in ('ImGui', 'IM_COL32', 'StyleColors', 'AddFontDefault')):
            continue
        code = strip_comments(raw)
        is_theme = rel in THEME_FILES
        is_screen = rel in OVERLAY_FILES or is_theme
        is_test = rel.startswith(TEST_DIRS)
        for n, ln in enumerate(code, 1):
            if RX_STOCK_STYLE.search(ln):
                bad.append(('stock-style', rel, n, raw[n - 1]))
            if RX_DEFAULT_FONT.search(ln):
                near = '\n'.join(raw[max(0, n - 8):n])
                if rel == 'runtime/src/gfx/play/cheats_theme.cpp' and FALLBACK_MARK in near:
                    fallbacks.append((rel, n))
                else:
                    bad.append(('default-font', rel, n, raw[n - 1]))
            if RX_DEBUG_WINDOW.search(ln) and 'ImGui::' in ln:
                bad.append(('debug-window', rel, n, raw[n - 1]))
            if RX_STOCK_WIDGET.search(ln) and rel != 'runtime/src/gfx/play/theme_widgets.cpp':
                bad.append(('stock-widget', rel, n, raw[n - 1]))
            if RX_USING_IMGUI.search(ln):
                bad.append(('stock-widget', rel, n, raw[n - 1]))
            if RX_NEW_FRAME.search(ln):
                before = '\n'.join(code[max(0, n - 13):n - 1])
                if 'frame_style(' not in before:
                    bad.append(('frame-style', rel, n, raw[n - 1]))
            if RX_CREATE_CONTEXT.search(ln):
                after = '\n'.join(code[n - 1:n + 25])
                if 'setup_context(' not in after:
                    bad.append(('setup-context', rel, n, raw[n - 1]))
            if RX_WINDOW.search(ln) and not (is_screen or is_test):
                bad.append(('window-file', rel, n, raw[n - 1]))
            if is_screen and not is_theme and not is_test:
                for m in RX_COLOUR.finditer(ln):
                    if int(m.group(4)) != 0 and 'theme-ok:' not in raw[n - 1]:
                        bad.append(('colour', rel, n, raw[n - 1]))
    if len(fallbacks) > 1:
        for rel, n in fallbacks[1:]:
            bad.append(('default-font', rel, n, 'a second last-resort font'))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default=ROOT, help='the checkout to check (default: this one)')
    ap.add_argument('--list', action='store_true', help='also list the screen files and the theme module')
    a = ap.parse_args()
    bad = check(a.root)
    if a.list:
        for f in sorted(OVERLAY_FILES | THEME_FILES):
            print('screen' if f in OVERLAY_FILES else 'theme ', f)
    for rule, rel, n, text in bad:
        print('%-13s %s:%d: %s' % (rule, rel, n, text.strip()[:140]))
    print('overlay_theme_guard: %s' % ('%d violation(s)' % len(bad) if bad else 'clean'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
