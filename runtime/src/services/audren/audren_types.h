// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Binary parameter structures of the Switch audio renderer (audren:u). Layouts only: no server logic.
//
// Adapted from Ryujinx (Ryubing fork) (MIT):
//   src/Ryujinx.Audio/Renderer/Parameter/ (incl. Effect/, Sink/, Performance/)
//   src/Ryujinx.Audio/Renderer/Common/ (BehaviourParameter, UpdateDataHeader, AuxiliaryBufferAddresses, WaveBuffer, VoiceState,
//     PlayState, EffectType, SinkType, MemoryPoolUserState, PerformanceEntryType, PerformanceDetailType, NodeIdType, Reverb*Mode)
//   src/Ryujinx.Audio/Renderer/Server/Types/ (AudioRendererExecutionMode, AudioRendererRenderingDevice, PlayState)
//   src/Ryujinx.Audio/Renderer/Dsp/State/ (BiquadFilterState, AdpcmLoopContext: layout only, needed by VoiceState)
//   src/Ryujinx.Audio/Renderer/Server/Effect/UsageState.cs, src/Ryujinx.Audio/Constants.cs, src/Ryujinx.Audio/ResultCode.cs,
//   src/Ryujinx.Audio/Common/SampleFormat.cs
//
// Conventions: C# names kept for types and constants; fields are lower_snake_case; C# Pack = 1 becomes #pragma pack(1).
// C# `_reserved`/`_padding` fields become `reserved*`/`padding`. Explicit C# Size attributes are kept as trailing padding where the
// fields end short of the declared size. Every size is asserted with static_assert; the offsets are checked in
// tests/test_svc_audren_types.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nxrt::audren {

// ---- Constants (Ryujinx.Audio/Constants.cs) ----
inline constexpr uint32_t ChannelCountMax = 6;
inline constexpr uint32_t VoiceChannelCountMax = ChannelCountMax;
inline constexpr uint32_t MixBufferCountMax = 24;
inline constexpr uint32_t VoiceWaveBufferCount = 4;
inline constexpr uint32_t VoiceBiquadFilterCount = 2;
inline constexpr uint32_t MaxErrorInfos = 10;

// ---- ResultCode (Ryujinx.Audio/ResultCode.cs) ----
inline constexpr int32_t ModuleId = 153;
inline constexpr int32_t ErrorCodeShift = 9;

enum class ResultCode : int32_t {
    Success = 0,
    DeviceNotFound = (1 << ErrorCodeShift) | ModuleId,
    OperationFailed = (2 << ErrorCodeShift) | ModuleId,
    UnsupportedSampleRate = (3 << ErrorCodeShift) | ModuleId,
    WorkBufferTooSmall = (4 << ErrorCodeShift) | ModuleId,
    BufferRingFull = (8 << ErrorCodeShift) | ModuleId,
    UnsupportedChannelConfiguration = (10 << ErrorCodeShift) | ModuleId,
    InvalidUpdateInfo = (41 << ErrorCodeShift) | ModuleId,
    InvalidAddressInfo = (42 << ErrorCodeShift) | ModuleId,
    InvalidMixSorting = (43 << ErrorCodeShift) | ModuleId,
    UnsupportedOperation = (513 << ErrorCodeShift) | ModuleId,
    InvalidExecutionContextOperation = (514 << ErrorCodeShift) | ModuleId,
};

