// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// audsmx:u, the Switch 2 sound mixer nn::audio's OutputStream / InputStream play through (ext_common.h): a user session creates
// streams, each over a ring buffer in the game's memory; a mixer thread takes the frames of every engaged output stream into the mix
// bus (audio_mixbus.h, so they reach the host output) and writes silence into every engaged input stream (there is no microphone).
// auddmg:u's virtual device manager (audio.cpp) attaches and detaches streams through sound_mixer_attach.
//
// Interfaces and command ids: https://switchbrew.org/wiki/Audio_services (audsmx:u: IUserSessionManager, IUserSession,
// IExternalStreamSession); audio results: https://switchbrew.org/wiki/Error_codes (module 153). The ring's layout, the command
// checks and the size rule are the ones coord/cleanroom/specs/ext_audsmx.cpp.md records from the runtime's behaviour:
//   ring + 0x44    u32 read cursor (slot index): output streams: moved by the mixer; input streams: by the game
//   ring + 0x1004  u32 write cursor: output: the game; input: the mixer
//   ring + 0x1040  the frames, slot k at + k x frame bytes, frames + 1 slots
// A stream holds filled = (write + slots - read) % slots frames; a cursor outside the slots makes it unusable until it is right again.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio_mixbus.h"
#include "ext_common.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "services_internal.h"

namespace nxrt::services::ext {

namespace {

constexpr uint32_t kNotFound = res(153, 1);
constexpr uint32_t kOperationFailed = res(153, 2);
constexpr uint32_t kInsufficientBuffer = res(153, 4);
constexpr uint32_t kInvalidAddress = res(153, 42);
constexpr uint32_t kNotSupported = res(153, 513);
constexpr uint32_t kInvalidArgument = res(153, 900);

constexpr uint64_t kReadCursor = 0x44, kWriteCursor = 0x1004, kFrames = 0x1040;
constexpr uint32_t kTypeS16 = 2, kTypeF32 = 5;
constexpr uint32_t kBlock = 240;                                   // 5 ms at 48 kHz: what one tick moves at most
constexpr auto kTick = std::chrono::milliseconds(5);

constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

struct Stream {
    Stream(uint32_t id_, bool input_, uint64_t ring_, uint64_t ring_size_, uint32_t flags_)
        : id(id_), input(input_), ring(ring_), ring_size(ring_size_), flags(flags_) {}
    ~Stream() {
        if (voice) audio::mixbus::close_voice(voice);
    }

    const uint32_t id;
    const bool input;
    const uint64_t ring, ring_size;
    const uint32_t flags;

    std::mutex mu;   // everything below
    bool open = false, engaged = false;
    uint32_t slots = 0, frame_bytes = 0, sample_type = kTypeS16;
    uint32_t data_mode = 0;
    int32_t threshold = 0;
    float gain = 1.0f;
    ServiceEvent state_event, data_event;
    uint32_t voice = 0;
    bool ticked = false;

    std::atomic<uint32_t>& cursor(uint64_t off) const {
        return *reinterpret_cast<std::atomic<uint32_t>*>(static_cast<uintptr_t>(ring + off));
    }
    uint8_t* frame(uint32_t slot) const {
        return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(ring + kFrames + uint64_t{slot} * frame_bytes));
    }

    // Both cursors, when they lie inside the slots.
    bool cursors(uint32_t* read, uint32_t* write) const {
        *read = cursor(kReadCursor).load(std::memory_order_acquire);
        *write = cursor(kWriteCursor).load(std::memory_order_acquire);
        return *read < slots && *write < slots;
    }
    static uint32_t filled(uint32_t read, uint32_t write, uint32_t slots) { return (write + slots - read) % slots; }

