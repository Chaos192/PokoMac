// SPDX-License-Identifier: GPL-3.0-or-later
// movie:: (Nintendo's multimedia library, a port of Android's stagefright) on AVFoundation: the macOS side of src/media/movie_mf.cpp.
// See include/nxrt/movie.h.
//
// Everything the game sees is movie_mf.cpp's: the Extractor/Decoder objects and their layouts, the buffer model of the decoder, the
// two-pool index scheme, the token that stands for a decoded sample, the result codes, the [media] keys, the log lines, the stats and the
// gauges. Only the demux+decode backend under the extractor differs (Objective-C++, ARC: runtime/CMakeLists.txt sets -fobjc-arc here):
//   Media Foundation (movie_mf.cpp)                      here
//   SHCreateMemStream + IMFByteStream over the bytes     an AVURLAsset on a private URL scheme whose bytes an AVAssetResourceLoaderDelegate
//                                                        serves from the copy in memory (no temporary file)
//   one IMFSourceReader, a stream selected per track     one AVAssetReader + AVAssetReaderTrackOutput per selected track, so no track's
//                                                        output can hold up another's
//   SetCurrentMediaType NV12 / PCM16                     output settings '420v' ('420f' for a full range stream, so the samples keep the
//                                                        values nn-colorspace describes) / LPCM 16-bit interleaved at the track's rate
//   ReadSample(stream)                                   copyNextSampleBuffer on the track's output, in the worker thread
//   SetCurrentPosition (SeekTo, the loop look-ahead)     new readers whose timeRange starts at the target
//   IMFSample                                            CMSampleBuffer: a CVPixelBuffer (NV12) or a CMBlockBuffer of PCM
// Edit lists: AVFoundation applies them itself (an empty edit delays the track, the media time of the first edit cuts its start, the AAC
// priming is trimmed), so a reader hands out the presentation times movie_mf.cpp computes from the 'elst' boxes: no offset is added here
// and SeekTo needs no conversion. One difference is evened out: the reader fills a leading empty edit with a black frame (silence for
// audio) at its start, where movie_mf.cpp's samples of the delayed track simply begin at the end of the edit; that filler is cut (the box
// walk gives the track's start), as is any sample still before 0. The box walk also matches the reader's tracks to the file's 'trak' order
// and gives the edit offsets of the log line.
// Seeking differs in one detail: a reader starts at the target itself, where Media Foundation starts at the sync sample before it.
// Containers: AVFoundation demuxes MP4/MOV only. Matroska and WebM (container types 2 and 3) fail in SetDataSource; Switch 2 games (whose
// movies are WebM/VP9) never reach this file anyway (install_movie_hle, src/media/video_codec.cpp).
#if !__has_feature(objc_arc)
#error "movie_avf.mm is written for ARC (-fobjc-arc, set in runtime/CMakeLists.txt)"
#endif

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/abi.h"
#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/movie.h"

// Serves the bytes of one movie to AVFoundation from memory: the role of SHCreateMemStream + MFCreateMFByteStreamOnStream in movie_mf.cpp.
// Every data request is answered at once with a view of the bytes (no copy; the view keeps the whole buffer alive).
@interface NxrtMovieBytes : NSObject <AVAssetResourceLoaderDelegate>
- (instancetype)initWithData:(NSData*)data contentType:(NSString*)type;
@end

@implementation NxrtMovieBytes {
    NSData* _data;
    NSString* _type;
}
- (instancetype)initWithData:(NSData*)data contentType:(NSString*)type {
    if ((self = [super init])) {
        _data = data;
        _type = type;
    }
    return self;
}
- (BOOL)resourceLoader:(AVAssetResourceLoader*)loader shouldWaitForLoadingOfRequestedResource:(AVAssetResourceLoadingRequest*)req {
    const long long size = static_cast<long long>(_data.length);
    if (AVAssetResourceLoadingContentInformationRequest* ci = req.contentInformationRequest) {
        ci.contentType = _type;
        ci.contentLength = size;
        ci.byteRangeAccessSupported = YES;
    }
    if (AVAssetResourceLoadingDataRequest* dr = req.dataRequest) {
        const long long off = dr.currentOffset;
        long long len = dr.requestsAllDataToEndOfResource ? size - off : dr.requestedOffset + dr.requestedLength - off;
        if (off >= 0 && off < size && len > 0) {
            len = std::min(len, size - off);
            NSData* whole = _data;
            NSData* part = [[NSData alloc] initWithBytesNoCopy:const_cast<uint8_t*>(static_cast<const uint8_t*>(whole.bytes) + off)
                                                        length:static_cast<NSUInteger>(len)
                                                   deallocator:^(void*, NSUInteger) { (void)whole; }];
            [dr respondWithData:part];
        }
    }
    [req finishLoading];
    return YES;
}
@end

namespace nxrt::media {
namespace {

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define RET(v) (c->x[0] = static_cast<uint64_t>(v))

constexpr int ST_OK = 0;
constexpr int ST_FAILED = 0x1000;
constexpr int ST_INVALID = 0x100b;
constexpr int ST_UNSUPPORTED = 0x1006;
constexpr int ST_END = 0x2000;
constexpr int ST_NO_INPUT = 0x1011;
constexpr int ST_NO_OUTPUT = 0x100e;
constexpr uint32_t FLAG_EOS = 4;
constexpr size_t kSlotBytes = 64;                     // one decoder input slot; holds a Token
constexpr uint64_t kTokenMagic = 0x4d4f564945544f4bull;   // "MOVIETOK"
constexpr uint64_t kObjMarker = 0x4e58524d4f564945ull;    // "NXRMOVIE": byte 0..7 of an object the host owns
constexpr int kAudioChunk = 0x8000;                   // what GetOutputBufferSize says for audio (the game allocates at least this)
constexpr size_t kVideoQueueMax = 6, kAudioQueueMax = 64, kOtherQueueMax = 8;
constexpr int kWaitMs = 8000;                         // longest the extractor makes a game thread wait for the worker
constexpr int kLoadMs = 10000;                        // longest SetDataSource waits for AVFoundation to parse the container

bool g_log = false;
bool g_lookahead = true, g_preroll = true, g_async_close = true;   // S21: [media] movie_lookahead, movie_preroll, movie_async_close
std::atomic<uint64_t> s_extractors_created{0}, s_extractors_destroyed{0}, s_decoders_created{0}, s_decoders_destroyed{0}, s_frames{0},
    s_audio_blocks{0}, s_audio_bytes{0}, s_samples{0}, s_read_errors{0};
std::atomic<uint64_t> s_url_serial{0};                 // makes every extractor's private URL unique
GaugeCounter g_media_workers("media.worker_threads");   // movie-read threads alive (one per started extractor)

// ---- guest memory --------------------------------------------------------------------------------------------------------------
inline uint64_t rd64(uint64_t a) { uint64_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 8); return v; }
inline uint32_t rd32(uint64_t a) { uint32_t v; std::memcpy(&v, reinterpret_cast<const void*>(a), 4); return v; }
inline void wr64(uint64_t a, uint64_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 8); }
inline void wr32(uint64_t a, uint32_t v) { std::memcpy(reinterpret_cast<void*>(a), &v, 4); }
inline void wr8(uint64_t a, uint8_t v) { *reinterpret_cast<uint8_t*>(a) = v; }
inline uint8_t rd8(uint64_t a) { return *reinterpret_cast<const uint8_t*>(a); }

// Calls a guest function with up to five arguments from inside a handler; returns x0. The handler's own argument registers are the
// caller's business (they are saved here only so a handler may keep using them after the call).
uint64_t gcall(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t save[9];
    std::memcpy(save, c->x, sizeof save);
    const uint64_t lr = c->x[30], f = c->fn;
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    std::memcpy(c->x, save, sizeof save);
    c->x[30] = lr;
    c->fn = f;
    return r;
}

// ---- CoreMedia helpers ---------------------------------------------------------------------------------------------------------
// One reference to a CMSampleBuffer (the ComPtr<IMFSample> of movie_mf.cpp). ARC does not manage CF types.
class SampleRef {
public:
    SampleRef() = default;
    explicit SampleRef(CMSampleBufferRef s) : s_(s) {}   // adopts a +1 reference (copyNextSampleBuffer, a Token)
    SampleRef(const SampleRef& o) : s_(o.s_) { if (s_) CFRetain(s_); }
    SampleRef(SampleRef&& o) noexcept : s_(o.s_) { o.s_ = nullptr; }
    SampleRef& operator=(SampleRef o) noexcept { std::swap(s_, o.s_); return *this; }
    ~SampleRef() { if (s_) CFRelease(s_); }
    CMSampleBufferRef get() const { return s_; }
    explicit operator bool() const { return s_ != nullptr; }
private:
    CMSampleBufferRef s_ = nullptr;
};

// The PCM bytes of a decoded audio sample (IMFSample::GetTotalLength).
uint32_t pcm_bytes(CMSampleBufferRef s) {
    CMBlockBufferRef bb = s ? CMSampleBufferGetDataBuffer(s) : nullptr;
    return bb ? static_cast<uint32_t>(CMBlockBufferGetDataLength(bb)) : 0;
}

int64_t to_us(CMTime t) {
    if (!CMTIME_IS_NUMERIC(t)) return 0;
    return CMTimeConvertScale(t, 1000000, kCMTimeRoundingMethod_RoundHalfAwayFromZero).value;
}

// Waits (bounded) for AVFoundation to load `keys` of an asset or track; false when one of them failed (`why` says why).
bool load_keys(id<AVAsynchronousKeyValueLoading> obj, NSArray<NSString*>* keys, std::string* why) {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [obj loadValuesAsynchronouslyForKeys:keys completionHandler:^{ dispatch_semaphore_signal(sem); }];
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(kLoadMs) * NSEC_PER_MSEC)) != 0) {
        if (why) *why = "timed out";
        return false;
    }
    for (NSString* k in keys) {
        NSError* err = nil;
        if ([obj statusOfValueForKey:k error:&err] != AVKeyValueStatusLoaded) {
            if (why) *why = std::string(k.UTF8String) + ": " + (err ? err.localizedDescription.UTF8String : "not loaded");
            return false;
        }
    }
    return true;
}

std::string ns_error(NSError* err) {
    if (!err) return "no error";
    std::string s = err.localizedDescription.UTF8String ? err.localizedDescription.UTF8String : "?";
    char b[64];
    std::snprintf(b, sizeof b, " (%s %ld)", err.domain.UTF8String ? err.domain.UTF8String : "?", static_cast<long>(err.code));
    s += b;
    if (NSError* under = err.userInfo[NSUnderlyingErrorKey]) {
        std::snprintf(b, sizeof b, ", underlying %s %ld", under.domain.UTF8String ? under.domain.UTF8String : "?", static_cast<long>(under.code));
        s += b;
    }
    return s;
}

