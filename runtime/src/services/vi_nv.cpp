// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Portions Copyright (c) 2026 Chaos192
// Display and GPU driver services (S3c): vi:u / vi:s / vi:m (the display, its layers and its vsync event) and nvdrv / nvdrv:a / nvdrv:s /
// nvdrv:t (the NV driver's device files), for the SDK's own nn::vi and nvn start-up code.
//
// No frame passes through here: the game's NVN calls are answered by the host NVN front end, which presents on the host. So the layer's
// buffer queue (IGraphicBufferProducer, answered inside TransactParcel) holds no buffers, channel submissions are accepted and never run,
// and no syncpoint moves. What the display does see comes the other way: the front end calls victl::note_present() after each present (the
// layer's frame history, nn::vi::ListFrameHistory / GetLatestFrameNumber), and frame_rate::note_present calls the present hook installed
// here (the vsync event ticks on every present while the frame rate is unlimited).
//
// Command ids, layouts and values: https://switchbrew.org/wiki/Display_services (DisplayInfo, DisplayName, the native window parcel),
// https://switchbrew.org/wiki/Nvnflinger_services (the binder parcel) and https://switchbrew.org/wiki/NV_services (device paths, ioctl
// words, NvError, the GM20B characteristics). Facts taken from Ryujinx (MIT): src/Ryujinx.HLE/HOS/Services/Vi (result codes, scaling
// mode values), SurfaceFlinger (transaction codes, status values, query attributes, NoVSyncCapability, buffer state Queued) and Nv
// (NvResult, the address space regions, the 56-bit GPU time, 192 syncpoints, 64 ctrl events). No code from either.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nxrt/frame_rate.h"
#include "nxrt/gauge.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "nxrt/vblank.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

// vi results (module 114): 1 for a bad argument, 7 for something that is not there.
const uint32_t kViInvalidArgument = res(114, 1);   // 0x272
const uint32_t kViNotFound = res(114, 7);          // 0xE72

struct Size2 {
    uint32_t w, h;
};
// The display follows the console's operation mode, read at each call.
Size2 display_size() { return amctl::docked() ? Size2{1920, 1080} : Size2{1280, 720}; }

void put32(uint8_t* p, size_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }
void put64(uint8_t* p, size_t off, uint64_t v) { std::memcpy(p + off, &v, 8); }

// ======================================================================================================================================
// vi state: the layer slots, the layer id counter and the vsync event (one lock); the frame history (its own lock)
// ======================================================================================================================================
constexpr int kSlots = 8;
constexpr uint64_t kFirstLayerId = 0x10000;

struct LayerSlot {
    bool used = false;
    uint64_t id = 0;
    bool connected = false;
    int32_t buffer_count = 2;
    uint32_t width = 0, height = 0;
    ServiceEvent event;
};

struct Vi {
    std::mutex mu;
    LayerSlot slot[kSlots];
    uint64_t next_layer_id = kFirstLayerId;
    uint32_t binder_codes_logged = 0;   // a warning per unknown transaction code (folded into 32 bits)
    // the vsync event and its thread
    ServiceEvent vsync;
    bool vsync_started = false;
    std::thread vsync_thread;
    std::atomic<bool> vsync_running{false};
    std::atomic<bool> vsync_stop{false};
    std::mutex wait_mu;
    std::condition_variable wait_cv;
};
Vi& vi() {
    static Vi* v = new Vi;   // never destroyed: the vsync thread may still run at exit
    return *v;
}

// binder id = slot + 1; 0 when no slot is free. Called with vi().mu held.
uint32_t open_layer(Vi& v, uint64_t id) {
    for (int i = 0; i < kSlots; ++i)
        if (v.slot[i].used && v.slot[i].id == id) return static_cast<uint32_t>(i + 1);
    for (int i = 0; i < kSlots; ++i) {
        LayerSlot& s = v.slot[i];
        if (s.used) continue;
        const Size2 d = display_size();
        s.used = true;
        s.id = id;
        s.connected = false;
        s.buffer_count = 2;
        s.width = d.w;
        s.height = d.h;
        s.event = ServiceEvent();
        return static_cast<uint32_t>(i + 1);
    }
    return 0;
}

void close_layer(Vi& v, uint64_t id) {
    for (LayerSlot& s : v.slot) {
        if (!s.used || s.id != id) continue;
        s.used = false;
        s.connected = false;
        s.event = ServiceEvent();
        return;
    }
    NXLOG_W("vi", "close of layer %llu, which is not open", static_cast<unsigned long long>(id));
}

LayerSlot* layer_of_binder(Vi& v, int32_t binder) {
    if (binder < 1 || binder > kSlots) return nullptr;
    LayerSlot& s = v.slot[binder - 1];
    return s.used ? &s : nullptr;
}

// The native window parcel OpenLayer / CreateStrayLayer hand out (0x3C bytes): a flattened binder object named "dispdrv".
constexpr uint64_t kNativeWindowSize = 0x3C;
void write_native_window(const ipc::Buffer& b, uint32_t binder) {
    if (!b.addr || b.size < kNativeWindowSize) {
        NXLOG_W("vi", "native window buffer of %llu bytes (0x3C needed): not written", static_cast<unsigned long long>(b.size));
        return;
    }
    uint8_t p[kNativeWindowSize] = {};
    put32(p, 0x00, 0x28);   // data size
    put32(p, 0x04, 0x10);   // data offset
    put32(p, 0x08, 4);      // objects size
    put32(p, 0x0C, 0x38);   // objects offset
    put32(p, 0x10, 2);      // flattened binder type
    put32(p, 0x14, 1);
    put64(p, 0x18, binder);
    std::memcpy(p + 0x28, "dispdrv", 8);
    buf_write(b, p, kNativeWindowSize);
}

// ---- the frame history ---------------------------------------------------------------------------------------------------------------
struct Presented {
    uint64_t frame;
    int64_t queue_ns, present_ns;
};
struct History {
    std::mutex mu;
    uint64_t frames = 0;
    std::deque<Presented> last;   // oldest first, at most kKept
};
constexpr size_t kKept = 8;
History& history() {
    static History* h = new History;
    return *h;
}

constexpr uint64_t kNsPerSec = 1000000000ull;
uint64_t ticks_to_ns(uint64_t t, uint64_t hz) { return hz ? (t / hz) * kNsPerSec + (t % hz) * kNsPerSec / hz : 0; }
uint64_t ns_to_ticks(int64_t ns, uint64_t hz) {
    if (ns < 0) return 0;
    const uint64_t u = static_cast<uint64_t>(ns);
    return (u / kNsPerSec) * hz + (u % kNsPerSec) * hz / kNsPerSec;
}