// ---- Enums (underlying types as in the C# declarations) ----
enum class SampleFormat : uint8_t {  // Ryujinx.Audio/Common/SampleFormat.cs
    Invalid = 0,
    PcmInt8 = 1,
    PcmInt16 = 2,
    PcmInt24 = 3,
    PcmInt32 = 4,
    PcmFloat = 5,
    Adpcm = 6,
};
enum class PlayState : uint8_t {  // Renderer/Common/PlayState.cs (the voice-parameter one; the Server one is below)
    Start,
    Stop,
    Pause,
};
enum class EffectType : uint8_t {
    Invalid,
    BufferMix,
    AuxiliaryBuffer,
    Delay,
    Reverb,
    Reverb3d,
    BiquadFilter,
    Limiter,
    CaptureBuffer,
    Compressor,
};
enum class SinkType : uint8_t {
    Invalid,
    Device,
    CircularBuffer,
};
enum class MemoryPoolUserState : uint32_t {
    Invalid = 0,
    New = 1,
    RequestDetach = 2,
    Detached = 3,
    RequestAttach = 4,
    Attached = 5,
    Released = 6,
};
enum class PerformanceEntryType : uint8_t {
    Invalid,
    Voice,
    SubMix,
    FinalMix,
    Sink,
};
enum class PerformanceDetailType : uint8_t {
    Unknown,
    PcmInt16,
    Adpcm,
    VolumeRamp,
    BiquadFilter,
    Mix,
    Delay,
    Aux,
    Reverb,
    Reverb3d,
    PcmFloat,
    Limiter,
    CaptureBuffer,
    Compressor,
};
enum class NodeIdType : uint8_t {
    Invalid = 0,
    Voice = 1,
    Mix = 2,
    Sink = 3,
    Performance = 15,
};
enum class ReverbEarlyMode : uint32_t {
    Room,
    Chamber,
    Hall,
    Cathedral,
    Disabled,
};
enum class ReverbLateMode : uint32_t {
    Room,
    Hall,
    Plate,
    Cathedral,
    NoDelay,
    Limit = NoDelay,
};
enum class EffectState : uint8_t {  // Renderer/Parameter/EffectState.cs
    Enabled = 3,
    Disabled = 4,
};
enum class UsageState : uint8_t {  // Renderer/Server/Effect/UsageState.cs
    Invalid,
    New,
    Enabled,
    Disabled,
};
enum class SampleRateConversionQuality : uint8_t {  // VoiceInParameter1.cs (namespace level in the C# source)
    Default,
    High,
    Low,
};
// [Flags] in C#
enum class DecodingBehaviour : uint16_t {
    Default = 0,
    PlayedSampleCountResetWhenLooping = 1,
    SkipPitchAndSampleRateConversion = 2,
};

// Renderer/Server/Types (namespace Ryujinx.Audio.Renderer.Server.Types). Kept in a sub-namespace because its PlayState
// clashes with the Common one; the C# enum has no underlying type, so it is int.
namespace server {
enum class PlayState : int32_t {
    Started,
    Stopped,
    Stopping,
    Paused,
};
enum class AudioRendererExecutionMode : uint8_t {
    Auto,
    Manual,
};
enum class AudioRendererRenderingDevice : uint8_t {
    Dsp,
    Cpu,
};
}  // namespace server

#pragma pack(push, 1)

// ---- Renderer/Common ----

// Renderer/Common/AuxiliaryBufferAddresses.cs. No C# size: 4 x u64 = 0x20.
struct AuxiliaryBufferAddresses {
    uint64_t send_buffer_info;
    uint64_t send_buffer_info_base;
    uint64_t return_buffer_info;
    uint64_t return_buffer_info_base;
};
static_assert(sizeof(AuxiliaryBufferAddresses) == 0x20);

// Renderer/Common/BehaviourParameter.cs. No C# size: 0x10 (computed).
struct BehaviourParameter {
    int32_t user_revision;
    uint32_t padding;
    uint64_t flags;

    // Nested ErrorInfo in C#: 0x10 (computed).
    struct ErrorInfo {
        ResultCode error_code;
        uint32_t padding;
        uint64_t extra_error_info;
    };
};
static_assert(sizeof(BehaviourParameter) == 0x10);
static_assert(sizeof(BehaviourParameter::ErrorInfo) == 0x10);

