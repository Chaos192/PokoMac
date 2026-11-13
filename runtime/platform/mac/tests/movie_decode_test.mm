// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright (c) 2026 Chaos192
// Standalone check of the movie:: AVFoundation backend (runtime/platform/mac/core/movie_avf.mm) without the game: the file is compiled into
// this program as is, the runtime functions it calls are stubbed below (logging to stdout, config defaults, a handler table, dispatch_call
// as a host call), and a "game" drives the 41 handlers by their mangled names the way the game's player does: SetDataSource through a
// ClientStreamReader (here a host stand-in that preads the file in place), GetTrackConfiguration, SelectTrack (+ preroll), two Decoders,
// then GetTrackIndexForAvailableData / Read / GetSampleTime / SendInputBufferForDecode / Advance and AcquireOutputBufferIndex /
// GetOutputBuffer / ReleaseOutputBufferIndex until the end of stream, a SeekTo(0) loop restart and a SeekTo into the middle.
// Not part of the product build.
//
// Build and run on macOS from the repository root:
//   clang++ -std=c++20 -fobjc-arc -O1 -Iruntime/include -Irecomp/include runtime/platform/mac/tests/movie_decode_test.mm \
//       -framework AVFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox -framework Foundation -o movie_decode_test
//   ./movie_decode_test [file[:container] ...]
// With no argument it writes four H.264 + AAC MP4s to $TMPDIR with AVAssetWriter (1600x900, 30 fps, 48 kHz stereo; plain, edit lists that cut
// the first 0.5 s, an empty edit that delays the video by 0.5 s, one that delays the audio by 0.25 s), plays them and checks the frame/sample
// counts, the timestamps and the decoded NV12 / PCM contents against what was encoded, then deletes them. Every file named is played the same
// way (container 1 mp4, 2 mkv, 3 webm; default from the extension) and only reported on.
#include "../core/movie_avf.mm"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdarg>