// ---- movie::MediaData ----------------------------------------------------------------------------------------------------------
struct MDEntry {
    enum Type { I32, I64, STR, PTR } t = I32;
    int64_t i = 0;
    std::string s;
    uint64_t p = 0;
};
struct MD {
    std::map<std::string, MDEntry> e;   // node based: the pointer FindString hands out stays valid until Clear / the destructor
    void set_i32(const char* k, int32_t v) { MDEntry& x = e[k]; x.t = MDEntry::I32; x.i = v; }
    void set_i64(const char* k, int64_t v) { MDEntry& x = e[k]; x.t = MDEntry::I64; x.i = v; }
    void set_str(const char* k, const char* v) { MDEntry& x = e[k]; x.t = MDEntry::STR; x.s = v ? v : ""; }
    void set_ptr(const char* k, uint64_t v) { MDEntry& x = e[k]; x.t = MDEntry::PTR; x.p = v; }
    const MDEntry* find(const char* k) const { auto it = e.find(k); return it == e.end() ? nullptr : &it->second; }
    bool get_i32(const char* k, int32_t* out) const {
        const MDEntry* x = find(k);
        if (!x || (x->t != MDEntry::I32 && x->t != MDEntry::I64)) return false;
        *out = static_cast<int32_t>(x->i);
        return true;
    }
    bool get_i64(const char* k, int64_t* out) const {
        const MDEntry* x = find(k);
        if (!x || (x->t != MDEntry::I32 && x->t != MDEntry::I64)) return false;
        *out = x->i;
        return true;
    }
};
MD* md_of(uint64_t guest) { return guest ? reinterpret_cast<MD*>(rd64(guest)) : nullptr; }

void mdata_ctor(::Ctx* c) { wr64(A0, reinterpret_cast<uint64_t>(new MD())); }
void mdata_copy(::Ctx* c) {
    MD* src = md_of(A1);
    MD* m = new MD();
    if (src) m->e = src->e;
    wr64(A0, reinterpret_cast<uint64_t>(m));
}
void mdata_dtor(::Ctx* c) { delete md_of(A0); wr64(A0, 0); }
void mdata_clear(::Ctx* c) { if (MD* m = md_of(A0)) m->e.clear(); }
void mdata_find_i32(::Ctx* c) {
    int32_t v = 0;
    MD* m = md_of(A0);
    const bool ok = m && A1 && m->get_i32(reinterpret_cast<const char*>(A1), &v);
    if (ok && A2) wr32(A2, static_cast<uint32_t>(v));
    RET(ok);
}
void mdata_find_i64(::Ctx* c) {
    int64_t v = 0;
    MD* m = md_of(A0);
    const bool ok = m && A1 && m->get_i64(reinterpret_cast<const char*>(A1), &v);
    if (ok && A2) wr64(A2, static_cast<uint64_t>(v));
    RET(ok);
}
void mdata_find_str(::Ctx* c) {
    MD* m = md_of(A0);
    const MDEntry* x = m && A1 ? m->find(reinterpret_cast<const char*>(A1)) : nullptr;
    const bool ok = x && x->t == MDEntry::STR;
    if (ok && A2) wr64(A2, reinterpret_cast<uint64_t>(x->s.c_str()));
    RET(ok);
}

// ---- movie::Buffer -------------------------------------------------------------------------------------------------------------
constexpr uint64_t B_OFFSET = 0x00, B_SIZE = 0x04, B_FLAGS = 0x0c, B_OWNS = 0x10, B_CAP = 0x18, B_I64 = 0x20, B_BASE = 0x28;
void buf_ctor(::Ctx* c) { std::memset(reinterpret_cast<void*>(A0), 0, 0x40); }
void buf_ctor_mem(::Ctx* c) {            // Buffer(void* base, size_t capacity): memory the game owns
    std::memset(reinterpret_cast<void*>(A0), 0, 0x40);
    wr64(A0 + B_BASE, A1);
    wr64(A0 + B_CAP, A2);
}
void buf_dtor(::Ctx* c) { wr64(A0 + B_BASE, 0); wr64(A0 + B_CAP, 0); }   // the game's buffers never own their memory
void buf_base(::Ctx* c) { RET(rd64(A0 + B_BASE)); }
void buf_size(::Ctx* c) { RET(rd32(A0 + B_SIZE)); }
void buf_offset(::Ctx* c) { RET(rd32(A0 + B_OFFSET)); }

// ---- the MP4 box walk: track order and edit lists ------------------------------------------------------------------------------
struct Mp4Track {
    uint32_t handler = 0;           // 'vide' / 'soun'
    uint32_t id = 0;                // tkhd track_ID: the AVAssetTrack's trackID
    uint32_t timescale = 0;
    int64_t delay_us = 0;           // leading empty edits: the track starts this much later
    int64_t media_time = 0;         // start of the first real edit, in the track's time scale
    bool has_edit = false;
    int64_t offset_us() const { return delay_us - (timescale ? media_time * 1000000 / static_cast<int64_t>(timescale) : 0); }   // presentation time of media time 0
};
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

struct Mp4Walker {
    const uint8_t* d;
    size_t n;
    uint32_t movie_ts = 1000;
    std::vector<Mp4Track> tracks;

    void walk(size_t off, size_t end, int depth, int trak) {
        while (off + 8 <= end) {
            uint64_t sz = be32(d + off);
            const uint32_t type = be32(d + off + 4);
            size_t hdr = 8;
            if (sz == 1 && off + 16 <= end) { sz = be64(d + off + 8); hdr = 16; }
            else if (sz == 0) sz = end - off;
            if (sz < hdr || off + sz > end) return;
            const uint8_t* body = d + off + hdr;
            const size_t blen = static_cast<size_t>(sz - hdr);
            auto is = [&](const char* s) { return type == be32(reinterpret_cast<const uint8_t*>(s)); };
            if (is("moov") || is("edts") || is("mdia")) {
                walk(off + hdr, off + static_cast<size_t>(sz), depth + 1, trak);
            } else if (is("trak")) {
                tracks.emplace_back();
                walk(off + hdr, off + static_cast<size_t>(sz), depth + 1, static_cast<int>(tracks.size()) - 1);
            } else if (is("mvhd") && blen >= 20) {
                movie_ts = body[0] == 1 ? (blen >= 24 ? be32(body + 20) : 1000) : be32(body + 12);
            } else if (is("tkhd") && trak >= 0 && blen >= 16) {
                tracks[trak].id = body[0] == 1 ? (blen >= 24 ? be32(body + 20) : 0) : be32(body + 12);
            } else if (is("mdhd") && trak >= 0 && blen >= 20) {
                tracks[trak].timescale = body[0] == 1 ? (blen >= 24 ? be32(body + 20) : 0) : be32(body + 12);
            } else if (is("hdlr") && trak >= 0 && blen >= 12) {
                tracks[trak].handler = be32(body + 8);
            } else if (is("elst") && trak >= 0 && blen >= 8) {
                const uint32_t cnt = be32(body + 4);
                const bool v1 = body[0] == 1;
                const size_t esz = v1 ? 20 : 12;
                int64_t delay = 0, media = 0;
                bool got = false;
                for (uint32_t k = 0; k < cnt && 8 + (k + 1) * esz <= blen && !got; k++) {
                    const uint8_t* e = body + 8 + k * esz;
                    const int64_t seg = v1 ? static_cast<int64_t>(be64(e)) : be32(e);
                    const int64_t mt = v1 ? static_cast<int64_t>(be64(e + 8)) : static_cast<int32_t>(be32(e + 4));
                    if (mt == -1) delay += seg * 1000000 / std::max<uint32_t>(movie_ts, 1);   // an empty edit: the track starts later
                    else { media = mt; got = true; }
                }
                Mp4Track& t = tracks[trak];
                t.has_edit = true;
                t.delay_us = delay;
                t.media_time = media;
            }
            off += static_cast<size_t>(sz);
        }
    }
};

// ---- the extractor -------------------------------------------------------------------------------------------------------------
struct Item {
    SampleRef s;
    int64_t us = 0;          // presentation time (the reader applied the edit list)
    uint32_t skip = 0;       // audio: leading bytes of the PCM to drop (priming)
};
struct Track {
    int kind = 2;            // 0 video, 1 audio, 2 other
    AVAssetTrack* at = nil;  // the asset's track (immutable once the extractor is open)
    std::string mime;
    int w = 0, h = 0, rate = 0, ch = 0, bitrate = 0, colorspace = 2;
    double fps = 0;
    int64_t offset_us = 0;   // the presentation time of media time 0 by the edit list (the reader applies it; logged only)
    int64_t start_us = 0;    // where the track's media starts: the leading empty edits (what the reader hands out before is filler)
    bool selected = false, eos = false, failed = false;
    uint32_t gen = 0;        // bumped by every (un)selection: the worker makes the track a new reader
    std::deque<Item> q;
    int64_t last_us = INT64_MIN;
    size_t limit() const { return kind == 0 ? kVideoQueueMax : kind == 1 ? kAudioQueueMax : kOtherQueueMax; }
};

// What a reader of a track is made from (copied out of the Track under the lock).
struct ReaderSpec {
    AVAssetTrack* at = nil;
    int kind = 2, rate = 0, ch = 0, colorspace = 2;
};
// The worker's reader of one track. Only the worker thread touches it (and the extractor's teardown, after the join).
struct Rd {
    AVAssetReader* r = nil;
    AVAssetReaderTrackOutput* o = nil;
    uint32_t gen = UINT32_MAX;   // the Track::gen it was made for
    std::string why;             // why it could not be made
};

struct Ext;
// Every live extractor and decoder, for the gauges near the end of this file. Lock order there: the registry first, then the object's m.
std::mutex g_exts_mu;
std::set<Ext*> g_exts;

struct Ext {
    Ext() { std::lock_guard<std::mutex> lk(g_exts_mu); g_exts.insert(this); }
    ~Ext() { std::lock_guard<std::mutex> lk(g_exts_mu); g_exts.erase(this); }
    Ext(const Ext&) = delete;
    Ext& operator=(const Ext&) = delete;
    int container = 1;
    std::string name;
    NxrtMovieBytes* bytes = nil;     // the movie in memory, served to the asset
    dispatch_queue_t bytes_q = nil;  // where AVFoundation asks for the bytes
    AVURLAsset* asset = nil;         // set once the container is parsed (the "open" test, movie_mf.cpp's e->rd)
    std::vector<Track> tr;
    std::vector<Rd> rd;              // per track, the worker's
    int64_t duration_us = 0;
    // the worker thread
    std::mutex m;                    // queues, flags, cur, seek state
    std::condition_variable cv_data, cv_work;
    std::thread worker;
    bool stop = false, started = false;
    bool seek_pending = false;
    bool reading = false;            // set by the first request for data: the game has selected its tracks by then (a track selected after reading began starts where the others are)
    int64_t seek_us = 0;
    int64_t base_us = 0;             // where the readers were last positioned (0, a SeekTo target): a reader made before anything was read starts there
    uint64_t read_since_base = 0;    // samples queued since then
    // Loop look-ahead (S21): when every selected track has reached its end the worker positions the readers at the start and decodes the first
    // samples into `nx`, so a SeekTo(0) that follows (a looping movie) swaps them in at once instead of waiting for the decoder to start again.
    struct Ahead {
        std::deque<Item> q;
        bool eos = false, failed = false;
        int64_t last_us = INT64_MIN;
    };
    std::vector<Ahead> nx;
    int ahead = 0;                   // 0 off, 1 the worker is positioning the readers, 2 the readers are at the start and fill nx
    bool ahead_tried = false;        // one attempt per end of stream
    uint32_t swaps = 0;              // look-aheads swapped in so far: a sample read for the look-ahead while it was swapped in belongs to the queue that replaced it
    // the current sample as NuMediaExtractor defines it: the earliest head among the selected tracks
    bool cur_valid = false;
    int cur_track = -1;
    int64_t cur_us = 0;
    uint64_t samples_read = 0;
};