// ---- the vsync event -----------------------------------------------------------------------------------------------------------------
// Its rate without the speed hack: the frame rate's vsync rate, or with none (unlimited) the host screen's refresh, else 60.
double event_hz() {
    const double hz = frame_rate::vsync_hz();
    if (hz > 0) return hz;
    vblank::Screen s;
    if (vblank::screen(&s) && s.period) return 1e9 / static_cast<double>(s.period);
    return 60.0;
}

double tick_rate() {
    const double r = event_hz() * kernel::time_scale();
    return r > 0 ? r : 60.0;
}

void vsync_main(ServiceEvent ev) {
    log_set_thread_name("vi vsync");
    Vi& v = vi();
    constexpr uint64_t kEarly = 200000;   // a wake within 0.2 ms of the deadline is the tick
    uint64_t deadline = vblank::now_ns() + static_cast<uint64_t>(1e9 / tick_rate());
    vblank::Ticks ticks;
    int mode = -1;   // 1: on the screen's refreshes, 0: on its own clock
    while (!v.vsync_stop.load()) {
        uint64_t now = vblank::now_ns();
        if (deadline > now + kEarly) {
            std::unique_lock<std::mutex> lk(v.wait_mu);
            v.wait_cv.wait_for(lk, std::chrono::nanoseconds(deadline - now), [&v] { return v.vsync_stop.load(); });
            continue;
        }
        ev.signal();
        const double rate = tick_rate();
        vblank::Screen s;
        const bool have_screen = vblank::screen(&s) && s.period;
        const bool on_screen = kernel::time_scale() == 1.0 && have_screen && rate <= 1.02 * (1e9 / static_cast<double>(s.period));
        if (static_cast<int>(on_screen) != mode) {
            mode = on_screen ? 1 : 0;
            ticks = vblank::Ticks{};
            if (on_screen)
                NXLOG_I("vi", "vsync event: %.2f Hz on the %s screen refresh (%.2f Hz, %u refreshes a tick)", rate, vblank::source_name(),
                        1e9 / static_cast<double>(s.period), vblank::refreshes_per_tick(s, rate));
            else
                NXLOG_I("vi", "vsync event: %.2f Hz on its own clock", rate);
        }
        now = vblank::now_ns();
        if (on_screen) {
            deadline = vblank::next_tick(ticks, s, rate, now);
        } else {
            uint64_t period = static_cast<uint64_t>(1e9 / rate);
            if (!period) period = 1;
            deadline += period;
            if (deadline <= now) deadline += ((now - deadline) / period + 1) * period;   // missed ticks are skipped
        }
    }
}

// frame_rate::note_present, while the frame rate is unlimited: every present is a tick.
void present_hook() {
    Vi& v = vi();
    if (!v.vsync_running.load() || v.vsync_stop.load()) return;
    ServiceEvent ev;
    {
        std::lock_guard<std::mutex> lk(v.mu);
        ev = v.vsync;
    }
    ev.signal();
}

uint32_t vsync_handle() {
    Vi& v = vi();
    std::lock_guard<std::mutex> lk(v.mu);
    if (!v.vsync_started) {
        v.vsync_started = true;
        v.vsync_stop = false;
        v.vsync_running = true;
        v.vsync_thread = std::thread(vsync_main, v.vsync);
        frame_rate::set_present_hook(&present_hook);
    }
    return v.vsync.new_handle();
}

// ======================================================================================================================================
// vi services
// ======================================================================================================================================
using H = ipc::Handler;

uint32_t ok(ipc::Request&, ipc::Response&) { return 0; }

// IManagerDisplayService's events: a new one per call, never signalled.
uint32_t fresh_event(ipc::Request&, ipc::Response& rsp) {
    ServiceEvent e;
    const uint32_t h = e.new_handle();
    if (!h) return kViInvalidArgument;
    rsp.out_copy.push_back(h);
    return 0;
}

// ---- IHOSBinderDriver: the layer's IGraphicBufferProducer --------------------------------------------------------------------------
constexpr uint32_t kStatusOk = 0;
constexpr uint32_t kStatusBusy = static_cast<uint32_t>(-16);
constexpr uint32_t kStatusNoInit = static_cast<uint32_t>(-19);
constexpr uint32_t kStatusBadValue = static_cast<uint32_t>(-22);

// The request parcel's words after its interface token; reads past the buffer (at most its first 0x10000 bytes) give 0.
class ParcelIn {
public:
    explicit ParcelIn(const ipc::Buffer& b) : p_(buf_ptr(b)), size_(std::min<uint64_t>(b.size, 0x10000)) {
        uint32_t off = 0;
        std::memcpy(&off, p_ + 4, 4);
        pos_ = off;
    }
    uint32_t u32() {
        uint32_t v = 0;
        if (pos_ < size_ && size_ - pos_ >= 4) std::memcpy(&v, p_ + pos_, 4);
        pos_ += 4;
        return v;
    }
    void skip_token() {
        u32();   // strict mode policy
        const uint32_t len = u32();
        if (len < 0x100) pos_ += (((static_cast<uint64_t>(len) + 1) * 2 + 3) & ~3ull);   // UTF-16 name and its NUL, padded to 4
    }

private:
    const uint8_t* p_;
    uint64_t size_;
    uint64_t pos_ = 0;
};

void query(const LayerSlot& l, uint32_t what, std::vector<uint32_t>& w) {
    switch (what) {
        case 0: case 6: w = {l.width, kStatusOk}; return;      // width, default width
        case 1: case 7: w = {l.height, kStatusOk}; return;     // height, default height
        case 2: w = {1, kStatusOk}; return;                    // format RGBA8888
        case 3: w = {1, kStatusOk}; return;                    // min undequeued buffers
        case 9: case 10: case 11: w = {0, kStatusOk}; return;  // consumer running behind, consumer usage bits, sticky transform
        case 12: w = {static_cast<uint32_t>(l.buffer_count), kStatusOk}; return;
        default: w = {0, kStatusBadValue}; return;
    }
}

void buffer_history(int32_t want, std::vector<uint32_t>& w) {
    if (want <= 0) {
        w = {kStatusBadValue, 0};
        return;
    }
    History& h = history();
    std::lock_guard<std::mutex> lk(h.mu);
    const size_t n = std::min<size_t>({static_cast<size_t>(want), h.last.size(), kKept});
    w = {kStatusOk, static_cast<uint32_t>(n)};
    for (size_t i = 0; i < n; ++i) {
        const Presented& f = h.last[h.last.size() - 1 - i];
        const uint64_t q = static_cast<uint64_t>(f.queue_ns), p = static_cast<uint64_t>(f.present_ns);
        const uint32_t rec[8] = {static_cast<uint32_t>(f.frame), static_cast<uint32_t>(f.frame >> 32), static_cast<uint32_t>(q),
                                 static_cast<uint32_t>(q >> 32), static_cast<uint32_t>(p), static_cast<uint32_t>(p >> 32), 2, 0};
        w.insert(w.end(), rec, rec + 8);
    }
}