// Renderer/Common/UpdateDataHeader.cs. No C# size: 0x40 (computed, Unsafe.SizeOf is what the C# uses).
struct UpdateDataHeader {
    int32_t revision;
    uint32_t behaviour_size;
    uint32_t memory_pools_size;
    uint32_t voices_size;
    uint32_t voice_resources_size;
    uint32_t effects_size;
    uint32_t mixes_size;
    uint32_t sinks_size;
    uint32_t performance_buffer_size;
    uint32_t splitter_size;
    uint32_t render_info_size;
    int32_t reserved[4];
    uint32_t total_size;
};
static_assert(sizeof(UpdateDataHeader) == 0x40);

// Renderer/Common/WaveBuffer.cs. No C# size: 0x30 (computed).
struct WaveBuffer {
    uint64_t buffer;
    uint64_t context;
    uint32_t buffer_size;
    uint32_t context_size;
    uint32_t start_sample_offset;
    uint32_t end_sample_offset;
    uint32_t loop_start_sample_offset;
    uint32_t loop_end_sample_offset;
    int32_t loop_count;
    bool looping;
    bool is_end_of_stream;
    uint16_t padding;
};
static_assert(sizeof(WaveBuffer) == 0x30);

// Renderer/Dsp/State/BiquadFilterState.cs (C# Size = 0x20).
struct BiquadFilterState {
    float state0;
    float state1;
    float state2;
    float state3;
    float state4;
    float state5;
    float state6;
    float state7;
};
static_assert(sizeof(BiquadFilterState) == 0x20);

// Renderer/Dsp/State/AdpcmLoopContext.cs (C# Size = 6).
struct AdpcmLoopContext {
    int16_t pred_scale;
    int16_t history0;
    int16_t history1;
};
static_assert(sizeof(AdpcmLoopContext) == 6);

// Renderer/Common/VoiceState.cs: C# Pack = Align (0x10), so natural alignment applies: 2 bytes of padding before
// last_samples (float, 4-byte aligned), total 0xD8 (computed; no C# size stated). Struct-level Align / BiquadStateOffset kept.
struct VoiceState {
    static constexpr uint32_t Align = 0x10;
    static constexpr uint32_t BiquadStateOffset = 0x0;

    BiquadFilterState biquad_filter_state[2];
    uint64_t played_sample_count;
    int32_t offset;
    uint32_t wave_buffer_index;
    bool is_wave_buffer_valid[VoiceWaveBufferCount];  // C# Size = 1 * VoiceWaveBufferCount
    uint32_t wave_buffer_consumed;
    int16_t pitch[8];
    float fraction;
    AdpcmLoopContext loop_context;
    uint8_t padding[2];  // alignment gap before last_samples in the natural layout
    float last_samples[24];
    int32_t loop_count;
};
static_assert(sizeof(VoiceState) == 0xD8);

// ---- Renderer/Parameter ----

// Renderer/Parameter/BiquadFilterParameter1.cs (C# Size = 0xC).
struct BiquadFilterParameter1 {
    bool enable;
    uint8_t reserved;
    int16_t numerator[3];
    int16_t denominator[2];
};
static_assert(sizeof(BiquadFilterParameter1) == 0xC);

// Renderer/Parameter/BiquadFilterParameter2.cs (C# Size = 0x18).
struct BiquadFilterParameter2 {
    bool enable;
    uint8_t reserved1;
    uint8_t reserved2;
    uint8_t reserved3;
    float numerator[3];
    float denominator[2];
};
static_assert(sizeof(BiquadFilterParameter2) == 0x18);

// Renderer/Parameter/AudioRendererConfiguration.cs. No C# size: 0x34 (computed).
struct AudioRendererConfiguration {
    uint32_t sample_rate;
    uint32_t sample_count;
    uint32_t mix_buffer_count;
    uint32_t sub_mix_buffer_count;
    uint32_t voice_count;
    uint32_t sink_count;
    uint32_t effect_count;
    uint32_t performance_metric_frames_count;
    bool voice_drop_enabled;
    uint8_t reserved;
    server::AudioRendererRenderingDevice rendering_device;
    server::AudioRendererExecutionMode execution_mode;
    uint32_t splitter_count;
    uint32_t splitter_destination_count;
    uint32_t external_context_size;
    int32_t revision;
};
static_assert(sizeof(AudioRendererConfiguration) == 0x34);

