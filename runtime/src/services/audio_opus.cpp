// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// hwopus's decoders (audio_opus.h): the libopus wrapper, the IHardwareOpusDecoder object and the Switch 2 hardware audio codec.
//
// Sources: libopus's public API (BSD-3-Clause: opus_decoder_create / opus_decode / opus_decoder_ctl(OPUS_RESET_STATE) and the
// opus_multistream_* counterparts); https://switchbrew.org/wiki/Audio_services for the interfaces and command ids,
// https://switchbrew.org/wiki/Error_codes for the audio results (module 153); Ryujinx (MIT)
// src/Ryujinx.Horizon/Sdk/Codec/Detail/HardwareOpusDecoder.cs for the framed packet and the decode replies (consumed bytes, samples,
// and the u64 the official processes ignore). The hardware codec's shared-memory layout, its checks and its numbers are the ones
// coord/cleanroom/specs/audio_opus.cpp.md records from the runtime's behaviour.
//
// The hardware codec, in short: OpenManager gets a 0x60-byte layout of two memories. The transfer memory (manager, instances, transfer
// queue regions) is only range-checked. The process memory holds io_count IO slots (a framed packet in, interleaved s16 out) and a
// completion ring: the server's producer index at +0x04, the game's consumer index at +0x44 and io_count + 1 entries of 0x40 bytes
// from +0x80. Execute takes 16-byte tasks, decodes each slot with the instance's decoder, writes a completion and moves the producer
// index; a slot stays leased until the game's consumer index has passed its completion.
#include "audio_opus.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <cstring>
#include <mutex>
#include <vector>

#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services.h"
#include "services_internal.h"

#ifndef NXRT_HAVE_OPUS
#define NXRT_HAVE_OPUS 0
#endif
#if NXRT_HAVE_OPUS
#include <opus.h>
#include <opus_multistream.h>
#endif