uint32_t transact(ipc::Request& req, ipc::Response&) {
    const int32_t binder = req.arg<int32_t>(0);
    const uint32_t code = req.arg<uint32_t>(4);
    const ipc::Buffer in = req.in_buf(0), out = req.out_buf(0);
    if (!in.addr || in.size < 16) return kViInvalidArgument;
    Vi& v = vi();
    std::lock_guard<std::mutex> lk(v.mu);
    LayerSlot* l = layer_of_binder(v, binder);
    if (!l) return kViNotFound;
    ParcelIn rd(in);
    rd.skip_token();
    std::vector<uint32_t> w;
    switch (code) {
        case 1: w = {0, kStatusNoInit}; break;                        // RequestBuffer: no buffer
        case 2: {                                                     // SetBufferCount
            const int32_t n = static_cast<int32_t>(rd.u32());
            if (n < 1) {
                w = {kStatusBadValue};
            } else {
                l->buffer_count = n;
                w = {kStatusOk};
            }
            break;
        }
        case 3: w = {0xFFFFFFFFu, kStatusBusy}; break;                // DequeueBuffer: slot -1
        case 7: w = {kStatusNoInit}; break;                           // QueueBuffer
        case 9: query(*l, rd.u32(), w); break;                        // Query
        case 10:                                                      // Connect: width, height, NoVSyncCapability, no pending buffers
            l->connected = true;
            w = {l->width, l->height, 0x10, 0, kStatusOk};
            break;
        case 11:                                                      // Disconnect
            w = {l->connected ? kStatusOk : kStatusNoInit};
            l->connected = false;
            break;
        case 17: buffer_history(static_cast<int32_t>(rd.u32()), w); break;   // GetBufferHistory
        case 4: case 5: case 6: case 8: case 13: case 14:             // Detach / DetachNext / Attach / Cancel / Allocate / SetPreallocated
            w = {kStatusOk};
            break;
        default:
            if (!(v.binder_codes_logged & (1u << (code & 31)))) {
                v.binder_codes_logged |= 1u << (code & 31);
                NXLOG_W("vi", "binder transaction code %u answered OK", code);
            }
            w = {kStatusOk};
            break;
    }
    const uint32_t data = static_cast<uint32_t>(4 * w.size());
    std::vector<uint8_t> reply(16 + data);
    put32(reply.data(), 0, data);
    put32(reply.data(), 4, 0x10);
    put32(reply.data(), 8, 0);
    put32(reply.data(), 12, 0x10 + data);
    if (data) std::memcpy(reply.data() + 16, w.data(), data);
    if (out.addr && out.size < reply.size())
        NXLOG_W("vi", "binder reply of %zu bytes cut to the %llu-byte buffer", reply.size(), static_cast<unsigned long long>(out.size));
    buf_write(out, reply.data(), reply.size());
    return 0;
}

std::shared_ptr<ipc::Service> make_binder() {
    return make_iface("nns::hosbinder::IHOSBinderDriver", [](ipc::Service& s) {
        s.add_cmd(0, "TransactParcel", transact);
        s.add_cmd(1, "AdjustRefcount", ok);
        s.add_cmd(2, "GetNativeHandle", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            Vi& v = vi();
            std::lock_guard<std::mutex> lk(v.mu);
            LayerSlot* l = layer_of_binder(v, req.arg<int32_t>(0));
            if (!l) return kViNotFound;
            const uint32_t h = l->event.new_handle();
            if (!h) return kViInvalidArgument;
            rsp.out_copy.push_back(h);
            return 0;
        });
        s.add_cmd(3, "TransactParcelAuto", transact);
    });
}

// ---- ISystemDisplayService / IManagerDisplayService -----------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_system_display() {
    return make_iface("nn::visrv::sf::ISystemDisplayService", [](ipc::Service& s) {
        s.add_cmd(1200, "GetZOrderCountMin", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<int64_t>(1);
            return 0;
        });
        s.add_cmd(1202, "GetZOrderCountMax", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<int64_t>(255);
            return 0;
        });
        s.add_cmd(1203, "GetDisplayLogicalResolution", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const Size2 d = display_size();
            rsp.push<int32_t>(static_cast<int32_t>(d.w));
            rsp.push<int32_t>(static_cast<int32_t>(d.h));
            return 0;
        });
        s.add_cmd(3200, "GetDisplayMode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const Size2 d = display_size();
            rsp.push<uint32_t>(d.w);
            rsp.push<uint32_t>(d.h);
            rsp.push<float>(static_cast<float>(event_hz()));
            rsp.push<uint32_t>(0);
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_manager_display() {
    return make_iface("nn::visrv::sf::IManagerDisplayService", [](ipc::Service& s) {
        s.add_cmd(2010, "CreateManagedLayer", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            Vi& v = vi();
            std::lock_guard<std::mutex> lk(v.mu);
            rsp.push<uint64_t>(v.next_layer_id++);
            return 0;
        });
        s.add_cmd(2300, "AcquireLayerTexturePresentingEvent", fresh_event);
        s.add_cmd(2302, "GetDisplayHotplugEvent", fresh_event);
        s.add_cmd(2303, "GetDisplayModeChangedEvent", fresh_event);
    });
}

// ---- IApplicationDisplayService ----------------------------------------------------------------------------------------------------------
// The display a DisplayName (the first 0x40 raw bytes, up to the first NUL) names; -1 for none.
int64_t display_id(const ipc::Request& req) {
    char name[0x41] = {};
    if (req.in) std::memcpy(name, req.in, std::min<size_t>(req.in_size, 0x40));
    static const char* const kNames[] = {"Default", "External", "Edid", "Internal", "Null"};
    for (int64_t i = 0; i < 5; ++i)
        if (std::strcmp(name, kNames[i]) == 0) return i;
    return -1;
}

uint32_t vsync_event(ipc::Request&, ipc::Response& rsp) {
    const uint32_t h = vsync_handle();
    if (!h) return kViInvalidArgument;
    rsp.out_copy.push_back(h);
    return 0;
}