Ext* ext_of(uint64_t guest) { return guest && rd64(guest) == kObjMarker ? reinterpret_cast<Ext*>(rd64(guest + 8)) : nullptr; }

constexpr uint32_t fourcc_be(char a, char b, char c, char d) { return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d)); }

const char* video_mime(FourCharCode f) {
    switch (f) {
    case kCMVideoCodecType_H264: return "video/avc";
    case kCMVideoCodecType_HEVC: case fourcc_be('h', 'e', 'v', '1'): return "video/hevc";
    case kCMVideoCodecType_VP9: return "video/x-vnd.on2.vp9";
    case fourcc_be('v', 'p', '0', '8'): return "video/x-vnd.on2.vp8";
    default: return "video/unknown";
    }
}
const char* audio_mime(FourCharCode f) {
    switch (f) {
    case kAudioFormatMPEG4AAC: case kAudioFormatMPEG4AAC_HE: case kAudioFormatMPEG4AAC_HE_V2: case kAudioFormatMPEG4AAC_LD:
    case kAudioFormatMPEG4AAC_ELD: case kAudioFormatMPEG4AAC_ELD_SBR: case kAudioFormatMPEG4AAC_ELD_V2: case kAudioFormatMPEG4AAC_Spatial:
        return "audio/mp4a-latm";
    case kAudioFormatMPEGLayer3: return "audio/mpeg";
    case kAudioFormatOpus: return "audio/opus";
    case fourcc_be('v', 'o', 'r', 'b'): return "audio/vorbis";
    default: return "audio/unknown";
    }
}

// Fills a Track from the asset's view of it (MF: the native media type).
Track track_of(AVAssetTrack* at) {
    Track t;
    t.at = at;
    NSString* mt = at.mediaType;
    t.kind = [mt isEqualToString:AVMediaTypeVideo] ? 0 : [mt isEqualToString:AVMediaTypeAudio] ? 1 : 2;
    CMFormatDescriptionRef fd = at.formatDescriptions.count ? (__bridge CMFormatDescriptionRef)at.formatDescriptions.firstObject : nullptr;
    const FourCharCode sub = fd ? CMFormatDescriptionGetMediaSubType(fd) : 0;
    t.bitrate = static_cast<int>(at.estimatedDataRate);
    if (t.kind == 0) {
        // the picture the decoder makes: the coded size with the clean aperture applied (MF_MT_FRAME_SIZE), not the display size of tkhd
        CGSize sz = fd ? CMVideoFormatDescriptionGetPresentationDimensions(fd, false, true) : CGSize{0, 0};
        if (sz.width <= 0 || sz.height <= 0) sz = at.naturalSize;
        t.w = static_cast<int>(sz.width + 0.5);
        t.h = static_cast<int>(sz.height + 0.5);
        t.fps = at.nominalFrameRate;
        // the game's colour conversion: 0 BT.601 limited, 1 BT.601 full, 2 BT.709 limited, 3 BT.709 full
        bool bt601 = false, full = false;
        if (fd) {
            CFTypeRef mat = CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_YCbCrMatrix);
            bt601 = mat && CFGetTypeID(mat) == CFStringGetTypeID() && CFEqual(mat, kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4);
            CFTypeRef fr = CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_FullRangeVideo);
            full = fr && CFGetTypeID(fr) == CFBooleanGetTypeID() && CFBooleanGetValue(static_cast<CFBooleanRef>(fr));
        }
        t.colorspace = bt601 ? (full ? 1 : 0) : (full ? 3 : 2);
        t.mime = video_mime(sub);
    } else if (t.kind == 1) {
        // the richest layer of the format (HE-AAC decodes to twice the core rate, parametric stereo to two channels): what the decoder makes
        if (fd) {
            size_t n = 0;
            const AudioFormatListItem* list = CMAudioFormatDescriptionGetFormatList(fd, &n);
            const AudioStreamBasicDescription* asbd = list && n >= sizeof(AudioFormatListItem) ? &list[0].mASBD : CMAudioFormatDescriptionGetStreamBasicDescription(fd);
            if (asbd) {
                t.rate = static_cast<int>(asbd->mSampleRate + 0.5);
                t.ch = static_cast<int>(asbd->mChannelsPerFrame);
            }
        }
        t.mime = audio_mime(sub);
    } else {
        t.mime = "application/octet-stream";
    }
    return t;
}

bool ext_open(Ext* e, NSData* data) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data.bytes);
    const size_t size = data.length;
    NSString* type = e->container == 2 ? @"org.matroska.mkv" : e->container == 3 ? @"org.webmproject.webm" : AVFileTypeMPEG4;
    const char* ext = e->container == 2 ? "mkv" : e->container == 3 ? "webm" : "mp4";
    e->bytes = [[NxrtMovieBytes alloc] initWithData:data contentType:type];
    e->bytes_q = dispatch_queue_create("nxrt.movie.bytes", DISPATCH_QUEUE_SERIAL);
    NSString* url = [NSString stringWithFormat:@"nxrt-movie://extractor/%llu.%s", static_cast<unsigned long long>(++s_url_serial), ext];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL URLWithString:url] options:@{AVURLAssetPreferPreciseDurationAndTimingKey: @YES}];
    [asset.resourceLoader setDelegate:e->bytes queue:e->bytes_q];
    std::string why;
    if (!load_keys(asset, @[@"tracks", @"duration"], &why)) {
        NXLOG_W("movie", "AVFoundation could not open the movie (%s container): %s", ext, why.c_str());
        if (e->container != 1) NXLOG_W("movie", "AVFoundation has no Matroska/WebM demuxer: container type %d is not supported on macOS", e->container);
        [asset cancelLoading];
        e->bytes = nil;                          // the bytes go now, not with the extractor
        e->bytes_q = nil;
        return false;
    }
    e->duration_us = to_us(asset.duration);

    // the container's own view of the tracks: order in the file and the edit lists
    Mp4Walker mw{bytes, size};
    if (e->container == 1) mw.walk(0, size, 0, -1);

    std::vector<Track> av_tracks;
    for (AVAssetTrack* at in asset.tracks) {
        NSString* mt = at.mediaType;
        if ([mt isEqualToString:AVMediaTypeTimecode] || [mt isEqualToString:AVMediaTypeMetadata]) continue;   // not streams of a source reader
        if (!load_keys(at, @[@"formatDescriptions", @"naturalSize", @"nominalFrameRate", @"estimatedDataRate"], &why))
            NXLOG_W("movie", "track %d: %s", static_cast<int>(at.trackID), why.c_str());
        av_tracks.push_back(track_of(at));
    }
    // file order: each video (audio) trak of the file is the asset's track with its track ID (else the next unused one of its kind); the edit
    // list of the trak gives the track its time offset
    std::vector<Track> ordered;
    {
        std::vector<bool> used(av_tracks.size(), false);
        const uint32_t vide = be32(reinterpret_cast<const uint8_t*>("vide")), soun = be32(reinterpret_cast<const uint8_t*>("soun"));
        for (const Mp4Track& mt : mw.tracks) {
            const int want = mt.handler == vide ? 0 : mt.handler == soun ? 1 : -1;
            if (want < 0) continue;
            int pick = -1;
            for (size_t k = 0; k < av_tracks.size() && pick < 0; k++)
                if (!used[k] && av_tracks[k].kind == want && av_tracks[k].at.trackID == static_cast<CMPersistentTrackID>(mt.id)) pick = static_cast<int>(k);
            for (size_t k = 0; k < av_tracks.size() && pick < 0; k++)
                if (!used[k] && av_tracks[k].kind == want) pick = static_cast<int>(k);
            if (pick < 0) continue;
            used[static_cast<size_t>(pick)] = true;
            Track t = av_tracks[static_cast<size_t>(pick)];
            if (mt.has_edit) {
                t.offset_us = mt.offset_us();
                t.start_us = std::max<int64_t>(0, mt.delay_us);
            }
            ordered.push_back(t);
        }
        for (const Track& t : av_tracks) if (t.kind == 2) ordered.push_back(t);
    }
    {   // under e->m: the gauges read the track queues from the sampler thread
        std::lock_guard<std::mutex> lk(e->m);
        e->tr = ordered.size() == av_tracks.size() ? ordered : av_tracks;
        e->rd.assign(e->tr.size(), Rd());
        e->asset = asset;
    }
    std::string desc;
    for (size_t i = 0; i < e->tr.size(); i++) {
        const Track& t = e->tr[i];
        char b[160];
        if (t.kind == 0) std::snprintf(b, sizeof b, " [%zu] %s %dx%d %.2f fps (edit offset %lld us)", i, t.mime.c_str(), t.w, t.h, t.fps, static_cast<long long>(t.offset_us));
        else if (t.kind == 1) std::snprintf(b, sizeof b, " [%zu] %s %d Hz %d ch (edit offset %lld us)", i, t.mime.c_str(), t.rate, t.ch, static_cast<long long>(t.offset_us));
        else std::snprintf(b, sizeof b, " [%zu] %s", i, t.mime.c_str());
        desc += b;
    }
    NXLOG_I("movie", "opened %zu bytes, %.3f s, %zu tracks:%s", size, e->duration_us / 1e6, e->tr.size(), desc.c_str());
    return true;
}

// The output settings of a track's reader: NV12 (MF: SetCurrentMediaType NV12), in the range the stream is coded in; PCM16 interleaved at the
// track's rate and channels (`exact`, up to stereo) or as the decoder makes it (the fallback, MF's second SetCurrentMediaType). Other tracks
// pass their samples through.
NSDictionary* output_settings(const ReaderSpec& s, bool exact) {
    if (s.kind == 0) {
        const OSType pf = (s.colorspace & 1) ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
        return @{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey: @(pf)};
    }
    if (s.kind != 1) return nil;
    NSMutableDictionary* d = [@{AVFormatIDKey: @(kAudioFormatLinearPCM), AVLinearPCMBitDepthKey: @16, AVLinearPCMIsFloatKey: @NO,
                                AVLinearPCMIsBigEndianKey: @NO, AVLinearPCMIsNonInterleaved: @NO} mutableCopy];
    if (exact && s.rate > 0 && s.ch > 0 && s.ch <= 2) {
        d[AVSampleRateKey] = @(s.rate);
        d[AVNumberOfChannelsKey] = @(s.ch);
    }
    return d;
}