// ---- the runtime functions movie_avf.mm calls ----------------------------------------------------------------------------------
namespace {
std::map<std::string, nxrt::HleFn> g_handlers;
std::atomic<int> g_warnings{0};
}
namespace nxrt {
void logv(LogLevel l, const char* tag, const char* fmt, va_list ap) {
    static std::mutex mu;
    static const auto t0 = std::chrono::steady_clock::now();
    if (l <= LogLevel::Warn) g_warnings++;
    char msg[1024];
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    std::lock_guard<std::mutex> lk(mu);
    std::printf("    [%8.3f %c %s] %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), "EWIDT"[static_cast<int>(l)], tag, msg);
}
void logf(LogLevel l, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv(l, tag, fmt, ap);
    va_end(ap);
}
bool log_enabled(LogLevel l) { return l <= LogLevel::Debug; }
void log_set_thread_name(const char*) {}
std::string config_get_str(const char*, const char*, const char* def) { return def ? def : ""; }
bool config_get_bool(const char*, const char*, bool def) { return def; }
uint64_t hle_register(const char* name, HleFn fn) { g_handlers[name] = fn; return 0; }
void module_override_symbol(const char*) {}
void dispatch_call(::Ctx* c, uint64_t addr) { reinterpret_cast<GuestFn>(addr)(c); }   // guest functions here are host functions
void gauge_register(const char*, std::function<int64_t()>) {}
void gauge_unregister(const char*) {}
GaugeCounter::GaugeCounter(const char* name) : name_(name) {}
GaugeCounter::~GaugeCounter() {}
}  // namespace nxrt

namespace {
using nxrt::media::ST_END;
using nxrt::media::ST_NO_INPUT;
using nxrt::media::ST_OK;

// ---- calling the handlers like the game ------------------------------------------------------------------------------------------
::Ctx g_ctx;
uint64_t call(const char* name, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0) {
    auto it = g_handlers.find(name);
    if (it == g_handlers.end()) { std::printf("FATAL: no handler for %s\n", name); std::exit(2); }
    std::memset(&g_ctx, 0, sizeof g_ctx);
    g_ctx.x[0] = a0; g_ctx.x[1] = a1; g_ctx.x[2] = a2; g_ctx.x[3] = a3; g_ctx.x[4] = a4; g_ctx.x[5] = a5;
    it->second(&g_ctx);
    return g_ctx.x[0];
}
template <class T> uint64_t P(T* p) { return reinterpret_cast<uint64_t>(p); }

const char* const EXT_CTOR = "_ZN5movie9ExtractorC1ENS_13ContainerTypeENS_9CacheSizeE";
const char* const EXT_DTOR = "_ZN5movie9ExtractorD1Ev";
const char* const EXT_SOURCE = "_ZN5movie9Extractor13SetDataSourceEPNS_18ClientStreamReaderEPKc";
const char* const EXT_COUNT = "_ZN5movie9Extractor13GetTrackCountEPi";
const char* const EXT_CONFIG = "_ZN5movie9Extractor21GetTrackConfigurationEiPNS_9MediaDataE";
const char* const EXT_SELECT = "_ZN5movie9Extractor11SelectTrackEi";
const char* const EXT_FOR_DATA = "_ZN5movie9Extractor29GetTrackIndexForAvailableDataEPm";
const char* const EXT_TIME = "_ZN5movie9Extractor13GetSampleTimeEPl";
const char* const EXT_READ = "_ZN5movie9Extractor4ReadEPNS_6BufferE";
const char* const EXT_ADVANCE = "_ZN5movie9Extractor7AdvanceEv";
const char* const EXT_SEEK = "_ZN5movie9Extractor6SeekToEl";
const char* const DEC_CTOR = "_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi";
const char* const DEC_DTOR = "_ZN5movie7DecoderD1Ev";
const char* const DEC_CONFIGURE = "_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE";
const char* const DEC_OUT_SIZE = "_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_";
const char* const DEC_START = "_ZN5movie7Decoder5StartEv";
const char* const DEC_STOP = "_ZN5movie7Decoder4StopEv";
const char* const DEC_ACQ_IN = "_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_";
const char* const DEC_GET_IN = "_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE";
const char* const DEC_SEND = "_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj";
const char* const DEC_ACQ_OUT = "_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_";
const char* const DEC_GET_OUT = "_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE";
const char* const DEC_REL_OUT = "_ZN5movie7Decoder24ReleaseOutputBufferIndexEi";
const char* const MD_CTOR = "_ZN5movie9MediaDataC1Ev";
const char* const MD_DTOR = "_ZN5movie9MediaDataD1Ev";
const char* const MD_I32 = "_ZNK5movie9MediaData9FindInt32EPKcPi";
const char* const MD_I64 = "_ZNK5movie9MediaData9FindInt64EPKcPl";
const char* const MD_STR = "_ZNK5movie9MediaData10FindStringEPKcPS2_";
const char* const BUF_CTOR = "_ZN5movie6BufferC1Ev";
const char* const BUF_CTOR_MEM = "_ZN5movie6BufferC1EPvm";
const char* const BUF_SIZE = "_ZNK5movie6Buffer4SizeEv";

int32_t md_i32(uint64_t* md, const char* k, int32_t def = -1) { int32_t v = def; return call(MD_I32, P(md), P(k), P(&v)) & 1 ? v : def; }
int64_t md_i64(uint64_t* md, const char* k) { int64_t v = -1; return call(MD_I64, P(md), P(k), P(&v)) & 1 ? v : -1; }
std::string md_str(uint64_t* md, const char* k) { const char* s = nullptr; return (call(MD_STR, P(md), P(k), P(&s)) & 1) && s ? s : "?"; }

// ---- the game's ClientStreamReader, on the host: vtable +0x10 Open, +0x18 ReadAt, +0x20 Close, +0x28 GetSize ----------------------
struct StreamReader { uint64_t vt; };
void sr_open(::Ctx* c) {      // (reader, path, void** handle) -> bool
    const int fd = ::open(reinterpret_cast<const char*>(c->x[1]), O_RDONLY);
    const uint64_t handle = static_cast<uint64_t>(fd) + 1;   // never 0
    if (fd >= 0) std::memcpy(reinterpret_cast<void*>(c->x[2]), &handle, 8);
    c->x[0] = fd >= 0;
}
void sr_read(::Ctx* c) {      // (reader, handle, offset, buf, n) -> bytes
    const ssize_t n = ::pread(static_cast<int>(c->x[1] - 1), reinterpret_cast<void*>(c->x[3]), c->x[4], static_cast<off_t>(c->x[2]));
    c->x[0] = static_cast<uint64_t>(n < 0 ? 0 : n);
}
void sr_close(::Ctx* c) { ::close(static_cast<int>(c->x[1] - 1)); c->x[0] = 0; }
void sr_size(::Ctx* c) {      // (reader, handle, int64*)
    struct stat st {};
    ::fstat(static_cast<int>(c->x[1] - 1), &st);
    const int64_t sz = st.st_size;
    std::memcpy(reinterpret_cast<void*>(c->x[2]), &sz, 8);
    c->x[0] = 0;
}
uint64_t g_sr_vt[6] = {0, 0, P(reinterpret_cast<void*>(&sr_open)), P(reinterpret_cast<void*>(&sr_read)), P(reinterpret_cast<void*>(&sr_close)),
                       P(reinterpret_cast<void*>(&sr_size))};

// ---- synthetic movies --------------------------------------------------------------------------------------------------------------
constexpr int kW = 1600, kH = 900, kFps = 30, kRate = 48000;
constexpr double kSecs = 2.0;
constexpr int kU = 64, kV = 192;
inline int luma_of_row(int r) { return 16 + r * 219 / kH; }
inline int16_t pcm_at(int64_t i) { return static_cast<int16_t>(8000 * std::sin(2 * M_PI * 440.0 * static_cast<double>(i) / kRate)); }

// H.264 High (B-frames) + AAC LC stereo, written by AVAssetWriter. A session start after 0 cuts the media before it with the edit lists; a
// video (audio) delay starts that track that much after the session start (an empty edit).
bool make_mp4(NSString* path, double session_start, double video_delay = 0, double audio_delay = 0) {
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    NSError* err = nil;
    AVAssetWriter* wr = [AVAssetWriter assetWriterWithURL:[NSURL fileURLWithPath:path] fileType:AVFileTypeMPEG4 error:&err];
    if (!wr) { std::printf("AVAssetWriter: %s\n", err.description.UTF8String); return false; }
    AVAssetWriterInput* vi = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{
        AVVideoCodecKey: AVVideoCodecTypeH264, AVVideoWidthKey: @(kW), AVVideoHeightKey: @(kH),
        AVVideoCompressionPropertiesKey: @{AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel, AVVideoAllowFrameReorderingKey: @YES,
                                           AVVideoAverageBitRateKey: @8000000}}];
    AudioChannelLayout acl{};
    acl.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
    AVAssetWriterInput* ai = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{
        AVFormatIDKey: @(kAudioFormatMPEG4AAC), AVSampleRateKey: @(kRate), AVNumberOfChannelsKey: @2,
        AVChannelLayoutKey: [NSData dataWithBytes:&acl length:sizeof acl], AVEncoderBitRateKey: @192000}];
    vi.expectsMediaDataInRealTime = NO;
    ai.expectsMediaDataInRealTime = NO;
    NSDictionary* pba = @{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
                          (__bridge NSString*)kCVPixelBufferWidthKey: @(kW), (__bridge NSString*)kCVPixelBufferHeightKey: @(kH)};
    AVAssetWriterInputPixelBufferAdaptor* ad = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:vi sourcePixelBufferAttributes:pba];
    [wr addInput:vi];
    [wr addInput:ai];
    if (![wr startWriting]) { std::printf("startWriting: %s\n", wr.error.description.UTF8String); return false; }
    [wr startSessionAtSourceTime:CMTimeMakeWithSeconds(session_start, 90000)];
    AudioStreamBasicDescription asbd{static_cast<Float64>(kRate), kAudioFormatLinearPCM, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, 4, 1, 4, 2, 16, 0};
    CMAudioFormatDescriptionRef afd = nullptr;
    CMAudioFormatDescriptionCreate(nullptr, &asbd, sizeof acl, &acl, 0, nullptr, nullptr, &afd);
    const int frames = static_cast<int>(kSecs * kFps);
    const int64_t atotal = static_cast<int64_t>(kSecs * kRate);
    int f = 0;
    int64_t apos = 0;
    while (f < frames || apos < atotal) {
        bool did = false;
        if (f < frames && vi.readyForMoreMediaData) {
            CVPixelBufferRef pb = nullptr;
            CVPixelBufferPoolCreatePixelBuffer(nullptr, ad.pixelBufferPool, &pb);
            CVPixelBufferLockBaseAddress(pb, 0);
            auto* y = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
            auto* uv = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
            const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
            for (int r = 0; r < kH; r++) std::memset(y + r * ys, luma_of_row(r), kW);
            for (int r = 0; r < kH / 2; r++)
                for (int cx = 0; cx < kW / 2; cx++) { uv[r * uvs + 2 * cx] = kU; uv[r * uvs + 2 * cx + 1] = kV; }
            CVPixelBufferUnlockBaseAddress(pb, 0);
            [ad appendPixelBuffer:pb withPresentationTime:CMTimeAdd(CMTimeMakeWithSeconds(video_delay, 90000), CMTimeMake(f, kFps))];
            CVPixelBufferRelease(pb);
            f++;
            did = true;
        }
        if (apos < atotal && ai.readyForMoreMediaData) {
            const int n = static_cast<int>(std::min<int64_t>(4096, atotal - apos));
            std::vector<int16_t> tmp(static_cast<size_t>(n) * 2);
            for (int i = 0; i < n; i++) tmp[2 * i] = tmp[2 * i + 1] = pcm_at(apos + i);
            CMBlockBufferRef bb = nullptr;
            CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, n * 4, nullptr, nullptr, 0, n * 4, 0, &bb);
            CMBlockBufferReplaceDataBytes(tmp.data(), bb, 0, n * 4);
            CMSampleBufferRef sb = nullptr;
            CMAudioSampleBufferCreateReadyWithPacketDescriptions(nullptr, bb, afd, n, CMTimeAdd(CMTimeMakeWithSeconds(audio_delay, kRate), CMTimeMake(apos, kRate)), nullptr, &sb);
            [ai appendSampleBuffer:sb];
            CFRelease(sb);
            CFRelease(bb);
            apos += n;
            did = true;
        }
        if (!did) usleep(500);
    }
    [vi markAsFinished];
    [ai markAsFinished];
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [wr finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(sem); }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    CFRelease(afd);
    if (wr.status != AVAssetWriterStatusCompleted) { std::printf("finishWriting: %s\n", wr.error.description.UTF8String); return false; }
    return true;
}