namespace nxrt::services::opus {

using ipc::Request;
using ipc::Response;
using ipc::Service;

namespace {

// Audio results (module 153, Switchbrew "Error codes").
constexpr uint32_t kAudOperationFailed = res(153, 2);
constexpr uint32_t kAudOutOfSessions = res(153, 5);
constexpr uint32_t kAudBufferCountReached = res(153, 8);
constexpr uint32_t kAudInvalidAddress = res(153, 42);
constexpr uint32_t kAudNotSupported = res(153, 513);
constexpr uint32_t kAudInvalidArgument = res(153, 900);
constexpr uint32_t kAudInvalidHandle = res(153, 1536);
constexpr uint32_t kAudInvalidRevision = res(153, 1537);

constexpr uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

uint32_t load_u32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
uint32_t load_be32(const uint8_t* p) {
    return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
}

#if NXRT_HAVE_OPUS
uint32_t from_libopus(int err) {
    switch (err) {
    case OPUS_OK: return 0;
    case OPUS_BAD_ARG: return kResBadArg;
    case OPUS_BUFFER_TOO_SMALL: return kResBufferTooSmall;
    case OPUS_INTERNAL_ERROR: return kResInternal;
    case OPUS_INVALID_PACKET: return kResInvalidPacket;
    case OPUS_UNIMPLEMENTED: return kResUnimplemented;
    case OPUS_INVALID_STATE: return kResInvalidState;
    case OPUS_ALLOC_FAIL: return kResAllocFail;
    default: return err < 0 ? kResInternal : 0;
    }
}
#endif

int64_t micros_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}
int64_t nanos_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

// ---- sizes ------------------------------------------------------------------------------------------------------------------------

uint32_t work_buffer_size(uint32_t channels) {
    if (channels == 1) return 18492;
    if (channels == 2) return 27260;
    return 0;
}

uint32_t work_buffer_size_multistream(uint32_t streams, uint32_t coupled) {
    if (streams < 1 || streams > 255 || coupled > streams) return 0;
    return 24 + 272 + coupled * 27240 + (streams - coupled) * 18472;
}

bool available() { return NXRT_HAVE_OPUS != 0; }

// ---- Decoder ----------------------------------------------------------------------------------------------------------------------

Decoder::~Decoder() { release(); }

void Decoder::release() {
#if NXRT_HAVE_OPUS
    if (state_) {
        if (cfg_.kind == Kind::Multistream)
            opus_multistream_decoder_destroy(static_cast<OpusMSDecoder*>(state_));
        else
            opus_decoder_destroy(static_cast<OpusDecoder*>(state_));
    }
#endif
    state_ = nullptr;
}

uint32_t Decoder::init(const Config& c) {
    release();
    cfg_ = c;
#if NXRT_HAVE_OPUS
    int err = OPUS_OK;
    if (c.kind == Kind::Multistream)
        state_ = opus_multistream_decoder_create(static_cast<opus_int32>(c.rate), static_cast<int>(c.channels), static_cast<int>(c.streams),
                                                 static_cast<int>(c.coupled), c.mapping.data(), &err);
    else
        state_ = opus_decoder_create(static_cast<opus_int32>(c.rate), static_cast<int>(c.channels), &err);
    if (err != OPUS_OK && state_) release();
    return from_libopus(err);
#else
    return kResUnimplemented;
#endif
}

uint32_t Decoder::decode(const uint8_t* framed, size_t framed_size, int16_t* out, size_t out_values, bool reset, uint32_t* samples,
                         uint32_t* consumed) {
    if (samples) *samples = 0;
    if (consumed) *consumed = 0;
#if NXRT_HAVE_OPUS
    if (!ok()) return kResInvalidState;
    if (reset) {
        const int err = cfg_.kind == Kind::Multistream ? opus_multistream_decoder_ctl(static_cast<OpusMSDecoder*>(state_), OPUS_RESET_STATE)
                                                       : opus_decoder_ctl(static_cast<OpusDecoder*>(state_), OPUS_RESET_STATE);
        if (const uint32_t r = from_libopus(err)) return r;
    }
#else
    (void)reset;
#endif
    if (!framed || framed_size < 8) return kResInputTooSmall;
    const uint32_t payload = load_be32(framed);   // the final range at +4 is not compared
    if (payload == 0) return kResInvalidPacket;
    if (payload > framed_size - 8) return kResBufferTooSmall;
    uint32_t n = 0;
    if (const uint32_t r = decode_raw(framed + 8, payload, out, out_values, &n)) return r;
    if (samples) *samples = n;
    if (consumed) *consumed = 8 + payload;
    return 0;
}

uint32_t Decoder::decode_raw(const uint8_t* packet, size_t size, int16_t* out, size_t out_values, uint32_t* samples) {
    if (samples) *samples = 0;
#if NXRT_HAVE_OPUS
    if (!ok()) return kResInvalidState;
    const size_t per_channel = cfg_.channels ? out_values / cfg_.channels : 0;
    const size_t frames = std::min<size_t>(cfg_.frame_samples, per_channel);
    if (frames == 0) return kResBufferTooSmall;
    const int n = cfg_.kind == Kind::Multistream
                      ? opus_multistream_decode(static_cast<OpusMSDecoder*>(state_), packet, static_cast<opus_int32>(size), out,
                                                static_cast<int>(frames), 0)
                      : opus_decode(static_cast<OpusDecoder*>(state_), packet, static_cast<opus_int32>(size), out,
                                    static_cast<int>(frames), 0);
    if (n < 0) return from_libopus(n);
    if (samples) *samples = static_cast<uint32_t>(n);
    return 0;
#else
    (void)packet, (void)size, (void)out, (void)out_values;
    return kResUnimplemented;
#endif
}

// ---- IHardwareOpusDecoder ---------------------------------------------------------------------------------------------------------

namespace {

std::atomic<uint32_t> g_decoder_ids{0};   // process-wide, never reset: the first object is 1

// The object: its decoder and what it did (counted for the log: a failed call in full, a summary every 4096 calls and at the end).
class DecoderObject : public Service {
public:
    explicit DecoderObject(const Config& c) : Service("IHardwareOpusDecoder") {
        const uint32_t r = dec_.init(c);
        id_ = ++g_decoder_ids;
        if (r)
            NXLOG_W("hwopus", "decoder %u (%u Hz, %u channels): libopus refused it (0x%x); every decode on it fails", id_, c.rate,
                    c.channels, r);
        struct Form {
            uint32_t id;
            const char* name;
            bool reset, perf;
        };
        static const Form kForms[] = {
            {0, "DecodeInterleavedOld", false, false},
            {2, "DecodeInterleavedForMultiStreamOld", false, false},
            {4, "DecodeInterleavedWithPerfOld", false, true},
            {5, "DecodeInterleavedForMultiStreamWithPerfOld", false, true},
            {6, "DecodeInterleavedWithPerfAndResetOld", true, true},
            {7, "DecodeInterleavedForMultiStreamWithPerfAndResetOld", true, true},
            {8, "DecodeInterleaved", true, true},
            {9, "DecodeInterleavedForMultiStream", true, true},
        };
        for (const Form& f : kForms)
            add_cmd(f.id, f.name, [this, f](Request& req, Response& rsp) { return decode(f.id, f.reset, f.perf, req, rsp); });
        add_cmd(1, "SetContext", [](Request&, Response&) -> uint32_t { return 0; });
        add_cmd(3, "SetContextForMultiStream", [](Request&, Response&) -> uint32_t { return 0; });
    }

