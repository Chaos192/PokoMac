// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Small services (S3c): csrng, mm:u, hwopus, aoc:u, caps:u, mii:u / mii:e, irs, nfc:user, nfc:mf:u, nfp:user, olsc:u, ectx:aw, pcm,
// banana, the TMA services (htc, htc:tenv, htcs), stpl:u (Switch 2 GameShare); and the registration of ldr:ro / ro:1 (ro.cpp), bt / btm:u
// (bluetooth.cpp), npns (npns.cpp), pm:dmnt (pm.cpp) and caps:su (album.cpp).
// A console with no IR camera, no NFC reader in use, no network, an empty album and no GameShare session. Command ids and layouts:
// Switchbrew (https://switchbrew.org/wiki/Services_API and the pages of each service) and the facts Ryujinx (MIT) src/Ryujinx.HLE/HOS/Services
// and src/Ryujinx.Horizon give (Spl, MmNv, Codec, Ns/Aoc, Caps, Mii, Hid/Irs, Nfc, Olsc, Ectx, Ptm/Pcm): ids, structs, result codes.
// The default Miis' face values are Ryujinx's default table with the Switch's common colours (nxnative dm/host/hle_mii.c, MIT).
//
// Answers that differ from a console on purpose: an Opus decoder libopus cannot serve (44.1 kHz, 3-8 channels) is a silent one, so
// opening never fails; the Mii database is empty and its default source has six Miis; conversions refuse (no converter here).
#if defined(_WIN32)
#include <windows.h>
#endif
#include <bcrypt.h>
#if defined(_WIN32)
#pragma comment(lib, "bcrypt")
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "audio_opus.h"
#include "nxrt/log.h"
#include "nxrt/ro.h"
#include "nxrt/tt.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

const uint32_t kNotSupported = res(11, 1);
const uint32_t kNfpNoDevice = res(115, 64);
const uint32_t kNfcNoDevice = res(127, 64);
const uint32_t kOlscNullArgument = res(179, 100);
const uint32_t kOlscNotInitialized = res(179, 101);
const uint32_t kMiiInvalidArgument = res(126, 1);
const uint32_t kMiiNotFound = res(126, 4);
const uint32_t kMiiInvalidCharInfo = res(126, 100);
const uint32_t kAocNoPurchase = res(164, 400);
const uint32_t kAocNotConnected = res(110, 300);

using H = ipc::Handler;

// "Accepted and ignored": result 0, nothing written.
H accept(const char* name) {
    return [name](ipc::Request& req, ipc::Response&) -> uint32_t {
        NXLOG_D("services", "%s (cmd %u) accepted and ignored", name, req.cmd);
        return 0;
    };
}
H refuse(const char* tag, const char* name, uint32_t result) {
    return [tag, name, result](ipc::Request& req, ipc::Response&) -> uint32_t {
        NXLOG_W(tag, "%s (cmd %u) answered with 0x%x", name, req.cmd, result);
        return result;
    };
}
template <class T>
H answer(T v) {
    return [v](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<T>(v);
        return 0;
    };
}
void zero_buffer(const ipc::Buffer& b) {
    if (b.addr == 0 || b.size == 0) return;   // no buffer: nothing to clear
    std::fill_n(buf_mut(b), static_cast<size_t>(b.size), uint8_t{0});
}

// The events no device here ever signals (misc_reset clears them).
struct Events {
    ServiceEvent aoc_changed, aoc_purchased;
    ServiceEvent nfc_avail, nfc_activate, nfc_deactivate;
    ServiceEvent nfp_avail, nfp_activate, nfp_deactivate;
    void clear_all() {
        for (ServiceEvent* e : {&aoc_changed, &aoc_purchased, &nfc_avail, &nfc_activate, &nfc_deactivate, &nfp_avail, &nfp_activate, &nfp_deactivate})
            e->clear();
    }
};
Events& events() {
    static Events e;
    return e;
}
H event_handle(ServiceEvent& e) {
    return [&e](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.out_copy.push_back(e.new_handle());
        return 0;
    };
}

// ---- csrng: random bytes --------------------------------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_csrng() {
    return make_iface("csrng", [](ipc::Service& s) {
        s.add_cmd(0, "GenerateRandomBytes", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const ipc::Buffer b = req.out_buf(0);
            if (!b.addr || !b.size) return 0;
            NXLOG_D("csrng", "GenerateRandomBytes(%llu)", static_cast<unsigned long long>(b.size));
            if (tt::deterministic()) {   // one call over the whole buffer: the seeded stream's steps stay what a replay saw
                tt::rng_fill(buf_mut(b), static_cast<size_t>(b.size));
                return 0;
            }
            uint8_t* p = buf_mut(b);
            for (uint64_t done = 0; done < b.size;) {
                const ULONG n = static_cast<ULONG>(std::min<uint64_t>(b.size - done, 0x10000000ull));
                if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, p + done, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
                    NXLOG_E("csrng", "the system random generator failed");
                    return kNotSupported;
                }
                done += n;
            }
            return 0;
        });
    });
}