std::shared_ptr<ipc::Service> make_display_service() {
    return make_iface("nn::visrv::sf::IApplicationDisplayService", [](ipc::Service& s) {
        s.add_cmd(100, "GetRelayService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_binder());
            return 0;
        });
        s.add_cmd(101, "GetSystemDisplayService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_system_display());
            return 0;
        });
        s.add_cmd(102, "GetManagerDisplayService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_manager_display());
            return 0;
        });
        s.add_cmd(103, "GetIndirectDisplayTransactionService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_binder());
            return 0;
        });
        s.add_cmd(1000, "ListDisplays", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            uint8_t info[0x60] = {};
            std::memcpy(info, "Default", 7);
            info[0x40] = 1;              // has a layer limit
            put64(info, 0x48, 1);        // at most one layer
            put64(info, 0x50, 1920);
            put64(info, 0x58, 1080);
            buf_write(req.out_buf(0), info, sizeof info);
            rsp.push<uint64_t>(1);
            return 0;
        });
        s.add_cmd(1010, "OpenDisplay", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int64_t id = display_id(req);
            if (id < 0) return kViInvalidArgument;
            rsp.push<uint64_t>(static_cast<uint64_t>(id));
            return 0;
        });
        s.add_cmd(1011, "OpenDefaultDisplay", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint64_t>(0);
            return 0;
        });
        s.add_cmd(1020, "CloseDisplay", ok);
        s.add_cmd(1101, "SetDisplayEnabled", ok);
        s.add_cmd(1102, "GetDisplayResolution", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            const Size2 d = display_size();
            rsp.push<int64_t>(d.w);
            rsp.push<int64_t>(d.h);
            return 0;
        });
        s.add_cmd(2020, "OpenLayer", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            if (display_id(req) < 0) return kViInvalidArgument;
            const uint64_t id = req.arg<uint64_t>(0x40);
            Vi& v = vi();
            std::lock_guard<std::mutex> lk(v.mu);
            const uint32_t binder = open_layer(v, id);
            if (!binder) return kViInvalidArgument;
            write_native_window(req.out_buf(0), binder);
            rsp.push<int64_t>(static_cast<int64_t>(kNativeWindowSize));
            return 0;
        });
        s.add_cmd(2021, "CloseLayer", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            Vi& v = vi();
            std::lock_guard<std::mutex> lk(v.mu);
            close_layer(v, req.arg<uint64_t>(0));
            return 0;
        });
        s.add_cmd(2030, "CreateStrayLayer", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            Vi& v = vi();
            std::lock_guard<std::mutex> lk(v.mu);
            const uint64_t id = v.next_layer_id++;
            const uint32_t binder = open_layer(v, id);
            if (!binder) return kViInvalidArgument;
            write_native_window(req.out_buf(0), binder);
            rsp.push<uint64_t>(id);
            rsp.push<int64_t>(static_cast<int64_t>(kNativeWindowSize));
            return 0;
        });
        s.add_cmd(2031, "DestroyStrayLayer", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            Vi& v = vi();
            std::lock_guard<std::mutex> lk(v.mu);
            close_layer(v, req.arg<uint64_t>(0));
            return 0;
        });
        s.add_cmd(2101, "SetLayerScalingMode", ok);
        s.add_cmd(2102, "ConvertScalingMode", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            static const uint64_t kMap[] = {3, 0, 1, 2, 4};
            const uint32_t mode = req.arg<uint32_t>(0);
            if (mode >= 5) return kViInvalidArgument;
            rsp.push<uint64_t>(kMap[mode]);
            return 0;
        });
        s.add_cmd(2460, "GetIndirectLayerImageRequiredMemoryInfo", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            // RGBA8, rounded up to 0x20000 (in signed 64 bits, wrapping like the hardware; the division rounds toward zero)
            const uint64_t product = static_cast<uint64_t>(req.arg<int64_t>(0)) * static_cast<uint64_t>(req.arg<int64_t>(8)) * 4u;
            const int64_t biased = static_cast<int64_t>(product + 0x1FFFFu);
            rsp.push<int64_t>((biased / 0x20000) * 0x20000);
            rsp.push<int64_t>(0x1000);
            return 0;
        });
        s.add_cmd(5202, "GetDisplayVsyncEvent", vsync_event);
        s.add_cmd(5203, "GetDisplayVsyncEventForDebug", vsync_event);
    });
}

uint32_t open_display_service(ipc::Request&, ipc::Response& rsp) {
    rsp.push_object(make_display_service());
    return 0;
}

std::shared_ptr<ipc::Service> make_root(const char* name, uint32_t plain, uint32_t exchange) {
    return make_iface(name, [plain, exchange](ipc::Service& s) {
        s.add_cmd(plain, "GetDisplayService", open_display_service);
        s.add_cmd(exchange, "GetDisplayServiceWithProxyNameExchange", open_display_service);
    });
}

// ======================================================================================================================================
// nvdrv: one process-wide driver state for all four names
// ======================================================================================================================================
// NvError
constexpr uint32_t kNvOk = 0;
constexpr uint32_t kNvNotImplemented = 1;
constexpr uint32_t kNvNotSupported = 2;
constexpr uint32_t kNvNotInitialized = 3;
constexpr uint32_t kNvBadParameter = 4;
constexpr uint32_t kNvNoMemory = 6;
constexpr uint32_t kNvBadSize = 10;
constexpr uint32_t kNvBadValue = 11;
constexpr uint32_t kNvFileOperationFailed = 0x30003;
constexpr uint32_t kNvNotInProduction = 0x30006;

enum class Dev { NvMap, Ctrl, CtrlGpu, AsGpu, Channel, Other };
const char* dev_name(Dev d) {
    switch (d) {
        case Dev::NvMap: return "nvmap";
        case Dev::Ctrl: return "nvhost-ctrl";
        case Dev::CtrlGpu: return "nvhost-ctrl-gpu";
        case Dev::AsGpu: return "nvhost-as-gpu";
        case Dev::Channel: return "nvhost-gpu";
        default: return "other";
    }
}

struct NvFile {
    Dev dev = Dev::Other;
    std::map<uint32_t, ServiceEvent> events;
    uint64_t user_data = 0;
    uint32_t syncpoint = 0;
    uint32_t big_page = 0x20000;
    std::map<uint64_t, uint64_t> ranges;   // as-gpu: start -> size, reservations and mappings alike
};

struct NvMapObject {
    uint32_t size = 0, align = 0, heap = 0, flags = 0, kind = 0;
    uint64_t addr = 0;
    bool allocated = false;
    uint32_t refs = 1;
};

struct Nv {
    std::mutex mu;
    bool initialized = false;
    int32_t next_fd = 1;
    uint32_t next_handle = 1;
    uint32_t next_syncpoint = 1;
    std::map<int32_t, NvFile> files;
    std::map<uint32_t, NvMapObject> objects;
    std::set<std::pair<int, uint32_t>> warned;   // (device, command word) answered by nobody
    bool submit_warned = false;
};
Nv& nv() {
    static Nv* n = new Nv;   // never destroyed: the gauges read it
    return *n;
}