    ~DecoderObject() override {
        if (calls_ > 0) summary("closed");
    }

private:
    uint32_t decode(uint32_t cmd, bool has_reset, bool perf, Request& req, Response& rsp) {
        std::lock_guard<std::mutex> lk(mu_);
        const ipc::Buffer in = req.in_buf(0), out = req.out_buf(0);
        const bool reset = has_reset && req.arg<uint8_t>(0) != 0;
        const auto t0 = std::chrono::steady_clock::now();
        uint32_t samples = 0, consumed = 0;
        const uint32_t r = dec_.decode(buf_ptr(in), static_cast<size_t>(in.size), reinterpret_cast<int16_t*>(buf_mut(out)),
                                       static_cast<size_t>(out.size / 2), reset, &samples, &consumed);
        const int64_t us = micros_since(t0);
        ++calls_;
        const Config& c = dec_.config();
        if (r) {
            if (++errors_ <= 4) {
                const uint8_t* p = buf_ptr(in);
                const uint32_t payload = p && in.size >= 4 ? load_be32(p) : 0;
                const uint32_t toc = p && in.size > 8 ? p[8] : 0;
                NXLOG_W("hwopus",
                        "decoder %u (%u Hz, %u channels): cmd %u failed 0x%x (input %llu bytes, payload %u, TOC 0x%02x; output room %llu "
                        "samples per channel)",
                        id_, c.rate, c.channels, cmd, r, static_cast<unsigned long long>(in.size), payload, toc,
                        static_cast<unsigned long long>(out.size / 2 / std::max<uint32_t>(c.channels, 1)));
            }
            return r;
        }
        samples_ += samples;
        if (samples > 0) {
            const int16_t* pcm = reinterpret_cast<const int16_t*>(buf_ptr(out));
            const size_t n = size_t{samples} * c.channels;
            if (std::all_of(pcm, pcm + n, [](int16_t v) { return v == 0; })) ++silent_;
        }
        rsp.push<uint32_t>(consumed);
        rsp.push<uint32_t>(samples);
        if (perf) rsp.push<uint64_t>(static_cast<uint64_t>(us));
        if (calls_ % 4096 == 0) summary("so far");
        return 0;
    }

    void summary(const char* when) const {
        const Config& c = dec_.config();
        NXLOG_I("hwopus", "decoder %u (%u Hz, %u channels) %s: %llu decodes, %llu samples per channel, %llu failed, %llu all silent", id_,
                c.rate, c.channels, when, static_cast<unsigned long long>(calls_), static_cast<unsigned long long>(samples_),
                static_cast<unsigned long long>(errors_), static_cast<unsigned long long>(silent_));
    }

