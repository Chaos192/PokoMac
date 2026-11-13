// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Input trace file (S5): the per-frame pad state of a run, recorded from a live session or written by a scripted run, replayed
// frame for frame. Text, delta encoded, human-editable:
//
//   # nxin 1
//   <frame> <buttons-hex> <lx> <ly> <rx> <ry> <mouse-dx> <mouse-dy>
//   ...
//   end <frame>
//
// A line gives the state that holds from that frame on, until the next line. Buttons are nn::hid::NpadButton bits (bit n = 1 << n),
// written as 0x<hex>. Sticks are -32767..32767 (hid units, y up = +). Mouse dx/dy are the raw counts delivered in THAT frame only
// (an impulse, not held): InputTrace::at() returns them only for a frame that has a line of its own, and a recording writes a line
// with zero mouse values in the frame after an impulse. Frames are numbered from 0; entries are strictly increasing in frame.
// The optional last line "end <frame>" records how long the run was. Floats are written with %.9g so that they read back exactly.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::tt {

struct PadFrame {
    uint64_t buttons = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    float mouse_dx = 0, mouse_dy = 0;          // impulse: counts during this frame only
    bool operator==(const PadFrame& o) const {
        return buttons == o.buttons && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry && mouse_dx == o.mouse_dx && mouse_dy == o.mouse_dy;
    }
    bool held_equal(const PadFrame& o) const {      // everything except the mouse impulse
        return buttons == o.buttons && lx == o.lx && ly == o.ly && rx == o.rx && ry == o.ry;
    }
};

class InputTrace {
public:
    struct Entry { uint64_t frame; PadFrame pad; };
    std::vector<Entry> entries;
    uint64_t end_frame = 0;                      // 0 = unknown

    // Recording: call once per frame in increasing order (frames may be skipped). Adds an entry when the held state changed, when the
    // mouse impulse is non-zero, or when the previous recorded frame had an impulse (the entry that zeroes it).
    void record(uint64_t frame, const PadFrame& pad);
    // The state for `frame`: held part from the last entry at or before it (zeros before the first entry); the mouse impulse only
    // when an entry sits exactly at `frame`.
    PadFrame at(uint64_t frame) const;
    bool write(const std::string& path, std::string* err) const;
    bool read(const std::string& path, std::string* err);
    std::string to_text() const;
    bool from_text(const std::string& text, std::string* err);   // error text = "line N: message"

private:
    PadFrame last_recorded_;
    uint64_t last_frame_ = 0;
    bool have_last_ = false;
    bool last_had_impulse_ = false;
};

}  // namespace nxrt::tt