// ---- mm:u -----------------------------------------------------------------------------------------------------------------------
// Clock requests, remembered (nothing is clocked): the old forms are keyed by module, the new ones by the id Initialize hands out.
struct MmSlot {
    bool used = false, old_form = false;
    uint32_t key = 0, module = 0, rate = 0;
};
struct Mm {
    std::mutex mu;
    uint32_t next_id = 0;
    std::array<MmSlot, 16> slots{};
};
Mm& mm() {
    static Mm m;
    return m;
}
uint32_t mm_open(bool old_form, uint32_t module) {   // a new id, and a slot for it when one is free
    Mm& m = mm();
    std::lock_guard<std::mutex> lk(m.mu);
    const uint32_t id = ++m.next_id;
    for (MmSlot& s : m.slots)
        if (!s.used) {
            s = MmSlot{true, old_form, old_form ? module : id, module, 0};
            NXLOG_D("mm", "request %u for module %u", id, module);
            return id;
        }
    NXLOG_W("mm", "every clock request slot is in use: module %u is not kept", module);
    return id;
}
MmSlot* mm_find(Mm& m, bool old_form, uint32_t key) {
    for (MmSlot& s : m.slots)
        if (s.used && s.old_form == old_form && s.key == key) return &s;
    return nullptr;
}
std::shared_ptr<ipc::Service> make_mm() {
    return make_iface("mmnv::IRequest", [](ipc::Service& s) {
        const auto fin = [](bool old_form) {
            return [old_form](ipc::Request& req, ipc::Response&) -> uint32_t {
                Mm& m = mm();
                std::lock_guard<std::mutex> lk(m.mu);
                if (MmSlot* p = mm_find(m, old_form, req.arg<uint32_t>(0))) *p = MmSlot{};
                return 0;
            };
        };
        const auto set = [](bool old_form) {
            return [old_form](ipc::Request& req, ipc::Response&) -> uint32_t {
                Mm& m = mm();
                std::lock_guard<std::mutex> lk(m.mu);
                if (MmSlot* p = mm_find(m, old_form, req.arg<uint32_t>(0))) p->rate = req.arg<uint32_t>(4);
                return 0;
            };
        };
        const auto get = [](bool old_form) {
            return [old_form](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
                Mm& m = mm();
                std::lock_guard<std::mutex> lk(m.mu);
                const MmSlot* p = mm_find(m, old_form, req.arg<uint32_t>(0));
                rsp.push<uint32_t>(p ? p->rate : 0);
                return 0;
            };
        };
        s.add_cmd(0, "InitializeOld", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            mm_open(true, req.arg<uint32_t>(0));
            return 0;
        });
        s.add_cmd(1, "FinalizeOld", fin(true));
        s.add_cmd(2, "SetAndWaitOld", set(true));
        s.add_cmd(3, "GetOld", get(true));
        s.add_cmd(4, "Initialize", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(mm_open(false, req.arg<uint32_t>(0)));
            return 0;
        });
        s.add_cmd(5, "Finalize", fin(false));
        s.add_cmd(6, "SetAndWait", set(false));
        s.add_cmd(7, "Get", get(false));
    });
}

// ---- hwopus ---------------------------------------------------------------------------------------------------------------------
bool libopus_rate(uint32_t r) { return r == 8000 || r == 12000 || r == 16000 || r == 24000 || r == 48000; }

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// Samples per channel an Opus packet decodes to (RFC 6716 section 3.1: the TOC byte), at `rate`.
uint32_t packet_samples(const uint8_t* pkt, uint32_t size, uint32_t rate) {
    if (size < 1) return 0;
    const uint32_t toc = pkt[0], config = toc >> 3;
    static const uint32_t silk[4] = {4, 8, 16, 24}, hybrid[2] = {4, 8}, celt[4] = {1, 2, 4, 8};   // in 2.5 ms units
    const uint32_t len = config < 12 ? silk[config & 3] : config < 16 ? hybrid[config & 1] : celt[config & 3];
    const uint32_t count_code = toc & 3;   // 0: one frame, 1 and 2: two frames, 3: the count is in the next byte
    uint32_t frames = 2;
    if (count_code == 0) {
        frames = 1;
    } else if (count_code == 3) {
        if (size < 2) return 0;
        frames = pkt[1] & 0x3F;
    }
    return frames * len * rate / 400;
}

