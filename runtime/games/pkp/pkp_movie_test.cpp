// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Test hook: plays one of the game's movies through the game's own movie player, for checking video decoding without reaching the
// story point that plays it (MV01/MV02 come late in the story).
//
//   [test] movie = MV02          movie_asset/ounce/<name>.webm; off when empty
//   [test] movie_frame = 300     game frame at which the player is created
//   [test] shot_dir              where the decoded frames go as movie_<name>_<n>.png (frames 1, 60, 300, 900, 1500, 3000)
//
// It uses the player class the game's movie component uses (main, Pokopia 2.0.1a): create 0x7101fd33ec with the component's config
// (NV12 output, as sub_7102579094 builds it), open 0x7101fd3940 with a stand-in file entry, start 0x7101fd3b38. The player's own threads
// then demux (nn::movie::MediaExtractor) and decode (movie:: in subsdk0 over nn::codec, on the host here). Every game frame the hook
// takes the next decoded frame from the player's video output queue the way 0x7101fd7a60 does (GetOutputBuffer into the output's NV12
// buffer, release) but does not upload or draw it: the game's draw path needs its render context.
#include "pkp_movie_test.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/dispatch.h"
#include "nxrt/game.h"
#include "nxrt/gfx.h"
#include "nxrt/log.h"
#include "nxrt/module.h"
#include "nxrt/video_codec.h"

namespace nxrt::games::pkp {

namespace {

constexpr const char* kTag = "pkp.movietest";
// main, Pokopia 2.0.1a (offsets from the main base)
constexpr uint64_t kPlayerCreate = 0x1fd33ec;    // (renderer, gfx, const Config*, Allocator*) -> Player*
constexpr uint64_t kPlayerOpen = 0x1fd3940;      // (Player*, Entry*, int video_track, int audio_track) -> bool
constexpr uint64_t kPlayerStart = 0x1fd3b38;     // (Player*) -> bool; its body is 0x1fd3b98 (Player*, start ms), run step by step below
// steps of 0x1fd3b98 (start) with no audio track, on Player+104 unless noted
constexpr uint64_t kStOutputs = 0x1fd4cec;       // makes the audio/video output objects (Player+1176 / +1184) -> result
constexpr uint64_t kStVideoOut = 0x1fd765c;      // (video out, *(Player+40) allocator, *(Player+48) renderer): 6 MiB + renderer object + nvn sync
constexpr uint64_t kStVideoOutDec = 0x1fd7878;   // (video out, *(Player+120) decoder, *(Player+2500), *(Player+1268) w, *(Player+1272) h)
constexpr uint64_t kStReadThread = 0x1fd48bc;    // AudioVideoInputReadThread -> result
constexpr uint64_t kStWorkThread = 0x1fd49bc;    // AudioVideoWorkerThread -> result
constexpr uint64_t kStStep4c78 = 0x1fd4c78, kStStep4c80 = 0x1fd4c80;
constexpr uint64_t kStDecStart = 0x1fd5578;      // the decoders' Start (vtable +40) -> result
constexpr uint64_t kStCleanup = 0x1fd4e04;       // what start runs when an output step fails
constexpr uint64_t kCacheOpen = 0x1fd8694;       // (Player+3520, path, File** out) -> bool   (steps of open, 0x1fd3940)
constexpr uint64_t kCacheUse = 0x1fd8760;        // (Player+3520, File*)
constexpr uint64_t kOpenExtractor = 0x1fd4e54;   // (Player+104, container, path, reader) -> result
constexpr uint64_t kOpenDecoders = 0x1fd4f20;    // (Player+104, output mode, video track, audio track, reverse) -> result
constexpr uint64_t kGlobRenderer = 0x3484108;    // GOT slots of the two globals the movie wrapper passes to create
constexpr uint64_t kGlobGfx = 0x3484340;
constexpr uint64_t kDefaultHeapGlobal = 0x36d6f00;   // qword: object whose +16 is the default KTGL allocator (the player's fallback)
constexpr uint64_t kMovieSysGot = 0x34842f8;         // GOT slot of the global whose object's +1800 is the allocator the game gives movie::
constexpr uint64_t kMovieSysHeap = 1800;             //   (sub_71000E0F88 puts it in the config's +32 and sub_7102579240 hands it to SetAllocator)
constexpr uint64_t kAllocFn = 0x25829b0, kFreeFn = 0x25829f8, kReallocFn = 0x2582a18;   // the game's movie::SetAllocator thunks
constexpr uint64_t kPlayerState = 3552, kPlayerVideoOut = 1184;
// video output (the object 0x7101fd7a60 works on)
constexpr uint64_t kVoDecoder = 8, kVoMode = 16, kVoCount = 40, kVoQueue = 56, kVoMutex = 72, kVoCap = 104, kVoBuf = 112, kVoW = 120, kVoH = 124;

template <class T> T rd(uint64_t a) { T v; std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T)); return v; }
template <class T> void wr(uint64_t a, T v) { std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T)); }