// A started reader of one track from `from_us` (presentation time) on. False when AVFoundation cannot decode the track (`out.why`).
bool make_reader(AVURLAsset* asset, const ReaderSpec& s, int64_t from_us, Rd& out) {
    out.r = nil;
    out.o = nil;
    out.why.clear();
    for (int attempt = 0; attempt < (s.kind == 1 ? 2 : 1); attempt++) {
        NSError* err = nil;
        AVAssetReader* r = [[AVAssetReader alloc] initWithAsset:asset error:&err];
        if (!r) { out.why = "AVAssetReader: " + ns_error(err); return false; }
        if (from_us > 0) r.timeRange = CMTimeRangeMake(CMTimeMake(from_us, 1000000), kCMTimePositiveInfinity);
        AVAssetReaderTrackOutput* o = [[AVAssetReaderTrackOutput alloc] initWithTrack:s.at outputSettings:output_settings(s, attempt == 0)];
        o.alwaysCopiesSampleData = NO;
        if (![r canAddOutput:o]) { out.why = "the output settings are refused"; continue; }
        [r addOutput:o];
        if (![r startReading]) { out.why = "startReading: " + ns_error(r.error); continue; }
        out.r = r;
        out.o = o;
        return true;
    }
    return false;
}

// Cancels the reader of a track (worker thread, or the teardown after the join).
void drop_reader(Rd& r) {
    if (r.r && r.r.status == AVAssetReaderStatusReading) [r.r cancelReading];
    r.r = nil;
    r.o = nil;
}

// Makes new readers at `us` for the tracks in `sel` (index, Track::gen, spec). Worker thread, without e->m. False when one of them failed.
bool reposition(Ext* e, const std::vector<std::pair<size_t, std::pair<uint32_t, ReaderSpec>>>& sel, int64_t us) {
    bool ok = true;
    for (const auto& s : sel) {
        Rd& r = e->rd[s.first];
        drop_reader(r);
        r.gen = s.second.first;
        if (!make_reader(e->asset, s.second.second, us, r)) {
            ok = false;
            NXLOG_W("movie", "track %zu: no reader at %.3f s: %s", s.first, us / 1e6, r.why.c_str());
        }
    }
    return ok;
}

ReaderSpec spec_of(const Track& t) {
    ReaderSpec s;
    s.at = t.at;
    s.kind = t.kind;
    s.rate = t.rate;
    s.ch = t.ch;
    s.colorspace = t.colorspace;
    return s;
}

// The selected tracks with their generation and reader spec (`stale_only`: only those whose reader was made for another generation). Caller holds e->m.
std::vector<std::pair<size_t, std::pair<uint32_t, ReaderSpec>>> selected_locked(Ext* e, bool stale_only) {
    std::vector<std::pair<size_t, std::pair<uint32_t, ReaderSpec>>> v;
    for (size_t i = 0; i < e->tr.size(); i++) {
        const Track& t = e->tr[i];
        if (!t.selected || (stale_only && e->rd[i].gen == t.gen)) continue;
        v.push_back({i, {t.gen, spec_of(t)}});
    }
    return v;
}

// Checks that AVFoundation decodes the track (MF: SetCurrentMediaType to NV12 / PCM16): a reader is made and dropped again; the worker makes
// its own once reading begins.
int ext_select_locked(Ext* e, int i, bool on) {
    if (!e->asset || i < 0 || i >= static_cast<int>(e->tr.size())) return ST_INVALID;
    Track& t = e->tr[static_cast<size_t>(i)];
    if (e->reading) NXLOG_W("movie", "track %d selected after reading began: the stream may start late", i);
    if (on && t.kind != 2) {
        ReaderSpec s;
        {
            std::lock_guard<std::mutex> lk(e->m);
            s = spec_of(t);
        }
        Rd probe;
        const bool ok = make_reader(e->asset, s, 0, probe);
        drop_reader(probe);
        if (!ok) {
            NXLOG_W("movie", "track %d (%s): no AVFoundation decoder path (%s); left unselected", i, t.mime.c_str(), probe.why.c_str());
            return ST_UNSUPPORTED;
        }
    }
    {
        std::lock_guard<std::mutex> lk(e->m);
        t.selected = on;
        t.gen++;
        e->nx.clear();
        e->ahead = 0;
        e->ahead_tried = false;
        t.eos = false;
        t.failed = false;
        t.q.clear();
        t.last_us = INT64_MIN;
        e->cur_valid = false;
    }
    e->cv_work.notify_all();
    NXLOG_I("movie", "%s track %d (%s)", on ? "selected" : "unselected", i, t.mime.c_str());
    return ST_OK;
}

void worker_main(Ext* e) {
    struct Alive {   // counts this thread while it runs, whatever way it leaves
        Alive() { g_media_workers.add(1); }
        ~Alive() { g_media_workers.sub(1); }
    } alive;
    log_set_thread_name("movie-read");
    std::unique_lock<std::mutex> lk(e->m);
    while (!e->stop) @autoreleasepool {
        if (e->seek_pending) {
            const int64_t us = e->seek_us;
            const auto sel = selected_locked(e, false);
            lk.unlock();
            const bool ok = reposition(e, sel, us);
            lk.lock();
            for (Track& t : e->tr) { t.q.clear(); t.eos = false; t.failed = false; t.last_us = INT64_MIN; }
            e->nx.clear();
            e->ahead = 0;
            e->ahead_tried = false;
            e->cur_valid = false;
            e->base_us = us;
            e->read_since_base = 0;
            e->seek_pending = false;
            NXLOG_I("movie", "SeekTo(%.3f s) %s", us / 1e6, ok ? "ok" : "failed");
            e->cv_data.notify_all();
            continue;
        }
        if (g_lookahead && e->reading && e->ahead == 0 && !e->ahead_tried && !e->seek_pending) {
            bool any = false, all_done = true;
            for (const Track& t : e->tr) {
                if (!t.selected) continue;
                any = true;
                if (!t.eos || t.failed) all_done = false;
            }
            if (any && all_done) {
                e->ahead_tried = true;
                e->ahead = 1;
                const auto sel = selected_locked(e, false);
                lk.unlock();
                const bool ok = reposition(e, sel, 0);
                lk.lock();
                if (e->ahead == 1 && ok && !e->seek_pending && !e->stop) {
                    e->nx.assign(e->tr.size(), Ext::Ahead());
                    e->ahead = 2;
                    NXLOG_D("movie", "end of stream: the start is being decoded ahead for a loop");
                } else {
                    e->ahead = 0;
                }
                e->cv_data.notify_all();
                continue;
            }
        }
        if (e->reading) {
            // readers for the tracks (re)selected since the last ones were made: together at the readers' position when nothing was read since,
            // else (a track selected after reading began) where the other tracks are
            const auto stale = selected_locked(e, true);
            if (!stale.empty()) {
                int64_t from = e->base_us;
                if (e->read_since_base) {
                    for (size_t i = 0; i < e->tr.size(); i++) {
                        const Track& t = e->tr[i];
                        if (t.selected && e->rd[i].gen == t.gen && t.last_us != INT64_MIN) from = std::max(from, t.last_us);
                    }
                }
                lk.unlock();
                reposition(e, stale, from);
                lk.lock();
                continue;
            }
        }
        const bool ah_read = e->ahead == 2;          // the sample about to be read belongs to the look-ahead
        const uint32_t swaps0 = e->swaps;
        const bool ah_pick = ah_read;
        bool ah = ah_pick;
        Track* pick = nullptr;
        for (Track& t : e->tr) {
            if (!e->reading) break;
            const size_t k = static_cast<size_t>(&t - e->tr.data());
            const bool t_eos = ah ? e->nx[k].eos : t.eos, t_failed = ah ? e->nx[k].failed : t.failed;
            const size_t qn = ah ? e->nx[k].q.size() : t.q.size();
            const int64_t last = ah ? e->nx[k].last_us : t.last_us;
            if (!t.selected || t_eos || t_failed || qn >= t.limit()) continue;
            if (!pick || last < (ah ? e->nx[static_cast<size_t>(pick - e->tr.data())].last_us : pick->last_us)) pick = &t;
        }
        if (!pick) { e->cv_work.wait(lk); continue; }
        const int kind = pick->kind;
        const int rate = pick->rate, ch = pick->ch;
        const int64_t start_us = pick->start_us;
        const uint32_t gen = pick->gen;
        const size_t ti = static_cast<size_t>(pick - e->tr.data());
        lk.unlock();
        Rd& r = e->rd[ti];
        SampleRef s;
        bool fail = false, end = false;
        std::string why;
        if (!r.r || !r.o) {
            fail = true;
            why = r.why.empty() ? "no reader" : r.why;
        } else if (CMSampleBufferRef sb = [r.o copyNextSampleBuffer]) {
            s = SampleRef(sb);
        } else if (r.r.status == AVAssetReaderStatusFailed) {
            fail = true;
            why = ns_error(r.r.error);
        } else {
            end = true;                                       // completed (or cancelled)
        }
        lk.lock();
        if (e->seek_pending || e->stop) continue;   // the sample belongs to the old position
        if (e->tr[ti].gen != gen) continue;         // (un)selected while the read ran: the sample belongs to the old reader
        if ((e->ahead == 2) != ah_pick) {
            // the look-ahead changed while the read ran: swapped in (the sample is the next one of the queue that replaced it) or dropped (it belongs to a position nobody wants)
            if (ah_pick && e->swaps != swaps0) ah = false;
            else continue;
        }
        Track& t = e->tr[ti];
        std::deque<Item>& Q = ah ? e->nx[ti].q : t.q;
        bool& EOS = ah ? e->nx[ti].eos : t.eos;
        bool& FAIL = ah ? e->nx[ti].failed : t.failed;
        int64_t& LAST = ah ? e->nx[ti].last_us : t.last_us;
        if (fail) {
            s_read_errors++;
            NXLOG_W("movie", "copyNextSampleBuffer(track %zu) failed (%s) after %llu samples: treated as its end", ti, why.c_str(),
                    static_cast<unsigned long long>(e->samples_read));
            FAIL = EOS = true;
            e->cv_data.notify_all();
            continue;
        }
        const int64_t pts = s ? to_us(CMSampleBufferGetPresentationTimeStamp(s.get())) : 0;
        if (g_log) NXLOG_I("movie", "copyNextSampleBuffer(track %zu): %s ts %lld sample %p", ti, end ? "end" : "ok", static_cast<long long>(pts), static_cast<void*>(s.get()));
        if (end) {
            NXLOG_D("movie", "track %zu: end of stream after %zu queued", ti, Q.size());
            EOS = true;
            e->cv_data.notify_all();
            continue;
        }
        // a buffer without media (a marker the reader may hand out, EmptyMedia / EditBoundary: no sample, no picture, no PCM) is skipped
        if (CMSampleBufferGetNumSamples(s.get()) == 0) continue;
        if (kind == 0 && !CMSampleBufferGetImageBuffer(s.get())) continue;
        if (kind == 1 && pcm_bytes(s.get()) == 0) continue;
        Item it;
        it.us = pts;
        it.s = std::move(s);
        e->samples_read++;
        s_samples++;
        // What comes before the track's start is cut: audio before 0 (priming the edit list did not trim), and the black frame / silence the
        // reader makes for a leading empty edit (Media Foundation's samples of a delayed track simply start at the end of the empty edit). A
        // sample that starts within 0.5 ms of a delayed start is the first real one (the two times are rounded differently).
        bool keep = true;
        const int64_t early = start_us - it.us;
        if (early > 0 && (start_us == 0 || early > 500)) {
            if (kind == 1 && rate > 0 && ch > 0) {                 // audio: cut the bytes before the start
                const uint32_t len = pcm_bytes(it.s.get());
                const uint64_t frame_bytes = static_cast<uint64_t>(ch) * 2;
                const uint64_t drop = (static_cast<uint64_t>(early) * static_cast<uint64_t>(rate) + 999999) / 1000000 * frame_bytes;
                if (drop >= len) keep = false;
                else { it.skip = static_cast<uint32_t>(drop); it.us = start_us; }
            } else {
                keep = false;                                      // a video frame the edit list cuts, or the filler of an empty edit
            }
        }
        if (keep) {
            LAST = it.us;
            Q.push_back(std::move(it));
            e->read_since_base++;
        }
        e->cv_data.notify_all();
    }
}