// A decoder for what libopus cannot decode here: it answers each packet's size and length with silence.
std::shared_ptr<ipc::Service> make_silent_decoder(uint32_t rate, uint32_t channels) {
    return make_iface("IHardwareOpusDecoder", [rate, channels](ipc::Service& s) {
        const auto decode = [rate, channels](bool perf) {
            return [rate, channels, perf](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
                const ipc::Buffer in = req.in_buf(0), out = req.out_buf(0);
                uint32_t consumed = 0, samples = 0;
                if (in.addr && in.size >= 8) {
                    const uint8_t* p = buf_ptr(in);
                    const uint32_t size = be32(p);
                    if (static_cast<uint64_t>(size) + 8 <= in.size) {   // the packet fits after its header
                        consumed = 8 + size;
                        samples = packet_samples(p + 8, size, rate);
                    }
                }
                if (out.addr && out.size > 0) {
                    const uint64_t frame = static_cast<uint64_t>(channels) * 2;
                    const uint64_t bytes = std::min<uint64_t>(out.size, static_cast<uint64_t>(samples) * frame);
                    samples = static_cast<uint32_t>(bytes / frame);
                    zero_buffer(out);
                }
                rsp.push<uint32_t>(consumed);
                rsp.push<uint32_t>(samples);
                if (perf) rsp.push<uint64_t>(0);
                return 0;
            };
        };
        s.add_cmd(0, "DecodeInterleavedOld", decode(false));
        s.add_cmd(1, "SetContext", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        s.add_cmd(2, "DecodeInterleavedForMultiStreamOld", decode(false));
        s.add_cmd(3, "SetContextForMultiStream", [](ipc::Request&, ipc::Response&) -> uint32_t { return 0; });
        s.add_cmd(4, "DecodeInterleavedWithPerfOld", decode(true));
        s.add_cmd(5, "DecodeInterleavedForMultiStreamWithPerfOld", decode(true));
        s.add_cmd(6, "DecodeInterleavedWithPerfAndResetOld", decode(true));
        s.add_cmd(7, "DecodeInterleavedForMultiStreamWithPerfAndResetOld", decode(true));
        s.add_cmd(8, "DecodeInterleaved", decode(true));
        s.add_cmd(9, "DecodeInterleavedForMultiStream", decode(true));
    });
}

std::atomic<int> g_single_opens{0}, g_multi_opens{0};

// What both open commands give libopus: the stream layout and room for 120 ms per packet.
opus::Config decoder_config(opus::Kind kind, uint32_t rate, uint32_t channels, uint32_t streams, uint32_t coupled) {
    opus::Config cfg;
    cfg.kind = kind;
    cfg.rate = rate;
    cfg.channels = channels;
    cfg.streams = streams;
    cfg.coupled = coupled;
    cfg.frame_samples = rate / 1000 * 120;
    return cfg;
}

uint32_t open_single(ipc::Request& req, ipc::Response& rsp) {
    const uint32_t asked_rate = req.arg<uint32_t>(0), asked_channels = req.arg<uint32_t>(4);
    if (g_single_opens.fetch_add(1) < 4)
        NXLOG_I("hwopus", "cmd %u: open a decoder for %u Hz, %u channels", req.cmd, asked_rate, asked_channels);
    const uint32_t rate = asked_rate ? asked_rate : 48000;
    const uint32_t channels = asked_channels == 0 || asked_channels > 8 ? 2 : asked_channels;
    if (opus::available() && channels <= 2 && libopus_rate(rate)) {
        rsp.push_object(opus::make_decoder_object(decoder_config(opus::Kind::Opus, rate, channels, 1, 0)));
    } else {
        rsp.push_object(make_silent_decoder(rate, channels));
    }
    return 0;
}

uint32_t open_multistream(ipc::Request& req, ipc::Response& rsp, uint64_t need, uint64_t mapping_at) {
    const ipc::Buffer b = req.in_buf(0);
    if (!b.addr || b.size < need) return opus::kResBadArg;
    const uint8_t* p = buf_ptr(b);
    uint32_t rate, channels, streams, coupled;
    std::memcpy(&rate, p, 4);
    std::memcpy(&channels, p + 4, 4);
    std::memcpy(&streams, p + 8, 4);
    std::memcpy(&coupled, p + 12, 4);
    if (!libopus_rate(rate)) return opus::kResInvalidRate;
    if (channels == 0 || channels > 255) return opus::kResInvalidChannels;
    if (streams == 0 || coupled > streams || streams + coupled > 255) return opus::kResBadArg;
    for (uint32_t i = 0; i < channels; ++i) {
        const uint8_t m = p[mapping_at + i];
        if (m != 255 && m >= streams + coupled) return opus::kResBadArg;
    }
    if (g_multi_opens.fetch_add(1) < 4)
        NXLOG_I("hwopus", "cmd %u: open a multistream decoder for %u Hz, %u channels, %u streams, %u stereo", req.cmd, rate, channels, streams,
                coupled);
    const bool have_libopus = opus::available();
    if (!have_libopus) return opus::kResUnimplemented;
    opus::Config cfg = decoder_config(opus::Kind::Multistream, rate, channels, streams, coupled);
    std::memcpy(cfg.mapping.data(), p + mapping_at, cfg.mapping.size());
    rsp.push_object(opus::make_decoder_object(cfg));
    return 0;
}

std::shared_ptr<ipc::Service> make_hwopus() {
    return make_iface("IHardwareOpusDecoderManager", [](ipc::Service& s) {
        const auto work = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            NXLOG_I("hwopus", "cmd %u: work buffer of 0x8000 bytes", req.cmd);
            rsp.push<uint32_t>(0x8000);
            return 0;
        };
        s.add_cmd(0, "OpenHardwareOpusDecoder", open_single);
        s.add_cmd(1, "GetWorkBufferSize", work);
        s.add_cmd(2, "OpenHardwareOpusDecoderForMultiStream",
                  [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return open_multistream(req, rsp, 0x110, 0x10); });
        s.add_cmd(3, "GetWorkBufferSizeForMultiStream", work);
        s.add_cmd(4, "OpenHardwareOpusDecoderEx", open_single);   // OpusParametersEx in the raw data, not a buffer
        s.add_cmd(5, "GetWorkBufferSizeEx", work);
        s.add_cmd(6, "OpenHardwareOpusDecoderForMultiStreamEx",
                  [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return open_multistream(req, rsp, 0x118, 0x18); });
        s.add_cmd(7, "GetWorkBufferSizeForMultiStreamEx", work);
        s.add_cmd(8, "GetWorkBufferSizeExEx", work);
        s.add_cmd(9, "GetWorkBufferSizeForMultiStreamExEx", work);
    });
}

// ---- aoc:u ----------------------------------------------------------------------------------------------------------------------
uint64_t app_id() { return profile().application_id; }

// ListAddOnContent*: the indices from list position `offset`, as many as `count` and the buffer allow.
uint32_t list_add_ons(const ipc::Buffer& b, uint32_t offset, uint32_t count, bool this_title) {
    zero_buffer(b);
    const uint64_t room = b.addr ? b.size / 4 : 0;
    const uint64_t want = count < room ? uint64_t{count} : room;   // the indices asked for that fit the buffer
    if (!this_title) return 0;
    const std::vector<AddOnContent>& list = add_on_contents();
    uint32_t n = 0;
    for (uint64_t i = offset; i < list.size() && n < want; ++i, ++n) {
        const uint32_t index = static_cast<uint32_t>(list[i].index);
        std::memcpy(buf_mut(b) + 4 * n, &index, 4);
    }
    return n;
}

std::shared_ptr<ipc::Service> make_purchase_manager() {
    return make_iface("IPurchaseEventManager", [](ipc::Service& s) {
        s.add_cmd(2, "GetPurchasedEventReadableHandle", event_handle(events().aoc_purchased));
        s.add_cmd(3, "PopPurchasedProductInfo", refuse("aoc", "PopPurchasedProductInfo", kAocNoPurchase));
        s.add_cmd(4, "PopPurchasedProductInfoWithUid", refuse("aoc", "PopPurchasedProductInfoWithUid", kAocNoPurchase));
    });
}