    // The data event: signalled while the game can move at least `threshold` frames (read them from an input, write them to an output).
    void data_rule(uint32_t now_filled) {
        const uint32_t available = input ? now_filled : slots - 1 - now_filled;
        if (data_mode == 2 && int64_t{available} >= threshold) data_event.signal();
    }
    void set_engaged(bool on) {
        if (engaged == on) return;
        engaged = on;
        state_event.signal();
    }
};

// Every stream of the process, in creation order (weak: the stream object owns its stream), and the mixer thread.
struct Mixer {
    std::mutex mu;
    std::vector<std::weak_ptr<Stream>> streams;
    bool running = false;
};
Mixer& mixer() {
    static Mixer m;
    return m;
}
std::atomic<uint32_t> g_stream_ids{0};   // process-wide, never reset: the first stream is 1

// One output sample as the bus takes it: the gain applied in float, clamped to full scale, truncated to int16.
int16_t to_s16(float v, float gain) {
    const float x = std::clamp(v * gain, -1.0f, 1.0f);
    return static_cast<int16_t>(x * 32767.0f);
}

void tick_stream(Stream& s) {
    std::lock_guard<std::mutex> lk(s.mu);
    uint32_t read, write;
    if (!s.open || !s.cursors(&read, &write)) return;
    uint32_t filled = Stream::filled(read, write, s.slots);
    if (!s.engaged) {
        s.data_rule(filled);
        return;
    }
    if (!s.ticked) {
        s.ticked = true;
        NXLOG_I("audsmx", "stream %u (%s, %u slots of %u bytes, %s) runs: first mixer tick", s.id, s.input ? "input" : "output", s.slots,
                s.frame_bytes, s.sample_type == kTypeF32 ? "f32" : "s16");
    }
    if (s.input) {   // no microphone: silence for the game to read
        const uint32_t n = std::min(s.slots - 1 - filled, kBlock);
        if (n) {
            for (uint32_t k = 0; k < n; ++k) std::memset(s.frame((write + k) % s.slots), 0, s.frame_bytes);
            s.cursor(kWriteCursor).store((write + n) % s.slots, std::memory_order_release);
            filled += n;
        }
        s.data_rule(filled);
        return;
    }
    if (!s.voice) s.voice = audio::mixbus::open_voice("Switch 2 mixer stream");
    const uint32_t n = std::min({filled, kBlock, audio::mixbus::room(s.voice)});
    if (n) {
        int16_t pcm[kBlock * 2];
        for (uint32_t k = 0; k < n; ++k) {
            const uint8_t* f = s.frame((read + k) % s.slots);
            for (int c = 0; c < 2; ++c) {
                float v;
                if (s.sample_type == kTypeF32) {
                    std::memcpy(&v, f + 4 * c, 4);
                    if (std::isnan(v)) v = 0.0f;
                } else {
                    int16_t x;
                    std::memcpy(&x, f + 2 * c, 2);
                    v = static_cast<float>(x) / 32768.0f;
                }
                pcm[2 * k + c] = to_s16(v, s.gain);
            }
        }
        uint32_t expect = read;
        if (s.cursor(kReadCursor).compare_exchange_strong(expect, (read + n) % s.slots, std::memory_order_acq_rel)) {
            filled -= n;
            audio::mixbus::push(s.voice, pcm, n);
        }
    }
    s.data_rule(filled);
}

// The mixer: every 5 ms (an absolute schedule) each live stream in creation order. Started by the first stream; never stops.
void mixer_thread() {
    auto deadline = std::chrono::steady_clock::now();
    for (;;) {
        deadline += kTick;
        std::this_thread::sleep_until(deadline);
        std::vector<std::shared_ptr<Stream>> live;
        {
            Mixer& m = mixer();
            std::lock_guard<std::mutex> lk(m.mu);
            live.reserve(m.streams.size());
            auto keep = m.streams.begin();
            for (auto& w : m.streams)
                if (auto s = w.lock()) {
                    live.push_back(std::move(s));
                    *keep++ = w;
                }
            m.streams.erase(keep, m.streams.end());
        }
        for (const auto& s : live) tick_stream(*s);
    }
}

void mixer_add(const std::shared_ptr<Stream>& s) {
    Mixer& m = mixer();
    std::lock_guard<std::mutex> lk(m.mu);
    m.streams.push_back(s);
    if (!m.running) {
        m.running = true;
        std::thread(mixer_thread).detach();
    }
}

std::shared_ptr<Stream> mixer_find(uint32_t id) {
    Mixer& m = mixer();
    std::lock_guard<std::mutex> lk(m.mu);
    for (const auto& w : m.streams)
        if (auto s = w.lock())
            if (s->id == id) return s;
    return nullptr;
}

// ---- audsmx:u:stream ----------------------------------------------------------------------------------------------------------------

uint32_t open_stream(Stream& s, Request& req) {
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.open) return kOperationFailed;
    const uint32_t type = req.arg<uint32_t>(0), channels = req.arg<uint32_t>(4), rate = req.arg<uint32_t>(8),
                   frames = req.arg<uint32_t>(12), prefill = req.arg<uint32_t>(16), flags = req.arg<uint32_t>(20);
    const bool format = (type == kTypeS16 || type == kTypeF32) && rate == 48000 && (channels == 2 || (s.input && channels == 1)) &&
                        prefill == 0 && flags == 0;
    if (!format) return kNotSupported;
    if (frames == 0 || frames >= 0x7FFFFFFF) return kInvalidArgument;
    const uint32_t slots = frames + 1;
    const uint32_t frame_bytes = channels * (type == kTypeS16 ? 2 : 4);
    const uint64_t data = uint64_t{slots} * frame_bytes;
    const uint64_t stamps = s.input ? 0 : uint64_t{std::min<uint32_t>(slots, 16)} * 8;
    const uint64_t minimum = align_up(0x1000 + align_up(0x40 + stamps, 64) + align_up(data, 64), 0x1000);
    if (s.ring_size < minimum || kFrames + data > s.ring_size) return kInsufficientBuffer;
    s.slots = slots;
    s.frame_bytes = frame_bytes;
    s.sample_type = type;
    uint32_t read, write;
    if (!s.cursors(&read, &write)) return kInvalidAddress;
    s.open = true;
    s.engaged = false;
    s.gain = 1.0f;
    s.state_event.signal();
    s.data_rule(Stream::filled(read, write, s.slots));
    return 0;
}