void ext_start_worker(Ext* e) {
    if (e->started) return;
    e->started = true;
    e->worker = std::thread(worker_main, e);
}

void ext_destroy(Ext* e) {
    {
        std::lock_guard<std::mutex> lk(e->m);
        e->stop = true;
    }
    e->cv_work.notify_all();
    e->cv_data.notify_all();
    if (e->worker.joinable()) e->worker.join();
    @autoreleasepool {
        for (Rd& r : e->rd) drop_reader(r);
        e->rd.clear();
        [e->asset cancelLoading];
        e->asset = nil;
    }
}

// The game's destructor call returns at once (S21): the worker may be inside a copyNextSampleBuffer and tearing the readers down stops their
// decoders, which would otherwise stall the game thread that closes the movie. A short-lived thread does the join and the release.
void ext_destroy_async(Ext* e) {
    {
        std::lock_guard<std::mutex> lk(e->m);
        e->stop = true;
    }
    e->cv_work.notify_all();
    e->cv_data.notify_all();
    try {
        std::thread([e] {
            log_set_thread_name("movie-close");
            @autoreleasepool {
                ext_destroy(e);
                delete e;
            }
        }).detach();
    } catch (...) {
        @autoreleasepool {
            ext_destroy(e);
            delete e;
        }
    }
}

// True when every selected track that has not ended has a decoded sample queued. Caller holds e->m.
bool ext_ready_locked(const Ext* e) {
    if (e->seek_pending) return false;
    for (const Track& t : e->tr)
        if (t.selected && t.q.empty() && !t.eos && !t.failed) return false;
    return true;
}

// The extractor's current sample: the one with the smallest timestamp among the next samples of the selected tracks. Waits (bounded)
// until every selected track that has not ended has a decoded sample queued. Caller holds `lk` on e->m.
int ext_fill(Ext* e, std::unique_lock<std::mutex>& lk) {
    if (e->cur_valid) return ST_OK;
    if (!e->reading) { e->reading = true; e->cv_work.notify_all(); }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    for (;;) {
        bool ready = !e->seek_pending;
        bool any_selected = false;
        for (const Track& t : e->tr) {
            if (!t.selected) continue;
            any_selected = true;
            if (t.q.empty() && !t.eos && !t.failed) ready = false;
        }
        if (!any_selected) return ST_END;
        if (ready) break;
        if (e->cv_data.wait_until(lk, deadline) == std::cv_status::timeout) {
            NXLOG_W("movie", "the worker did not deliver a sample in %d ms", kWaitMs);
            return ST_FAILED;
        }
    }
    int best = -1;
    for (size_t i = 0; i < e->tr.size(); i++) {
        const Track& t = e->tr[i];
        if (!t.selected || t.q.empty()) continue;
        if (best < 0 || t.q.front().us < e->tr[static_cast<size_t>(best)].q.front().us) best = static_cast<int>(i);
    }
    if (best < 0) return ST_END;
    e->cur_valid = true;
    e->cur_track = best;
    e->cur_us = e->tr[static_cast<size_t>(best)].q.front().us;
    return ST_OK;
}

// ---- Extractor handlers --------------------------------------------------------------------------------------------------------
void ext_ctor(::Ctx* c) {                    // Extractor(ContainerType, CacheSize): 1 mp4, 2 mkv, 3 webm
    Ext* e = new Ext();
    e->container = static_cast<int>(A1);
    wr64(A0, kObjMarker);
    wr64(A0 + 8, reinterpret_cast<uint64_t>(e));
    s_extractors_created++;
    NXLOG_I("movie", "Extractor created (container type %d)", e->container);
}
void ext_dtor(::Ctx* c) {
    Ext* e = ext_of(A0);
    if (!e) return;
    wr64(A0 + 8, 0);
    wr64(A0, 0);
    NXLOG_I("movie", "Extractor destroyed (%llu samples read)", static_cast<unsigned long long>(e->samples_read));
    if (g_async_close) {
        ext_destroy_async(e);
    } else {
        @autoreleasepool {
            ext_destroy(e);
            delete e;
        }
    }
    s_extractors_destroyed++;
}

void ext_set_source(::Ctx* c) {              // SetDataSource(ClientStreamReader*, const char* path)
    Ext* e = ext_of(A0);
    const uint64_t reader = A1;
    const char* path = reinterpret_cast<const char*>(A2);
    if (!e || !reader) { RET(ST_INVALID); return; }
    if (path) e->name = path;
    const uint64_t vt = rd64(reader);
    uint64_t handle = 0;
    const uint64_t ok = gcall(c, rd64(vt + 0x10), reader, A2, reinterpret_cast<uint64_t>(&handle), 0, 0);
    if (!(ok & 1)) { NXLOG_W("movie", "SetDataSource: the game's reader could not open the movie"); RET(ST_FAILED); return; }
    int64_t size = 0;
    gcall(c, rd64(vt + 0x28), reader, handle, reinterpret_cast<uint64_t>(&size), 0, 0);
    if (size <= 0 || size > (int64_t{2} << 30)) {
        NXLOG_W("movie", "SetDataSource: size %lld", static_cast<long long>(size));
        gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0);
        RET(ST_FAILED);
        return;
    }
    std::unique_ptr<unsigned char, decltype(&std::free)> data(static_cast<unsigned char*>(std::malloc(static_cast<size_t>(size))), &std::free);
    if (!data) { gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0); RET(ST_FAILED); return; }
    const auto t0 = std::chrono::steady_clock::now();
    int64_t got = 0;
    while (got < size) {
        const uint64_t chunk = std::min<uint64_t>(static_cast<uint64_t>(size - got), 8u << 20);
        const int64_t n = static_cast<int64_t>(gcall(c, rd64(vt + 0x18), reader, handle, static_cast<uint64_t>(got), reinterpret_cast<uint64_t>(data.get() + got), chunk));
        if (n <= 0) break;
        got += n;
    }
    gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0);
    NXLOG_I("movie", "SetDataSource: read %lld of %lld bytes through the game's reader in %.2f s", static_cast<long long>(got), static_cast<long long>(size),
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    if (got != size) { RET(ST_FAILED); return; }
    if (size < 12 || std::memcmp(data.get() + 4, "ftyp", 4) != 0) NXLOG_W("movie", "the movie data does not start with an MP4 'ftyp' box");
    bool opened = false;
    @autoreleasepool {
        NSData* bytes = [[NSData alloc] initWithBytesNoCopy:data.release() length:static_cast<NSUInteger>(size) freeWhenDone:YES];
        opened = ext_open(e, bytes);
    }
    if (!opened) { RET(ST_FAILED); return; }
    ext_start_worker(e);
    RET(ST_OK);
}

void ext_track_count(::Ctx* c) {
    Ext* e = ext_of(A0);
    if (!e || !A1) { RET(ST_INVALID); return; }
    wr32(A1, static_cast<uint32_t>(e->asset ? e->tr.size() : 0));
    RET(e->asset ? ST_OK : ST_FAILED);
}

void ext_track_config(::Ctx* c) {            // GetTrackConfiguration(int track, MediaData* out)
    Ext* e = ext_of(A0);
    const int i = static_cast<int>(A1);
    MD* m = md_of(A2);
    if (!e || !m || !e->asset || i < 0 || i >= static_cast<int>(e->tr.size())) { RET(ST_INVALID); return; }
    const Track& t = e->tr[static_cast<size_t>(i)];
    m->e.clear();
    m->set_str("mime", t.mime.c_str());
    m->set_i64("durationUs", e->duration_us);
    m->set_i32("max-input-size", 0x10000);
    if (t.bitrate > 0) m->set_i32("bit-rate", t.bitrate);
    if (t.kind == 0) {
        m->set_i32("width", t.w);
        m->set_i32("height", t.h);
        m->set_i32("frame-rate", static_cast<int>(t.fps + 0.5));
        m->set_i32("nn-colorspace", t.colorspace);
    } else if (t.kind == 1) {
        m->set_i32("sample-rate", t.rate);
        m->set_i32("channel-count", t.ch);
    }
    m->set_ptr("nn-extractor", reinterpret_cast<uint64_t>(e));   // lets the decoder find its source
    m->set_i32("nn-track", i);
    RET(ST_OK);
}