std::shared_ptr<ipc::Service> make_aoc() {
    return make_iface("IAddOnContentManager", [](ipc::Service& s) {
        s.add_cmd(0, "CountAddOnContentByApplicationId", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint64_t id = req.arg<uint64_t>(0);
            const int32_t n = id == app_id() ? static_cast<int32_t>(add_on_contents().size()) : 0;
            NXLOG_I("aoc", "CountAddOnContentByApplicationId(%016llx) = %d", static_cast<unsigned long long>(id), n);
            rsp.push<int32_t>(n);
            return 0;
        });
        s.add_cmd(1, "ListAddOnContentByApplicationId", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint64_t id = req.arg<uint64_t>(0);
            const uint32_t n = list_add_ons(req.out_buf(0), req.arg<uint32_t>(8), req.arg<uint32_t>(12), id == app_id());
            NXLOG_I("aoc", "ListAddOnContentByApplicationId(%016llx) = %u", static_cast<unsigned long long>(id), n);
            rsp.push<uint32_t>(n);
            return 0;
        });
        s.add_cmd(2, "CountAddOnContent", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const int32_t n = static_cast<int32_t>(add_on_contents().size());
            NXLOG_I("aoc", "CountAddOnContent = %d", n);
            rsp.push<int32_t>(n);
            return 0;
        });
        s.add_cmd(3, "ListAddOnContent", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t n = list_add_ons(req.out_buf(0), req.arg<uint32_t>(0), req.arg<uint32_t>(4), true);
            NXLOG_I("aoc", "ListAddOnContent = %u", n);
            rsp.push<uint32_t>(n);
            return 0;
        });
        s.add_cmd(4, "GetAddOnContentBaseIdByApplicationId", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(req.arg<uint64_t>(0) + 0x1000);
            return 0;
        });
        s.add_cmd(5, "GetAddOnContentBaseId", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            NXLOG_I("aoc", "GetAddOnContentBaseId = %016llx", static_cast<unsigned long long>(app_id() + 0x1000));
            rsp.push<uint64_t>(app_id() + 0x1000);
            return 0;
        });
        s.add_cmd(6, "PrepareAddOnContentByApplicationId", accept("PrepareAddOnContentByApplicationId"));
        s.add_cmd(7, "PrepareAddOnContent", accept("PrepareAddOnContent"));
        s.add_cmd(8, "GetAddOnContentListChangedEvent", event_handle(events().aoc_changed));
        s.add_cmd(9, "GetAddOnContentLostErrorCode", answer<uint64_t>(0));
        s.add_cmd(10, "GetAddOnContentListChangedEventWithProcessId", event_handle(events().aoc_changed));
        s.add_cmd(11, "NotifyMountAddOnContent", accept("NotifyMountAddOnContent"));
        s.add_cmd(12, "NotifyUnmountAddOnContent", accept("NotifyUnmountAddOnContent"));
        s.add_cmd(13, "IsAddOnContentMountedForDebug", answer<uint8_t>(0));
        s.add_cmd(50, "CheckAddOnContentMountStatus", accept("CheckAddOnContentMountStatus"));
        const auto purchase = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_purchase_manager());
            return 0;
        };
        s.add_cmd(100, "CreateEcPurchasedEventManager", purchase);
        s.add_cmd(101, "CreatePermanentEcPurchasedEventManager", purchase);
        s.add_cmd(110, "CreateContentsServiceManager", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_iface("IContentsServiceManager", [](ipc::Service& c) {
                c.add_cmd(0, "RequestContentsAuthorizationToken", refuse("aoc", "RequestContentsAuthorizationToken", kAocNotConnected));
            }));
            return 0;
        });
        s.add_cmd(200, "SetRequiredAddOnContentsOnContentsAvailabilityTransition",
                  accept("SetRequiredAddOnContentsOnContentsAvailabilityTransition"));
    });
}

// ---- caps:u ---------------------------------------------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_caps() {
    return make_iface("IAlbumApplicationService", [](ipc::Service& s) {
        const auto empty_list = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<uint64_t>(0);
            return 0;
        };
        s.add_cmd(32, "SetShimLibraryVersion", accept("SetShimLibraryVersion"));
        s.add_cmd(102, "GetAlbumFileList0AafeAruidDeprecated", empty_list);
        s.add_cmd(103, "DeleteAlbumFileByAruid", accept("DeleteAlbumFileByAruid"));
        s.add_cmd(104, "GetAlbumFileSizeByAruid", answer<uint64_t>(0));
        s.add_cmd(105, "DeleteAlbumFileByAruidForDebug", accept("DeleteAlbumFileByAruidForDebug"));
        s.add_cmd(110, "LoadAlbumScreenShotImageByAruid", accept("LoadAlbumScreenShotImageByAruid"));
        s.add_cmd(120, "LoadAlbumScreenShotThumbnailImageByAruid", accept("LoadAlbumScreenShotThumbnailImageByAruid"));
        s.add_cmd(130, "PrecheckToCreateContentsByAruid", accept("PrecheckToCreateContentsByAruid"));
        s.add_cmd(140, "GetAlbumFileList1AafeAruidDeprecated", empty_list);
        s.add_cmd(141, "GetAlbumFileList2AafeUidAruidDeprecated", empty_list);
        s.add_cmd(142, "GetAlbumFileList3AaeAruidDeprecated", empty_list);
    });
}

// ---- mii ------------------------------------------------------------------------------------------------------------------------
constexpr size_t kCharInfo = 0x58, kCharInfoElement = 0x5C, kStoreData = 0x44, kDefaultMiis = 6;