// Renderer/Parameter/BehaviourErrorInfoOutStatus.cs. No C# size: 0xB0 (computed).
struct BehaviourErrorInfoOutStatus {
    BehaviourParameter::ErrorInfo error_infos[MaxErrorInfos];
    uint32_t error_infos_count;
    uint32_t reserved[3];
};
static_assert(sizeof(BehaviourErrorInfoOutStatus) == 0xB0);

// Renderer/Parameter/EffectResultState.cs (C# Size = 0x80 on the nested specific-data struct).
struct EffectResultState {
    uint8_t specific_data[0x80];
};
static_assert(sizeof(EffectResultState) == 0x80);

// Renderer/Parameter/EffectInParameterVersion1.cs. Specific data is a nested C# struct of Size 0xA0. Total 0xC0 (computed).
struct EffectInParameterVersion1 {
    EffectType type;
    bool is_new;
    bool is_enabled;
    uint8_t reserved1;
    int32_t mix_id;
    uint64_t buffer_base;
    uint64_t buffer_size;
    uint32_t processing_order;
    uint32_t reserved2;
    uint8_t specific_data[0xA0];
};
static_assert(sizeof(EffectInParameterVersion1) == 0xC0);

// Renderer/Parameter/EffectInParameterVersion2.cs. Same layout as version 1.
struct EffectInParameterVersion2 {
    EffectType type;
    bool is_new;
    bool is_enabled;
    uint8_t reserved1;
    int32_t mix_id;
    uint64_t buffer_base;
    uint64_t buffer_size;
    uint32_t processing_order;
    uint32_t reserved2;
    uint8_t specific_data[0xA0];
};
static_assert(sizeof(EffectInParameterVersion2) == 0xC0);

// Renderer/Parameter/EffectOutStatusVersion1.cs. No C# size: 0x10 (computed).
struct EffectOutStatusVersion1 {
    EffectState state;
    uint8_t reserved[15];
};
static_assert(sizeof(EffectOutStatusVersion1) == 0x10);

// Renderer/Parameter/EffectOutStatusVersion2.cs. No C# size: 0x90 (computed).
struct EffectOutStatusVersion2 {
    EffectState state;
    uint8_t reserved[15];
    EffectResultState result_state;
};
static_assert(sizeof(EffectOutStatusVersion2) == 0x90);

// Renderer/Parameter/MemoryPoolInParameter.cs. No C# size: 0x20 (computed).
struct MemoryPoolInParameter {
    uint64_t cpu_address;
    uint64_t size;
    MemoryPoolUserState state;
    uint32_t reserved[3];
};
static_assert(sizeof(MemoryPoolInParameter) == 0x20);

// Renderer/Parameter/MemoryPoolOutStatus.cs. No C# size: 0x10 (computed).
struct MemoryPoolOutStatus {
    MemoryPoolUserState state;
    uint32_t reserved[3];
};
static_assert(sizeof(MemoryPoolOutStatus) == 0x10);

// Renderer/Parameter/MixInParameterDirtyOnlyUpdate.cs. No C# size: 0x20 (computed).
struct MixInParameterDirtyOnlyUpdate {
    uint32_t magic;
    uint32_t mix_count;
    uint8_t reserved[24];
};
static_assert(sizeof(MixInParameterDirtyOnlyUpdate) == 0x20);

// Renderer/Parameter/MixParameter.cs. Nested MixVolumeArray has C# Size 4 * 24 * 24 = 0x900. No C# size: 0x930 (computed).
struct MixParameter {
    float volume;
    uint32_t sample_rate;
    uint32_t buffer_count;
    bool is_used;
    bool is_dirty;
    uint16_t reserved1;
    int32_t mix_id;
    uint32_t effect_count;
    int32_t node_id;
    uint64_t reserved2;
    float mix_buffer_volume[MixBufferCountMax * MixBufferCountMax];  // C# MixVolumeArray, Size 0x900
    int32_t destination_mix_id;
    uint32_t destination_splitter_id;
    uint32_t reserved3;
};
static_assert(sizeof(MixParameter) == 0x930);