// ---- playing a movie like the game -------------------------------------------------------------------------------------------------
struct Expect {
    bool synthetic = false;
    int frames = 0;          // video frames expected
    int64_t samples = 0;     // audio sample frames expected
    double cut = 0;          // seconds the edit list cuts (the content at t=0 is the source's content at `cut`)
    int64_t video_first = 0; // the first video timestamp (an empty edit delays the video)
    int64_t audio_first = 0; // the first audio timestamp
};
int g_failures = 0;
void check(bool ok, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("  %s ", ok ? "ok  " : "FAIL");
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    if (!ok) g_failures++;
}

struct Stream {
    int track = -1;
    alignas(16) uint8_t dec[16]{};
    uint64_t fmt_md = 0;
    std::vector<uint8_t> buf;
    bool video = false, eos_sent = false, eos_seen = false;
    int w = 0, h = 0, rate = 0, ch = 0;
    uint64_t outputs = 0, bytes = 0;
    int64_t first = INT64_MIN, last = INT64_MIN;
    bool monotonic = true;
    size_t frame_bytes = 0;
    // content checks
    int luma_err = 0, chroma_err = 0;
    double pcm_err = 0;
    int64_t pcm_pos = 0;
};

void drain(Stream& s, const Expect& ex) {
    for (;;) {
        int idx = -1, more = 0;
        int64_t ts = 0;
        uint32_t flags = 0;
        if (static_cast<int>(call(DEC_ACQ_OUT, P(s.dec), P(&idx), P(&ts), P(&flags), P(&more))) != ST_OK) return;
        alignas(16) uint8_t ob[0x40];
        call(BUF_CTOR_MEM, P(ob), P(s.buf.data()), s.buf.size());
        call(DEC_GET_OUT, P(s.dec), static_cast<uint64_t>(idx), P(ob), P(&s.fmt_md));
        const uint32_t n = static_cast<uint32_t>(call(BUF_SIZE, P(ob)));
        if (flags & 4) s.eos_seen = true;
        if (n) {
            if (s.first == INT64_MIN) s.first = ts;
            if (s.last != INT64_MIN && ts <= s.last) s.monotonic = false;
            s.last = ts;
            s.outputs++;
            s.bytes += n;
            if (s.video) {
                s.frame_bytes = n;
                if (ex.synthetic && (s.outputs == 1 || s.outputs % 10 == 0)) {
                    const uint8_t* y = s.buf.data();
                    const uint8_t* uv = y + static_cast<size_t>(s.w) * s.h;
                    for (int r : {0, 1, s.h / 3, s.h / 2, s.h - 2, s.h - 1}) {
                        const int got = y[static_cast<size_t>(r) * s.w + s.w / 2], want = luma_of_row(r);
                        s.luma_err = std::max(s.luma_err, std::abs(got - want));
                    }
                    for (int r : {0, (s.h + 1) / 4, (s.h + 1) / 2 - 1}) {
                        const uint8_t* row = uv + static_cast<size_t>(r) * s.w;
                        s.chroma_err = std::max({s.chroma_err, std::abs(row[0] - kU), std::abs(row[1] - kV), std::abs(row[s.w - 2] - kU), std::abs(row[s.w - 1] - kV)});
                    }
                }
            } else if (ex.synthetic && s.ch == 2) {
                // the PCM must continue the encoded sine from the source time of this block (AAC is lossy: compared by RMS of the difference)
                const auto* pcm = reinterpret_cast<const int16_t*>(s.buf.data());
                const int64_t base = s.pcm_pos + static_cast<int64_t>(ex.cut * kRate);
                double se = 0;
                const uint32_t frames = n / 4;
                for (uint32_t i = 0; i < frames; i++) { const double d = pcm[2 * i] - pcm_at(base + i); se += d * d; }
                if (frames && s.pcm_pos > 2048 && s.pcm_pos + frames + 2048 < ex.samples) s.pcm_err = std::max(s.pcm_err, std::sqrt(se / frames));
                s.pcm_pos += frames;
            }
        }
        call(DEC_REL_OUT, P(s.dec), static_cast<uint64_t>(idx));
    }
}