// nn::mii::CharInfo of default Mii k (k < 6), or the BuildRandom form (random create id, colour 0, the given gender).
std::array<uint8_t, kCharInfo> char_info(int k, bool random, uint32_t gender) {
    std::array<uint8_t, kCharInfo> c{};
    if (random) {
        BCryptGenRandom(nullptr, c.data(), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    } else {
        const uint64_t a = 0x4D49494E44454641ull + static_cast<uint64_t>(k), b = 0x554C54444D484F53ull;
        std::memcpy(c.data(), &a, 8);
        std::memcpy(c.data() + 8, &b, 8);
    }
    c[6] = static_cast<uint8_t>((c[6] & 0x0F) | 0x40);   // a version-4 UUID
    c[8] = static_cast<uint8_t>((c[8] & 0x3F) | 0x80);
    const char* nick = "no name";
    for (size_t i = 0; nick[i]; ++i) c[0x10 + 2 * i] = static_cast<uint8_t>(nick[i]);
    c[0x27] = random ? 0 : static_cast<uint8_t>(2 * k);                  // favourite colour
    c[0x28] = random ? (gender == 1 ? 1 : 0) : (k >= 3 ? 1 : 0);        // gender
    static const uint8_t face[] = {
        64, 64, 0, 0, 0, 0, 0, 0,                     // 0x29 height, build, type, region move, faceline type / colour / wrinkle / make
        33, 1, 0,                                     // 0x31 hair type, colour, flip
        2, 8, 4, 3, 4, 2, 12,                         // 0x34 eye type, colour, scale, aspect, rotate, x, y
        6, 1, 4, 3, 6, 2, 10,                         // 0x3B eyebrow type, colour, scale, aspect, rotate, x, y
        1, 4, 9,                                      // 0x42 nose type, scale, y
        23, 19, 4, 3, 13,                             // 0x45 mouth type, colour, scale, aspect, y
        8, 0, 0, 4, 10,                               // 0x4A beard colour, beard type, mustache type, scale, y
        0, 8, 4, 10,                                  // 0x4F glass type, colour, scale, y
        0, 4, 2, 20,                                  // 0x53 mole type, scale, x, y
    };
    std::memcpy(c.data() + 0x29, face, sizeof face);
    return c;
}

// Get / Get1: the default source's Miis (flag bit 1), as many whole elements as fit.
uint32_t write_miis(const ipc::Request& req, size_t stride) {
    const ipc::Buffer b = req.out_buf(0);
    zero_buffer(b);
    if (!(req.arg<uint32_t>(0) & 2) || !b.addr) return 0;
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(kDefaultMiis, b.size / stride));
    for (uint32_t k = 0; k < n; ++k) {
        const auto c = char_info(static_cast<int>(k), false, 0);
        std::memcpy(buf_mut(b) + k * stride, c.data(), c.size());
        if (stride == kCharInfoElement) {
            const uint32_t source = 1;   // Default
            std::memcpy(buf_mut(b) + k * stride + kCharInfo, &source, 4);
        }
    }
    return n;
}

H echo(size_t n) {   // the first n raw bytes as they came, or n zeros when fewer came
    return [n](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        std::vector<uint8_t> v(n, 0);
        if (req.in_size >= n) std::memcpy(v.data(), req.in, n);
        rsp.out.insert(rsp.out.end(), v.begin(), v.end());
        return 0;
    };
}

std::shared_ptr<ipc::Service> make_mii_database() {
    return make_iface("IDatabaseService", [](ipc::Service& s) {
        s.add_cmd(0, "IsUpdated", answer<uint8_t>(0));
        s.add_cmd(1, "IsFullDatabase", answer<uint8_t>(0));
        s.add_cmd(2, "GetCount", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            rsp.push<int32_t>(req.arg<uint32_t>(0) & 2 ? static_cast<int32_t>(kDefaultMiis) : 0);
            return 0;
        });
        s.add_cmd(3, "Get", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            rsp.push<int32_t>(static_cast<int32_t>(write_miis(req, kCharInfoElement)));
            return 0;
        });
        s.add_cmd(4, "Get1", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            rsp.push<int32_t>(static_cast<int32_t>(write_miis(req, kCharInfo)));
            return 0;
        });
        s.add_cmd(5, "UpdateLatest", echo(kCharInfo));
        s.add_cmd(6, "BuildRandom", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const auto c = char_info(0, true, req.arg<uint32_t>(4));
            rsp.out.insert(rsp.out.end(), c.begin(), c.end());
            return 0;
        });
        s.add_cmd(7, "BuildDefault", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t k = req.arg<uint32_t>(0);
            if (k >= kDefaultMiis) return kMiiInvalidArgument;
            const auto c = char_info(static_cast<int>(k), false, 0);
            rsp.out.insert(rsp.out.end(), c.begin(), c.end());
            return 0;
        });
        const auto none = [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<int32_t>(0);
            return 0;
        };
        s.add_cmd(8, "Get2", none);
        s.add_cmd(9, "Get3", none);
        s.add_cmd(10, "UpdateLatest1", echo(kStoreData));
        s.add_cmd(11, "FindIndex", refuse("mii", "FindIndex", kMiiNotFound));
        for (const auto& c : std::vector<std::pair<uint32_t, const char*>>{{12, "Move"},          {13, "AddOrReplace"}, {14, "Delete"},
                                                                         {15, "DestroyFile"},   {16, "DeleteFile"},   {17, "Format"},
                                                                         {18, "Import"},        {19, "Export"},       {22, "SetInterfaceVersion"},
                                                                         {26, "Append"}})
            s.add_cmd(c.first, c.second, accept(c.second));
        s.add_cmd(20, "IsBrokenDatabaseWithClearFlag", answer<uint8_t>(0));
        s.add_cmd(21, "GetIndex", refuse("mii", "GetIndex", kMiiNotFound));
        s.add_cmd(23, "Convert", refuse("mii", "Convert", kMiiInvalidCharInfo));
        s.add_cmd(24, "ConvertCoreDataToCharInfo", refuse("mii", "ConvertCoreDataToCharInfo", kMiiInvalidCharInfo));
        s.add_cmd(25, "ConvertCharInfoToCoreData", refuse("mii", "ConvertCharInfoToCoreData", kMiiInvalidCharInfo));
    });
}

std::shared_ptr<ipc::Service> make_mii() {
    return make_iface("IStaticService", [](ipc::Service& s) {
        s.add_cmd(0, "GetDatabaseService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_mii_database());
            return 0;
        });
    });
}

// ---- irs ------------------------------------------------------------------------------------------------------------------------
// The IR sensor shared memory: 9 device formats of 0xE30 bytes, every camera Unconnected (2). Made once, never reset.
ServiceSharedMemory* irs_memory() {
    static std::mutex mu;
    static std::unique_ptr<ServiceSharedMemory> m;
    std::lock_guard<std::mutex> lk(mu);
    if (!m) {
        m = std::make_unique<ServiceSharedMemory>(0x8000, "irs");
        if (m->ok() && m->host()) {
            uint8_t* p = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(m->host()));
            for (uint32_t k = 0; k < 9; ++k) {
                const uint32_t unconnected = 2;
                std::memcpy(p + k * 0xE30, &unconnected, 4);
            }
        }
    }
    return m.get();
}