    std::mutex mu_;
    Decoder dec_;
    uint32_t id_ = 0;
    uint64_t calls_ = 0, samples_ = 0, errors_ = 0, silent_ = 0;
};

}  // namespace

std::shared_ptr<Service> make_decoder_object(const Config& c) { return std::make_shared<DecoderObject>(c); }

// ---- the hardware audio codec -----------------------------------------------------------------------------------------------------

namespace {

// Codec parameters (0x120 bytes): u64 kind, s32 rate, s32 channels, s64 frame duration (ns), s32 streams, s32 coupled, u8 mapping[255].
uint32_t parse_parameters(const ipc::Buffer& b, Config* out) {
    if (!b.addr || b.size != 0x120) return kAudInvalidArgument;
    const uint8_t* p = buf_ptr(b);
    uint64_t kind;
    int32_t rate, channels, streams, coupled;
    int64_t duration;
    std::memcpy(&kind, p, 8);
    std::memcpy(&rate, p + 0x08, 4);
    std::memcpy(&channels, p + 0x0C, 4);
    std::memcpy(&duration, p + 0x10, 8);
    std::memcpy(&streams, p + 0x18, 4);
    std::memcpy(&coupled, p + 0x1C, 4);
    if (kind == 0) return kAudNotSupported;
    if (kind != 1 && kind != 2) return kAudInvalidArgument;
    if (rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) return kResInvalidRate;
    if (channels <= 0 || channels > (kind == 1 ? 2 : 255)) return kResInvalidChannels;
    const int64_t micros = duration / 1000;
    if (micros != 2500 && micros != 5000 && micros != 10000 && micros != 20000) return kAudInvalidArgument;
    const uint64_t frame = (static_cast<uint64_t>(rate) * static_cast<uint64_t>(duration) + 999999999ull) / 1000000000ull;
    Config c;
    c.kind = static_cast<Kind>(kind);
    c.rate = static_cast<uint32_t>(rate);
    c.channels = static_cast<uint32_t>(channels);
    c.frame_samples = static_cast<uint32_t>(frame);
    if (kind == 2) {
        if (streams <= 0 || streams > 255 || coupled < 0 || coupled > streams || streams + coupled > channels) return kAudInvalidArgument;
        for (int32_t i = 0; i < channels; ++i)
            if (p[0x20 + i] != 255 && p[0x20 + i] >= streams + coupled) return kAudInvalidArgument;
        c.streams = static_cast<uint32_t>(streams);
        c.coupled = static_cast<uint32_t>(coupled);
        std::memcpy(c.mapping.data(), p + 0x20, c.mapping.size());
    }
    *out = c;
    return 0;
}

// What one instance needs: 0x140 bytes of its own and the decoder's work size, 64-aligned.
uint32_t instance_size(const Config& c, uint32_t* bytes) {
    const uint32_t r = c.rate;
    const uint32_t allowed[] = {r / 400, r / 400 + 1, r / 200, r / 200 + 1, r / 100, r / 100 + 1, r / 50, r / 50 + 1};
    if (std::find(std::begin(allowed), std::end(allowed), c.frame_samples) == std::end(allowed)) return kResBadArg;
    const uint32_t work =
        c.kind == Kind::Multistream ? work_buffer_size_multistream(c.streams, c.coupled) : work_buffer_size(c.channels);
    if (work == 0) return c.kind == Kind::Multistream ? kResBadArg : kResInvalidChannels;
    *bytes = static_cast<uint32_t>(0x140 + align_up(work, 64));
    return 0;
}

// Session tags: 1..65535, handed out round-robin from `next` and held by a manager until it goes away. Never reset.
struct Tags {
    std::mutex mu;
    std::vector<bool> used = std::vector<bool>(65536, false);
    uint32_t next = 1;
};
Tags& tags() {
    static Tags t;
    return t;
}
uint16_t acquire_tag() {
    Tags& t = tags();
    std::lock_guard<std::mutex> lk(t.mu);
    for (uint32_t looked = 0; looked < 65535; ++looked) {
        const uint32_t tag = t.next;
        t.next = tag == 65535 ? 1 : tag + 1;
        if (!t.used[tag]) {
            t.used[tag] = true;
            return static_cast<uint16_t>(tag);
        }
    }
    return 0;
}
void release_tag(uint16_t tag) {
    Tags& t = tags();
    std::lock_guard<std::mutex> lk(t.mu);
    t.used[tag] = false;
}

// The 0x60-byte layout OpenManager gets (24 u32).
struct Layout {
    uint32_t version, manager_size, manager_offset, instances_size, instances_offset, transfer_queue_size, transfer_queue_offset,
        transfer_size, io_size, io_offset, completion_size, completion_offset, process_size, instance_stride, instance_count,
        input_stride, output_stride, io_count;
    uint32_t reserved[6];
};
static_assert(sizeof(Layout) == 0x60, "Layout is 0x60 bytes");

// Two regions overlap when the one starting later starts inside the other (at equal offsets: when `as` is not 0).
bool overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) { return a <= b ? b - a < as : a - b < bs; }
bool inside(uint64_t offset, uint64_t size, uint64_t total) { return offset <= total && size <= total - offset; }

uint32_t check_layout(const Layout& l, uint64_t tsize, uint64_t paddr, uint64_t psize, uint64_t props) {
    if (l.version != 1) return kAudInvalidRevision;
    for (uint32_t w : l.reserved)
        if (w != 0) return kAudInvalidArgument;
    if ((props & 7) == 0) return kAudInvalidArgument;
    if (l.instance_count < 1 || l.instance_count > 65536 || l.instance_stride == 0 || l.instance_stride % 64 != 0 ||
        l.input_stride < 0x600 || l.input_stride % 64 != 0 || l.output_stride == 0 || l.output_stride % 64 != 0)
        return kAudInvalidArgument;
    if (l.io_count < 1 || l.io_count > 65536) return kAudInvalidArgument;
    if (tsize == 0 || tsize % 4096 != 0 || psize == 0 || psize % 4096 != 0 || paddr % 4096 != 0 || psize > UINT64_MAX - paddr ||
        l.transfer_size > tsize || l.process_size > psize || l.transfer_size % 4096 != 0 || l.process_size % 4096 != 0)
        return kAudInvalidAddress;
    const uint64_t ring_need = align_up(0xC0 + uint64_t{l.io_count} * 0x40, 4096);
    if (l.manager_size < align_up(64 + uint64_t{l.io_count} * 16, 64) ||
        l.instances_size < uint64_t{l.instance_count} * l.instance_stride ||
        l.io_size < uint64_t{l.io_count} * (uint64_t{l.input_stride} + l.output_stride) || l.transfer_queue_size < ring_need ||
        l.completion_size < ring_need)
        return kResBufferTooSmall;
    struct Region {
        uint64_t offset, size, align;
    };
    const Region tm[3] = {{l.manager_offset, l.manager_size, 64}, {l.instances_offset, l.instances_size, 64},
                          {l.transfer_queue_offset, l.transfer_queue_size, 4096}};
    for (const Region& r : tm)
        if (r.offset % r.align != 0 || !inside(r.offset, r.size, l.transfer_size)) return kAudInvalidAddress;
    for (int later = 1; later < 3; ++later)   // the later region first: at equal offsets its size decides
        for (int earlier = 0; earlier < later; ++earlier)
            if (overlaps(tm[later].offset, tm[later].size, tm[earlier].offset, tm[earlier].size)) return kAudInvalidAddress;
    if (l.io_offset % 64 != 0 || l.completion_offset % 4096 != 0 || !inside(l.io_offset, l.io_size, l.process_size) ||
        !inside(l.completion_offset, l.completion_size, l.process_size) ||
        overlaps(l.io_offset, l.io_size, l.completion_offset, l.completion_size))
        return kAudInvalidAddress;
    return 0;
}

// IHardwareAudioCodecManagerSession: one game's batch decoder.
class CodecManager : public Service {
public:
    CodecManager(const Layout& l, uint64_t process, uint16_t tag)
        : Service("IHardwareAudioCodecManagerSession"), l_(l), process_(process), ring_(process + l.completion_offset), tag_(tag),
          capacity_(l.io_count + 1), decoders_(l.instance_count), leases_(l.io_count) {
        add_cmd(0, "InitializeInstance", [this](Request& req, Response&) { return initialize(req); });
        add_cmd(1, "FinalizeInstance", [this](Request& req, Response&) { return finalize(req); });
        add_cmd(10, "Execute", [this](Request& req, Response&) { return execute(req); });
    }
    ~CodecManager() override { release_tag(tag_); }