// Called when a track has just been selected (S21). Once every audio and video track is selected, decoding starts here and the call waits for
// the first decoded samples, on the thread that sets the movie up (the game's loader), so that the first GetSampleTime of the game's main thread finds
// them queued. The wait is bounded. With only some of the tracks selected nothing happens: a track selected after reading began would start late.
void ext_preroll(Ext* e) {
    if (!g_preroll) return;
    std::unique_lock<std::mutex> lk(e->m);
    if (e->reading) return;
    bool any = false;
    for (const Track& t : e->tr) {
        if (t.kind == 2) continue;
        if (!t.selected) return;
        any = true;
    }
    if (!any) return;
    const auto t0 = std::chrono::steady_clock::now();
    e->reading = true;
    e->cv_work.notify_all();
    const auto deadline = t0 + std::chrono::milliseconds(2000);
    while (!ext_ready_locked(e)) {
        if (e->cv_data.wait_until(lk, deadline) == std::cv_status::timeout) {
            NXLOG_W("movie", "preroll: no decoded sample after 2 s");
            return;
        }
    }
    NXLOG_I("movie", "preroll: first samples decoded in %.1f ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void ext_select(::Ctx* c) {
    Ext* e = ext_of(A0);
    if (!e) { RET(ST_INVALID); return; }
    int r;
    @autoreleasepool {
        r = ext_select_locked(e, static_cast<int>(A1), true);
    }
    if (r == ST_OK) ext_preroll(e);
    RET(r);
}

void ext_seek(::Ctx* c) {                    // SeekTo(int64 timeUs)
    Ext* e = ext_of(A0);
    if (!e || !e->asset) { RET(ST_INVALID); return; }
    int64_t us = static_cast<int64_t>(A1);
    std::unique_lock<std::mutex> lk(e->m);
    if (us < 0) us = 0;                            // the target is in presentation time, the readers' clock (no edit offset to take off)
    if (us == 0 && e->ahead != 0) {                // a loop: the start was decoded ahead
        const auto wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
        while (e->ahead == 1 && e->cv_data.wait_until(lk, wait_until) != std::cv_status::timeout) {}
        if (e->ahead == 2 && !e->seek_pending && e->nx.size() == e->tr.size()) {
            size_t n = 0;
            for (size_t i = 0; i < e->tr.size(); i++) {
                Track& t = e->tr[i];
                Ext::Ahead& a = e->nx[i];
                t.q = std::move(a.q);
                t.eos = a.eos;
                t.failed = a.failed;
                t.last_us = a.last_us;
                n += t.q.size();
            }
            e->nx.clear();
            e->ahead = 0;
            e->ahead_tried = false;
            e->swaps++;
            e->cur_valid = false;
            e->base_us = 0;
            e->read_since_base = n;
            NXLOG_I("movie", "SeekTo(0): loop restart from the samples decoded ahead (%zu queued)", n);
            lk.unlock();
            e->cv_work.notify_all();
            RET(ST_OK);
            return;
        }
    }
    e->nx.clear();
    e->ahead = 0;
    e->ahead_tried = false;
    e->seek_us = us;
    e->seek_pending = true;
    e->cur_valid = false;
    lk.unlock();
    e->cv_work.notify_all();
    lk.lock();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWaitMs);
    while (e->seek_pending) {
        if (e->cv_data.wait_until(lk, deadline) == std::cv_status::timeout) { RET(ST_FAILED); return; }
    }
    RET(ST_OK);
}

void ext_track_for_data(::Ctx* c) {          // GetTrackIndexForAvailableData(size_t* track)
    Ext* e = ext_of(A0);
    if (!e || !A1) { RET(ST_INVALID); return; }
    std::unique_lock<std::mutex> lk(e->m);
    const int r = ext_fill(e, lk);
    if (r == ST_OK) wr64(A1, static_cast<uint64_t>(e->cur_track));
    RET(r);
}

struct Token {
    uint64_t magic;
    CMSampleBufferRef s;   // one reference, owned by the token until a decoder takes it
    int64_t ts;
    uint32_t skip;
    uint32_t pad;
};
static_assert(sizeof(Token) <= kSlotBytes, "token must fit a decoder input slot");

void ext_read(::Ctx* c) {                    // Read(Buffer* out): the token for the current sample goes into the decoder's input slot
    Ext* e = ext_of(A0);
    const uint64_t b = A1;
    if (!e || !b) { RET(ST_INVALID); return; }
    std::unique_lock<std::mutex> lk(e->m);
    int r = ext_fill(e, lk);
    if (r == ST_OK) {
        const uint64_t base = rd64(b + B_BASE), cap = rd64(b + B_CAP);
        Track& t = e->tr[static_cast<size_t>(e->cur_track)];
        if (base && cap >= sizeof(Token) && !t.q.empty()) {
            const Item& it = t.q.front();
            CFRetain(it.s.get());
            Token tok{kTokenMagic, it.s.get(), it.us, it.skip, 0};
            std::memcpy(reinterpret_cast<void*>(base), &tok, sizeof tok);
            wr32(b + B_OFFSET, 0);
            wr32(b + B_SIZE, static_cast<uint32_t>(sizeof tok));
            wr64(b + B_I64, static_cast<uint64_t>(it.us));
        } else {
            r = ST_FAILED;
        }
    }
    RET(r);
}

void ext_advance(::Ctx* c) {
    Ext* e = ext_of(A0);
    if (!e) { RET(ST_INVALID); return; }
    {
        std::lock_guard<std::mutex> lk(e->m);
        if (e->cur_valid) {
            Track& t = e->tr[static_cast<size_t>(e->cur_track)];
            if (!t.q.empty()) t.q.pop_front();
            e->cur_valid = false;
        }
    }
    e->cv_work.notify_all();
    RET(ST_OK);
}

void ext_sample_time(::Ctx* c) {             // GetSampleTime(int64* timeUs)
    Ext* e = ext_of(A0);
    if (!e || !A1) { RET(ST_INVALID); return; }
    std::unique_lock<std::mutex> lk(e->m);
    const int r = ext_fill(e, lk);
    wr64(A1, static_cast<uint64_t>(r == ST_OK ? e->cur_us : int64_t{-1}));
    RET(r);
}

// ---- the decoder ---------------------------------------------------------------------------------------------------------------
constexpr int MAX_IN = 16, MAX_OUT = 64;
enum { IN_CODEC = 0, IN_OFFERED, IN_CLIENT, IN_DECODING };
enum { OUT_FREE = 0, OUT_READY, OUT_CLIENT };
struct OutBuf {
    int state = OUT_FREE;
    SampleRef s;
    uint32_t off = 0, len = 0;         // audio: the bytes of the sample this buffer carries
    int64_t ts = 0;
    uint32_t flags = 0;
};
struct DecItem {
    SampleRef s;
    uint32_t off = 0, remaining = 0;   // audio: the bytes still to hand out
    int64_t ts = 0;
    uint32_t flags = 0;
    int in_idx = 0;
};
struct Dec;
std::mutex g_decs_mu;                    // registry of live decoders (see g_exts_mu)
std::set<Dec*> g_decs;

struct Dec {
    Dec() { std::lock_guard<std::mutex> lk(g_decs_mu); g_decs.insert(this); }
    ~Dec() { std::lock_guard<std::mutex> lk(g_decs_mu); g_decs.erase(this); }
    Dec(const Dec&) = delete;
    Dec& operator=(const Dec&) = delete;
    std::mutex m;
    int type = 0, fmt = 0, mode = 0;
    bool is_video = false;
    int w = 0, h = 0, rate = 0, ch = 0, colorspace = 2;
    int nin = 8, nout = 16;
    bool started = false;
    unsigned char in_state[MAX_IN] = {};
    int in_list[MAX_IN] = {}, in_n = 0;
    std::deque<DecItem> dec_q;
    OutBuf out[MAX_OUT];
    int out_list[MAX_OUT] = {}, out_n = 0;
    std::unique_ptr<unsigned char[]> mem;
    uint64_t events = 0;
    uint64_t in_count = 0, copied = 0, dropped = 0;
    bool warned_small = false, warned_token = false, warned_format = false;
};
Dec* dec_of(uint64_t guest) { return guest && rd64(guest) == kObjMarker ? reinterpret_cast<Dec*>(rd64(guest + 8)) : nullptr; }

// ---- gauges (include/nxrt/gauge.h) ----
// Queued samples over every live extractor (kind 0 video, 1 audio): the registry lock first, then each extractor's m.
int64_t queued_samples(int kind) {
    std::lock_guard<std::mutex> lk(g_exts_mu);
    int64_t n = 0;
    for (Ext* e : g_exts) {
        std::lock_guard<std::mutex> el(e->m);
        for (const Track& t : e->tr)
            if (t.kind == kind) n += static_cast<int64_t>(t.q.size());
    }
    return n;
}
// Input items the decoders hold for the transforms, over every live decoder.
int64_t decoder_queue() {
    std::lock_guard<std::mutex> lk(g_decs_mu);
    int64_t n = 0;
    for (Dec* d : g_decs) {
        std::lock_guard<std::mutex> dl(d->m);
        n += static_cast<int64_t>(d->dec_q.size());
    }
    return n;
}
// Removes its gauge when this file's statics are torn down (declared after the registries, so it goes first).
struct GaugeHook {
    const char* name;
    GaugeHook(const char* n, std::function<int64_t()> f) : name(n) { nxrt::gauge_register(n, std::move(f)); }
    ~GaugeHook() { nxrt::gauge_unregister(name); }
};
GaugeHook gauge_queued_video("media.queued_video", [] { return queued_samples(0); });
GaugeHook gauge_queued_audio("media.queued_audio", [] { return queued_samples(1); });
GaugeHook gauge_decoder_queue("media.decoder_queue", [] { return decoder_queue(); });
GaugeHook gauge_extractors("media.extractors_open", [] {
    return static_cast<int64_t>(s_extractors_created.load()) - static_cast<int64_t>(s_extractors_destroyed.load());
});
GaugeHook gauge_decoders("media.decoders_open", [] {
    return static_cast<int64_t>(s_decoders_created.load()) - static_cast<int64_t>(s_decoders_destroyed.load());
});
GaugeHook gauge_video_frames("media.video_frames", [] { return static_cast<int64_t>(s_frames.load()); });   // frames handed to the game
GaugeHook gauge_audio_blocks("media.audio_blocks", [] { return static_cast<int64_t>(s_audio_blocks.load()); });
GaugeHook gauge_samples_read("media.samples_read", [] { return static_cast<int64_t>(s_samples.load()); });   // samples read from the source
GaugeHook gauge_read_errors("media.read_errors", [] { return static_cast<int64_t>(s_read_errors.load()); });

void dec_release_all(Dec* d) {
    for (int i = 0; i < d->nout; i++) d->out[i] = OutBuf();
    for (int i = 0; i < d->nin; i++) {
        Token t;
        std::memcpy(&t, d->mem.get() + static_cast<size_t>(i) * kSlotBytes, sizeof t);
        if (t.magic == kTokenMagic && t.s) CFRelease(t.s);         // read into the slot but never sent
        std::memset(d->mem.get() + static_cast<size_t>(i) * kSlotBytes, 0, kSlotBytes);
        d->in_state[i] = IN_CODEC;
    }
    d->dec_q.clear();
    d->in_n = d->out_n = 0;
}

// The codec's own progress: what waits in the decode queue moves to a free output buffer and its input buffer is offered again.
void dec_pump(Dec* d) {
    while (!d->dec_q.empty()) {
        int o = -1;
        for (int i = 0; i < d->nout; i++) if (d->out[i].state == OUT_FREE) { o = i; break; }
        if (o < 0) break;
        DecItem& it = d->dec_q.front();
        OutBuf& ob = d->out[o];
        ob.s = it.s;
        ob.ts = it.ts;
        ob.flags = it.flags;
        bool done = true;
        if (!d->is_video && it.s) {
            const uint32_t frame_bytes = static_cast<uint32_t>(std::max(d->ch, 1)) * 2;
            uint32_t n = std::min<uint32_t>(it.remaining, static_cast<uint32_t>(kAudioChunk) / frame_bytes * frame_bytes);
            ob.off = it.off;
            ob.len = n;
            it.off += n;
            it.remaining -= n;
            if (d->rate > 0) it.ts += static_cast<int64_t>(n / frame_bytes) * 1000000 / d->rate;
            done = it.remaining == 0;
        }
        ob.state = OUT_READY;
        d->out_list[d->out_n++] = o;
        if (done) {
            const int in = it.in_idx;
            d->dec_q.pop_front();
            d->in_state[in] = IN_OFFERED;
            d->in_list[d->in_n++] = in;
        }
    }
}