std::shared_ptr<ipc::Service> make_irs() {
    return make_iface("IIrSensorServer", [](ipc::Service& s) {
        for (const auto& c : std::vector<std::pair<uint32_t, const char*>>{
                 {302, "ActivateIrsensor"},          {303, "DeactivateIrsensor"},     {305, "StopImageProcessor"},
                 {306, "RunMomentProcessor"},        {307, "RunClusteringProcessor"}, {308, "RunImageTransferProcessor"},
                 {310, "RunTeraPluginProcessor"},    {314, "CheckFirmwareVersion"},   {318, "StopImageProcessorAsync"},
                 {319, "ActivateIrsensorWithFunctionLevel"}})
            s.add_cmd(c.first, c.second, accept(c.second));
        s.add_cmd(304, "GetIrsensorSharedMemoryHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            ServiceSharedMemory* m = irs_memory();
            if (!m || !m->ok()) return kNotSupported;
            rsp.out_copy.push_back(m->new_handle());
            return 0;
        });
        s.add_cmd(309, "GetImageTransferProcessorState", answer<uint32_t>(0));
        s.add_cmd(311, "GetNpadIrCameraHandle", answer<uint32_t>(0));
    });
}

// ---- nfc:user, nfc:mf:u, nfp:user --------------------------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_nfc_user() {
    auto state = std::make_shared<std::atomic<uint32_t>>(0);   // 0 NonInitialized, 1 Initialized
    return make_iface("nn::nfc::detail::IUser", [state](ipc::Service& s) {
        const auto init = [state](ipc::Request&, ipc::Response&) -> uint32_t {
            state->store(1);
            return 0;
        };
        const auto fin = [state](ipc::Request&, ipc::Response&) -> uint32_t {
            state->store(0);
            return 0;
        };
        const auto get = [state](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(state->load());
            return 0;
        };
        s.add_cmd(0, "InitializeOld", init);
        s.add_cmd(1, "FinalizeOld", fin);
        s.add_cmd(2, "GetStateOld", get);
        s.add_cmd(3, "IsNfcEnabledOld", answer<uint8_t>(0));
        s.add_cmd(400, "Initialize", init);
        s.add_cmd(401, "Finalize", fin);
        s.add_cmd(402, "GetState", get);
        s.add_cmd(403, "IsNfcEnabled", answer<uint8_t>(0));
        s.add_cmd(404, "ListDevices", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<int32_t>(0);
            return 0;
        });
        for (const auto& c : std::vector<std::pair<uint32_t, const char*>>{
                 {405, "GetDeviceState"}, {406, "GetNpadId"},  {408, "StartDetection"},      {409, "StopDetection"},
                 {410, "GetTagInfo"},     {1000, "ReadMifare"}, {1001, "WriteMifare"},       {1300, "SendCommandByPassThrough"},
                 {1301, "KeepPassThroughSession"}, {1302, "ReleasePassThroughSession"}})
            s.add_cmd(c.first, c.second, refuse("nfc", c.second, kNfcNoDevice));
        s.add_cmd(407, "GetAvailabilityChangeEventHandle", event_handle(events().nfc_avail));
        s.add_cmd(411, "GetActivateEventHandle", event_handle(events().nfc_activate));
        s.add_cmd(412, "GetDeactivateEventHandle", event_handle(events().nfc_deactivate));
    });
}

std::shared_ptr<ipc::Service> make_nfp_user() {
    const auto state = std::make_shared<std::atomic<uint32_t>>(0u);   // GetState: 1 once initialized, 0 after Finalize
    return make_iface("nn::nfp::detail::IUser", [state](ipc::Service& s) {
        const auto init = [state](ipc::Request& req, ipc::Response&) -> uint32_t {
            NXLOG_D("nfp", "cmd %u: initialized (no reader in use)", req.cmd);
            state->store(1);
            return 0;
        };
        s.add_cmd(0, "Initialize", init);
        s.add_cmd(1, "Finalize", [state](ipc::Request&, ipc::Response&) -> uint32_t {
            NXLOG_D("nfp", "Finalize");
            state->store(0);
            return 0;
        });
        s.add_cmd(2, "ListDevices", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            zero_buffer(req.out_buf(0));
            rsp.push<int32_t>(0);
            return 0;
        });
        for (const auto& c : std::vector<std::pair<uint32_t, const char*>>{
                 {3, "StartDetection"},       {4, "StopDetection"},           {5, "Mount"},           {6, "Unmount"},
                 {7, "OpenApplicationArea"},  {8, "GetApplicationArea"},      {9, "SetApplicationArea"}, {10, "Flush"},
                 {11, "Restore"},             {12, "CreateApplicationArea"},  {13, "GetTagInfo"},     {14, "GetRegisterInfo"},
                 {15, "GetCommonInfo"},       {16, "GetModelInfo"},           {20, "GetDeviceState"}, {21, "GetNpadId"},
                 {22, "GetApplicationAreaSize"}, {24, "RecreateApplicationArea"}, {25, "StartDetectionWithFilter"},
                 {301, "GetExtendedApplicationAreaInfo"}, {302, "GetExtendedApplicationArea"}, {303, "SetExtendedApplicationArea"}})
            s.add_cmd(c.first, c.second, refuse("nfp", c.second, kNfpNoDevice));
        s.add_cmd(17, "AttachActivateEvent", event_handle(events().nfp_activate));
        s.add_cmd(18, "AttachDeactivateEvent", event_handle(events().nfp_deactivate));
        s.add_cmd(19, "GetState", [state](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(state->load());
            return 0;
        });
        s.add_cmd(23, "GetAvailabilityChangeEventHandle", event_handle(events().nfp_avail));
        s.add_cmd(300, "InitializeWithExtendedApplicationArea", init);
    });
}

template <std::shared_ptr<ipc::Service> (*Make)()>
std::shared_ptr<ipc::Service> make_user_manager(const char* name) {
    return make_iface(name, [](ipc::Service& s) {
        s.add_cmd(0, "CreateUserInterface", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(Make());
            return 0;
        });
    });
}
std::shared_ptr<ipc::Service> make_mifare_user() {
    return make_iface("nn::nfc::mifare::detail::IUser", [](ipc::Service&) {});   // answered by ext_system.cpp
}