// Renderer/Parameter/RendererInfoOutStatus.cs. No C# size: 0x10 (computed).
struct RendererInfoOutStatus {
    uint64_t elapsed_frame_count;
    uint64_t reserved;
};
static_assert(sizeof(RendererInfoOutStatus) == 0x10);

// Renderer/Parameter/SinkInParameter.cs. Nested specific-data struct has C# Size 0x120. Total 0x140 (computed).
struct SinkInParameter {
    SinkType type;
    bool is_used;
    uint16_t magic;  // C# _magic, 0xCAFE
    int32_t node_id;
    uint64_t reserved2[3];
    uint8_t specific_data[0x120];
};
static_assert(sizeof(SinkInParameter) == 0x140);

// Renderer/Parameter/SinkOutStatus.cs. No C# size: 0x20 (computed).
struct SinkOutStatus {
    uint32_t last_written_offset;
    uint32_t padding;
    uint64_t reserved[3];
};
static_assert(sizeof(SinkOutStatus) == 0x20);

// Renderer/Parameter/SplitterDestinationInParameterVersion1.cs. Nested MixArray has C# Size sizeof(float) * 24 = 0x60.
// No C# size on the outer struct: 0x70 (computed).
struct SplitterDestinationInParameterVersion1 {
    uint32_t magic;
    int32_t id;
    float mix_buffer_volume[MixBufferCountMax];
    int32_t destination_id;
    bool is_used;
    bool reset_prev_volume;
    uint8_t reserved[2];

    static constexpr uint32_t ValidMagic = 0x44444E53;
    bool IsMagicValid() const { return magic == ValidMagic; }
};
static_assert(sizeof(SplitterDestinationInParameterVersion1) == 0x70);

// Renderer/Parameter/SplitterDestinationInParameterVersion2a.cs. No C# size: 0x90 (computed).
struct SplitterDestinationInParameterVersion2a {
    uint32_t magic;
    int32_t id;
    float mix_buffer_volume[MixBufferCountMax];
    int32_t destination_id;
    BiquadFilterParameter1 biquad_filters[2];
    bool is_used;
    bool reset_prev_volume;
    uint8_t reserved[10];

    static constexpr uint32_t ValidMagic = 0x44444E53;
    bool IsMagicValid() const { return magic == ValidMagic; }
};
static_assert(sizeof(SplitterDestinationInParameterVersion2a) == 0x90);

// Renderer/Parameter/SplitterDestinationInParameterVersion2b.cs. No C# size: 0xA8 (computed).
struct SplitterDestinationInParameterVersion2b {
    uint32_t magic;
    int32_t id;
    float mix_buffer_volume[MixBufferCountMax];
    int32_t destination_id;
    BiquadFilterParameter2 biquad_filters[2];
    bool is_used;
    bool reset_prev_volume;
    uint8_t reserved[10];

    static constexpr uint32_t ValidMagic = 0x44444E53;
    bool IsMagicValid() const { return magic == ValidMagic; }
};
static_assert(sizeof(SplitterDestinationInParameterVersion2b) == 0xA8);

// Renderer/Parameter/SplitterInParameter.cs. No C# size: 0x10 (computed).
struct SplitterInParameter {
    uint32_t magic;
    int32_t id;
    uint32_t sample_rate;
    int32_t destination_count;

    static constexpr uint32_t ValidMagic = 0x49444E53;
    bool IsMagicValid() const { return magic == ValidMagic; }
};
static_assert(sizeof(SplitterInParameter) == 0x10);

// Renderer/Parameter/SplitterInParameterHeader.cs. No C# size: 0x20 (computed).
struct SplitterInParameterHeader {
    uint32_t magic;
    uint32_t splitter_count;
    uint32_t splitter_destination_count;
    uint32_t reserved[5];