    uint32_t new_event_handle() const { return event_.new_handle(); }

private:
    struct Lease {
        bool held = false;
        uint32_t completion = 0;   // the ring entry its task was published at
        uint32_t instance = 0;
    };

    std::atomic<uint32_t>& word(uint64_t off) const {
        return *reinterpret_cast<std::atomic<uint32_t>*>(static_cast<uintptr_t>(ring_ + off));
    }

    // Frees the slots whose completions the game has consumed since the last look.
    uint32_t reclaim() {
        if (faulted_) return kAudOperationFailed;
        const uint32_t consumer = word(0x44).load(std::memory_order_acquire);
        const uint32_t producer = word(0x04).load(std::memory_order_acquire);
        if (consumer >= capacity_ || producer != write_) return kAudInvalidArgument;
        const uint32_t consumed = (consumer + capacity_ - reclaimed_) % capacity_;
        if (consumed > outstanding_) return kAudInvalidArgument;
        for (Lease& s : leases_)
            if (s.held && (s.completion + capacity_ - reclaimed_) % capacity_ < consumed) s.held = false;
        reclaimed_ = consumer;
        outstanding_ -= consumed;
        return 0;
    }

    uint32_t initialize(Request& req) {
        std::lock_guard<std::mutex> lk(mu_);
        if (faulted_) return kAudOperationFailed;
        const uint32_t id = req.arg<uint32_t>(0);
        if (id >= l_.instance_count || decoders_[id]) return kAudInvalidArgument;
        Config c;
        if (const uint32_t r = parse_parameters(req.in_buf(0), &c)) return r;
        uint32_t need = 0;
        if (const uint32_t r = instance_size(c, &need)) return r;
        if (need > l_.instance_stride) return kResBufferTooSmall;
        if (align_up(uint64_t{c.frame_samples} * c.channels * 2, 64) > l_.output_stride) return kResBufferTooSmall;
        auto d = std::make_unique<Decoder>();
        if (const uint32_t r = d->init(c)) return r;
        decoders_[id] = std::move(d);
        return 0;
    }