struct NvGauges {
    NvGauges() {
        gauge_register("nv.files", [] {
            Nv& n = nv();
            std::lock_guard<std::mutex> lk(n.mu);
            return static_cast<int64_t>(n.files.size());
        });
        gauge_register("nv.nvmap_objects", [] {
            Nv& n = nv();
            std::lock_guard<std::mutex> lk(n.mu);
            return static_cast<int64_t>(n.objects.size());
        });
    }
    ~NvGauges() {
        gauge_unregister("nv.files");
        gauge_unregister("nv.nvmap_objects");
    }
};
NvGauges g_nv_gauges;

// An ioctl's argument block: `size` bytes; a field that does not fit reads 0 and is not written.
struct Args {
    std::vector<uint8_t> b;
    template <typename T>
    T get(size_t off) const {
        T v{};
        if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
        return v;
    }
    template <typename T>
    void put(size_t off, T v) {
        if (off + sizeof(T) <= b.size()) std::memcpy(b.data() + off, &v, sizeof(T));
    }
};

// What a handler answered: an NvError, or nobody took the command.
struct Answer {
    bool taken;
    uint32_t err;
};
constexpr Answer kUntaken{false, 0};
Answer done(uint32_t err = kNvOk) { return {true, err}; }

// ---- /dev/nvmap ---------------------------------------------------------------------------------------------------------------------------
Answer nvmap_ioctl(Nv& n, uint32_t nr, Args& a) {
    auto find = [&n](uint32_t h) -> NvMapObject* {
        auto it = n.objects.find(h);
        return it == n.objects.end() ? nullptr : &it->second;
    };
    switch (nr) {
        case 0x01: {   // CREATE
            const uint32_t size = a.get<uint32_t>(0);
            if (!size) return done(kNvBadValue);
            NvMapObject o;
            o.size = static_cast<uint32_t>((static_cast<uint64_t>(size) + 0xFFF) & ~0xFFFull);
            const uint32_t h = n.next_handle++;
            if (!n.next_handle) n.next_handle = 1;
            n.objects[h] = o;
            a.put<uint32_t>(4, h);
            return done();
        }
        case 0x03: {   // FROM_ID (the id is the handle)
            const uint32_t id = a.get<uint32_t>(0);
            NvMapObject* o = find(id);
            if (!o) return done(kNvBadValue);
            ++o->refs;
            a.put<uint32_t>(4, id);
            return done();
        }
        case 0x04: {   // ALLOC
            NvMapObject* o = find(a.get<uint32_t>(0));
            if (!o) return done(kNvBadValue);
            uint32_t align = a.get<uint32_t>(0xC);
            if (align & (align - 1)) return done(kNvBadValue);
            if (align < 0x1000) align = 0x1000;
            o->align = align;
            if (!o->allocated) {
                o->heap = a.get<uint32_t>(4);
                o->flags = a.get<uint32_t>(8);
                o->kind = a.get<uint8_t>(0x10);
                o->addr = a.get<uint64_t>(0x18);
                o->allocated = true;
            }
            a.put<uint32_t>(0xC, align);
            return done();
        }
        case 0x05: {   // FREE
            const uint32_t h = a.get<uint32_t>(0);
            NvMapObject* o = find(h);
            if (!o) return done(kNvBadValue);
            if (o->refs) --o->refs;
            const uint32_t size = o->size;
            if (!o->refs) {
                a.put<uint64_t>(8, o->addr);
                a.put<uint32_t>(0x14, 0);
                n.objects.erase(h);
            } else {
                a.put<uint64_t>(8, 0);
                a.put<uint32_t>(0x14, 1);   // not freed yet
            }
            a.put<uint32_t>(0x10, size);
            return done();
        }
        case 0x09: {   // PARAM
            NvMapObject* o = find(a.get<uint32_t>(0));
            if (!o) return done(kNvBadValue);
            uint32_t value = 0;
            switch (a.get<uint32_t>(4)) {
                case 1: value = o->size; break;
                case 2: value = o->align; break;
                case 4: value = 0x40000000; break;   // heap
                case 5: value = o->kind; break;
                case 6: value = 0; break;            // compression
                default: return done(kNvBadValue);
            }
            a.put<uint32_t>(8, value);
            return done();
        }
        case 0x0E: {   // GET_ID
            const uint32_t h = a.get<uint32_t>(4);
            if (!find(h)) return done(kNvBadValue);
            a.put<uint32_t>(0, h);
            return done();
        }
        case 0x02: case 0x06: case 0x07: case 0x08: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0F: case 0x10: case 0x11:
            return done(kNvNotSupported);
        default: return kUntaken;
    }
}

// ---- /dev/nvhost-ctrl ---------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kSyncpoints = 192;
Answer ctrl_ioctl(uint32_t nr, Args& a) {
    const bool bad_id = a.get<uint32_t>(0) >= kSyncpoints;
    switch (nr) {
        case 0x14: case 0x1A:   // SYNCPT_READ, SYNCPT_READ_MAX: nothing ever advances
            if (bad_id) return done(kNvBadValue);
            a.put<uint32_t>(4, 0);
            return done();
        case 0x15: case 0x16:   // SYNCPT_INCR, SYNCPT_WAIT (reached at once)
            return done(bad_id ? kNvBadValue : kNvOk);
        case 0x19:              // SYNCPT_WAITEX
            if (bad_id) return done(kNvBadValue);
            a.put<uint32_t>(0xC, 0);
            return done();
        case 0x1B: return done(kNvNotInProduction);   // GET_CONFIG
        case 0x1C: case 0x1F: case 0x20: case 0x21: return done();   // event slots: no bookkeeping
        case 0x1D: case 0x1E:   // SYNCPT_WAIT_EVENT(_EX)
            a.put<uint32_t>(0xC, 0);
            return done();
        default: return kUntaken;
    }
}

// ---- /dev/nvhost-ctrl-gpu -----------------------------------------------------------------------------------------------------------------
// The GM20B's GpuCharacteristics (0xA0 bytes).
void gpu_characteristics(uint8_t* p) {
    std::memset(p, 0, 0xA0);
    const std::pair<size_t, uint32_t> words[] = {
        {0x00, 0x120},   {0x04, 0xB},     {0x08, 0xA1},    {0x0C, 1},       {0x20, 2},       {0x24, 0x20},    {0x28, 0x20000},
        {0x2C, 0x20000}, {0x30, 0x1B},    {0x34, 0x30000}, {0x38, 1},       {0x3C, 0x503},   {0x40, 0x503},   {0x44, 0x80},
        {0x48, 0x28},    {0x58, 0x902D},  {0x5C, 0xB197},  {0x60, 0xB1C0},  {0x64, 0xB06F},  {0x68, 0xA140},  {0x6C, 0xB0B5},
        {0x70, 1},       {0x78, 2},       {0x7C, 1},       {0x84, 1},       {0x88, 0x21D70},
    };
    for (const auto& w : words) put32(p, w.first, w.second);
    put64(p, 0x10, 0x40000);          // L2 cache size
    put64(p, 0x50, 0x55);             // flags
    put64(p, 0x90, 0x6230326D67ull);  // "gm20b"
}