uint64_t call(::Ctx* c, uint64_t fn, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0, uint64_t a4 = 0) {
    ::Ctx saved = *c;
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4;
    dispatch_call(c, fn);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}

void noop(::Ctx* c) { c->x[0] = 0; }

struct State {
    bool init = false, on = false, failed = false, started = false;
    std::string name, dir;
    uint64_t start_frame = 300, frames = 0, base = 0, player = 0;
    uint64_t taken = 0;
    std::chrono::steady_clock::time_point last_frame{};   // start, then the last frame taken
    bool idle_logged = false;
    alignas(16) uint8_t cfg[64]{};
    alignas(16) uint8_t entry[128]{};
    alignas(16) uint64_t entry_vt[16]{};
    alignas(16) uint8_t buffer[64]{};   // movie::Buffer
    char path[256]{};
    uint64_t lock = 0, unlock = 0, buf_ctor = 0, buf_dtor = 0, buf_set = 0, buf_range = 0;
    uint64_t player_heap = 0;   // the allocator the player object came from (create's 4th argument, or its default)
};
State& st() { static State s; return s; }

void save(const State& s, uint64_t n, const uint8_t* nv12, uint32_t w, uint32_t h) {
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    const uint8_t* uv = nv12 + size_t(w) * h;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {   // BT.709 limited range
            const int Y = (nv12[size_t(y) * w + x] - 16) * 298;
            const int U = uv[size_t(y / 2) * w + (x & ~1u)] - 128, V = uv[size_t(y / 2) * w + (x & ~1u) + 1] - 128;
            uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
            o[0] = static_cast<uint8_t>(std::clamp((Y + 459 * V + 128) >> 8, 0, 255));
            o[1] = static_cast<uint8_t>(std::clamp((Y - 55 * U - 136 * V + 128) >> 8, 0, 255));
            o[2] = static_cast<uint8_t>(std::clamp((Y + 541 * U + 128) >> 8, 0, 255));
            o[3] = 255;
        }
    const std::string p = s.dir + "/movie_" + s.name + "_" + std::to_string(n) + ".png";
    if (gfx::save_png_rgba(p, rgba.data(), w, h)) NXLOG_I(kTag, "frame %llu (%ux%u) -> %s", (unsigned long long)n, w, h, p.c_str());
}