int container_of(const std::string& path) {
    const size_t dot = path.rfind('.');
    const std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    return ext == "mkv" ? 2 : ext == "webm" ? 3 : 1;
}

void play(const std::string& path, int container, const Expect& ex) {
    std::printf("\n== %s (container type %d)\n", path.c_str(), container);
    alignas(16) uint8_t ext[16]{};
    call(EXT_CTOR, P(ext), static_cast<uint64_t>(container), 0);
    StreamReader sr{P(g_sr_vt)};
    const auto t_open = std::chrono::steady_clock::now();
    const int st = static_cast<int>(call(EXT_SOURCE, P(ext), P(&sr), P(path.c_str())));
    std::printf("  SetDataSource -> 0x%x in %.1f ms\n", st, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_open).count());
    if (st != ST_OK) {
        check(!ex.synthetic, "SetDataSource failed (expected only for a container AVFoundation cannot demux)");
        call(EXT_DTOR, P(ext));
        return;
    }
    int count = 0;
    call(EXT_COUNT, P(ext), P(&count));
    Stream vs, as;
    vs.video = true;
    std::vector<uint64_t> mds(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        uint64_t* md = &mds[static_cast<size_t>(i)];
        call(MD_CTOR, P(md));
        call(EXT_CONFIG, P(ext), static_cast<uint64_t>(i), P(md));
        const std::string mime = md_str(md, "mime");
        std::printf("  track %d: mime %s durationUs %lld bit-rate %d", i, mime.c_str(), static_cast<long long>(md_i64(md, "durationUs")), md_i32(md, "bit-rate"));
        if (mime.rfind("video/", 0) == 0) {
            std::printf(" %dx%d frame-rate %d nn-colorspace %d\n", md_i32(md, "width"), md_i32(md, "height"), md_i32(md, "frame-rate"), md_i32(md, "nn-colorspace"));
            if (vs.track < 0) vs.track = i;
        } else if (mime.rfind("audio/", 0) == 0) {
            std::printf(" sample-rate %d channel-count %d\n", md_i32(md, "sample-rate"), md_i32(md, "channel-count"));
            if (as.track < 0) as.track = i;
        } else {
            std::printf("\n");
        }
    }
    // decoders first (as the game: configure from the track configuration), then the selection (its preroll decodes the first samples)
    for (Stream* s : {&vs, &as}) {
        if (s->track < 0) continue;
        uint64_t* md = &mds[static_cast<size_t>(s->track)];
        call(DEC_CTOR, P(s->dec), s->video ? 1 : 4, s->video ? 4 : 0, 0, 0);
        call(DEC_CONFIGURE, P(s->dec), P(md));
        int size = 0, align = 0;
        call(DEC_OUT_SIZE, P(s->dec), P(&size), P(&align));
        s->buf.assign(static_cast<size_t>(size), 0);
        s->w = md_i32(md, "width", 0);
        s->h = md_i32(md, "height", 0);
        s->rate = md_i32(md, "sample-rate", 0);
        s->ch = md_i32(md, "channel-count", 0);
        call(MD_CTOR, P(&s->fmt_md));
        call(DEC_START, P(s->dec));
        std::printf("  %s decoder: output buffer %d bytes (align %d)\n", s->video ? "video" : "audio", size, align);
    }
    const auto t_sel = std::chrono::steady_clock::now();
    for (Stream* s : {&vs, &as})
        if (s->track >= 0) check(static_cast<int>(call(EXT_SELECT, P(ext), static_cast<uint64_t>(s->track))) == ST_OK, "SelectTrack(%d)", s->track);
    std::printf("  selection + preroll %.1f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_sel).count());

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t inputs = 0;
    for (;;) {
        size_t ti = 0;
        const int r = static_cast<int>(call(EXT_FOR_DATA, P(ext), P(&ti)));
        if (r == ST_END) break;
        if (r != ST_OK) { check(false, "GetTrackIndexForAvailableData -> 0x%x", r); break; }
        Stream& s = static_cast<int>(ti) == vs.track ? vs : as;
        int idx = -1, more = 0;
        while (static_cast<int>(call(DEC_ACQ_IN, P(s.dec), P(&idx), P(&more))) == ST_NO_INPUT) drain(s, ex);
        alignas(16) uint8_t ib[0x40];
        call(BUF_CTOR, P(ib));
        call(DEC_GET_IN, P(s.dec), static_cast<uint64_t>(idx), P(ib));
        const int rr = static_cast<int>(call(EXT_READ, P(ext), P(ib)));
        int64_t t = 0;
        call(EXT_TIME, P(ext), P(&t));
        if (rr != ST_OK) { check(false, "Read -> 0x%x", rr); break; }
        call(DEC_SEND, P(s.dec), static_cast<uint64_t>(idx), 0, call(BUF_SIZE, P(ib)), static_cast<uint64_t>(t), 0);
        call(EXT_ADVANCE, P(ext));
        inputs++;
        drain(s, ex);
    }
    for (Stream* s : {&vs, &as}) {   // end of stream: an empty input with the EOS flag, then everything out
        if (s->track < 0) continue;
        int idx = -1, more = 0;
        while (static_cast<int>(call(DEC_ACQ_IN, P(s->dec), P(&idx), P(&more))) == ST_NO_INPUT) drain(*s, ex);
        call(DEC_SEND, P(s->dec), static_cast<uint64_t>(idx), 0, 0, 0, 4);
        drain(*s, ex);
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  played %llu inputs in %.2f s\n", static_cast<unsigned long long>(inputs), secs);
    if (vs.track >= 0) {
        std::printf("  video: %llu frames (%.0f fps decoded), %dx%d, NV12 %zu bytes = Y %d + CbCr %d, first %.6f s, last %.6f s, EOS %s\n",
                    static_cast<unsigned long long>(vs.outputs), vs.outputs / secs, vs.w, vs.h, vs.frame_bytes, vs.w * vs.h, vs.w * ((vs.h + 1) / 2),
                    vs.first / 1e6, vs.last / 1e6, vs.eos_seen ? "seen" : "missing");
        check(vs.frame_bytes == static_cast<size_t>(vs.w) * vs.h + static_cast<size_t>(vs.w) * ((vs.h + 1) / 2), "NV12 frame size w*h + w*((h+1)/2)");
        check(vs.first == ex.video_first, "first video timestamp %lld == %lld us", static_cast<long long>(vs.first), static_cast<long long>(ex.video_first));
        check(vs.monotonic, "video timestamps increase");
        check(vs.eos_seen, "video EOS flag delivered");
        if (ex.synthetic) {
            check(static_cast<int>(vs.outputs) == ex.frames, "video frames %llu == %d", static_cast<unsigned long long>(vs.outputs), ex.frames);
            check(std::llabs(vs.last - ex.video_first - static_cast<int64_t>((ex.frames - 1) * 1e6 / kFps)) <= 1000, "last video timestamp %.6f s", vs.last / 1e6);
            check(vs.luma_err <= 8, "luma rows match the encoded ramp (max error %d)", vs.luma_err);
            check(vs.chroma_err <= 8, "CbCr plane matches the encoded (Cb %d, Cr %d) (max error %d)", kU, kV, vs.chroma_err);
        }
    }
    if (as.track >= 0) {
        const int64_t samples = as.ch ? static_cast<int64_t>(as.bytes / (static_cast<uint64_t>(as.ch) * 2)) : 0;
        std::printf("  audio: %llu blocks, %d Hz, %d ch, %lld samples (%.3f s), first %.6f s, last block %.6f s, EOS %s\n",
                    static_cast<unsigned long long>(as.outputs), as.rate, as.ch, static_cast<long long>(samples), as.rate ? double(samples) / as.rate : 0.0,
                    as.first / 1e6, as.last / 1e6, as.eos_seen ? "seen" : "missing");
        check(as.first == ex.audio_first, "first audio timestamp %lld == %lld us", static_cast<long long>(as.first), static_cast<long long>(ex.audio_first));
        check(as.monotonic, "audio timestamps increase");
        check(as.eos_seen, "audio EOS flag delivered");
        if (ex.synthetic) {
            check(std::llabs(samples - ex.samples) <= 1, "audio samples %lld == %lld (the AAC priming is cut)", static_cast<long long>(samples), static_cast<long long>(ex.samples));
            check(as.pcm_err < 400, "PCM continues the encoded sine from t=%.1f s (RMS error %.1f of amplitude 8000)", ex.cut, as.pcm_err);
        }
    }

    // a loop: SeekTo(0) right after the end swaps in the samples decoded ahead
    const auto t_seek = std::chrono::steady_clock::now();
    const int s0 = static_cast<int>(call(EXT_SEEK, P(ext), 0));
    int64_t t = -1;
    const int r0 = static_cast<int>(call(EXT_TIME, P(ext), P(&t)));
    check(s0 == ST_OK && r0 == ST_OK && t == 0, "SeekTo(0) (loop) -> 0x%x, next sample at %lld us, %.1f ms", s0, static_cast<long long>(t),
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_seek).count());
    // a seek into the middle
    const int s1 = static_cast<int>(call(EXT_SEEK, P(ext), 1000000));
    t = -1;
    const int r1 = static_cast<int>(call(EXT_TIME, P(ext), P(&t)));
    check(s1 == ST_OK && r1 == ST_OK && t >= 900000 && t <= 1100000, "SeekTo(1 s) -> 0x%x, next sample at %lld us", s1, static_cast<long long>(t));

    for (Stream* s : {&vs, &as}) {
        if (s->track < 0) continue;
        call(DEC_STOP, P(s->dec));
        call(DEC_DTOR, P(s->dec));
        call(MD_DTOR, P(&s->fmt_md));
    }
    for (uint64_t& md : mds) call(MD_DTOR, P(&md));
    call(EXT_DTOR, P(ext));
}
}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        size_t n = 0;
        nxrt::media::movie_import_names(&n);
        check(nxrt::media::install_movie_hle() && g_handlers.size() == n && n == 41, "install_movie_hle registers %zu of %zu handlers", g_handlers.size(), n);
        std::printf("VideoToolbox hardware VP9 decode: %s\n", VTIsHardwareDecodeSupported(kCMVideoCodecType_VP9) ? "yes" : "no");
        if (argc > 1) {
            for (int i = 1; i < argc; i++) {
                std::string a = argv[i];
                int container = 0;
                const size_t colon = a.rfind(':');
                if (colon != std::string::npos && colon + 2 == a.size() && a[colon + 1] >= '1' && a[colon + 1] <= '3') { container = a[colon + 1] - '0'; a.resize(colon); }
                play(a, container ? container : container_of(a), Expect{});
            }
        } else {
            NSString* dir = NSTemporaryDirectory();
            NSString* plain = [dir stringByAppendingPathComponent:@"nxrt_movie_test_plain.mp4"];
            NSString* cut = [dir stringByAppendingPathComponent:@"nxrt_movie_test_cut.mp4"];
            NSString* late = [dir stringByAppendingPathComponent:@"nxrt_movie_test_late.mp4"];
            NSString* late_a = [dir stringByAppendingPathComponent:@"nxrt_movie_test_late_audio.mp4"];
            if (!make_mp4(plain, 0.0) || !make_mp4(cut, 0.5) || !make_mp4(late, 0.0, 0.5) || !make_mp4(late_a, 0.0, 0.0, 0.25)) return 2;
            play(plain.UTF8String, 1, Expect{true, static_cast<int>(kSecs * kFps), static_cast<int64_t>(kSecs * kRate), 0.0, 0, 0});
            play(cut.UTF8String, 1, Expect{true, static_cast<int>((kSecs - 0.5) * kFps), static_cast<int64_t>((kSecs - 0.5) * kRate), 0.5, 0, 0});
            play(late.UTF8String, 1, Expect{true, static_cast<int>(kSecs * kFps), static_cast<int64_t>(kSecs * kRate), 0.0, 500000, 0});
            play(late_a.UTF8String, 1, Expect{true, static_cast<int>(kSecs * kFps), static_cast<int64_t>(kSecs * kRate), 0.0, 0, 250000});
            for (NSString* p in @[plain, cut, late, late_a]) [[NSFileManager defaultManager] removeItemAtPath:p error:nil];
        }
        // the extractors close on their own threads ([media] movie_async_close): wait for them
        for (int i = 0; i < 200; i++) {
            {
                std::lock_guard<std::mutex> lk(nxrt::media::g_exts_mu);
                if (nxrt::media::g_exts.empty()) break;
            }
            usleep(10000);
        }
        {
            std::lock_guard<std::mutex> lk(nxrt::media::g_exts_mu);
            check(nxrt::media::g_exts.empty(), "every extractor torn down");
        }
        const nxrt::media::MovieStats s = nxrt::media::movie_stats();
        std::printf("\nmovie_stats: extractors %llu/%llu, decoders %llu/%llu, video frames %llu, audio blocks %llu (%llu bytes), samples read %llu, read errors %llu\n",
                    (unsigned long long)s.extractors_created, (unsigned long long)s.extractors_destroyed, (unsigned long long)s.decoders_created,
                    (unsigned long long)s.decoders_destroyed, (unsigned long long)s.video_frames_delivered, (unsigned long long)s.audio_blocks_delivered,
                    (unsigned long long)s.audio_bytes_delivered, (unsigned long long)s.samples_read, (unsigned long long)s.read_errors);
        std::printf("%s: %d check(s) failed, %d warning line(s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_warnings.load());
        return g_failures ? 1 : 0;
    }
}