uint64_t gpu_time_ns() {
    static const auto start = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    return static_cast<uint64_t>(ns) & ((1ull << 56) - 1);
}

Answer ctrl_gpu_ioctl(uint32_t nr, Args& a, const ipc::Buffer& inline_out) {
    switch (nr) {
        case 0x01: a.put<uint32_t>(0, 1); return done();   // ZCULL_GET_CTX_SIZE
        case 0x02: {                                        // ZCULL_GET_INFO
            static const uint32_t kInfo[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xC0, 0x20, 0x40, 0x10};
            for (int i = 0; i < 10; ++i) a.put<uint32_t>(4 * i, kInfo[i]);
            return done();
        }
        case 0x03: return done();   // ZBC_SET_TABLE
        case 0x05: {                // GET_CHARACTERISTICS
            uint8_t c[0xA0];
            gpu_characteristics(c);
            a.put<uint64_t>(0, 0xA0);
            if (a.b.size() >= 0xB0) std::memcpy(a.b.data() + 0x10, c, 0xA0);
            if (inline_out.addr && inline_out.size >= 0xA0) buf_write(inline_out, c, 0xA0);
            return done();
        }
        case 0x06:                  // GET_TPC_MASKS
            if (a.get<uint32_t>(0)) {
                a.put<uint32_t>(0x10, 3);
                const uint32_t mask = 3;
                if (inline_out.addr && inline_out.size >= 4) buf_write(inline_out, &mask, 4);
            }
            return done();
        case 0x1C: a.put<uint64_t>(0, gpu_time_ns()); return done();   // GET_GPU_TIME
        default: return kUntaken;
    }
}

// ---- /dev/nvhost-as-gpu -------------------------------------------------------------------------------------------------------------------
struct VaRegion {
    uint64_t start, limit, page;
};
constexpr VaRegion kSmallPages{0x100000000ull, 0x400000000ull, 0x1000};
constexpr VaRegion kBigPages{0x400000000ull, 0x2000000000ull, 0x10000};
const VaRegion& region_for(uint64_t page) { return page == 0x10000 ? kBigPages : kSmallPages; }

bool align_up(uint64_t x, uint64_t align, uint64_t* out) {
    if (!align) align = 1;
    const uint64_t r = x % align;
    if (!r) {
        *out = x;
        return true;
    }
    const uint64_t add = align - r;
    if (x > UINT64_MAX - add) return false;
    *out = x + add;
    return true;
}

bool overlaps_any(const NvFile& f, uint64_t addr, uint64_t size) {
    const uint64_t end = addr + size < addr ? UINT64_MAX : addr + size;
    for (const auto& r : f.ranges) {
        const uint64_t r_end = r.first + r.second < r.first ? UINT64_MAX : r.first + r.second;
        if (r.first < end && addr < r_end) return true;
    }
    return false;
}

bool fits(const VaRegion& g, uint64_t addr, uint64_t size) { return addr >= g.start && addr <= g.limit && size <= g.limit - addr; }

bool place_fixed(const NvFile& f, const VaRegion& g, uint64_t addr, uint64_t size) {
    return fits(g, addr, size) && !overlaps_any(f, addr, size);
}

// The lowest address of the region, aligned, where the range overlaps nothing the file holds.
bool place_first_fit(const NvFile& f, const VaRegion& g, uint64_t align, uint64_t size, uint64_t* out) {
    uint64_t cand = 0;
    if (!align_up(g.start, align, &cand)) return false;
    for (const auto& r : f.ranges) {
        const uint64_t end = cand + size < cand ? UINT64_MAX : cand + size;
        if (r.first >= end) break;
        const uint64_t r_end = r.first + r.second < r.first ? UINT64_MAX : r.first + r.second;
        if (r_end > cand && !align_up(r_end, align, &cand)) return false;
    }
    if (!fits(g, cand, size)) return false;
    *out = cand;
    return true;
}

Answer as_gpu_ioctl(Nv& n, NvFile& f, uint32_t nr, Args& a) {
    switch (nr) {
        case 0x01: return done();   // BIND_CHANNEL
        case 0x02: {                // ALLOC_SPACE
            const uint64_t page = a.get<uint32_t>(4);
            const uint64_t size = static_cast<uint64_t>(a.get<uint32_t>(0)) * page;
            if (!size) return done(kNvBadValue);
            const VaRegion& g = region_for(page);
            const uint64_t want = a.get<uint64_t>(0x10);
            uint64_t addr = want;
            const bool placed = (a.get<uint32_t>(8) & 1) ? place_fixed(f, g, want, size) : place_first_fit(f, g, want ? want : page, size, &addr);
            if (!placed) {
                a.put<uint64_t>(0x10, 0);
                return done(kNvNoMemory);
            }
            f.ranges[addr] = size;
            a.put<uint64_t>(0x10, addr);
            return done();
        }
        case 0x03: {                // FREE_SPACE
            auto it = f.ranges.find(a.get<uint64_t>(0));
            if (it == f.ranges.end()) return done(kNvBadValue);
            f.ranges.erase(it);
            return done();
        }
        case 0x05:                  // UNMAP_BUFFER
            f.ranges.erase(a.get<uint64_t>(0));
            return done();
        case 0x06: {                // MAP_BUFFER_EX
            auto it = n.objects.find(a.get<uint32_t>(8));
            if (it == n.objects.end()) return done(kNvBadValue);
            const NvMapObject& o = it->second;
            const uint64_t mapping = a.get<uint64_t>(0x18);
            const uint64_t size = mapping ? mapping : o.size;
            uint64_t page = a.get<uint32_t>(0xC);
            if (!page) page = o.align ? o.align : 0x1000;
            const VaRegion& g = region_for(page);
            const uint64_t want = a.get<uint64_t>(0x20);
            uint64_t addr = want;
            const bool placed = (a.get<uint32_t>(0) & 1) ? place_fixed(f, g, want, size) : place_first_fit(f, g, page, size, &addr);
            if (!placed) {
                a.put<uint64_t>(0x20, 0);
                return done(kNvBadValue);
            }
            f.ranges[addr] = size;
            a.put<uint64_t>(0x20, addr);
            return done();
        }
        case 0x08: {                // GET_VA_REGIONS
            const uint32_t entries = std::min<uint32_t>(a.get<uint32_t>(8) / 24, 2);
            const VaRegion* regions[2] = {&kSmallPages, &kBigPages};
            for (uint32_t i = 0; i < entries; ++i) {
                const VaRegion& g = *regions[i];
                const size_t at = 0x10 + 24 * i;
                a.put<uint64_t>(at, g.start);
                a.put<uint32_t>(at + 8, static_cast<uint32_t>(g.page));
                a.put<uint32_t>(at + 12, 0);
                a.put<uint64_t>(at + 16, (g.limit - g.start) / g.page);
            }
            a.put<uint32_t>(8, 48);
            return done();
        }
        case 0x09: {                // INITIALIZE_EX
            const uint32_t big = a.get<uint32_t>(8);
            f.big_page = big ? big : 0x20000;
            return done();
        }
        default: return kUntaken;
    }
}