    uint32_t finalize(Request& req) {
        std::lock_guard<std::mutex> lk(mu_);
        if (const uint32_t r = reclaim()) return r;
        const uint32_t id = req.arg<uint32_t>(0);
        if (id >= l_.instance_count || !decoders_[id]) return kAudInvalidArgument;
        for (const Lease& s : leases_)
            if (s.held && s.instance == id) return kAudBufferCountReached;
        decoders_[id].reset();
        return 0;
    }

    struct Task {
        uint16_t tag, instance, io, sequence;
        uint32_t input_bytes;
        uint8_t reset, notify;
        uint16_t reserved;
    };
    static_assert(sizeof(Task) == 16, "Task is 16 bytes");

    uint32_t execute(Request& req) {
        std::lock_guard<std::mutex> lk(mu_);
        if (const uint32_t r = reclaim()) return r;
        const ipc::Buffer b = req.in_buf(0);
        if (b.size % 16 != 0 || b.size / 16 > l_.io_count) return kAudInvalidArgument;
        const size_t count = static_cast<size_t>(b.size / 16);
        if (count && !b.addr) return kAudInvalidArgument;   // deviation: tasks at address 0 (the older file faulted on the read)
        if (count > l_.io_count - outstanding_) return kAudBufferCountReached;
        std::vector<Task> tasks(count);
        if (count) std::memcpy(tasks.data(), buf_ptr(b), count * sizeof(Task));
        std::vector<bool> taken(l_.io_count, false);
        for (const Task& t : tasks) {
            if (t.tag != tag_ || t.instance >= l_.instance_count || !decoders_[t.instance] || t.io >= l_.io_count ||
                t.input_bytes > l_.input_stride || t.reset > 1 || t.notify > 1 || t.reserved != 0)
                return kAudInvalidArgument;
            if (leases_[t.io].held || taken[t.io]) return kAudBufferCountReached;
            taken[t.io] = true;
        }
        const uint64_t slot_stride = uint64_t{l_.input_stride} + l_.output_stride;
        for (const Task& t : tasks) {
            Decoder& dec = *decoders_[t.instance];
            uint8_t* slot = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(process_ + l_.io_offset + t.io * slot_stride));
            const auto t0 = std::chrono::steady_clock::now();
            uint32_t samples = 0;
            const uint32_t r = dec.decode(slot, t.input_bytes, reinterpret_cast<int16_t*>(slot + l_.input_stride), l_.output_stride / 2,
                                          t.reset != 0, &samples, nullptr);
            const int64_t ns = nanos_since(t0);
            uint8_t entry[0x40] = {};
            std::memcpy(entry + 0x00, &t.tag, 2);
            std::memcpy(entry + 0x02, &t.instance, 2);
            std::memcpy(entry + 0x04, &t.io, 2);
            std::memcpy(entry + 0x06, &t.sequence, 2);
            std::memcpy(entry + 0x08, &ns, 8);
            const uint32_t out_bytes = r ? 0 : samples * dec.config().channels * 2;
            std::memcpy(entry + 0x10, &out_bytes, 4);
            std::memcpy(entry + 0x14, &r, 4);
            std::memcpy(reinterpret_cast<void*>(static_cast<uintptr_t>(ring_ + 0x80 + uint64_t{write_} * 0x40)), entry, sizeof entry);
            uint32_t expect = write_;
            if (!word(0x04).compare_exchange_strong(expect, (write_ + 1) % capacity_, std::memory_order_acq_rel)) {
                faulted_ = true;
                event_.signal();
                NXLOG_E("hwacod", "manager %u: the game moved the producer index (%u, expected %u) while a batch ran: faulted", tag_,
                        expect, write_);
                return kAudOperationFailed;
            }
            leases_[t.io] = {true, write_, t.instance};
            write_ = (write_ + 1) % capacity_;
            ++outstanding_;
            if (t.notify) event_.signal();
            if (r && ++failed_logged_ <= 4)
                NXLOG_W("hwacod", "manager %u: instance %u, IO slot %u, sequence %u: decode of %u bytes failed 0x%x", tag_, t.instance,
                        t.io, t.sequence, t.input_bytes, r);
        }
        if (count && ++batches_logged_ <= 3)
            NXLOG_I("hwacod", "manager %u: batch of %zu tasks published, producer index %u, %u outstanding", tag_, count, write_,
                    outstanding_);
        return 0;
    }

