// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// hwopus's Opus decoding on libopus (BSD-3-Clause, Xiph.Org; its public API, opus.h / opus_multistream.h): the IHardwareOpusDecoder
// objects the hwopus manager (misc.cpp) hands out, and the Switch 2 hardware audio codec behind hwopus cmd 100 GetClientSession
// (IClientSession -> IHardwareAudioCodecManagerSession: a batch decoder over the game's own memory). audio_opus.cpp.
//
// Command ids and names: https://switchbrew.org/wiki/Audio_services (hwopus, IHardwareOpusDecoder, IClientSession,
// IHardwareAudioCodecManagerSession). The framed packet (u32 big-endian payload size, u32 big-endian final range, then one Opus packet,
// RFC 6716) and the codec result values 111-3 / 111-1001 / 111-1002 as Ryujinx (MIT) src/Ryujinx.Horizon/Sdk/Codec gives them.
// The work-buffer sizes below are fixed numbers: games size their memory from them, so they never follow the linked libopus.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "nxrt/ipc.h"
#include "nxrt/services.h"

namespace nxrt::services::opus {

// What a decoder decodes. The values are also the "kind" of the hardware codec's parameters.
enum class Kind : uint32_t { Opus = 1, Multistream = 2 };

struct Config {
    Kind kind = Kind::Opus;
    uint32_t rate = 48000, channels = 2, streams = 1, coupled = 0;
    uint32_t frame_samples = 960;              // the most samples per channel one packet may decode to
    std::array<uint8_t, 255> mapping{};        // multistream channel mapping (libopus reads `channels` of them)
};

// Codec results (module 111). Every libopus error maps to one of the first seven.
constexpr uint32_t kResBadArg = res(111, 2);            // OPUS_BAD_ARG
constexpr uint32_t kResBufferTooSmall = res(111, 3);    // OPUS_BUFFER_TOO_SMALL; also a payload past its input
constexpr uint32_t kResInternal = res(111, 4);          // OPUS_INTERNAL_ERROR and any unknown libopus error
constexpr uint32_t kResUnimplemented = res(111, 5);     // OPUS_UNIMPLEMENTED; a build without libopus
constexpr uint32_t kResInvalidState = res(111, 6);      // OPUS_INVALID_STATE; a decoder libopus would not make
constexpr uint32_t kResAllocFail = res(111, 7);         // OPUS_ALLOC_FAIL
constexpr uint32_t kResInputTooSmall = res(111, 8);     // less than the 8-byte frame header
constexpr uint32_t kResInvalidPacket = res(111, 17);    // OPUS_INVALID_PACKET; a payload size of 0
constexpr uint32_t kResInvalidRate = res(111, 1001);
constexpr uint32_t kResInvalidChannels = res(111, 1002);

// The decoder work size the console reports: 18492 bytes mono, 27260 stereo, 0 for any other channel count.
uint32_t work_buffer_size(uint32_t channels);
// The multistream one: 24 + 272 + coupled x 27240 + (streams - coupled) x 18472; 0 when streams is not 1..255 or coupled > streams.
uint32_t work_buffer_size_multistream(uint32_t streams, uint32_t coupled);

// One libopus decoder (single or multistream) with the hwopus framing on top.
class Decoder {
public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Makes the libopus decoder for `c` (any earlier one is freed). 0, or the mapped libopus error (111-5 without libopus).
    uint32_t init(const Config& c);
    const Config& config() const { return cfg_; }
    // One framed packet into `out` (interleaved s16, room for `out_values` values). `reset` clears the decoder state first. On success
    // *samples = samples per channel and *consumed = 8 + payload size; both are 0 on failure. `consumed` may be null.
    uint32_t decode(const uint8_t* framed, size_t framed_size, int16_t* out, size_t out_values, bool reset, uint32_t* samples,
                    uint32_t* consumed);
    // One bare Opus packet, at most config().frame_samples samples per channel.
    uint32_t decode_raw(const uint8_t* packet, size_t size, int16_t* out, size_t out_values, uint32_t* samples);
    bool ok() const { return state_ != nullptr; }

private:
    void release();
    Config cfg_;
    void* state_ = nullptr;   // OpusDecoder* or OpusMSDecoder*, by cfg_.kind
};

// Whether this runtime was built with libopus (NXRT_HAVE_OPUS).
bool available();
// A new IHardwareOpusDecoder object decoding with `c`.
std::shared_ptr<ipc::Service> make_decoder_object(const Config& c);
// Adds hwopus cmd 100 GetClientSession to the manager (called once by register_extensions()).
void register_opus_codec();

}  // namespace nxrt::services::opus