void dec_ctor(::Ctx* c) {                    // Decoder(DecoderType, DecoderOutputFormat, DecoderMode, int priority)
    Dec* d = new Dec();
    d->type = static_cast<int>(A1);
    d->fmt = static_cast<int>(A2);
    d->mode = static_cast<int>(A3);
    d->is_video = d->type >= 1 && d->type <= 3;
    d->nin = 8;
    d->nout = d->is_video ? 16 : 64;
    d->mem.reset(new unsigned char[static_cast<size_t>(MAX_IN) * kSlotBytes]());
    wr64(A0, kObjMarker);
    wr64(A0 + 8, reinterpret_cast<uint64_t>(d));
    s_decoders_created++;
    NXLOG_I("movie", "Decoder created (type %d = %s, output format %d, mode %d)", d->type,
            d->type == 1 ? "H.264" : d->type == 2 ? "VP8" : d->type == 3 ? "VP9" : d->type == 4 ? "AAC" : d->type == 5 ? "Vorbis" : "?", d->fmt, d->mode);
    if (d->is_video && d->fmt != 4) NXLOG_W("movie", "output format %d requested for video: only 4 (NV12) is produced", d->fmt);
}
void dec_dtor(::Ctx* c) {
    Dec* d = dec_of(A0);
    if (!d) return;
    wr64(A0 + 8, 0);
    wr64(A0, 0);
    {
        std::lock_guard<std::mutex> lk(d->m);
        dec_release_all(d);
    }
    NXLOG_I("movie", "Decoder(%s) destroyed: %llu inputs, %llu buffers copied to the game, %llu refused", d->is_video ? "video" : "audio",
            static_cast<unsigned long long>(d->in_count), static_cast<unsigned long long>(d->copied), static_cast<unsigned long long>(d->dropped));
    delete d;
    s_decoders_destroyed++;
}

void dec_configure(::Ctx* c) {               // Configure(MediaData* format): the track configuration the extractor made
    Dec* d = dec_of(A0);
    MD* m = md_of(A1);
    if (!d || !m) { RET(ST_INVALID); return; }
    int32_t v = 0;
    if (m->get_i32("width", &v)) d->w = v;
    if (m->get_i32("height", &v)) d->h = v;
    if (m->get_i32("sample-rate", &v)) d->rate = v;
    if (m->get_i32("channel-count", &v)) d->ch = v;
    if (m->get_i32("nn-colorspace", &v)) d->colorspace = v;
    NXLOG_I("movie", "Decoder(%s) configured: %dx%d, %d Hz, %d ch", d->is_video ? "video" : "audio", d->w, d->h, d->rate, d->ch);
    RET(ST_OK);
}
void dec_register_events(::Ctx* c) {         // the game passes four null event pointers: everything is polled
    Dec* d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    d->events = A1;
    if (A1 && (rd64(A1) || rd64(A1 + 8))) NXLOG_W("movie", "Decoder client events with real event objects are not signalled (the game polls)");
    RET(ST_OK);
}

void fill_format(Dec* d, MD* m) {
    m->e.clear();
    if (d->is_video) {
        const int w = d->w, h = d->h;
        m->set_str("mime", "video/raw");
        m->set_i32("width", w); m->set_i32("height", h);
        m->set_i32("stride", w); m->set_i32("slice-height", h);
        m->set_i32("crop-width", w); m->set_i32("crop-height", h);
        m->set_i32("color-format", 21);                  // OMX_COLOR_FormatYUV420SemiPlanar = NV12
        m->set_i32("nv12-width", w); m->set_i32("nv12-height", h);
        m->set_i32("nv12-y-stride", w); m->set_i32("nv12-uv-stride", w);
        m->set_i32("nv12-y-offset", 0); m->set_i32("nv12-uv-offset", w * h);
        m->set_i32("nv12-colorspace", d->colorspace);
    } else {
        m->set_str("mime", "audio/raw");
        m->set_i32("sample-rate", d->rate); m->set_i32("channel-count", d->ch);
        m->set_i32("pcm-encoding", 2);                   // 16 bit
    }
}
void dec_get_format(::Ctx* c) {
    Dec* d = dec_of(A0);
    MD* m = md_of(A1);
    if (!d || !m) { RET(ST_INVALID); return; }
    fill_format(d, m);
    RET(ST_OK);
}
void dec_output_size(::Ctx* c) {             // GetOutputBufferSize(int* size, int* alignment)
    Dec* d = dec_of(A0);
    if (!d || !A1) { RET(ST_INVALID); return; }
    const int size = d->is_video ? d->w * d->h + 2 * ((d->w + 1) / 2) * ((d->h + 1) / 2) : kAudioChunk;
    wr32(A1, static_cast<uint32_t>(size));
    if (A2) wr32(A2, 16);
    RET(ST_OK);
}

void dec_start(::Ctx* c) {
    Dec* d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    if (!d->started) {
        dec_release_all(d);
        for (int i = 0; i < d->nin; i++) { d->in_state[i] = IN_OFFERED; d->in_list[i] = i; }
        d->in_n = d->nin;
        d->started = true;
    }
    RET(ST_OK);
}
void dec_stop(::Ctx* c) {
    Dec* d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    dec_release_all(d);
    d->started = false;
    RET(ST_OK);
}

void dec_acquire_input(::Ctx* c) {           // AcquireInputBufferIndex(int* index, int* moreAvailable)
    Dec* d = dec_of(A0);
    if (!d || !A1 || !A2) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    int found = -1, more = 0;
    if (d->in_n > 0) {
        found = d->in_list[0];
        std::memmove(d->in_list, d->in_list + 1, static_cast<size_t>(d->in_n - 1) * sizeof(int));
        more = --d->in_n;
        d->in_state[found] = IN_CLIENT;
    }
    wr32(A1, static_cast<uint32_t>(found));
    wr32(A2, static_cast<uint32_t>(more));
    RET(found >= 0 ? ST_OK : ST_NO_INPUT);
}
void dec_get_input(::Ctx* c) {               // GetInputBuffer(int index, Buffer* out)
    Dec* d = dec_of(A0);
    const int i = static_cast<int>(A1);
    const uint64_t b = A2;
    if (!d || !b || i < 0 || i >= d->nin) { RET(ST_INVALID); return; }
    wr64(b + B_BASE, reinterpret_cast<uint64_t>(d->mem.get() + static_cast<size_t>(i) * kSlotBytes));
    wr64(b + B_CAP, kSlotBytes);
    wr32(b + B_OFFSET, 0);
    wr32(b + B_SIZE, 0);
    RET(ST_OK);
}

void dec_send_input(::Ctx* c) {              // SendInputBufferForDecode(int index, int offset, int size, int64 timeUs, unsigned flags)
    Dec* d = dec_of(A0);
    const int i = static_cast<int>(A1), size = static_cast<int>(A3);
    int64_t ts = static_cast<int64_t>(A4);
    const uint32_t flags = static_cast<uint32_t>(A5);
    if (!d || i < 0 || i >= d->nin) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    SampleRef sample;
    uint32_t skip = 0;
    Token t;
    std::memcpy(&t, d->mem.get() + static_cast<size_t>(i) * kSlotBytes, sizeof t);
    std::memset(d->mem.get() + static_cast<size_t>(i) * kSlotBytes, 0, sizeof t);
    if (t.magic == kTokenMagic && t.s) {
        sample = SampleRef(t.s);
        skip = t.skip;
        if (ts <= 0) ts = t.ts;
    } else if (!(flags & FLAG_EOS) && !d->warned_token) {
        d->warned_token = true;
        NXLOG_W("movie", "Decoder(%s): input buffer %d holds no sample token (size %d): dropped", d->is_video ? "video" : "audio", i, size);
    }
    if (d->in_state[i] != IN_CLIENT) {           // an index voided by Stop: the codec refuses it
        d->dropped++;
        RET(ST_INVALID);
        return;
    }
    d->in_count++;
    const bool empty_audio = sample && !d->is_video && !(flags & FLAG_EOS) && pcm_bytes(sample.get()) <= skip;
    if ((!sample || empty_audio) && !(flags & FLAG_EOS)) {   // nothing to decode: the buffer is offered again
        d->dropped++;
        d->in_state[i] = IN_OFFERED;
        d->in_list[d->in_n++] = i;
        RET(ST_OK);
        return;
    }
    DecItem it;
    it.s = sample;
    it.ts = ts;
    it.flags = flags & FLAG_EOS;
    it.in_idx = i;
    if (sample && !d->is_video) {
        const uint32_t len = pcm_bytes(sample.get());
        it.off = std::min<uint32_t>(skip, len);
        it.remaining = len - it.off;
    }
    if (g_log) NXLOG_I("movie", "SendInput(%s) idx %d t=%.3f flags %x", d->is_video ? "video" : "audio", i, ts / 1e6, flags);
    d->in_state[i] = IN_DECODING;
    d->dec_q.push_back(std::move(it));
    dec_pump(d);
    RET(ST_OK);
}

void dec_acquire_output(::Ctx* c) {          // AcquireOutputBufferIndex(int* index, int64* timeUs, unsigned* flags, int* moreAvailable)
    Dec* d = dec_of(A0);
    if (!d || !A1 || !A2 || !A3 || !A4) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    int r = ST_NO_OUTPUT, idx = -1, more = 0;
    int64_t ts = 0;
    uint32_t flags = 0;
    if (d->out_n > 0) {
        idx = d->out_list[0];
        std::memmove(d->out_list, d->out_list + 1, static_cast<size_t>(d->out_n - 1) * sizeof(int));
        more = --d->out_n;
        OutBuf& s = d->out[idx];
        s.state = OUT_CLIENT;
        ts = s.ts;
        flags = s.flags;
        r = ST_OK;
    }
    wr32(A1, static_cast<uint32_t>(idx));
    wr64(A2, static_cast<uint64_t>(ts));
    wr32(A3, flags);
    wr32(A4, static_cast<uint32_t>(more));
    RET(r);
}

