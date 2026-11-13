// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The scripted-run executor (S5): runs the steps of a parsed script (tt_script.h) one frame boundary at a time.
// Each call to Runner::frame is one frame; a step that blocks consumes the frame, a step that does not lets the next one run in the
// same call. Failure and pass call tt::end_run (exit code 1 / 0).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <thread>

#include "nxrt/config.h"
#include "nxrt/diag.h"
#include "nxrt/ftrace.h"
#include "nxrt/log.h"
#include "tt_internal.h"

namespace nxrt::tt::detail {

namespace {
std::string tt_guid_text_safe(const uint8_t id[16], bool have) { return have ? tt_guid_to_text(id) : std::string(); }
int16_t to_stick(double v) {
    const double c = v < -1 ? -1 : v > 1 ? 1 : v;
    return static_cast<int16_t>(std::lround(c * 32767.0));
}
}  // namespace

bool Runner::load(const std::string& script_file, const RegionTable* regions, std::string* err) {
    script_ = Script();
    idx_ = 0; started_ = false; done_ = false; warped_ = false; remaining_ = 0;
    default_timeout_ = 3600;
    held_ = pressing_ = 0;
    std::memset(stick_, 0, sizeof stick_);
    stick_override_ = false;
    mouse_x_ = mouse_y_ = 0;
    fail_line_ = 0;
    igt_base_ = -1;
    has_keys_ = false;
    regions_ = regions;
    live_path_ = config_get_str("test", "live", "");
    warp_wait_loader_ = config_get_bool("test", "warp_wait_loader", true);
    warp_loader_max_ms_ = static_cast<uint64_t>(std::max<int64_t>(0, config_get_int("test", "warp_loader_max_ms", 30000)));
    gate_open_ = false; idle_frames_ = 0; gate_start_ms_ = 0;
    live_off_ = 0;
    live_lines_ = 0;
    const bool ok = parse_script_file(script_file, regions, &script_, err);
    for (const Step& st : script_.steps) {
        if (st.kind == StepKind::Key || st.kind == StepKind::KeyDown || st.kind == StepKind::KeyUp) has_keys_ = true;
    }
    return ok;
}

PadFrame Runner::pad() const {
    PadFrame p;
    p.buttons = held_ | pressing_;
    p.lx = stick_[0][0]; p.ly = stick_[0][1];
    p.rx = stick_[1][0]; p.ry = stick_[1][1];
    if (stick_override_) {
        if (stick_tmp_which_ == 0) { p.lx = stick_tmp_[0]; p.ly = stick_tmp_[1]; }
        else { p.rx = stick_tmp_[0]; p.ry = stick_tmp_[1]; }
    }
    p.mouse_dx = mouse_x_;
    p.mouse_dy = mouse_y_;
    return p;
}

void Runner::fail(const Step& st, const std::string& msg) {
    fail_line_ = st.line;
    done_ = true;
    char b[64];
    std::snprintf(b, sizeof b, ":%d: ", st.line);
    end_run(1, "fail", script_.path + b + msg + "  [" + st.src + "]");
}

bool Runner::eval_assert(const Step& st, const GameState& s, bool valid, std::string* detail) {
    auto num = [&](double* v) -> bool {
        switch (st.field) {
            case AssertField::Frame: *v = static_cast<double>(main_frame_count() ? main_frame_count() - 1 : 0); return true;
            case AssertField::Igt: *v = s.igt; return valid && s.have_igt;
            case AssertField::Health: *v = s.hp; return valid && s.have_hp;
            case AssertField::PosX: *v = s.pos[0]; return valid && s.have_player;
            case AssertField::PosY: *v = s.pos[1]; return valid && s.have_player;
            case AssertField::PosZ: *v = s.pos[2]; return valid && s.have_player;
            case AssertField::VelX: *v = s.vel[0]; return valid && s.have_player;
            case AssertField::VelY: *v = s.vel[1]; return valid && s.have_player;
            case AssertField::VelZ: *v = s.vel[2]; return valid && s.have_player;
            case AssertField::Speed: *v = std::sqrt(double(s.vel[0]) * s.vel[0] + double(s.vel[1]) * s.vel[1] + double(s.vel[2]) * s.vel[2]); return valid && s.have_player;
            case AssertField::State: *v = s.move_state; return valid && s.have_player;
            case AssertField::Tod: *v = s.tod; return valid && s.have_tod;
            case AssertField::Prog: *v = s.prog; return valid && s.have_prog;
            default: return false;
        }
    };
    if (st.field == AssertField::Area || st.field == AssertField::World) {
        const uint8_t* have = st.field == AssertField::Area ? s.area : s.world;
        if (!valid || !s.have_world) { *detail = "the id is not readable (no game state)"; return false; }
        const bool eq = std::memcmp(have, st.id, 16) == 0;
        *detail = "have " + tt_guid_hex(have) + ", want " + tt_guid_hex(st.id);
        return st.op == AssertOp::Eq ? eq : !eq;
    }
    double v = 0;
    if (!num(&v)) { *detail = "the value is not readable (no game state)"; return false; }
    char b[96];
    std::snprintf(b, sizeof b, "have %.6g, want %.6g", v, st.x);
    *detail = b;
    switch (st.op) {
        case AssertOp::Eq: return v == st.x;
        case AssertOp::Ne: return v != st.x;
        case AssertOp::Lt: return v < st.x;
        case AssertOp::Le: return v <= st.x;
        case AssertOp::Gt: return v > st.x;
        case AssertOp::Ge: return v >= st.x;
    }
    return false;
}

bool Runner::write_live_status(uint64_t k, const GameState& s, bool valid, const char* note_in) {
    std::string note_s(note_in);
    for (char& ch : note_s) if (ch == 92 || ch == 34 || static_cast<unsigned char>(ch) < 32) ch = 32;       // keep the JSON line valid (paths have backslashes)
    const char* note = note_s.c_str();
    char b[1024];
    std::snprintf(b, sizeof b,
                  "{\"lines\":%d,\"frame\":%llu,\"valid\":%d,\"world\":\"%s\",\"area\":\"%s\",\"igt\":%.3f,\"hp\":%.1f,\"pos\":[%.3f,%.3f,%.3f],\"vel\":[%.3f,%.3f,%.3f],"
                  "\"yaw\":%.4f,\"pitch\":%.4f,\"state\":%u,\"cam\":%d,\"morph\":%d,\"orbit\":%d,\"beam\":%d,\"visor\":%d,\"missiles\":%d,\"note\":\"%s\"}\n",
                  live_lines_, static_cast<unsigned long long>(k), valid ? 1 : 0, tt_guid_text_safe(s.world, s.have_world).c_str(), tt_guid_text_safe(s.area, s.have_world).c_str(),
                  s.igt, s.hp, s.pos[0], s.pos[1], s.pos[2], s.vel[0], s.vel[1], s.vel[2], s.yaw, s.pitch, s.move_state, s.cam_state, s.morph_state, s.orbit_state,
                  s.beam, s.visor, s.missiles, note);
    const std::string tmp = live_path_ + ".status.tmp", fin = live_path_ + ".status";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << b;
    }
    return MoveFileExA(tmp.c_str(), fin.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;       // fails while a reader has the old file open: the caller retries
}

bool Runner::poll_live(uint64_t k, const GameState& s, bool valid) {
    bool wrote = false;
    for (;;) {
        std::string text;
        {
            std::ifstream f(live_path_, std::ios::binary);
            if (f) {
                f.seekg(0, std::ios::end);
                const size_t size = static_cast<size_t>(f.tellg());
                if (size > live_off_) {
                    f.seekg(static_cast<std::streamoff>(live_off_));
                    text.resize(size - live_off_);
                    f.read(&text[0], static_cast<std::streamsize>(text.size()));
                    text.resize(static_cast<size_t>(f.gcount()));
                }
            }
        }
        const size_t nl = text.rfind('\n');
        if (nl != std::string::npos) {
            text.resize(nl + 1);
            live_off_ += text.size();
            int n = 0;
            for (char ch : text) if (ch == '\n') n++;
            Script add;
            std::string err;
            if (!parse_script(text, live_path_, regions_, &add, &err)) {
                NXLOG_E("tt", "live: %s (these %d lines are skipped)", err.c_str(), n);
                live_lines_ += n;
                wrote = write_live_status(k, s, valid, ("parse error: " + err).c_str());
                continue;
            }
            live_lines_ += n;
            for (Step& st : add.steps) {
                st.line += 0;
                script_.steps.push_back(st);
            }
            if (!add.steps.empty()) return true;       // the status is written when these have run and the runner is idle again
            wrote = write_live_status(k, s, valid, "ok");        // comments and blank lines only
            continue;
        }
        if (!wrote) wrote = write_live_status(k, s, valid, "idle");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void Runner::frame(uint64_t k, const GameState& s, bool valid, ::Ctx* c) {
    if (done_) return;
    detail::watch_flush(k);
    mouse_x_ = mouse_y_ = 0;       // the impulse lasts for the frame of the step that sets it
    if (!live_path_.empty()) {         // express lane of live mode: a file <live>.shot holding a name asks for a screenshot now, whatever step is running
        const std::string abort_req = live_path_ + ".abort";       // drops every step that has not finished (a wait that can never be met), releases the pad
        if (GetFileAttributesA(abort_req.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileA(abort_req.c_str());
            NXLOG_W("tt", "live: abort requested, dropping %zu pending steps", script_.steps.size() - idx_);
            idx_ = script_.steps.size();
            started_ = false;
            remaining_ = 0;
            held_ = pressing_ = 0;
            std::memset(stick_, 0, sizeof stick_);
            stick_override_ = false;
            warped_ = false;
        }
        const std::string req = live_path_ + ".shot";
        if (GetFileAttributesA(req.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::string name;
            { std::ifstream f(req, std::ios::binary); std::getline(f, name); }
            DeleteFileA(req.c_str());
            while (!name.empty() && (name.back() == 13 || name.back() == ' ')) name.pop_back();
            std::string why;
            if (!name.empty() && !take_shot(name, &why)) NXLOG_W("tt", "live shot %s: %s", name.c_str(), why.c_str());
        }
    }
    for (int guard = 0; guard < 100000; guard++) {
        if (done_) return;
        if (idx_ >= script_.steps.size() && !live_path_.empty()) {
            if (poll_live(k, s, valid)) continue;
            if (done_) return;
        }
        if (idx_ >= script_.steps.size()) {
            done_ = true;
            end_run(0, "pass", "script finished");
            return;
        }
        const Step& st = script_.steps[idx_];
        const bool first = !started_;
        if (first) { started_ = true; start_frame_ = k; }
        auto timeout_of = [&]() { return st.timeout < 0 ? default_timeout_ : st.timeout; };
        auto wait_cond = [&](bool met, const char* what) -> int {       // 1 = continue to next step, 0 = block this frame, -1 = failed
            if (met) { advance(); return 1; }
            const int64_t to = timeout_of();
            if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) {
                fail(st, std::string("timeout after ") + std::to_string(to) + " frames waiting for " + what);
                return -1;
            }
            return 0;
        };
        int r = 0;
        switch (st.kind) {
            case StepKind::Wait:
                if (first) remaining_ = st.count;
                if (remaining_ <= 0) { advance(); continue; }
                remaining_--;
                return;
            case StepKind::Press:
                if (first) { pressing_ = st.buttons; remaining_ = st.count < 0 ? 4 : st.count; }
                if (remaining_ <= 0) { pressing_ = 0; advance(); continue; }
                remaining_--;
                return;
            case StepKind::Hold: held_ |= st.buttons; advance(); continue;
            case StepKind::Release: held_ = st.buttons == ~0ull ? 0 : (held_ & ~st.buttons); pressing_ = st.buttons == ~0ull ? 0 : (pressing_ & ~st.buttons); advance(); continue;
            case StepKind::Stick: {
                const int16_t x = to_stick(st.x), y = to_stick(st.y);
                if (st.count <= 0) { stick_[st.which][0] = x; stick_[st.which][1] = y; advance(); continue; }
                if (first) { remaining_ = st.count; stick_override_ = true; stick_tmp_which_ = st.which; stick_tmp_[0] = x; stick_tmp_[1] = y; }
                if (remaining_ <= 0) { stick_override_ = false; advance(); continue; }
                remaining_--;
                return;
            }
            case StepKind::Mouse:
                if (first) remaining_ = st.count < 0 ? 1 : st.count;
                if (remaining_ <= 0) { advance(); continue; }
                mouse_x_ = static_cast<float>(st.x);
                mouse_y_ = static_cast<float>(st.y);
                remaining_--;
                return;
            case StepKind::WaitFrame: r = wait_cond(k >= static_cast<uint64_t>(st.count), "the frame"); break;
            case StepKind::WaitArea: r = wait_cond(valid && s.have_world && std::memcmp(s.area, st.id, 16) == 0, ("area " + (st.name1.empty() ? tt_guid_to_text(st.id) : st.name1)).c_str()); break;
            case StepKind::WaitWorld: r = wait_cond(valid && s.have_world && std::memcmp(s.world, st.id, 16) == 0, ("world " + (st.name1.empty() ? tt_guid_to_text(st.id) : st.name1)).c_str()); break;
            case StepKind::WaitPos: {
                bool met = false;
                if (valid && s.have_player) {
                    const double dx = s.pos[0] - st.x, dy = s.pos[1] - st.y, dz = s.pos[2] - st.z;
                    met = std::sqrt(dx * dx + dy * dy + dz * dz) <= st.r;
                }
                char b[96];
                std::snprintf(b, sizeof b, "position (%.2f, %.2f, %.2f) within %.2f", st.x, st.y, st.z, st.r);
                r = wait_cond(met, b);
                break;
            }
            case StepKind::GoTo: {
                // Heading h = atan2(dx, dz). The stick angle theta (0 = up, + = right) moves the player along h = cam + sign * theta, where cam
                // follows the camera; cam is re-estimated from the measured velocity each frame the player moves, so the loop corrects itself.
                static const double sign = config_get_bool("test", "goto_mirror", false) ? -1.0 : 1.0;
                constexpr double kPi = 3.14159265358979323846;
                auto wrap = [](double a) { while (a > kPi) a -= 2 * kPi; while (a < -kPi) a += 2 * kPi; return a; };
                if (first) { goto_have_cam_ = false; goto_cam_ = 0; goto_theta_ = 0; }
                bool met = false;
                if (valid && s.have_player) {
                    const double dx = st.x - s.pos[0], dz = st.z - s.pos[2];
                    met = std::sqrt(dx * dx + dz * dz) <= st.r;
                    if (!met) {
                        const double sp = std::sqrt(double(s.vel[0]) * s.vel[0] + double(s.vel[2]) * s.vel[2]);
                        if (sp > 30.0) {
                            const double meas = wrap(std::atan2(double(s.vel[0]), double(s.vel[2])) - sign * goto_theta_);
                            goto_cam_ = goto_have_cam_ ? wrap(goto_cam_ + 0.35 * wrap(meas - goto_cam_)) : meas;
                            goto_have_cam_ = true;
                        }
                        goto_theta_ = wrap(sign * (std::atan2(dx, dz) - goto_cam_));
                        stick_[0][0] = to_stick(std::sin(goto_theta_));
                        stick_[0][1] = to_stick(std::cos(goto_theta_));
                    }
                }
                if (met) { stick_[0][0] = stick_[0][1] = 0; }
                char b[96];
                std::snprintf(b, sizeof b, "the player at (%.1f, %.1f) within %.1f", st.x, st.z, st.r);
                if (st.soft && !met) {
                    const int64_t to = timeout_of();
                    if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) {
                        NXLOG_W("script", "SOFT-TIMEOUT line %d: %s not reached within %lld frames (at %.1f, %.1f), going on", st.line, b,
                                static_cast<long long>(to), s.pos[0], s.pos[2]);
                        stick_[0][0] = stick_[0][1] = 0;
                        advance();
                        continue;
                    }
                    return;
                }
                r = wait_cond(met, b);
                if (r < 0) stick_[0][0] = stick_[0][1] = 0;
                break;
            }
            case StepKind::WaitIgt: r = wait_cond(valid && s.have_igt && s.igt >= st.x, "the in-game time"); break;
            case StepKind::WaitRun: {
                if (first) igt_base_ = -1;
                bool met = false;
                if (valid && s.have_igt) {
                    if (igt_base_ < 0 || s.igt < igt_base_) igt_base_ = s.igt;     // (re)baseline: a new game state restarts the clock
                    met = s.igt - igt_base_ >= st.x;
                }
                char b[64];
                std::snprintf(b, sizeof b, "%.1f s of play time", st.x);
                if (st.soft && !met) {
                    const int64_t to = timeout_of();
                    if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) {
                        NXLOG_W("script", "SOFT-TIMEOUT line %d: no %s within %lld frames, going on", st.line, b, static_cast<long long>(to));
                        advance();
                        continue;
                    }
                    return;
                }
                r = wait_cond(met, b);
                break;
            }
            case StepKind::Mash: {
                const uint64_t now_ms = GetTickCount64();
                if (first) { start_ms_ = now_ms; next_mash_ms_ = now_ms; remaining_ = 0; }
                const bool met = valid && s.have_world && std::memcmp(st.text == "area" ? s.area : s.world, st.id, 16) == 0;
                if (met) { pressing_ = 0; advance(); continue; }
                const int64_t to_ms = st.timeout < 0 ? 600000 : st.timeout;
                if (to_ms > 0 && static_cast<int64_t>(now_ms - start_ms_) >= to_ms) { pressing_ = 0; fail(st, "timeout waiting for " + st.text + " " + (st.name1.empty() ? tt_guid_to_text(st.id) : st.name1)); return; }
                // The press lasts 250 ms of host time: in deterministic mode a frame is a tiny slice of host time and the front end runs on the host clock.
                if (remaining_ > 0) { if (now_ms >= mash_release_ms_) { remaining_ = 0; pressing_ = 0; } }
                else if (now_ms >= next_mash_ms_) { pressing_ = st.buttons; remaining_ = 1; mash_release_ms_ = now_ms + 250; next_mash_ms_ = now_ms + static_cast<uint64_t>(st.x); }
                return;
            }
            case StepKind::Rate:
                set_clock_rate(st.x);
                advance();
                continue;
            case StepKind::Give:
                if (!detail::give_item(static_cast<int>(st.x), static_cast<int>(st.y))) { fail(st, "@give: the player state is not readable yet"); return; }
                advance();
                continue;
            case StepKind::WaitMs: {
                const uint64_t now_ms = GetTickCount64();
                if (first) start_ms_ = now_ms;
                if (static_cast<double>(now_ms - start_ms_) >= st.x) { advance(); continue; }
                return;
            }
            case StepKind::Warp: {
                if (!warped_ && first) { gate_open_ = !warp_wait_loader_; gate_logged_ = false; idle_frames_ = 0; gate_start_ms_ = GetTickCount64(); }
                if (!warped_ && !gate_open_) {
                    uint32_t pending = 0;
                    const detail::LoaderState ls = detail::loader_state(&pending);
                    const uint64_t waited = GetTickCount64() - gate_start_ms_;
                    if (ls == detail::LoaderState::Unknown) {
                        gate_open_ = true;                     // nothing readable to wait for (no game globals yet, or a test rig without them)
                    } else if (ls == detail::LoaderState::Idle) {
                        if (++idle_frames_ >= kWarpIdleFrames) {
                            gate_open_ = true;
                            if (waited >= 100) NXLOG_I("tt", "warp gate: the resource loader was idle after %llu ms", static_cast<unsigned long long>(waited));
                        }
                    } else {
                        if (!gate_logged_) { gate_logged_ = true; NXLOG_I("tt", "warp gate: waiting for the resource loader (%u queued)", pending); }
                        idle_frames_ = 0;
                        if (waited >= warp_loader_max_ms_) {
                            gate_open_ = true;
                            NXLOG_W("tt", "warp gate: the resource loader is still busy after %llu ms (%u queued); warping anyway",
                                    static_cast<unsigned long long>(waited), pending);
                        }
                    }
                    if (!gate_open_) return;
                }
                if (!warped_) {
                    if (detail::warp(c, st.id2, st.id)) {
                        warped_ = true;
                        start_frame_ = k;
                        NXLOG_I("tt", "warp issued: world %s area %s (frame %llu)", tt_guid_to_text(st.id2).c_str(), tt_guid_to_text(st.id).c_str(), static_cast<unsigned long long>(k));
                        return;                       // the quit is processed by the game over the next frames
                    }
                    const int64_t to = timeout_of();
                    if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) { fail(st, "cannot warp: no state manager seen (the game is not in a level, or the hooks are not built in)"); return; }
                    return;
                }
                const bool met = valid && s.have_world && std::memcmp(s.area, st.id, 16) == 0 && std::memcmp(s.world, st.id2, 16) == 0;
                if (met) { warped_ = false; advance(); continue; }
                const int64_t to = timeout_of();
                if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) { fail(st, "timeout waiting for the warp target area to become current"); return; }
                return;
            }
            case StepKind::Grant:
                if (!detail::grant(st.count, static_cast<int32_t>(st.x), static_cast<int32_t>(st.y))) {
                    const int64_t to = timeout_of();
                    if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) { fail(st, "@grant: the player state is not available"); return; }
                    return;                       // retry next frame (the game may not have a player state yet)
                }
                NXLOG_I("tt", "grant %s: amount %d capacity %d", st.text.c_str(), static_cast<int>(st.x), static_cast<int>(st.y));
                advance();
                continue;
            case StepKind::Objs: {
                std::string why;
                if (!detail::dump_objects(st.text, out_dir() + "/objs.txt", &why)) { fail(st, "@objs: " + why); return; }
                advance();
                continue;
            }
            case StepKind::Probe: {
                std::string why;
                const size_t sp = st.text.find(' ');
                if (sp == std::string::npos || !detail::probe_positions(c, st.text.substr(0, sp), st.text.substr(sp + 1), &why)) { fail(st, "@probe: " + why); return; }
                advance();
                continue;
            }
            case StepKind::Poke: {
                std::string why;
                if (!detail::poke_memory(st.text, &why)) { fail(st, "@poke: " + why); return; }
                advance();
                continue;
            }
            case StepKind::Peek: {
                std::string why;
                const size_t sp = st.text.find(' ');
                if (sp == std::string::npos || !detail::peek_memory(st.text.substr(0, sp), st.text.substr(sp + 1), &why)) { fail(st, "@peek: " + why); return; }
                advance();
                continue;
            }
            case StepKind::Rays: {
                std::string why;
                const size_t sp = st.text.find(' ');
                if (sp == std::string::npos || !detail::cast_rays(c, st.text.substr(0, sp), st.text.substr(sp + 1), &why)) { fail(st, "@rays: " + why); return; }
                advance();
                continue;
            }
            case StepKind::Watch:
                if (st.text == "off") detail::watch_stop();
                else if (!detail::watch_start(c, static_cast<int>(st.y))) { fail(st, "@watch: the player is not readable (not in a level, or not called from the game hook)"); return; }
                advance();
                continue;
            case StepKind::Call: {
                uint64_t res = 0;
                std::string why;
                if (!detail::call_guest(c, static_cast<uint64_t>(st.count), st.text, &res, &why)) {
                    const int64_t to = timeout_of();
                    if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) { fail(st, "@call: " + why); return; }
                    return;
                }
                NXLOG_I("tt", "call main+0x%llx(%s) = 0x%llx", static_cast<unsigned long long>(st.count), st.text.c_str(), static_cast<unsigned long long>(res));
                advance();
                continue;
            }
            case StepKind::PressUntil: {
                std::string d;
                if (valid && eval_assert(st, s, valid, &d)) { pressing_ = 0; advance(); continue; }
                const int64_t to = timeout_of();
                if (to > 0 && static_cast<int64_t>(k - start_frame_) >= to) {
                    pressing_ = 0;
                    fail(st, "timeout after " + std::to_string(to) + " frames waiting for the @mashuntil condition (" + d + ")");
                    return;
                }
                pressing_ = ((k - start_frame_) % static_cast<uint64_t>(st.count)) < 4 ? st.buttons : 0;
                return;
            }
            case StepKind::Shot: {
                std::string why;
                if (!take_shot(st.text, &why) && require_shots()) { fail(st, "@shot " + st.text + ": " + why); return; }
                shot_frame_ = k;
                advance();
                continue;
            }
            case StepKind::Hash: add_checkpoint(st.text.empty() ? "hash" + std::to_string(idx_) : st.text); advance(); continue;
            case StepKind::Log:
                NXLOG_I("script", "%s", st.text.c_str());
                if (ft::on() && st.text.compare(0, 6, "SCENE ") == 0) ft::mark(st.text.c_str());   // S34: scene windows in the frame trace
                if (st.text.compare(0, 12, "SCENE start ") == 0) prof_sampler_label(st.text.c_str() + 12);   // and in the sampling profiler
                else if (st.text.compare(0, 10, "SCENE end ") == 0) prof_sampler_label("");
                advance();
                continue;
            case StepKind::Assert: {
                std::string d;
                if (!eval_assert(st, s, valid, &d)) { fail(st, "assertion failed: " + d); return; }
                advance();
                continue;
            }
            case StepKind::Timeout: default_timeout_ = st.count; advance(); continue;
            case StepKind::Pass:
                if (shot_frame_ && k - shot_frame_ < 3) return;   // the shot is taken at the end of a later frame on the thread that ends the run
                done_ = true; end_run(0, "pass", "@pass at " + script_.path + ":" + std::to_string(st.line)); return;
            case StepKind::Fail: fail(st, st.text.empty() ? "@fail" : st.text); return;
            case StepKind::Key:
                if (first) {
                    if (!key_event(st.text, true)) { fail(st, "unknown input '" + st.text + "'"); return; }
                    remaining_ = st.count < 0 ? 4 : st.count;
                }
                if (remaining_ <= 0) {
                    if (!key_event(st.text, false)) { fail(st, "unknown input '" + st.text + "'"); return; }
                    advance();
                    continue;
                }
                remaining_--;
                return;
            case StepKind::KeyDown:
                if (!key_event(st.text, true)) { fail(st, "unknown input '" + st.text + "'"); return; }
                advance();
                continue;
            case StepKind::KeyUp:
                if (!key_event(st.text, false)) { fail(st, "unknown input '" + st.text + "'"); return; }
                advance();
                continue;
        }
        if (r == 1) continue;
        return;       // 0 blocked, -1 failed
    }
    NXLOG_E("tt", "script runner: more than 100000 steps without consuming a frame (a loop of non-blocking steps?)");
    done_ = true;
    end_run(6, "error", "runner guard tripped");
}

}  // namespace nxrt::tt::detail