// ---- /dev/nvhost-gpu (a channel) ----------------------------------------------------------------------------------------------------------
Answer channel_ioctl(Nv& n, NvFile& f, uint32_t type, uint32_t nr, Args& a) {
    if (type == 0x47) {
        if (nr != 0x14) return kUntaken;
        f.user_data = a.get<uint64_t>(0);   // SET_USER_DATA
        return done();
    }
    if (type != 0x48) return kUntaken;
    switch (nr) {
        case 0x01: return done();   // SET_NVMAP_FD
        case 0x08: case 0x1B:       // SUBMIT_GPFIFO(2): the host NVN front end draws; nothing runs here
            if (!n.submit_warned) {
                n.submit_warned = true;
                NXLOG_W("nvdrv", "channel submissions are accepted and not executed (the host NVN front end renders)");
            }
            return done();
        case 0x09: a.put<uint64_t>(8, a.get<uint32_t>(0)); return done();   // ALLOC_OBJ_CTX: the object id is the class
        case 0x16: std::fill(a.b.begin(), a.b.end(), 0); return done();     // the error notifier: no error
        case 0x1A:                  // ALLOC_GPFIFO_EX2: the channel's syncpoint in the fence
            if (!f.syncpoint) f.syncpoint = n.next_syncpoint++;
            a.put<uint32_t>(0xC, f.syncpoint);
            a.put<uint32_t>(0x10, 0);
            return done();
        default: return kUntaken;
    }
}

Answer dispatch(Nv& n, NvFile& f, uint32_t cmd, Args& a, const ipc::Buffer& inline_out) {
    const uint32_t nr = cmd & 0xFF, type = (cmd >> 8) & 0xFF;
    switch (f.dev) {
        case Dev::NvMap: return type == 0x01 ? nvmap_ioctl(n, nr, a) : kUntaken;
        case Dev::Ctrl: return type == 0x00 ? ctrl_ioctl(nr, a) : kUntaken;
        case Dev::CtrlGpu: return type == 0x47 ? ctrl_gpu_ioctl(nr, a, inline_out) : kUntaken;
        case Dev::AsGpu: return type == 0x41 ? as_gpu_ioctl(n, f, nr, a) : kUntaken;
        case Dev::Channel: return channel_ioctl(n, f, type, nr, a);
        default: return kUntaken;
    }
}

uint32_t ioctl(ipc::Request& req, ipc::Response& rsp, const ipc::Buffer& inline_out) {
    const int32_t fd = req.arg<int32_t>(0);
    const uint32_t cmd = req.arg<uint32_t>(4);
    const ipc::Buffer in = req.in_buf(0), out = req.out_buf(0);
    Nv& n = nv();
    std::lock_guard<std::mutex> lk(n.mu);
    uint32_t err = kNvOk;
    auto it = n.files.find(fd);
    const uint64_t size = (cmd >> 16) & 0x3FFF;
    const bool copy_in = (cmd >> 30) & 1, copy_out = (cmd >> 31) & 1;
    if (!n.initialized) {
        err = kNvNotInitialized;
    } else if (fd < 0) {
        err = kNvBadParameter;
    } else if (it == n.files.end()) {
        err = kNvNotImplemented;
    } else if ((copy_out && size > out.size) || (copy_in && size > in.size)) {
        err = kNvBadSize;
    } else {
        Args a;
        a.b.assign(static_cast<size_t>(size), 0);
        if (copy_in) buf_read(in, a.b.data(), size);
        const Answer r = dispatch(n, it->second, cmd, a, inline_out);
        if (r.taken) {
            err = r.err;
        } else {
            std::fill(a.b.begin(), a.b.end(), 0);
            if (n.warned.insert({static_cast<int>(it->second.dev), cmd}).second)
                NXLOG_W("nvdrv", "%s ioctl 0x%08X not handled: answered with zeros", dev_name(it->second.dev), cmd);
        }
        if (copy_out) buf_write(out, a.b.data(), size);
    }
    rsp.push<uint32_t>(err);
    return 0;
}

Dev device_for(const std::string& path, bool* known, bool* debug_only) {
    static const std::pair<const char*, Dev> kPaths[] = {
        {"/dev/nvmap", Dev::NvMap},           {"/dev/nvhost-ctrl", Dev::Ctrl},       {"/dev/nvhost-ctrl-gpu", Dev::CtrlGpu},
        {"/dev/nvhost-as-gpu", Dev::AsGpu},   {"/dev/nvhost-gpu", Dev::Channel},     {"/dev/nvhost-nvdec", Dev::Other},
        {"/dev/nvhost-nvjpg", Dev::Other},    {"/dev/nvhost-vic", Dev::Other},       {"/dev/nvdisp-ctrl", Dev::Other},
        {"/dev/nvsched-ctrl-gpu", Dev::Other},
    };
    *known = *debug_only = false;
    for (const auto& p : kPaths) {
        if (path == p.first) {
            *known = true;
            return p.second;
        }
    }
    *debug_only = path == "/dev/nvhost-dbg-gpu" || path == "/dev/nvhost-prof-gpu";
    return Dev::Other;
}

