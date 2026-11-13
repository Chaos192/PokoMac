// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// video_codec_probe: plays a WebM movie through the host nn::codec VP9 decoder (src/media/video_codec.cpp) the way a Switch 2 movie
// player drives it, through the HLE entry points (guest registers in, result in x0): one image pool per output buffer, buffers
// registered and acquired with a fence, compressed frames submitted, the oldest fence polled with nn::gs::TryWaitNativeSync, the
// picture read back with ReadDecoderOutputImageNv12 into a pitch-linear NV12 buffer (luma rows then interleaved chroma rows, the
// stride the caller chose) and the buffer released and acquired again. No guest code runs: the container is split here (a minimal
// Matroska reader: SimpleBlock / Block of the first VP9 track, no lacing), not by nn::movie.
//
//   video_codec_probe <movie.webm> <out.png> [frame=100]
//
// Writes frame <frame> (1-based, in decode order) as a PNG (BT.709 limited range, like the game's shader) and prints the decode
// stats. Exit code 0 when that frame was decoded and written.
// Build (from the folder with nxrt.lib of a pkp build, after the runtime changed):
//   clang-cl /std:c++20 /O2 /MT /EHsc /I runtime/include /I recomp/include apps/video_codec_probe.cpp nxrt.lib vpx.lib ...
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/video_codec.h"

using namespace nxrt;