    static constexpr uint32_t ValidMagic = 0x48444E53;
    bool IsMagicValid() const { return magic == ValidMagic; }
};
static_assert(sizeof(SplitterInParameterHeader) == 0x20);

// Renderer/Parameter/VoiceChannelResourceInParameter.cs. C# Size = 0x70; fields end at 0x65, so 11 bytes of padding.
struct VoiceChannelResourceInParameter {
    uint32_t id;
    float mix[24];
    bool is_used;
    uint8_t padding[11];
};
static_assert(sizeof(VoiceChannelResourceInParameter) == 0x70);

// Renderer/Parameter/VoiceInParameter1.cs (WaveBufferInternal, DecodingBehaviour and SampleRateConversionQuality are
// namespace-level in C#). C# Size = 0x170; fields end at 0x16F, so 1 byte of padding.
struct WaveBufferInternal {  // C# Size = 0x38
    uint64_t address;
    uint64_t size;
    uint32_t start_sample_offset;
    uint32_t end_sample_offset;
    bool should_loop;
    bool is_end_of_stream;
    bool sent_to_server;
    uint8_t reserved;
    int32_t loop_count;
    uint64_t context_address;
    uint64_t context_size;
    uint32_t loop_first_sample_offset;
    uint32_t loop_last_sample_offset;
};
static_assert(sizeof(WaveBufferInternal) == 0x38);

struct VoiceInParameter1 {
    int32_t id;
    int32_t node_id;
    bool is_new;
    bool in_use;
    PlayState play_state;
    SampleFormat sample_format;
    uint32_t sample_rate;
    uint32_t priority;
    uint32_t sorting_order;
    uint32_t channel_count;
    float pitch;
    float volume;
    BiquadFilterParameter1 biquad_filters[2];
    uint32_t wave_buffers_count;
    uint32_t wave_buffers_index;
    uint32_t reserved1;
    uint64_t data_source_state_address;
    uint64_t data_source_state_size;
    int32_t mix_id;
    uint32_t splitter_id;
    WaveBufferInternal wave_buffers[VoiceWaveBufferCount];
    int32_t channel_resource_ids[VoiceChannelCountMax];
    bool reset_voice_drop_flag;
    uint8_t flush_wave_buffer_count;
    uint16_t reserved2;
    DecodingBehaviour decoding_behaviour_flags;
    SampleRateConversionQuality src_quality;
    uint32_t external_context;
    uint32_t external_context_size;
    uint32_t reserved3[2];
    uint8_t padding;  // C# Size = 0x170
};
static_assert(sizeof(VoiceInParameter1) == 0x170);

// Renderer/Parameter/VoiceInParameter2.cs. C# Size = 0x188; fields end at 0x187, so 1 byte of padding.
struct VoiceInParameter2 {
    int32_t id;
    int32_t node_id;
    bool is_new;
    bool in_use;
    PlayState play_state;
    SampleFormat sample_format;
    uint32_t sample_rate;
    uint32_t priority;
    uint32_t sorting_order;
    uint32_t channel_count;
    float pitch;
    float volume;
    BiquadFilterParameter2 biquad_filters[2];
    uint32_t wave_buffers_count;
    uint32_t wave_buffers_index;
    uint32_t reserved1;
    uint64_t data_source_state_address;
    uint64_t data_source_state_size;
    int32_t mix_id;
    uint32_t splitter_id;
    WaveBufferInternal wave_buffers[VoiceWaveBufferCount];
    int32_t channel_resource_ids[VoiceChannelCountMax];
    bool reset_voice_drop_flag;
    uint8_t flush_wave_buffer_count;
    uint16_t reserved2;
    DecodingBehaviour decoding_behaviour_flags;
    SampleRateConversionQuality src_quality;
    uint32_t external_context;
    uint32_t external_context_size;
    uint32_t reserved3[2];
    uint8_t padding;  // C# Size = 0x188
};
static_assert(sizeof(VoiceInParameter2) == 0x188);