bool null_uid(const std::pair<uint64_t, uint64_t>& uid) { return (uid.first | uid.second) == 0; }

// ---- olsc:u ---------------------------------------------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_olsc() {
    struct State {
        std::mutex mu;
        bool initialized = false;
        std::map<std::pair<uint64_t, uint64_t>, bool> backup;
    };
    auto st = std::make_shared<State>();
    return make_iface("olsc:u", [st](ipc::Service& s) {
        s.add_cmd(0, "Initialize", [st](ipc::Request&, ipc::Response&) -> uint32_t {
            NXLOG_D("olsc", "Initialize");
            std::lock_guard<std::mutex> lk(st->mu);
            st->initialized = true;
            return 0;
        });
        s.add_cmd(13, "GetSaveDataBackupSetting", [st](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(st->mu);
            if (!st->initialized) return kOlscNotInitialized;
            const std::pair<uint64_t, uint64_t> uid{req.arg<uint64_t>(0), req.arg<uint64_t>(8)};
            if (null_uid(uid)) return kOlscNullArgument;
            const auto it = st->backup.find(uid);
            rsp.push<uint8_t>(it != st->backup.end() && it->second ? 1 : 2);   // 1 enabled, 2 not
            return 0;
        });
        s.add_cmd(14, "SetSaveDataBackupSettingEnabled", [st](ipc::Request& req, ipc::Response&) -> uint32_t {
            std::lock_guard<std::mutex> lk(st->mu);
            if (!st->initialized) return kOlscNotInitialized;
            const std::pair<uint64_t, uint64_t> uid{req.arg<uint64_t>(8), req.arg<uint64_t>(16)};
            if (null_uid(uid)) return kOlscNullArgument;
            st->backup[uid] = req.arg<uint64_t>(0) != 0;
            return 0;
        });
    });
}

// ---- banana, TMA --------------------------------------------------------------------------------------------------------------
class Banana : public ipc::StubService {
public:
    Banana() : ipc::StubService("banana") {}
    uint32_t handle(ipc::Request& req, ipc::Response& rsp) override {
        NXLOG_W("banana", "command %u (msg type %u) asked of banana; answering StubService", req.cmd, req.msg_type);
        return StubService::handle(req, rsp);   // the base class answers
    }
};

std::shared_ptr<ipc::Service> make_tma(const char* name, const std::vector<std::pair<uint32_t, const char*>>& cmds) {
    return make_iface(name, [&cmds](ipc::Service& s) {
        for (const auto& c : cmds) s.add_cmd(c.first, c.second, refuse("tma", c.second, kNotSupported));
    });
}
const std::vector<std::pair<uint32_t, const char*>>& htc_cmds() {
    static const std::vector<std::pair<uint32_t, const char*>> v = {
        {0, "GetEnvironmentVariable"}, {1, "GetEnvironmentVariableLength"}, {2, "BindHostConnectionEvent"},
        {3, "BindHostDisconnectionEvent"}, {4, "BindHostConnectionEventForSystem"}, {5, "BindHostDisconnectionEventForSystem"},
        {6, "GetBridgeIpAddress"}, {7, "GetBridgePort"}, {8, "SetCradleAttached"}, {9, "GetBridgeSubnetMask"}, {10, "GetBridgeMacAddress"},
        {11, "GetWorkingDirectoryPath"}, {12, "GetWorkingDirectoryPathSize"}, {13, "RunOnHostStart"}, {14, "RunOnHostResults"},
        {15, "SetBridgeIpAddress"}, {16, "SetBridgeSubnetMask"}, {17, "SetBridgePort"}, {18, "GetBridgeSerialNumber"},
        {19, "GetBridgeFwVersion"}, {20, "ResetBridgeSettings"}, {21, "BeginUpdateBridge"}, {22, "ContinueUpdateBridge"},
        {23, "EndUpdateBridge"}, {24, "GetBridgeType"}, {25, "GetBridgeDefaultGateway"}, {26, "SetBridgeDefaultGateway"},
        {27, "GetBridgeDhcp"}, {28, "SetBridgeDhcp"}, {29, "GetBridgeProgramVersion"}};
    return v;
}
const std::vector<std::pair<uint32_t, const char*>>& htcs_cmds() {
    static const std::vector<std::pair<uint32_t, const char*>> v = {
        {0, "Socket"}, {1, "Close"}, {2, "Connect"}, {3, "Bind"}, {4, "Listen"}, {5, "Accept"}, {6, "Recv"}, {7, "Send"}, {8, "Shutdown"},
        {9, "Fcntl"}, {10, "GetPeerNameAny"}, {11, "GetDefaultHostName"}, {12, "CreateSocketOld"}, {13, "CreateSocket"},
        {100, "RegisterProcessId"}, {101, "MonitorManager"}, {130, "Cmd130"}, {131, "Cmd131"}};
    return v;
}
const std::vector<std::pair<uint32_t, const char*>>& tenv_cmds() {
    static const std::vector<std::pair<uint32_t, const char*>> v = {{0, "GetServiceInterface"}};
    return v;
}

// ---- stpl:u -----------------------------------------------------------------------------------------------------------------------
// Switch 2 GameShare: no session, nobody connects. One process-wide state: the type and mode Initialize got, the connection event.
struct Streamplay {
    std::mutex mu;
    uint8_t type = 0, mode = 0;
    ServiceEvent connection;
};
Streamplay& streamplay() {
    static Streamplay s;
    return s;
}
constexpr uint64_t kStreamplayPage = 0x1000;
constexpr uint64_t kStreamplayHidBytes = 0x84000;

H sp_accept(const char* name) {
    return [name](ipc::Request& req, ipc::Response&) -> uint32_t {
        NXLOG_I("streamplay", "%s (cmd %u) accepted", name, req.cmd);
        return 0;
    };
}