namespace {

// ---- calling the HLE handlers like guest code does ----------------------------------------------------------------------------------
alignas(16) unsigned char g_stack[4096];
uint64_t call(const char* name, std::initializer_list<uint64_t> regs, std::initializer_list<uint64_t> stack = {}, uint64_t x8 = 0) {
    ::Ctx c{};
    c.x[31] = reinterpret_cast<uint64_t>(g_stack + 2048);
    c.x[30] = 0x7100000100ull;
    c.x[8] = x8;
    int i = 0;
    for (uint64_t a : regs) c.x[i++] = a;
    int j = 0;
    for (uint64_t a : stack) std::memcpy(g_stack + 2048 + 8 * j++, &a, 8);
    dispatch_call(&c, hle_address(name));
    return c.x[0];
}
template <class T> uint64_t P(T* p) { return reinterpret_cast<uint64_t>(p); }

constexpr const char* kMakePref = "_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE";
constexpr const char* kCalcPool = "_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj";
constexpr const char* kInitPool = "_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEjNS0_9CodecModeE";
constexpr const char* kReadNv12 = "_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm";
constexpr const char* kInitVp9 = "_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE";
constexpr const char* kFiniVp9 = "_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE";
constexpr const char* kRegister = "_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj";
constexpr const char* kSubmit = "_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb";
constexpr const char* kAcquire = "_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE";
constexpr const char* kRelease = "_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE";
constexpr const char* kProps = "_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj";
constexpr const char* kTryWait = "_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE";

// ---- a minimal Matroska reader --------------------------------------------------------------------------------------------------
struct Packet {
    const uint8_t* data;
    size_t size;
    int64_t ms;
};
struct Webm {
    std::vector<uint8_t> bytes;
    uint64_t video_track = 0;
    uint32_t w = 0, h = 0;
    std::string codec;
    std::vector<Packet> video;
};
bool vint(const std::vector<uint8_t>& b, size_t& o, uint64_t& v, bool keep_marker) {
    if (o >= b.size()) return false;
    const uint8_t first = b[o];
    int len = 1;
    while (len <= 8 && !(first & (0x80 >> (len - 1)))) len++;
    if (len > 8 || o + len > b.size()) return false;
    v = keep_marker ? first : first & (0xff >> len);
    for (int i = 1; i < len; i++) v = (v << 8) | b[o + i];
    if (!keep_marker && v == (1ull << (7 * len)) - 1) v = ~0ull;   // unknown size
    o += len;
    return true;
}
uint64_t uint_be(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}
bool parse(Webm& m, size_t max_frames) {
    const auto& b = m.bytes;
    size_t o = 0;
    int64_t cluster_ms = 0;
    uint64_t cur_track = 0;
    std::string cur_codec;
    uint32_t cur_w = 0, cur_h = 0;
    while (o < b.size() && m.video.size() < max_frames) {
        uint64_t id, size;
        if (!vint(b, o, id, true) || !vint(b, o, size, false)) break;
        const size_t body = o;
        switch (id) {
        case 0x18538067: case 0x1F43B675: case 0x1654AE6B: case 0xE0: case 0xA0:   // Segment, Cluster, Tracks, Video, BlockGroup: enter
            continue;
        case 0xAE:   // TrackEntry: enter, the track is taken at its end (below) - entries are small, so parse its children here
        {
            cur_track = 0; cur_codec.clear(); cur_w = cur_h = 0;
            const size_t end = body + size;
            size_t q = body;
            while (q < end) {
                uint64_t cid, cs;
                if (!vint(b, q, cid, true) || !vint(b, q, cs, false)) break;
                if (cid == 0xD7) cur_track = uint_be(&b[q], cs);
                else if (cid == 0x86) cur_codec.assign(reinterpret_cast<const char*>(&b[q]), cs);
                else if (cid == 0xE0) continue;   // Video: enter
                else if (cid == 0xB0) cur_w = static_cast<uint32_t>(uint_be(&b[q], cs));
                else if (cid == 0xBA) cur_h = static_cast<uint32_t>(uint_be(&b[q], cs));
                q += cs;
            }
            if (!m.video_track && cur_codec.rfind("V_VP", 0) == 0) {
                m.video_track = cur_track; m.codec = cur_codec; m.w = cur_w; m.h = cur_h;
            }
            o = end;
            continue;
        }
        case 0xE7: cluster_ms = static_cast<int64_t>(uint_be(&b[body], size)); break;   // Cluster Timecode (ms at the default scale)
        case 0xA3: case 0xA1: {   // SimpleBlock, Block
            size_t q = body;
            uint64_t track;
            if (!vint(b, q, track, false)) return false;
            const int16_t rel = static_cast<int16_t>((b[q] << 8) | b[q + 1]);
            const uint8_t flags = b[q + 2];
            q += 3;
            if (track == m.video_track) {
                if (flags & 0x06) { std::fprintf(stderr, "laced video block: not handled\n"); return false; }
                m.video.push_back(Packet{&b[q], body + size - q, cluster_ms + rel});
            }
            break;
        }
        default: break;
        }
        if (size == ~0ull) break;
        o = body + size;
    }
    return m.video_track != 0 && !m.video.empty();
}

// ---- PNG (stored deflate blocks) ------------------------------------------------------------------------------------------------
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t k = i; for (int j = 0; j < 8; j++) k = k & 1 ? 0xedb88320u ^ (k >> 1) : k >> 1; t[i] = k; }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
void be32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, crc32(&out[start], out.size() - start));
}
bool write_png(const std::string& path, const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t(w) * 3 + 1) * h);
    for (uint32_t y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), &rgb[size_t(y) * w * 3], &rgb[size_t(y) * w * 3] + size_t(w) * 3); }
    std::vector<uint8_t> z{0x78, 0x01};
    uint32_t a = 1, bsum = 0;
    for (uint8_t x : raw) { a = (a + x) % 65521; bsum = (bsum + a) % 65521; }
    for (size_t o = 0; o < raw.size() || o == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - o);
        z.push_back(o + n >= raw.size() ? 1 : 0);
        z.push_back(n & 0xff); z.push_back(n >> 8); z.push_back(~n & 0xff); z.push_back((~n >> 8) & 0xff);
        z.insert(z.end(), raw.begin() + o, raw.begin() + o + n);
        o += n;
        if (n == 0) break;
    }
    be32(z, (bsum << 16) | a);
    std::vector<uint8_t> ihdr;
    be32(ihdr, w); be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    std::vector<uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}
