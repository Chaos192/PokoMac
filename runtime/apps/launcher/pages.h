// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The tabs of the launcher. Each factory returns a page the window owns.
#pragma once
#include <memory>

#include "ui.h"

namespace mprl {

std::unique_ptr<Page> make_display_page();      // window, frame rate, picture                       (page_display.cpp)
std::unique_ptr<Page> make_extras_page();      // HDR, low latency, frame rate counter                               (page_display.cpp)
std::unique_ptr<Page> make_controls_page();     // mouse and button prompts                           (page_controls.cpp)
std::unique_ptr<Page> make_keys_page();         // keyboard and mouse bindings                        (page_keys.cpp)
std::unique_ptr<Page> make_pad_page();          // controller options and button remapping           (page_pad.cpp)
void add_fastboot_rows(Form& f);                // start-up and menu tweaks, rows of the Extras tab    (page_fastboot.cpp)
std::unique_ptr<Page> make_tweaks_page();       // optional looks: the glow colour                   (page_tweaks.cpp)

}  // namespace mprl