std::shared_ptr<ipc::Service> make_streamplay_user_service() {
    return make_iface("nn::streamplay::detail::IUserService", [](ipc::Service& s) {
        s.add_cmd(1, "GetRequiredMemorySize", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint8_t type = req.arg<uint8_t>(0);
            const uint64_t guests = std::min<uint32_t>(req.arg<uint32_t>(4), 3);
            const uint64_t size = type == 1 ? guests * kStreamplayPage : kStreamplayPage + guests * 3 * kStreamplayPage;
            rsp.push<uint32_t>(static_cast<uint32_t>(size));
            return 0;
        });
        s.add_cmd(2, "Initialize", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            Streamplay& sp = streamplay();
            std::lock_guard<std::mutex> lk(sp.mu);
            sp.type = req.arg<uint8_t>(0);
            sp.mode = req.arg<uint8_t>(1);
            NXLOG_I("streamplay", "Initialize(type %u, mode %u, %u guests)", sp.type, sp.mode, req.arg<uint32_t>(4));
            return 0;
        });
        s.add_cmd(3, "Finalize", sp_accept("Finalize"));
        s.add_cmd(5, "CloseSession", sp_accept("CloseSession"));
        s.add_cmd(7, "GetConnectionStateChangedEvent", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.out_copy.push_back(streamplay().connection.new_handle());
            return 0;
        });
        s.add_cmd(8, "GetConnectionState", answer<uint64_t>(0));
        s.add_cmd(9, "GetWorkBufferSizes", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            for (int i = 0; i < 4; ++i) rsp.push<uint64_t>(kStreamplayPage);
            return 0;
        });
        s.add_cmd(25, "SetSupportedGuestNpadStyleSet", sp_accept("SetSupportedGuestNpadStyleSet"));
        s.add_cmd(27, "SetGuestNpadJoyHoldType", sp_accept("SetGuestNpadJoyHoldType"));
        s.add_cmd(300, "GetHidSharedMemory", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            ServiceSharedMemory m(kStreamplayHidBytes, "streamplay.hid");   // no guest pad, every ring empty
            if (!m.ok() || !m.host()) return kNotSupported;
            std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(m.host())), 0, static_cast<size_t>(m.size()));
            const uint32_t handle = m.new_handle();
            if (handle == 0) return kNotSupported;
            rsp.push<uint64_t>(kStreamplayHidBytes);
            rsp.push<uint64_t>(0);
            rsp.out_copy.push_back(handle);
            return 0;
        });
        s.add_cmd(502, "GetStreamPlayType", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            Streamplay& sp = streamplay();
            std::lock_guard<std::mutex> lk(sp.mu);
            rsp.push<uint8_t>(sp.type);
            return 0;
        });
        s.add_cmd(503, "GetStreamingMode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            Streamplay& sp = streamplay();
            std::lock_guard<std::mutex> lk(sp.mu);
            rsp.push<uint8_t>(sp.mode);
            return 0;
        });
        s.add_cmd(504, "PrepareMatchmake", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            NXLOG_I("streamplay", "PrepareMatchmake");
            rsp.out.resize(rsp.out.size() + 0x80, 0);
            return 0;
        });
        s.add_cmd(505, "SetWorkBuffer", sp_accept("SetWorkBuffer"));
        s.add_cmd(506, "SetGuestWorkBuffer", sp_accept("SetGuestWorkBuffer"));
        s.add_cmd(507, "Cmd507", sp_accept("Cmd507"));
        s.add_cmd(509, "Cmd509", sp_accept("Cmd509"));
        s.add_cmd(510, "SetAudioOutputVolumes", sp_accept("SetAudioOutputVolumes"));
    });
}

std::shared_ptr<ipc::Service> make_streamplay() {
    return make_iface("stpl:u", [](ipc::Service& s) {
        s.add_cmd(0, "CreateUserService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_streamplay_user_service());
            return 0;
        });
    });
}

// The factory of the TMA manager `name`: every command `cmds` lists is refused.
ipc::ServiceFactory tma_factory(const char* name, const std::vector<std::pair<uint32_t, const char*>>& (*cmds)()) {
    return [name, cmds] { return make_tma(name, cmds()); };
}

}  // namespace

void register_misc() {
    add("stpl:u", make_streamplay);
    add("csrng", make_csrng);
    register_ro();
    register_bluetooth();
    register_npns();
    register_pm();
    add("mm:u", make_mm);
    add("hwopus", make_hwopus);
    add("aoc:u", make_aoc);
    add("caps:u", make_caps);
    add("caps:su", make_screenshot_app);
    add("mii:u", make_mii);
    add("mii:e", make_mii);   // the same database (Mario Kart 8 Deluxe opens mii:e first)
    add("irs", make_irs);
    add("nfc:user", [] { return make_user_manager<&make_nfc_user>("nn::nfc::detail::IUserManager"); });
    add("nfc:mf:u", [] { return make_user_manager<&make_mifare_user>("nn::nfc::mifare::detail::IUserManager"); });
    add("nfp:user", [] { return make_user_manager<&make_nfp_user>("nn::nfp::detail::IUserManager"); });
    add("ectx:aw", [] {
        return make_iface("nn::err::context::IWriterForApplication", [](ipc::Service& s) {
            s.add_cmd(0, "CreateContextRegistrar", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
                rsp.push_object(make_iface("nn::err::context::IContextRegistrar", [](ipc::Service& r) {
                    r.add_cmd(0, "Complete", answer<int32_t>(0));
                }));
                return 0;
            });
        });
    });
    add("pcm", [] { return make_iface("pcm", [](ipc::Service&) {}); });
    add("banana", [] { return std::make_shared<Banana>(); });
    add("htc", tma_factory("nn::tma::IHtcManager", htc_cmds));
    add("htc:tenv", tma_factory("nn::htc::tenv::IServiceManager", tenv_cmds));
    add("htcs", tma_factory("nn::tma::IHtcsManager", htcs_cmds));
    add("olsc:u", make_olsc);
}

void misc_reset() {
    ro::reset_for_tests();
    bluetooth_reset();
    npns_reset();
    {
        Mm& m = mm();
        std::lock_guard<std::mutex> lk(m.mu);
        m.next_id = 0;
        m.slots = {};
    }
    events().clear_all();
}

}  // namespace nxrt::services