uint8_t clamp8(int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: video_codec_probe <movie.webm> <out.png> [frame=100]\n"); return 2; }
    const size_t want = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 100;
    static game::Info info;
    info.id = "pkp";
    info.platform = game::Platform::Switch2;
    game::set_current(&info);
    if (!media::install_video_codec_hle()) { std::fprintf(stderr, "host video decoders not installed (built without libvpx?)\n"); return 3; }

    Webm m;
    {
        std::ifstream f(argv[1], std::ios::binary);
        if (!f) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 4; }
        f.seekg(0, std::ios::end);
        const size_t n = std::min<size_t>(static_cast<size_t>(f.tellg()), size_t(64) << 20);   // the first 64 MiB hold far more than 100 frames
        f.seekg(0);
        m.bytes.resize(n);
        f.read(reinterpret_cast<char*>(m.bytes.data()), static_cast<std::streamsize>(n));
    }
    if (!parse(m, want + 16)) { std::fprintf(stderr, "no VP8/VP9 video track found\n"); return 5; }
    std::printf("%s: track %llu %s %ux%u, %zu packets read\n", argv[1], (unsigned long long)m.video_track, m.codec.c_str(), m.w, m.h, m.video.size());
    if (m.codec != "V_VP9") { std::fprintf(stderr, "only VP9 is driven here\n"); return 5; }

    // the player's setup: a preference for the size, one image pool per output buffer, the decoder, the buffers registered
    constexpr uint32_t kBuffers = 4, kNv12 = 3, kMode = 0;
    alignas(16) uint8_t pref[64]{};
    call(kMakePref, {m.w, m.h, kNv12, kMode}, {}, P(pref));
    uint64_t pool_size = 0, pool_align = 0;
    call(kCalcPool, {P(&pool_size), P(&pool_align), P(pref), 1});
    alignas(16) uint8_t pools[kBuffers][32]{};
    std::vector<uint8_t> pool_mem(pool_size * kBuffers + 0x1000);
    for (uint32_t i = 0; i < kBuffers; i++) call(kInitPool, {P(pools[i]), P(pool_mem.data()) + i * pool_size, pool_size, P(pref), 1, kMode});
    alignas(16) uint8_t dec[256]{};
    if (call(kInitVp9, {P(dec), 0, m.w, m.h, 0, 0, kNv12}) != 0) { std::fprintf(stderr, "InitializeVp9Decoder failed\n"); return 6; }
    for (uint32_t i = 0; i < kBuffers; i++) call(kRegister, {P(dec), i, P(pools[i]), 0});
    struct Acq { uint32_t index; alignas(8) uint64_t fence[2]; };
    std::vector<Acq> acquired;
    auto acquire = [&] {
        Acq a{};
        call(kAcquire, {P(&a.index), P(a.fence), P(dec)});
        acquired.push_back(a);
    };
    for (uint32_t i = 0; i < kBuffers; i++) acquire();

    // pitch-linear NV12 destination with the stride a 256-byte aligned surface would have
    const uint32_t w = m.w, h = m.h, stride = (w + 255) & ~255u;
    std::vector<uint8_t> ybuf(size_t(stride) * h), uvbuf(size_t(stride) * (h / 2));
    size_t got = 0;
    bool written = false;
    for (size_t k = 0; k < m.video.size() && !written; k++) {
        uint64_t submitted = 0;
        const Packet& pk = m.video[k];
        call(kSubmit, {P(&submitted), P(dec), P(pk.data), pk.size, static_cast<uint64_t>(pk.ms) * 1000000ull, 0});
        while (!acquired.empty() && (call(kTryWait, {P(acquired.front().fence)}) & 1)) {
            const Acq a = acquired.front();
            acquired.erase(acquired.begin());
            uint16_t pw = 0, ph = 0, aw = 0, ah = 0;
            int64_t pts = 0;
            uint8_t eos = 0;
            call(kProps, {P(&pw), P(&ph), P(&aw), P(&ah), P(&pts), P(&eos), P(dec), a.index});
            ++got;
            if (got == want) {
                call(kReadNv12, {P(pools[a.index]), 0, 0, 0, w, h, P(ybuf.data()), stride}, {ybuf.size(), P(uvbuf.data()), stride, uvbuf.size()});
                std::vector<uint8_t> rgb(size_t(w) * h * 3);
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++) {
                        const int Y = (ybuf[size_t(y) * stride + x] - 16) * 298;
                        const uint8_t* uv = &uvbuf[size_t(y / 2) * stride + (x & ~1u)];
                        const int U = uv[0] - 128, V = uv[1] - 128;
                        uint8_t* o = &rgb[(size_t(y) * w + x) * 3];
                        o[0] = clamp8((Y + 459 * V + 128) >> 8);
                        o[1] = clamp8((Y - 55 * U - 136 * V + 128) >> 8);
                        o[2] = clamp8((Y + 541 * U + 128) >> 8);
                    }
                written = write_png(argv[2], rgb, w, h);
                std::printf("frame %zu: buffer %u, %ux%u, pts %.3f s -> %s%s\n", got, a.index, pw, ph, pts / 1e9, argv[2], written ? "" : " (write FAILED)");
            }
            call(kRelease, {P(dec), a.index, 0, 0});
            acquire();
        }
    }
    const media::VideoCodecStats st = media::video_codec_stats();
    std::printf("stats: packets %llu, decoded %llu, placed %llu, read %llu, errors %llu\n", (unsigned long long)st.packets_submitted,
                (unsigned long long)st.frames_decoded, (unsigned long long)st.frames_delivered, (unsigned long long)st.frames_read,
                (unsigned long long)st.decode_errors);
    call(kFiniVp9, {P(dec)});
    return written ? 0 : 1;
}