// Copies the decoded sample of an output slot into the game's buffer; returns the bytes written.
size_t copy_out(Dec* d, const OutBuf& s, unsigned char* dst, size_t cap) {
    if (!s.s) return 0;
    size_t written = 0;
    if (d->is_video) {
        CVImageBufferRef ib = CMSampleBufferGetImageBuffer(s.s.get());
        if (!ib || CFGetTypeID(ib) != CVPixelBufferGetTypeID()) return 0;
        CVPixelBufferRef pb = static_cast<CVPixelBufferRef>(ib);
        const OSType pf = CVPixelBufferGetPixelFormatType(pb);
        if (!CVPixelBufferIsPlanar(pb) || CVPixelBufferGetPlaneCount(pb) < 2 ||
            (pf != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange && pf != kCVPixelFormatType_420YpCbCr8BiPlanarFullRange)) {
            if (!d->warned_format) {
                d->warned_format = true;
                NXLOG_W("movie", "the decoder made pixel format %c%c%c%c, not NV12: frame dropped", char(pf >> 24), char(pf >> 16), char(pf >> 8), char(pf));
            }
            return 0;
        }
        if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return 0;
        // A pixel buffer keeps each plane at its own address with its own row pitch (VideoToolbox pads rows and pads the luma plane to whole
        // macroblocks: 1600x900 comes as 1600x912 luma rows), so the planes are copied row by row into the game's packed NV12: w*h luma,
        // then (h+1)/2 rows of w interleaved CbCr bytes.
        const uint8_t* y = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 0));
        const uint8_t* uv = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pb, 1));
        const size_t ypitch = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvpitch = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
        const size_t yrows = CVPixelBufferGetHeightOfPlane(pb, 0), uvrows_have = CVPixelBufferGetHeightOfPlane(pb, 1);
        const int w = d->w, h = d->h;
        const size_t ysize = static_cast<size_t>(w) * h, uvrows = static_cast<size_t>((h + 1) / 2), need = ysize + static_cast<size_t>(w) * uvrows;
        if (cap < need) {
            if (!d->warned_small) { d->warned_small = true; NXLOG_W("movie", "the game's buffer of %zu bytes is smaller than a %dx%d NV12 frame (%zu)", cap, w, h, need); }
        } else if (y && uv) {
            const size_t yw = std::min<size_t>(static_cast<size_t>(w), ypitch), uvw = std::min<size_t>(static_cast<size_t>(w), uvpitch);
            const size_t yn = std::min<size_t>(static_cast<size_t>(h), yrows), uvn = std::min(uvrows, uvrows_have);
            for (size_t r = 0; r < yn; r++) std::memcpy(dst + r * static_cast<size_t>(w), y + r * ypitch, yw);
            for (size_t r = 0; r < uvn; r++) std::memcpy(dst + ysize + r * static_cast<size_t>(w), uv + r * uvpitch, uvw);
            written = need;
        }
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    } else {
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(s.s.get());
        const size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
        const size_t off = std::min<size_t>(s.off, len);
        const size_t n = std::min<size_t>(s.len, len - off);
        written = std::min(n, cap);
        if (written && CMBlockBufferCopyDataBytes(bb, off, written, dst) != kCMBlockBufferNoErr) written = 0;
        if (n > cap && !d->warned_small) { d->warned_small = true; NXLOG_W("movie", "the game's audio buffer of %zu bytes is smaller than a block (%zu)", cap, n); }
    }
    return written;
}

void dec_get_output(::Ctx* c) {              // GetOutputBuffer(int index, Buffer* out, MediaData* format)
    Dec* d = dec_of(A0);
    const int i = static_cast<int>(A1);
    const uint64_t b = A2;
    if (!d || !b || i < 0 || i >= d->nout) { RET(ST_INVALID); return; }
    if (MD* m = md_of(A3)) fill_format(d, m);
    std::lock_guard<std::mutex> lk(d->m);
    OutBuf& s = d->out[i];
    if (s.state != OUT_CLIENT) { RET(ST_INVALID); return; }
    const uint64_t base = rd64(b + B_BASE), cap = rd64(b + B_CAP);
    const size_t n = base ? copy_out(d, s, reinterpret_cast<unsigned char*>(base), static_cast<size_t>(cap)) : 0;
    wr32(b + B_OFFSET, 0);
    wr32(b + B_SIZE, static_cast<uint32_t>(n));
    wr64(b + B_I64, static_cast<uint64_t>(s.ts));
    wr32(b + B_FLAGS, s.flags);
    if (n) {
        d->copied++;
        if (d->is_video) s_frames++;
        else { s_audio_blocks++; s_audio_bytes += n; }
        if (d->copied == 1) NXLOG_I("movie", "Decoder(%s): first %s copied to the game (%zu bytes, t=%.3f s)", d->is_video ? "video" : "audio", d->is_video ? "frame" : "block", n, s.ts / 1e6);
    }
    RET(ST_OK);
}
void dec_release_output(::Ctx* c) {          // ReleaseOutputBufferIndex(int index)
    Dec* d = dec_of(A0);
    const int i = static_cast<int>(A1);
    if (!d || i < 0 || i >= d->nout) { RET(ST_INVALID); return; }
    std::lock_guard<std::mutex> lk(d->m);
    OutBuf& s = d->out[i];
    if (s.state == OUT_CLIENT) {
        s = OutBuf();
        dec_pump(d);
    }
    RET(ST_OK);
}

// ---- free functions ------------------------------------------------------------------------------------------------------------
void movie_set_allocator(::Ctx* c) { NXLOG_D("movie", "SetAllocator(alloc %llx, free %llx)", static_cast<unsigned long long>(A0), static_cast<unsigned long long>(A1)); }
void movie_set_core_mask(::Ctx*) {}
void movie_thread_priority(::Ctx* c) { RET(0); }

struct Import { const char* name; HleFn fn; };
const Import kImports[] = {
    {"_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", movie_set_allocator},
    {"_ZN5movie11SetCoreMaskEm", movie_set_core_mask},
    {"_ZN5movie17GetThreadPriorityEv", movie_thread_priority},
    {"_ZN5movie6BufferC1Ev", buf_ctor},
    {"_ZN5movie6BufferC1EPvm", buf_ctor_mem},
    {"_ZN5movie6BufferD1Ev", buf_dtor},
    {"_ZNK5movie6Buffer4BaseEv", buf_base},
    {"_ZNK5movie6Buffer4SizeEv", buf_size},
    {"_ZNK5movie6Buffer6OffsetEv", buf_offset},
    {"_ZN5movie9MediaDataC1Ev", mdata_ctor},
    {"_ZN5movie9MediaDataC1ERKS0_", mdata_copy},
    {"_ZN5movie9MediaDataD1Ev", mdata_dtor},
    {"_ZN5movie9MediaData5ClearEv", mdata_clear},
    {"_ZNK5movie9MediaData9FindInt32EPKcPi", mdata_find_i32},
    {"_ZNK5movie9MediaData9FindInt64EPKcPl", mdata_find_i64},
    {"_ZNK5movie9MediaData10FindStringEPKcPS2_", mdata_find_str},
    {"_ZN5movie9ExtractorC1ENS_13ContainerTypeENS_9CacheSizeE", ext_ctor},
    {"_ZN5movie9ExtractorD1Ev", ext_dtor},
    {"_ZN5movie9Extractor13SetDataSourceEPNS_18ClientStreamReaderEPKc", ext_set_source},
    {"_ZN5movie9Extractor13GetTrackCountEPi", ext_track_count},
    {"_ZN5movie9Extractor21GetTrackConfigurationEiPNS_9MediaDataE", ext_track_config},
    {"_ZN5movie9Extractor11SelectTrackEi", ext_select},
    {"_ZN5movie9Extractor29GetTrackIndexForAvailableDataEPm", ext_track_for_data},
    {"_ZN5movie9Extractor13GetSampleTimeEPl", ext_sample_time},
    {"_ZN5movie9Extractor4ReadEPNS_6BufferE", ext_read},
    {"_ZN5movie9Extractor7AdvanceEv", ext_advance},
    {"_ZN5movie9Extractor6SeekToEl", ext_seek},
    {"_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", dec_ctor},
    {"_ZN5movie7DecoderD1Ev", dec_dtor},
    {"_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", dec_register_events},
    {"_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", dec_configure},
    {"_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", dec_output_size},
    {"_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", dec_get_format},
    {"_ZN5movie7Decoder5StartEv", dec_start},
    {"_ZN5movie7Decoder4StopEv", dec_stop},
    {"_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", dec_acquire_input},
    {"_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", dec_get_input},
    {"_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", dec_send_input},
    {"_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_", dec_acquire_output},
    {"_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", dec_get_output},
    {"_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", dec_release_output},
};
constexpr size_t kImportCount = sizeof(kImports) / sizeof(kImports[0]);

const char* g_names[kImportCount];

}  // namespace

const char* const* movie_import_names(size_t* count) {
    for (size_t i = 0; i < kImportCount; i++) g_names[i] = kImports[i].name;
    if (count) *count = kImportCount;
    return g_names;
}

bool install_movie_hle() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        // Switch 2's movie:: (Pokopia's subsdk0) is a compatibility layer with virtual Extractor/Decoder objects over nn::movie and
        // nn::codec: these Switch 1 handlers (a marker where it keeps a vtable) would break it. There the decoders answer on the host
        // one level down instead (src/media/video_codec.cpp).
        // [media] movie is not read there: AVFoundation never takes these imports on Switch 2.
        if (game::info().platform == game::Platform::Switch2) {
            NXLOG_I("movie", "Switch 2: movie:: stays in subsdk0 (nn::movie demuxer, nn::codec decoders on the host); [media] movie is ignored");
            return;
        }
        // the key keeps its Windows values (mf = the host library, here on AVFoundation; avf is the same): only off changes anything
        const std::string mode = config_get_str("media", "movie", "mf");
        if (mode == "off") {
            NXLOG_W("movie", "[media] movie = off: the lifted subsdk0 library handles movie::");
            return;
        }
        g_log = config_get_bool("media", "movie_log", false);
        g_lookahead = config_get_bool("media", "movie_lookahead", true);
        g_preroll = config_get_bool("media", "movie_preroll", true);
        g_async_close = config_get_bool("media", "movie_async_close", true);
        // VP9 in MP4 decodes through VideoToolbox's supplemental decoder once it is registered (Apple silicon)
        VTRegisterSupplementalVideoDecoderIfAvailable(kCMVideoCodecType_VP9);
        for (const Import& im : kImports) {
            hle_register(im.name, im.fn);
            module_override_symbol(im.name);
        }
        ok = true;
        NXLOG_I("movie", "movie:: on AVFoundation: %zu imports routed to the host", kImportCount);
    });
    return ok;
}

MovieStats movie_stats() {
    MovieStats s;
    s.extractors_created = s_extractors_created;
    s.extractors_destroyed = s_extractors_destroyed;
    s.decoders_created = s_decoders_created;
    s.decoders_destroyed = s_decoders_destroyed;
    s.video_frames_delivered = s_frames;
    s.audio_blocks_delivered = s_audio_blocks;
    s.audio_bytes_delivered = s_audio_bytes;
    s.samples_read = s_samples;
    s.read_errors = s_read_errors;
    return s;
}

}  // namespace nxrt::media