std::shared_ptr<ipc::Service> make_nvdrv() {
    return make_iface("nns::nvdrv::INvDrvServices", [](ipc::Service& s) {
        s.add_cmd(0, "Open", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const ipc::Buffer b = req.in_buf(0);
            std::string path;
            if (b.addr) {
                const char* p = reinterpret_cast<const char*>(buf_ptr(b));
                size_t len = 0;
                while (len < b.size && p[len]) ++len;
                path.assign(p, len);
            }
            Nv& n = nv();
            std::lock_guard<std::mutex> lk(n.mu);
            int32_t fd = -1;
            uint32_t err = kNvOk;
            bool known = false, debug_only = false;
            const Dev dev = device_for(path, &known, &debug_only);
            if (!n.initialized) {
                err = kNvNotInitialized;
            } else if (debug_only) {
                err = kNvNotSupported;
            } else if (!known) {
                err = kNvFileOperationFailed;
                NXLOG_W("nvdrv", "Open of unknown device '%s'", path.c_str());
            } else {
                fd = n.next_fd++;
                n.files[fd].dev = dev;
                NXLOG_D("nvdrv", "Open %s -> fd %d", path.c_str(), fd);
            }
            rsp.push<int32_t>(fd);
            rsp.push<uint32_t>(err);
            return 0;
        });
        s.add_cmd(1, "Ioctl", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return ioctl(req, rsp, ipc::Buffer{}); });
        s.add_cmd(2, "Close", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int32_t fd = req.arg<int32_t>(0);
            Nv& n = nv();
            std::lock_guard<std::mutex> lk(n.mu);
            uint32_t err = kNvOk;
            if (!n.initialized) err = kNvNotInitialized;
            else if (fd < 0) err = kNvBadParameter;
            else if (!n.files.erase(fd)) err = kNvNotImplemented;
            rsp.push<uint32_t>(err);
            return 0;
        });
        s.add_cmd(3, "Initialize", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            Nv& n = nv();
            std::lock_guard<std::mutex> lk(n.mu);
            n.initialized = true;
            rsp.push<uint32_t>(kNvOk);
            return 0;
        });
        s.add_cmd(4, "QueryEvent", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int32_t fd = req.arg<int32_t>(0);
            const uint32_t id = req.arg<uint32_t>(4);
            Nv& n = nv();
            std::lock_guard<std::mutex> lk(n.mu);
            uint32_t err = kNvOk, h = 0;
            auto it = n.files.find(fd);
            if (!n.initialized) {
                err = kNvNotInitialized;
            } else if (it == n.files.end()) {
                err = kNvNotImplemented;
            } else {
                NvFile& f = it->second;
                uint32_t key = id;
                switch (f.dev) {
                    case Dev::Ctrl:
                        key = (id >> 28) == 1 ? (id & 0xFFFF) : (id & 0xFF);
                        if (key >= 64) err = kNvBadValue;
                        break;
                    case Dev::CtrlGpu: if (id != 1 && id != 2) err = kNvBadValue; break;   // error, semaphore
                    case Dev::Channel: if (id < 1 || id > 3) err = kNvBadValue; break;     // bpt int, bpt pause, error notifier
                    default: err = kNvNotSupported; break;
                }
                if (!err) {
                    h = f.events[key].new_handle();
                    if (!h) err = kNvNoMemory;
                }
            }
            rsp.push<uint32_t>(err);
            if (!err) rsp.out_copy.push_back(h);
            return 0;
        });
        s.add_cmd(6, "GetStatus", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            Nv& n = nv();
            std::lock_guard<std::mutex> lk(n.mu);
            if (!n.initialized) {
                rsp.push<uint32_t>(kNvNotInitialized);
                return 0;
            }
            for (int i = 0; i < 8; ++i) rsp.push<uint32_t>(0);   // NvDrvStatus
            rsp.push<uint32_t>(kNvOk);
            return 0;
        });
        const H aruid = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(kNvOk);
            return 0;
        };
        s.add_cmd(7, "SetAruidWithoutCheck", aruid);
        s.add_cmd(8, "SetAruid", aruid);
        s.add_cmd(9, "DumpStatus", ok);
        s.add_cmd(11, "Ioctl2", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return ioctl(req, rsp, ipc::Buffer{}); });
        s.add_cmd(12, "Ioctl3", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return ioctl(req, rsp, req.out_buf(1)); });
        s.add_cmd(13, "SetGraphicsFirmwareMemoryMarginEnabled", ok);
    });
}

}  // namespace

// ======================================================================================================================================
// entry points
// ======================================================================================================================================
namespace victl {
void note_present() {
    const uint64_t now = ticks_to_ns(kernel::system_tick(), kernel::tick_hz());
    History& h = history();
    std::lock_guard<std::mutex> lk(h.mu);
    ++h.frames;
    h.last.push_back({h.frames, static_cast<int64_t>(now), static_cast<int64_t>(now)});
    while (h.last.size() > kKept) h.last.pop_front();
}

uint64_t frames_presented() {
    History& h = history();
    std::lock_guard<std::mutex> lk(h.mu);
    return h.frames;
}

int frame_history(uint64_t* out, int max) {
    if (max <= 0) return 0;
    const uint64_t hz = kernel::tick_hz();
    History& h = history();
    std::lock_guard<std::mutex> lk(h.mu);
    const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(max), h.last.size()));
    for (int i = 0; i < n; ++i) {
        const Presented& f = h.last[h.last.size() - 1 - static_cast<size_t>(i)];
        uint64_t* e = out + 5 * i;
        e[0] = 2;   // shown
        e[1] = f.frame;
        e[2] = ns_to_ticks(f.queue_ns, hz);
        e[3] = e[4] = ns_to_ticks(f.present_ns, hz);
    }
    return n;
}
}  // namespace victl

namespace viapi {
uint32_t display_vsync_handle() { return vsync_handle(); }
}  // namespace viapi

void register_vi_nv() {
    add("vi:u", [] { return make_root("nn::visrv::sf::IApplicationRootService", 0, 1); });
    add("vi:s", [] { return make_root("nn::visrv::sf::ISystemRootService", 1, 3); });
    add("vi:m", [] { return make_root("nn::visrv::sf::IManagerRootService", 2, 3); });
    for (const char* name : {"nvdrv", "nvdrv:a", "nvdrv:s", "nvdrv:t"}) add(name, [] { return make_nvdrv(); });
}

void vi_nv_reset() {
    Vi& v = vi();
    std::thread t;
    {
        std::lock_guard<std::mutex> lk(v.mu);
        if (v.vsync_started) {
            v.vsync_stop = true;
            t = std::move(v.vsync_thread);
        }
    }
    {
        std::lock_guard<std::mutex> lk(v.wait_mu);
        v.wait_cv.notify_all();
    }
    if (t.joinable()) t.join();
    {
        History& h = history();
        std::lock_guard<std::mutex> lk(h.mu);
        h.frames = 0;
        h.last.clear();
    }
    {
        std::lock_guard<std::mutex> lk(v.mu);
        v.vsync_started = false;
        v.vsync_running = false;
        v.vsync_stop = false;
        for (LayerSlot& s : v.slot) {
            s.used = false;
            s.connected = false;
            s.id = 0;
            s.event = ServiceEvent();
        }
        v.next_layer_id = kFirstLayerId;
        v.vsync = ServiceEvent();
    }
    Nv& n = nv();
    std::lock_guard<std::mutex> lk(n.mu);
    n.initialized = false;
    n.next_fd = 1;
    n.next_handle = 1;
    n.next_syncpoint = 1;
    n.files.clear();
    n.objects.clear();
    n.warned.clear();
    n.submit_warned = false;
}

}  // namespace nxrt::services