// 0x7101fd3b98 (the body of start, 0x7101fd3b38) step by step for a video-only movie, so a failure names its step and result. On success
// it leaves the player as that function does (state 2, or 1 for a WebM with a frame count; the clock fields reset).
bool start_steps(::Ctx* c, State& s, uint64_t p) {
    const uint64_t v = p + 104;
    wr<uint8_t>(p + 2506, rd<uint8_t>(p + 96) != 0);
    const bool has_audio = rd<uint8_t>(p + 2504) != 0, has_video = rd<uint8_t>(p + 2505) != 0;
    if (has_audio) {
        NXLOG_E(kTag, "start: the movie has an audio track; the step-by-step start only covers video");
        return false;
    }
    auto fail = [&](const char* step, uint64_t r) {
        NXLOG_E(kTag, "start: %s failed: 0x%llx (video out %llx, allocator %llx, renderer %llx, decoder %llx, %ux%u)", step, (unsigned long long)r,
                (unsigned long long)rd<uint64_t>(p + 1184), (unsigned long long)rd<uint64_t>(p + 40), (unsigned long long)rd<uint64_t>(p + 48),
                (unsigned long long)rd<uint64_t>(p + 120), rd<uint32_t>(p + 1268), rd<uint32_t>(p + 1272));
        return false;
    };
    if (const uint64_t r = call(c, s.base + kStOutputs, v) & 0xffffffff) return fail("outputs (0x1fd4cec)", r);
    if (has_video) {
        const uint64_t vo = rd<uint64_t>(p + 1184);
        if (!vo) return fail("no video output object", 0);
        if (const uint64_t r = call(c, s.base + kStVideoOut, vo, rd<uint64_t>(p + 40), rd<uint64_t>(p + 48)) & 0xffffffff) {
            fail(r == 4098 ? "video output: 6 MiB from the player's allocator (0x1fd765c)"
                 : r == 8197 ? "video output: renderer object or nvnSyncInitialize (0x1fd765c)" : "video output (0x1fd765c)", r);
            call(c, s.base + kStCleanup, v);
            return false;
        }
        if (const uint64_t r = call(c, s.base + kStVideoOutDec, vo, rd<uint64_t>(p + 120), rd<uint32_t>(p + 2500), rd<uint32_t>(p + 1268),
                                    rd<uint32_t>(p + 1272)) & 0xffffffff) {
            fail("video output decoder (0x1fd7878)", r);
            call(c, s.base + kStCleanup, v);
            return false;
        }
    }
    if (const uint64_t r = call(c, s.base + kStReadThread, v) & 0xffffffff) return fail("input read thread (0x1fd48bc)", r);
    if (const uint64_t r = call(c, s.base + kStWorkThread, v) & 0xffffffff) return fail("worker thread (0x1fd49bc)", r);
    call(c, s.base + kStStep4c78, v);
    call(c, s.base + kStStep4c80, v);
    if (const uint64_t r = call(c, s.base + kStDecStart, v) & 0xffffffff) return fail("decoder start (0x1fd5578)", r);
    wr<uint64_t>(p + 3544, 0);
    uint32_t state = 2;
    const int32_t frames = rd<int32_t>(p + 1276);
    if (rd<uint64_t>(p + 104) && frames >= 1 && rd<uint64_t>(p + 32) && (rd<uint32_t>(p + 20) & ~1u) == 2) {
        wr<uint64_t>(p + 3544, 3333000ull * static_cast<uint32_t>(frames));
        state = 1;
    }
    wr<uint32_t>(p + kPlayerState, state);
    wr<uint8_t>(p + 1256, 0);
    wr<int64_t>(p + 1248, -1);
    const uint64_t active = module_find_symbol("_ZN2nn2oe25GetProgramTotalActiveTimeEv");
    const int64_t t = active ? static_cast<int64_t>(call(c, active, 0)) : 0;
    wr<uint8_t>(p + 1224, 0);
    wr<uint64_t>(p + 3560, 0);
    wr<int64_t>(p + 1200, t / 1000);
    NXLOG_I(kTag, "start: every step passed (state %u)", state);
    return true;
}