// Renderer/Parameter/VoiceOutStatus.cs. No C# size: 0x10 (computed).
struct VoiceOutStatus {
    uint64_t played_sample_count;
    uint32_t played_wave_buffers_count;
    bool voice_drop_flag;
    uint8_t reserved[3];
};
static_assert(sizeof(VoiceOutStatus) == 0x10);

// Renderer/Parameter/PerformanceInParameter.cs (Performance/). No C# size: 0x10 (computed).
struct PerformanceInParameter {
    int32_t target_node_id;
    uint32_t reserved[3];
};
static_assert(sizeof(PerformanceInParameter) == 0x10);

// Renderer/Parameter/PerformanceOutStatus.cs (Performance/). No C# size: 0x10 (computed).
struct PerformanceOutStatus {
    uint32_t history_size;
    uint32_t reserved[3];
};
static_assert(sizeof(PerformanceOutStatus) == 0x10);

// ---- Renderer/Parameter/Effect ----

// Effect/AuxiliaryBufferParameter.cs. No C# size: 0x6C (computed).
struct AuxiliaryBufferParameter {
    uint8_t input[24];
    uint8_t output[24];
    uint32_t channel_count;
    uint32_t sample_rate;
    uint32_t buffer_storage_size;
    uint32_t channel_count_max;
    uint64_t send_buffer_info_address;
    uint64_t send_buffer_storage_address;
    uint64_t return_buffer_info_address;
    uint64_t return_buffer_storage_address;
    uint32_t mix_buffer_sample_size;
    uint32_t total_sample_count;
    uint32_t mix_buffer_sample_count;
};
static_assert(sizeof(AuxiliaryBufferParameter) == 0x6C);

// Effect/BiquadFilterEffectParameter1.cs. No C# size: 0x18 (computed).
struct BiquadFilterEffectParameter1 {
    uint8_t input[6];
    uint8_t output[6];
    int16_t numerator[3];
    int16_t denominator[2];
    uint8_t channel_count;
    UsageState status;
};
static_assert(sizeof(BiquadFilterEffectParameter1) == 0x18);

// Effect/BiquadFilterEffectParameter2.cs. No C# size: 0x28 (computed).
struct BiquadFilterEffectParameter2 {
    uint8_t input[6];
    uint8_t output[6];
    uint32_t padding;
    float numerator[3];
    float denominator[2];
    uint8_t channel_count;
    UsageState status;
    uint16_t reserved;
};
static_assert(sizeof(BiquadFilterEffectParameter2) == 0x28);

// Effect/BufferMixerParameter.cs (C# type name BufferMixParameter). No C# size: 0x94 (computed).
struct BufferMixParameter {
    uint8_t input[24];
    uint8_t output[24];
    float volumes[24];
    uint32_t mixes_count;
};
static_assert(sizeof(BufferMixParameter) == 0x94);

// Effect/CompressorParameter.cs. No C# size: 0x38 (computed).
struct CompressorParameter {
    uint8_t input[6];
    uint8_t output[6];
    uint16_t channel_count_max;
    uint16_t channel_count;
    int32_t sample_rate;
    float threshold;
    float ratio;
    int32_t attack_time;
    int32_t release_time;
    float input_gain;
    float attack_coefficient;
    float release_coefficient;
    float output_gain;
    UsageState status;
    bool makeup_gain_enabled;
    bool statistics_enabled;
    bool statistics_reset;
};
static_assert(sizeof(CompressorParameter) == 0x38);

// Effect/CompressorStatistics.cs. No C# size: 0x20 (computed).
struct CompressorStatistics {
    float maximum_mean;
    float minimum_gain;
    float last_samples[6];
};
static_assert(sizeof(CompressorStatistics) == 0x20);