uint32_t purge_stream(Stream& s, Request& req, Response& rsp) {
    const uint32_t frames = req.arg<uint32_t>(0);
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.open) return kOperationFailed;
    uint32_t read, write;
    if (!s.cursors(&read, &write)) return kInvalidAddress;
    const uint32_t filled = Stream::filled(read, write, s.slots);
    const uint32_t n = frames == 0 || frames > filled ? filled : frames;
    uint32_t expect = read;
    if (!s.cursor(kReadCursor).compare_exchange_strong(expect, (read + n) % s.slots, std::memory_order_acq_rel)) return kOperationFailed;
    s.data_rule(filled - n);
    rsp.push<uint32_t>(n);
    return 0;
}

std::shared_ptr<Service> make_stream_object(std::shared_ptr<Stream> st) {
    return make_iface("audsmx:u:stream", [st](Service& s) {
        s.add_cmd(0, "OpenStream", [st](Request& req, Response&) { return open_stream(*st, req); });
        s.add_cmd(1, "CloseStream", [st](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(st->mu);
            st->open = st->engaged = false;
            st->data_mode = 0;
            st->state_event.signal();
            return 0;
        });
        s.add_cmd(2, "GetStreamState", [st](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(st->mu);
            rsp.push<uint32_t>(!st->open ? 0u : st->engaged ? 2u : 1u);
            return 0;
        });
        s.add_cmd(3, "GetStreamGain", [st](Request&, Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(st->mu);
            rsp.push<float>(st->gain);
            return 0;
        });
        s.add_cmd(4, "SetStreamGain", [st](Request& req, Response&) -> uint32_t {
            const float gain = req.arg<float>(0);
            if (req.arg<int64_t>(8) != 0) return kNotSupported;   // no ramps
            if (!(gain >= 0.0f && gain <= 128.0f)) return kInvalidArgument;
            std::lock_guard<std::mutex> lk(st->mu);
            st->gain = gain;
            return 0;
        });
        s.add_cmd(5, "AcquireStreamStateChangeEvent", [st](Request&, Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(st->state_event.new_handle());
            return 0;
        });
        s.add_cmd(6, "ReleaseStreamStateChangeEvent", [](Request&, Response&) -> uint32_t { return 0; });
        s.add_cmd(7, "AcquireStreamDataEvent", [st](Request& req, Response& rsp) -> uint32_t {
            const uint32_t mode = req.arg<uint32_t>(0);
            const int32_t threshold = req.arg<int32_t>(4);
            if (mode != 0 && mode != 2) return kNotSupported;
            std::lock_guard<std::mutex> lk(st->mu);
            if (!st->open || threshold < 0 || static_cast<uint32_t>(threshold) >= st->slots) return kInvalidArgument;
            uint32_t read, write;
            if (!st->cursors(&read, &write)) return kInvalidAddress;
            st->data_mode = mode;
            st->threshold = threshold;
            st->data_rule(Stream::filled(read, write, st->slots));
            rsp.out_copy.push_back(st->data_event.new_handle());
            return 0;
        });
        s.add_cmd(8, "ReleaseStreamDataEvent", [st](Request&, Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(st->mu);
            st->data_mode = 0;
            return 0;
        });
        s.add_cmd(9, "ClockStream", [](Request&, Response&) -> uint32_t { return kNotSupported; });
        s.add_cmd(10, "PrimeStream", [](Request&, Response&) -> uint32_t { return kNotSupported; });
        s.add_cmd(11, "PurgeStream", [st](Request& req, Response& rsp) { return purge_stream(*st, req, rsp); });
        s.add_cmd(12, "RenameStream", [](Request&, Response&) -> uint32_t { return 0; });
    });
}