bool start(::Ctx* c, State& s) {
    s.lock = module_find_symbol("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE");
    s.unlock = module_find_symbol("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE");
    s.buf_ctor = module_find_symbol("_ZN5movie6BufferC1Ev");
    s.buf_dtor = module_find_symbol("_ZN5movie6BufferD1Ev");
    s.buf_set = module_find_symbol("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
    s.buf_range = module_find_symbol("_ZN5movie6Buffer8SetRangeEii");
    if (!s.lock || !s.unlock || !s.buf_ctor || !s.buf_set || !s.buf_range) {
        NXLOG_E(kTag, "guest functions missing");
        return false;
    }
    // the config the game's movie component builds (sub_71008B11E0 + sub_71000E0F88): +0 flags 1 (read through the game's file cache),
    // +4 movie::ContainerType 3 (WebM; the default 1 is MP4 and the extractor rejects a .webm with 0x4000), +8 output 1 (NV12),
    // +16 250, +24 0, +32 the allocator's user pointer (set below)
    wr<uint32_t>(reinterpret_cast<uint64_t>(s.cfg), 1);
    wr<uint32_t>(reinterpret_cast<uint64_t>(s.cfg) + 4, 3);
    wr<uint32_t>(reinterpret_cast<uint64_t>(s.cfg) + 8, 1);
    wr<uint64_t>(reinterpret_cast<uint64_t>(s.cfg) + 16, 250);
    // movie:: allocates through the allocator the game sets before every player (sub_7102579094: movie::SetAllocator with three thunks
    // over a KTGL allocator); without it the library falls back to MovieMalloc, which no module exports. The allocator here is the one the
    // player itself defaults to.
    // The heap is the one the game's movie component uses (sub_71000E0F88: config +32 = the movie system's +1800). The default KTGL
    // heap is only the fallback: the extractor cache and the decoder buffers taken from it starved the heap the game's job threads are
    // made from (a later WorldGenerateJob thread came back null and nn::os::SignalEvent ran on it: work/audit/crashes.md).
    const uint64_t set_alloc = module_find_symbol("_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_");
    const uint64_t def_heap = rd<uint64_t>(rd<uint64_t>(s.base + kDefaultHeapGlobal) + 16);
    const uint64_t sys = rd<uint64_t>(rd<uint64_t>(s.base + kMovieSysGot));
    const uint64_t movie_heap = sys ? rd<uint64_t>(sys + kMovieSysHeap) : 0;
    const uint64_t heap = movie_heap ? movie_heap : def_heap;
    s.player_heap = def_heap;
    if (!set_alloc || !heap) {
        NXLOG_E(kTag, "movie::SetAllocator %llx or an allocator (movie %llx, default %llx) missing", (unsigned long long)set_alloc,
                (unsigned long long)movie_heap, (unsigned long long)def_heap);
        return false;
    }
    NXLOG_I(kTag, "movie:: allocator: %s heap %llx", movie_heap ? "the game's movie" : "the default (no movie heap)", (unsigned long long)heap);
    wr<uint64_t>(reinterpret_cast<uint64_t>(s.cfg) + 32, heap);
    call(c, set_alloc, s.base + kAllocFn, s.base + kFreeFn, s.base + kReallocFn, heap);
    if (const uint64_t core_mask = module_find_symbol("_ZN5movie11SetCoreMaskEm")) call(c, core_mask, 7);   // as sub_71000E0F88 does
    const uint64_t ren = rd<uint64_t>(rd<uint64_t>(s.base + kGlobRenderer)), gfx = rd<uint64_t>(rd<uint64_t>(s.base + kGlobGfx));
    // the player object and its video output's 6 MiB staging buffer (0x7101fd765c) come from create's allocator; with 0 that is the
    // default heap. The movie heap is passed instead (same KTGL allocator interface), like the game's component passes its own.
    s.player_heap = heap;
    s.player = call(c, s.base + kPlayerCreate, ren, gfx, reinterpret_cast<uint64_t>(s.cfg), heap);
    if (!s.player) {
        NXLOG_E(kTag, "player create failed");
        return false;
    }
    // stand-in file entry: vtable (slot 4 = release at refcount 0), refcount at +8, path at +32
    const uint64_t thunk = hle_register("pkp_movie_test_entry_noop", noop);
    for (auto& v : s.entry_vt) v = thunk;
    std::snprintf(s.path, sizeof s.path, "rom:/movie_asset/ounce/%s.webm", s.name.c_str());
    const uint64_t e = reinterpret_cast<uint64_t>(s.entry);
    wr<uint64_t>(e, reinterpret_cast<uint64_t>(s.entry_vt));
    wr<uint32_t>(e + 8, 1);
    wr<uint64_t>(e + 32, reinterpret_cast<uint64_t>(s.path));
    // 0x7101fd3940 (open) step by step, so a failure names its step: the game's file cache (config flag 1), the extractor, the decoders
    const uint64_t p = s.player;
    alignas(16) uint64_t file = 0;
    const uint64_t r_cache = call(c, s.base + kCacheOpen, p + 3520, reinterpret_cast<uint64_t>(s.path), reinterpret_cast<uint64_t>(&file));
    if (!(r_cache & 1)) {
        NXLOG_E(kTag, "open %s: the game's file cache could not open it", s.path);
        return false;
    }
    call(c, s.base + kCacheUse, p + 3520, file);
    wr<uint32_t>(e + 8, rd<uint32_t>(e + 8) + 1);
    wr<uint64_t>(p + 80, e);
    wr<uint32_t>(p + 88, 0);            // video track
    wr<uint32_t>(p + 92, 0xffffffffu);  // no audio track
    const uint32_t r_ext = static_cast<uint32_t>(call(c, s.base + kOpenExtractor, p + 104, rd<uint32_t>(p + 20), reinterpret_cast<uint64_t>(s.path), p + 3520));
    if (r_ext) {
        // 0x4000: movie::Extractor::SetDataSource opened the source but ValidateContainerType disagreed (no tracks, no
        // "container-mime", or a mime that is not the config's ContainerType). The [media] movie_trace lines before this say which.
        int32_t tracks = -1;
        if (const uint64_t ex = rd<uint64_t>(p + 104)) {
            alignas(8) int32_t n = 0;
            if (call(c, rd<uint64_t>(rd<uint64_t>(ex) + 32), ex, reinterpret_cast<uint64_t>(&n)) == 0) tracks = n;   // GetTrackCount
        }
        NXLOG_E(kTag, "open %s: extractor 0x%x (%d tracks)", s.path, r_ext, tracks);
        if (r_ext != 0x4000 || tracks <= 0) return false;
        // the source has tracks: carry on to the decoders (the game's player would stop here), so the decode path still gets tested
        NXLOG_W(kTag, "open %s: container check failed but %d tracks are there: going on with the decoders (test only)", s.path, tracks);
        wr<uint32_t>(p + 104 + 1160, static_cast<uint32_t>(tracks));
    }
    const uint32_t r_dec = static_cast<uint32_t>(call(c, s.base + kOpenDecoders, p + 104, rd<uint32_t>(p + 24), 0, 0xffffffffull, 0));
    if (r_dec) {
        NXLOG_E(kTag, "open %s: decoders 0x%x (tracks %d)", s.path, r_dec, rd<int32_t>(p + 104 + 1160));
        return false;
    }
    wr<uint32_t>(p + kPlayerState, 6);
    if (!start_steps(c, s, p)) return false;
    NXLOG_I(kTag, "playing %s (player %llx, state %u, host decoder %s)", s.path, (unsigned long long)s.player, rd<uint32_t>(s.player + kPlayerState),
            media::video_codec_enabled() ? "on" : "off");
    return true;
}

// One frame from the video output queue, as 0x7101fd7a60 takes it.
void pull(::Ctx* c, State& s) {
    const uint64_t vo = rd<uint64_t>(s.player + kPlayerVideoOut);
    if (!vo) return;
    if (rd<uint32_t>(vo + kVoMode) != 4) {
        static bool once = false;
        if (!once) { once = true; NXLOG_W(kTag, "video output mode %u (expected 4, NV12)", rd<uint32_t>(vo + kVoMode)); }
        return;
    }
    call(c, s.lock, vo + kVoMutex);
    const int64_t count = rd<int64_t>(vo + kVoCount);
    const uint64_t buf = rd<uint64_t>(vo + kVoBuf), cap = rd<uint64_t>(vo + kVoCap);
    const uint32_t w = rd<uint32_t>(vo + kVoW), h = rd<uint32_t>(vo + kVoH);
    bool got = false;
    if (count >= 1 && buf && cap) {
        const uint64_t q = rd<uint64_t>(vo + kVoQueue);
        const uint32_t index = rd<uint32_t>(q);
        const uint64_t dec = rd<uint64_t>(vo + kVoDecoder), vt = rd<uint64_t>(dec);
        const uint64_t b = reinterpret_cast<uint64_t>(s.buffer);
        call(c, s.buf_ctor, b);
        call(c, s.buf_set, b, buf, cap);
        call(c, s.buf_range, b, 0, cap);
        call(c, rd<uint64_t>(vt + 104), dec, index, b);   // GetOutputBuffer
        call(c, rd<uint64_t>(vt + 120), dec, index);      // ReleaseOutputBufferIndex
        if (s.buf_dtor) call(c, s.buf_dtor, b);
        std::memmove(reinterpret_cast<void*>(q), reinterpret_cast<const void*>(q + 24), size_t(24) * (count - 1));
        wr<int64_t>(vo + kVoCount, count - 1);
        got = true;
    }
    call(c, s.unlock, vo + kVoMutex);
    if (!got) {
        // by wall time: the game loop can run at hundreds of frames a second, and the first picture needs the player's threads to read,
        // send and decode the first packets
        const double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.last_frame).count();
        if (!s.idle_logged && waited >= 10.0) {
            s.idle_logged = true;
            NXLOG_I(kTag, "no frame for %.0f s after %llu frames (%s)", waited, (unsigned long long)s.taken,
                    s.taken ? "end of movie?" : "nothing decoded: see the vcodec lines");
        }
        return;
    }
    s.last_frame = std::chrono::steady_clock::now();
    s.idle_logged = false;
    ++s.taken;
    static const uint64_t kSave[] = {1, 60, 300, 900, 1500, 3000};
    if (std::find(std::begin(kSave), std::end(kSave), s.taken) != std::end(kSave) && w && h && cap >= uint64_t(w) * h * 3 / 2)
        save(s, s.taken, reinterpret_cast<const uint8_t*>(buf), w, h);
    if (s.taken % 300 == 0) NXLOG_I(kTag, "%llu frames taken", (unsigned long long)s.taken);
}

}  // namespace

void movie_test_frame(::Ctx* c) {
    State& s = st();
    if (!s.init) {
        s.init = true;
        s.name = config_get_str("test", "movie", "");
        s.on = !s.name.empty();
        s.start_frame = static_cast<uint64_t>(config_get_int("test", "movie_frame", 300));
        s.dir = config_get_str("test", "shot_dir", "testout");
        s.base = game::info().main_base;
        if (s.on) NXLOG_I(kTag, "[test] movie = %s: plays at game frame %llu", s.name.c_str(), (unsigned long long)s.start_frame);
    }
    if (!s.on || s.failed) return;
    if (++s.frames < s.start_frame) return;
    if (!s.started) {
        s.started = true;
        if (!start(c, s)) s.failed = true;
        s.last_frame = std::chrono::steady_clock::now();
        return;
    }
    pull(c, s);
}

}  // namespace nxrt::games::pkp