    std::mutex mu_;
    const Layout l_;
    const uint64_t process_, ring_;
    const uint16_t tag_;
    const uint32_t capacity_;
    std::vector<std::unique_ptr<Decoder>> decoders_;
    std::vector<Lease> leases_;
    ServiceEvent event_;
    uint32_t write_ = 0, reclaimed_ = 0, outstanding_ = 0;
    bool faulted_ = false;
    uint32_t failed_logged_ = 0, batches_logged_ = 0;
};

uint32_t open_manager(Request& req, Response& rsp) {
    if (req.in_copy.size() < 2) return kAudInvalidHandle;
    Layout l;
    static_assert(sizeof(Layout) / 4 == 24, "24 words");
    uint32_t* w = reinterpret_cast<uint32_t*>(&l);
    for (size_t i = 0; i < 24; ++i) w[i] = req.arg<uint32_t>(i * 4);
    const uint64_t tsize = req.arg<uint64_t>(0x60), paddr = req.arg<uint64_t>(0x68), psize = req.arg<uint64_t>(0x70),
                   props = req.arg<uint64_t>(0x78);
    if (const uint32_t r = check_layout(l, tsize, paddr, psize, props)) return r;
    if (!mem::is_mapped(paddr, psize)) return kAudInvalidAddress;
    const uint16_t tag = acquire_tag();
    if (!tag) return kAudOutOfSessions;
    uint8_t* ring = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(paddr + l.completion_offset));
    if (load_u32(ring + 0x44) != 0) {
        release_tag(tag);
        return kAudInvalidArgument;
    }
    std::memset(ring, 0, 0x40);
    std::memset(ring + 0x80, 0, (size_t{l.io_count} + 1) * 0x40);
    auto mgr = std::make_shared<CodecManager>(l, paddr, tag);
    NXLOG_I("hwacod", "manager %u: %u IO slots (in %u, out %u bytes), %u instances of %u bytes", tag, l.io_count, l.input_stride,
            l.output_stride, l.instance_count, l.instance_stride);
    rsp.push<uint32_t>(tag);
    rsp.out_copy.push_back(mgr->new_event_handle());
    rsp.push_object(mgr);
    return 0;
}

std::shared_ptr<Service> make_client_session() {
    return make_iface("IClientSession", [](Service& s) {
        s.add_cmd(0, "GetInstanceSize", [](Request& req, Response& rsp) -> uint32_t {
            Config c;
            if (const uint32_t r = parse_parameters(req.in_buf(0), &c)) return r;
            uint32_t bytes = 0;
            if (const uint32_t r = instance_size(c, &bytes)) return r;
            rsp.push<uint32_t>(bytes);
            return 0;
        });
        s.add_cmd(1, "GetManagerSize", [](Request& req, Response& rsp) -> uint32_t {
            const uint32_t io_count = req.arg<uint32_t>(0);
            if (io_count == 0 || io_count > 65536) return kAudInvalidArgument;
            rsp.push<uint32_t>(static_cast<uint32_t>(align_up(64 + uint64_t{io_count} * 16, 64)));
            return 0;
        });
        s.add_cmd(10, "OpenManager", [](Request& req, Response& rsp) { return open_manager(req, rsp); });
    });
}

}  // namespace

void register_opus_codec() {
    ipc::extend("IHardwareOpusDecoderManager", 100, "GetClientSession", [](Service&, Request&, Response& rsp) -> uint32_t {
        rsp.push_object(make_client_session());
        return 0;
    });
}

}  // namespace nxrt::services::opus