// ---- audsmx:u:session -------------------------------------------------------------------------------------------------------------

struct UserSession {
    std::mutex mu;
    std::vector<std::weak_ptr<Stream>> streams;   // the streams this session created

    std::shared_ptr<Stream> find(uint32_t id) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& w : streams)
            if (auto s = w.lock())
                if (s->id == id) return s;
        return nullptr;
    }
};

uint32_t create_stream(UserSession& us, Request& req, Response& rsp) {
    const uint32_t device = req.arg<uint32_t>(0x00), flags = req.arg<uint32_t>(0x04);
    const uint64_t ring = req.arg<uint64_t>(0x10), size = req.arg<uint64_t>(0x18);   // 0x08: the applet resource user id, unused
    if (device != 1 && device != 2) return kInvalidArgument;
    const bool input = device == 2;
    if (flags != (input ? 2u : 0x24u)) return kNotSupported;
    if (ring == 0 || ring % 0x1000 != 0 || size % 0x1000 != 0 || size < 0x2000 || !mem::is_mapped(ring, size)) return kInvalidAddress;
    const uint32_t id = ++g_stream_ids;
    auto st = std::make_shared<Stream>(id, input, ring, size, flags);
    {
        std::lock_guard<std::mutex> lk(us.mu);
        us.streams.push_back(st);
    }
    mixer_add(st);
    NXLOG_I("audsmx", "stream %u: %s, ring 0x%llx (0x%llx bytes)", id, input ? "input" : "output", static_cast<unsigned long long>(ring),
            static_cast<unsigned long long>(size));
    rsp.push<uint32_t>(id);
    rsp.push_object(make_stream_object(std::move(st)));
    return 0;
}

// Engage / Disengage: every id of the buffer must be an open stream of this session; the ones before a failing id stay changed.
uint32_t engage_streams(UserSession& us, Request& req, bool on) {
    const ipc::Buffer b = req.in_buf(0);
    if (b.size % 4 != 0) return kInvalidArgument;
    const uint8_t* p = buf_ptr(b);
    const size_t count = p ? static_cast<size_t>(b.size / 4) : 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t id;
        std::memcpy(&id, p + 4 * i, 4);
        const std::shared_ptr<Stream> s = us.find(id);
        if (!s) return kInvalidArgument;
        std::lock_guard<std::mutex> lk(s->mu);
        if (!s->open) return kOperationFailed;
        s->set_engaged(on);
    }
    return 0;
}

std::shared_ptr<Service> make_user_session() {
    auto us = std::make_shared<UserSession>();
    return make_iface("audsmx:u:session", [us](Service& s) {
        s.add_cmd(1, "CreateStream", [us](Request& req, Response& rsp) { return create_stream(*us, req, rsp); });
        s.add_cmd(2, "EngageStreams", [us](Request& req, Response&) { return engage_streams(*us, req, true); });
        s.add_cmd(3, "DisengageStreams", [us](Request& req, Response&) { return engage_streams(*us, req, false); });
    });
}

}  // namespace

uint32_t sound_mixer_attach(uint32_t id, uint32_t functionality, bool attach) {
    const std::shared_ptr<Stream> s = mixer_find(id);
    if (!s) return kNotFound;
    if (attach && functionality != (s->input ? 2u : 1u)) return kNotSupported;
    std::lock_guard<std::mutex> lk(s->mu);
    if (!s->open) return kOperationFailed;
    s->set_engaged(attach);
    return 0;
}

void register_sound_mixer() {
    services::add("audsmx:u", [] {
        return make_iface("audsmx:u", [](Service& s) {
            // The process handle and the values Switchbrew lists are not needed: the session is this process's.
            s.add_cmd(0, "OpenUserSession", [](Request&, Response& rsp) -> uint32_t {
                rsp.push_object(make_user_session());
                return 0;
            });
        });
    });
}

}  // namespace nxrt::services::ext