// Effect/DelayParameter.cs. No C# size: 0x35 (computed).
struct DelayParameter {
    uint8_t input[6];
    uint8_t output[6];
    uint16_t channel_count_max;
    uint16_t channel_count;
    uint32_t delay_time_max;
    uint32_t delay_time;
    uint32_t sample_rate;
    uint32_t in_gain;
    uint32_t feedback_gain;
    uint32_t out_gain;
    uint32_t dry_gain;
    uint32_t channel_spread;
    uint32_t low_pass_amount;
    UsageState status;
};
static_assert(sizeof(DelayParameter) == 0x35);

// Effect/LimiterParameter.cs. No C# size: 0x44 (computed).
struct LimiterParameter {
    uint8_t input[6];
    uint8_t output[6];
    uint16_t channel_count_max;
    uint16_t channel_count;
    int32_t sample_rate;
    int32_t look_ahead_time_max;
    int32_t attack_time;
    int32_t release_time;
    int32_t look_ahead_time;
    float attack_coefficient;
    float release_coefficient;
    float threshold;
    float input_gain;
    float output_gain;
    int32_t delay_buffer_sample_count_min;
    int32_t delay_buffer_sample_count_max;
    UsageState status;
    bool statistics_enabled;
    bool statistics_reset;
    uint8_t reserved;
};
static_assert(sizeof(LimiterParameter) == 0x44);

// Effect/LimiterStatistics.cs. No C# size: 0x30 (computed).
struct LimiterStatistics {
    float input_max[6];
    float compression_gain_min[6];
};
static_assert(sizeof(LimiterStatistics) == 0x30);

// Effect/Reverb3dParameter.cs. No C# size: 0x49 (computed).
struct Reverb3dParameter {
    uint8_t input[6];
    uint8_t output[6];
    uint16_t channel_count_max;
    uint16_t channel_count;
    uint32_t reserved;
    uint32_t sample_rate;
    float room_hf;
    float hf_reference;
    float decay_time;
    float hf_decay_ratio;
    float room_gain;
    float reflections_gain;
    float reverb_gain;
    float diffusion;
    float reflection_delay;
    float reverb_delay_time;
    float density;
    float dry_gain;
    UsageState parameter_status;
};
static_assert(sizeof(Reverb3dParameter) == 0x49);

// Effect/ReverbParameter.cs. No C# size: 0x41 (computed).
struct ReverbParameter {
    uint8_t input[6];
    uint8_t output[6];
    uint16_t channel_count_max;
    uint16_t channel_count;
    int32_t sample_rate;
    ReverbEarlyMode early_mode;
    int32_t early_gain;
    int32_t pre_delay_time;
    ReverbLateMode late_mode;
    int32_t late_gain;
    int32_t decay_time;
    int32_t high_frequency_decay_ratio;
    int32_t coloration;
    int32_t reverb_gain;
    int32_t out_gain;
    int32_t dry_gain;
    UsageState status;
};
static_assert(sizeof(ReverbParameter) == 0x41);

// ---- Renderer/Parameter/Sink ----

// Sink/CircularBufferParameter.cs. No C# size: 0x24 (computed).
struct CircularBufferParameter {
    uint64_t buffer_address;
    uint32_t buffer_size;
    uint32_t input_count;
    uint32_t sample_count;
    uint32_t last_read_offset;
    SampleFormat sample_format;
    uint8_t reserved1[3];
    uint8_t input[6];
    uint16_t reserved2;
};
static_assert(sizeof(CircularBufferParameter) == 0x24);

// Sink/DeviceParameter.cs. Nested DeviceNameStruct has C# Size 0xFF. No C# size on the outer struct: 0x11C (computed).
struct DeviceParameter {
    uint8_t device_name[0xFF];
    uint8_t padding;
    uint32_t input_count;
    uint8_t input[6];
    uint8_t reserved;
    bool down_mix_parameter_enabled;
    float down_mix_parameter[4];
};
static_assert(sizeof(DeviceParameter) == 0x11C);

#pragma pack(pop)

}  // namespace nxrt::audren
