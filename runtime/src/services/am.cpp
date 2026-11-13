// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Applet manager (S3c): appletOE / appletAE as one application sees them. The application proxy and every interface it hands out
// (common state, self, window, audio, display, process winding, library applet creator and accessors, application functions, debug,
// storages, lock accessors, graphics scaling), the applet message queue and the focus, the library applets answered on the host
// (software keyboard, normal and inline; player select; error; controller; the web family; the ones that end cancelled), launch
// parameters, save size queries and the ExecuteProgram / relaunch hand-off (docs/EXECUTE_PROGRAM.md). Also the host's controls (amctl)
// and the software keyboard prompt switch (swkbdui::enable_prompt / prompt_enabled).
//
// Command ids, layouts and values: https://switchbrew.org/wiki/Applet_Manager_services (commands, AppletMessage, OperationMode,
// FocusState, LibraryAppletMode), https://switchbrew.org/wiki/Software_Keyboard (the keyboard config, its output and text check
// storages, the inline keyboard's requests, replies and Calc argument) and https://switchbrew.org/wiki/Internet_Browser (the web
// argument TLVs, session messages, exit reasons). Facts taken from Ryujinx (MIT), no code: src/Ryujinx.HLE/HOS/Services/Am (ResultCode.cs,
// AppletAE/Types AppletId and LibraryAppletMode, the storage and its accessor, the preselected user storage) and
// src/Ryujinx.HLE/HOS/Applets (SoftwareKeyboard: inline keyboard states, requests, replies and Calc flags; Controller: the support
// argument header and the result info; Browser: the argument header and TLVs, WebCommonReturnValue, exit reasons; PlayerSelect; Error:
// the common header and the application error argument).
//
// AnySwitch's own rules on top of those (they win where the two differ):
//   * messages: the queue starts with FocusStateChanged; OperationModeChanged / PerformanceModeChanged are queued at every set_docked
//     the game asked to hear about, also when nothing changed; ReceiveMessage does not signal again while messages remain;
//   * a library applet holds the application's focus while it is open (Mario Kart 8 Deluxe applies the controller applet's answer at the
//     InFocus that follows it), except the inline keyboard and an offline web session started hidden;
//   * every applet answers without a window unless a prompt is on (keyboard, the controller applet's wait for players) or a web view
//     host shows a page; shop, MyPage, data erase, net connect and GameShare matchmaking end cancelled;
//   * one lock (am's) guards the process state and every applet; the hid calls (hid_sdk.h) are made without it, as hid asks am for the
//     operation mode.
// Commands of these interfaces that ext_am.cpp adds (ipc::extend) are not registered here: a handler of the interface's own would
// always win over the extension.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nxrt/config.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services_api.h"
#include "nxrt/webview.h"
#include "hid_sdk.h"
#include "services_internal.h"

namespace nxrt::services {

namespace {

using Req = ipc::Request;
using Rsp = ipc::Response;
using Blob = std::vector<uint8_t>;
using BlobPtr = std::shared_ptr<Blob>;

constexpr const char* kTag = "am";

// am results (module 128)
const uint32_t kNoData = res(128, 2);             // 0x480: no storage to pop
const uint32_t kNoMessages = res(128, 3);         // 0x680: ReceiveMessage with an empty queue
const uint32_t kAppletCancelled = res(128, 22);   // 0x2C80: the applets that end cancelled
const uint32_t kLaunchFailed = res(128, 35);      // 0x4680: another application asked for
const uint32_t kBadOffset = res(128, 503);        // 0x3EE80: a storage range outside the storage
const uint32_t kNotAStorage = res(128, 511);      // 0x3FE80: an object that is not one of am's storages
const uint32_t kNoMemory = res(128, 600);         // 0x4B080: a storage size out of range

// AppletMessage values and the focus states
constexpr uint32_t kMsgFocusChanged = 15, kMsgOperationMode = 30, kMsgPerformanceMode = 31, kMsgStartupLogoGone = 95;
constexpr uint8_t kInFocus = 1, kOutOfFocus = 2;

constexpr int64_t kStorageMax = 0x10000000;           // CreateStorage and the memory storages: 0 .. 256 MiB
constexpr uint64_t kChannelEntryMax = 64ull << 20;    // an entry of a launch user channel file above this stops the reading
constexpr uint32_t kTextMax = 500;                    // the keyboards' length limit (UTF-16 units)
constexpr uint32_t kLanguages = static_cast<uint32_t>(Language::Count);

// Applet kinds: the low 16 bits of the id the game creates.
constexpr uint32_t kController = 0x0C, kDataErase = 0x0D, kErrorApplet = 0x0E, kNetConnect = 0x0F, kPlayerSelect = 0x10,
                   kKeyboard = 0x11, kWeb = 0x13, kShop = 0x14, kOffline = 0x17, kLoginShare = 0x18, kWifiAuth = 0x19,
                   kMyPage = 0x1A, kGameShare = 0x24;

bool ends_cancelled(uint32_t kind) {
    return kind == kShop || kind == kMyPage || kind == kDataErase || kind == kNetConnect || kind == kGameShare;
}

// ---- bytes ---------------------------------------------------------------------------------------------------------------------
uint8_t rd8(const Blob& b, uint64_t off) { return off < b.size() ? b[off] : 0; }
uint16_t rd16(const Blob& b, uint64_t off) {
    uint16_t v = 0;
    if (off + 2 <= b.size()) std::memcpy(&v, b.data() + off, 2);
    return v;
}
uint32_t rd32(const Blob& b, uint64_t off) {
    uint32_t v = 0;
    if (off + 4 <= b.size()) std::memcpy(&v, b.data() + off, 4);
    return v;
}
uint64_t rd64(const Blob& b, uint64_t off) {
    uint64_t v = 0;
    if (off + 8 <= b.size()) std::memcpy(&v, b.data() + off, 8);
    return v;
}
void wr32(Blob& b, size_t off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); }
void wr64(Blob& b, size_t off, uint64_t v) { std::memcpy(b.data() + off, &v, 8); }
void append32(Blob& b, uint32_t v) {
    const size_t at = b.size();
    b.resize(at + 4);
    std::memcpy(b.data() + at, &v, 4);
}
void put_bytes(Blob& b, size_t off, const void* src, size_t n) {
    if (n) std::memcpy(b.data() + off, src, n);
}

// C files closed when they go out of scope (the writers close them themselves, to see fclose's result).
struct FileCloser {
    void operator()(std::FILE* f) const { std::fclose(f); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;
FilePtr open_file(const std::string& path, const char* mode) { return FilePtr(std::fopen(path.c_str(), mode)); }

// ---- text (UTF-16 <-> UTF-8 as the keyboards convert it) -----------------------------------------------------------------------
void utf8_append(std::string& out, uint32_t cp) {
    if (cp < 0x80) {   // ASCII: the byte itself
        out += static_cast<char>(cp);
        return;
    }
    // continuation bytes after the lead: 1 below U+0800, 2 below U+10000, 3 above
    const int tail = cp >= 0x10000 ? 3 : (cp >= 0x800 ? 2 : 1);
    static constexpr uint8_t kLead[4] = {0x00, 0xC0, 0xE0, 0xF0};
    out += static_cast<char>(kLead[tail] | (cp >> (6 * tail)));
    for (int k = tail - 1; k >= 0; --k) out += static_cast<char>(0x80 | ((cp >> (6 * k)) & 0x3F));
}

// A surrogate pair becomes one code point; a surrogate without its partner is written as the 3-byte form of its own value.
std::string to_utf8(const std::u16string& s) {
    std::string out;
    out.reserve(s.size());
    size_t at = 0;
    while (at < s.size()) {
        const uint32_t unit = s[at++];
        const bool high = unit >= 0xD800 && unit <= 0xDBFF;
        if (high && at < s.size() && s[at] >= 0xDC00 && s[at] <= 0xDFFF) {
            const uint32_t low = s[at++];
            utf8_append(out, 0x10000 + (((unit & 0x3FF) << 10) | (low & 0x3FF)));
        } else {
            utf8_append(out, unit);
        }
    }
    return out;
}

// Lead bytes 0xxxxxxx / 110xxxxx / 1110xxxx / 11110xxx start sequences of 1 / 2 / 3 / 4 bytes. Any other lead, or a sequence the string
// cuts short, gives U+FFFD for one byte; a byte that is not a continuation at position k gives U+FFFD for the k bytes before it (it starts
// the next character). No overlong or surrogate checks; above U+10FFFF gives U+FFFD.
std::u16string to_utf16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const uint8_t lead = static_cast<uint8_t>(s[i]);
        if (lead < 0x80) {
            out += static_cast<char16_t>(lead);
            ++i;
            continue;
        }
        size_t len = 0;
        uint32_t cp = 0;
        if ((lead & 0xE0) == 0xC0) {
            len = 2;
            cp = lead & 0x1F;
        } else if ((lead & 0xF0) == 0xE0) {
            len = 3;
            cp = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            len = 4;
            cp = lead & 0x07;
        }
        if (len == 0 || i + len > s.size()) {
            out += u'\xFFFD';
            ++i;
            continue;
        }
        size_t k = 1;
        for (; k < len; ++k) {
            const uint8_t c = static_cast<uint8_t>(s[i + k]);
            if ((c & 0xC0) != 0x80) break;
            cp = (cp << 6) | (c & 0x3F);
        }
        if (k < len) {
            out += u'\xFFFD';
            i += k;
            continue;
        }
        i += len;
        if (cp < 0x10000) {
            out += static_cast<char16_t>(cp);
        } else if (cp <= 0x10FFFF) {
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += u'\xFFFD';
        }
    }
    return out;
}

// A UTF-16 text field of a storage: at most `units` units from byte `off`, up to a 0 unit or the storage's end; units below 0x20 are left
// out except LF. Returned as UTF-8.
std::string read_utf16(const Blob& b, uint64_t off, uint64_t units) {
    std::u16string t;
    for (uint64_t i = 0; i < units; ++i) {
        const uint64_t at = off + 2 * i;
        if (at + 2 > b.size()) break;
        const uint16_t u = rd16(b, at);
        if (u == 0) break;
        if (u < 0x20 && u != 0x0A) continue;
        t += static_cast<char16_t>(u);
    }
    return to_utf8(t);
}

std::u16string cut(std::u16string s, size_t units) {
    if (s.size() > units) s.resize(units);
    return s;
}

// ---- am's lock and the process state -------------------------------------------------------------------------------------------
// Host threads (amctl, prompt answers, the controller applet's wait, web view hosts) take the lock as the IPC threads do.
std::recursive_mutex& am_lock() {
    static auto* m = new std::recursive_mutex();
    return *m;
}
using Guard = std::lock_guard<std::recursive_mutex>;

std::atomic<void (*)(int, const char*)> g_on_exit{nullptr};   // amctl::set_exit_hook (not reset)
std::atomic<bool> g_swkbd_prompt{false};                         // swkbdui::enable_prompt (not reset)
std::atomic<int> g_content_logged{0};                            // content messages of a stand-in session logged so far (not reset)

struct Proc {
    std::deque<uint32_t> messages;
    ServiceEvent message_event{true};
    ServiceEvent sleep_lock{false};
    ServiceEvent launchable{true};       // signalled from the start, never cleared
    ServiceEvent suspended_tick{true};   // the same
    ServiceEvent never{false};           // the events that never fire: one object, a new handle each time
    bool docked = true;
    uint8_t focus = kInFocus;            // what GetCurrentFocusState reports
    bool host_focus = true;
    int open_applets = 0;
    bool focus_notify = true;            // SetFocusHandlingMode's first byte
    bool mode_notify = false, perf_notify = false;
    bool vr = false, vr3d = false;
    bool auto_sleep_disabled = false;
    uint32_t idle_extension = 0;
    uint64_t next_layer = 1;
    bool preselected_popped = false;
    std::vector<BlobPtr> user_channel;  // oldest first
    bool save_extended = false;
    int64_t save_normal = 0, save_journal = 0;
};

// [am] launch_user_channel (a plain path, not the title's): the launch parameters the previous program left (EXECUTE_PROGRAM.md format:
// u32 count, then u64 size and the bytes of each). An entry above 64 MiB or one the file cuts short ends the reading; the entries before
// it are kept.
void load_launch_channel(std::vector<BlobPtr>& out) {
    const std::string chan_file = config_get_str("am", "launch_user_channel", "");
    if (chan_file.empty()) return;
    const FilePtr in = open_file(chan_file, "rb");
    if (!in) {
        NXLOG_W(kTag, "launch user channel %s cannot be opened", chan_file.c_str());
        return;
    }
    uint32_t count = 0;
    if (std::fread(&count, 4, 1, in.get()) == 1) {
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t size = 0;
            if (std::fread(&size, 8, 1, in.get()) != 1 || size > kChannelEntryMax) break;
            auto entry = std::make_shared<Blob>();
            bool whole = true;
            for (uint64_t got = 0; got < size;) {   // grown as it is read: the file's length bounds the memory
                const size_t n = static_cast<size_t>(std::min<uint64_t>(size - got, 1u << 20));
                const size_t at = entry->size();
                entry->resize(at + n);
                if (std::fread(entry->data() + at, 1, n, in.get()) != n) {
                    whole = false;
                    break;
                }
                got += n;
            }
            if (!whole) break;
            out.push_back(std::move(entry));
        }
    }
    NXLOG_I(kTag, "launch user channel %s: %zu storage(s)", chan_file.c_str(), out.size());
}

std::unique_ptr<Proc> start_state() {
    auto p = std::make_unique<Proc>();
    p->messages.push_back(kMsgFocusChanged);   // the event starts signalled with it
    p->docked = profile().docked;
    load_launch_channel(p->user_channel);
    return p;
}

std::unique_ptr<Proc>& proc_slot() {
    static auto* slot = new std::unique_ptr<Proc>();
    return *slot;
}
// am's lock held.
Proc& proc() {
    std::unique_ptr<Proc>& slot = proc_slot();
    if (!slot) slot = start_state();
    return *slot;
}

void queue_message(Proc& p, uint32_t msg) {
    p.messages.push_back(msg);
    p.message_event.signal();
}

// The focus the game sees: in focus while the host window has it and no applet is open. A change is stored always and notified only
// when the game's focus handling mode asks for it.
void refresh_focus(Proc& p) {
    const uint8_t now = p.host_focus && p.open_applets == 0 ? kInFocus : kOutOfFocus;
    if (now == p.focus) return;
    p.focus = now;
    if (p.focus_notify) queue_message(p, kMsgFocusChanged);
}
void applet_opened(Proc& p) {
    ++p.open_applets;
    refresh_focus(p);
}
void applet_closed(Proc& p) {
    if (p.open_applets > 0) --p.open_applets;
    refresh_focus(p);
}

// ---- command helpers -----------------------------------------------------------------------------------------------------------
// Accepted and ignored: success with `zeros` zero bytes.
void ignored(ipc::Service& s, uint32_t id, const char* name, size_t zeros = 0) {
    const std::string iface = s.name();
    s.add_cmd(id, name, [iface, name, zeros](Req&, Rsp& rsp) -> uint32_t {
        NXLOG_D(kTag, "%s %s: accepted, nothing to do", iface.c_str(), name);
        rsp.out.assign(zeros, 0);
        return 0;
    });
}

// A new copy handle of one of the process events.
void process_event(ipc::Service& s, uint32_t id, const char* name, ServiceEvent Proc::*event) {
    s.add_cmd(id, name, [event](Req&, Rsp& rsp) -> uint32_t {
        Guard g(am_lock());
        rsp.out_copy.push_back((proc().*event).new_handle());
        return 0;
    });
}

std::shared_ptr<ipc::Service> empty_iface(const char* name) { return make_iface(name, [](ipc::Service&) {}); }

// ---- storages ------------------------------------------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_storage_accessor(const BlobPtr& data);

// am's IStorage: one byte vector, shared with every accessor opened on it. Only these are taken where the game passes a storage in.
class AmStorage final : public ipc::Service {
public:
    explicit AmStorage(BlobPtr bytes) : ipc::Service("IStorage"), bytes_(std::move(bytes)) {
        const BlobPtr data = bytes_;
        add_cmd(0, "Open", [data](Req&, Rsp& rsp) -> uint32_t {
            rsp.push_object(make_storage_accessor(data));
            return 0;
        });
    }
    const BlobPtr& bytes() const { return bytes_; }

private:
    BlobPtr bytes_;
};

std::shared_ptr<ipc::Service> storage_object(BlobPtr bytes) { return std::make_shared<AmStorage>(std::move(bytes)); }

bool object_passed(const Req& req) { return !req.in_services.empty() && req.in_services[0] != nullptr; }
// The storage passed in, or null when none was or the object is another kind (fs's IStorage has the same name).
std::shared_ptr<AmStorage> storage_passed(const Req& req) {
    if (!object_passed(req)) return nullptr;
    return std::dynamic_pointer_cast<AmStorage>(req.in_services[0]);
}
// A copy of a storage's bytes as they are now (am's lock held).
BlobPtr bytes_now(const AmStorage& st) { return std::make_shared<Blob>(*st.bytes()); }

bool range_inside(int64_t offset, uint64_t n, size_t size) {
    if (offset < 0 || static_cast<uint64_t>(offset) > size) return false;
    return n <= size - static_cast<uint64_t>(offset);
}

std::shared_ptr<ipc::Service> make_storage_accessor(const BlobPtr& data) {
    return make_iface("IStorageAccessor", [data](ipc::Service& s) {
        s.add_cmd(0, "GetSize", [data](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<int64_t>(static_cast<int64_t>(data->size()));
            return 0;
        });
        s.add_cmd(10, "Write", [data](Req& req, Rsp&) -> uint32_t {
            const int64_t offset = req.arg<int64_t>(0);
            const ipc::Buffer b = req.in_buf(0);
            Guard g(am_lock());
            if (!range_inside(offset, b.size, data->size())) return kBadOffset;
            if (b.size) buf_read(b, data->data() + offset, b.size);
            return 0;
        });
        s.add_cmd(11, "Read", [data](Req& req, Rsp&) -> uint32_t {
            const int64_t offset = req.arg<int64_t>(0);
            const ipc::Buffer b = req.out_buf(0);
            Guard g(am_lock());
            if (!range_inside(offset, b.size, data->size())) return kBadOffset;
            if (b.size) buf_write(b, data->data() + offset, b.size);
            return 0;
        });
    });
}

// CreateTransferMemoryStorage / CreateHandleStorage: the bytes of the memory behind the handle as they are now, when the handle is a
// transfer memory that holds the whole size and is mapped; zeros otherwise.
uint32_t memory_storage(const Req& req, Rsp& rsp, int64_t size) {
    if (size < 0 || size > kStorageMax) return kNoMemory;
    auto bytes = std::make_shared<Blob>(static_cast<size_t>(size), 0);
    const uint32_t h = req.in_copy.empty() ? 0 : req.in_copy[0];
    if (auto tm = kernel::handle_as<kernel::KTransferMemory>(h)) {
        const uint64_t n = static_cast<uint64_t>(size);
        if (tm->addr != 0 && n > 0 && n <= tm->size && mem::is_mapped(tm->addr, n))
            std::memcpy(bytes->data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(tm->addr)), static_cast<size_t>(n));
    }
    rsp.push_object(storage_object(std::move(bytes)));
    return 0;
}

// Hands the oldest storage of a queue out (the stored bytes themselves); the event is cleared once the queue is empty.
uint32_t pop_storage(std::deque<BlobPtr>& q, ServiceEvent& ev, Rsp& rsp) {
    const size_t queued = q.size();
    if (queued == 0) {
        ev.clear();
        return kNoData;
    }
    BlobPtr oldest = std::move(q.front());
    q.pop_front();
    if (queued == 1) ev.clear();   // that was the last one
    rsp.push_object(storage_object(std::move(oldest)));
    return 0;
}

// ---- library applets -----------------------------------------------------------------------------------------------------------
struct Applet {
    uint32_t id = 0, kind = 0, mode = 0;
    bool inline_kb = false;   // the software keyboard created in a mode other than AllForeground
    bool started = false, completed = false, owns_focus = false;
    std::deque<BlobPtr> in_data, game_in, out_data, interactive_out;
    ServiceEvent end_event, out_ready, reply_ready;
    // the normal keyboard
    bool kb_utf8 = false, kb_check = false;
    uint32_t kb_max = kTextMax;
    std::u16string kb_text;
    swkbdui::Request kb_request;
    // the inline keyboard
    uint32_t ik_state = 0;    // 0 not initialized, 1 hidden, 2 appearing, 3 shown, 4 disappearing
    bool ik_utf8 = false, ik_v2 = false, ik_cursor_v2 = false;
    int32_t ik_cursor = 0;
    uint32_t ik_max = kTextMax;
    std::u16string ik_text;
    uint64_t ik_serial = 0;   // a prompt answer counts only while it is the latest question
    // the web family
    bool session = false, page = false, online = false;
};
using AppletPtr = std::shared_ptr<Applet>;

// in-data[0] the common arguments, [1] the applet's argument, [2] the keyboard's work buffer.
const Blob* in_entry(const Applet& a, size_t i) { return i < a.in_data.size() ? a.in_data[i].get() : nullptr; }

// A storage for the game to pop, and its queue's event.
void enqueue(std::deque<BlobPtr>& q, ServiceEvent& ready, Blob v) {
    q.push_back(std::make_shared<Blob>(std::move(v)));
    ready.signal();
}
void push_out(Applet& a, Blob v) { enqueue(a.out_data, a.out_ready, std::move(v)); }
void push_reply(Applet& a, Blob v) { enqueue(a.interactive_out, a.reply_ready, std::move(v)); }
void take_focus(Applet& a) {
    a.owns_focus = true;
    applet_opened(proc());
}
void end_applet(Applet& a) {
    a.completed = true;
    a.end_event.signal();
    if (!a.owns_focus) return;
    a.owns_focus = false;
    applet_closed(proc());
}

// ---- the software keyboard, normal (AllForeground) ----
struct KbConfig {
    swkbdui::Request request;   // title, texts, limits
    bool utf8 = false, check = false;
};

// The keyboard config (in-data[1], read when it has at least 0x3D4 bytes): the common offsets, and two layouts for the rest told apart by
// the size (0x530: the packed one of newer SDKs). The initial text is in the work buffer (in-data[2]).
KbConfig kb_config(const Applet& a) {
    KbConfig c;
    c.request.title = profile().name;
    c.request.max_len = kTextMax;
    const Blob* cfg = in_entry(a, 1);
    if (!cfg || cfg->size() < 0x3D4) {
        NXLOG_W(kTag, "software keyboard without a usable config (%zu bytes): defaults", cfg ? cfg->size() : size_t{0});
        return c;
    }
    const Blob& k = *cfg;
    const bool packed = k.size() == 0x530;
    c.request.ok_text = read_utf16(k, 0x04, 9);
    c.request.header = read_utf16(k, 0x24, 65);
    c.request.sub = read_utf16(k, 0xA6, 129);
    c.request.guide = read_utf16(k, 0x1A8, 257);
    const uint32_t max = rd32(k, 0x3AC);
    c.request.max_len = max >= 1 && max <= kTextMax ? max : kTextMax;
    c.request.min_len = rd32(k, 0x3B0);
    c.request.password = rd32(k, 0x3B4) != 0;
    c.utf8 = rd8(k, packed ? 0x3AA : 0x3BD) != 0;
    const int32_t init_off = static_cast<int32_t>(rd32(k, packed ? 0x3BC : 0x3C0));
    const int32_t init_len = static_cast<int32_t>(rd32(k, packed ? 0x3C0 : 0x3C4));
    c.check = rd8(k, packed ? 0x48D : 0x3D0) != 0;
    const Blob* work = in_entry(a, 2);
    if (work && init_off >= 0 && init_len > 0)
        c.request.initial = read_utf16(*work, static_cast<uint64_t>(init_off), static_cast<uint64_t>(init_len));
    return c;
}

// The text in the config's encoding: UTF-8 up to `cap8` bytes, or UTF-16LE up to `cap16` bytes.
Blob kb_text_bytes(const std::u16string& text, bool utf8, size_t cap8, size_t cap16) {
    if (utf8) {
        const std::string s = to_utf8(text);
        return Blob(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(std::min(s.size(), cap8)));
    }
    Blob v(std::min(text.size() * 2, cap16));
    put_bytes(v, 0, text.data(), v.size());
    return v;
}

// The output storage (0x7D8): u32 result (0 OK, 1 cancel), then the text on OK.
Blob kb_output(const Applet& a, bool ok) {
    Blob v(0x7D8, 0);
    if (!ok) {
        wr32(v, 0, 1);
        return v;
    }
    const Blob t = kb_text_bytes(a.kb_text, a.kb_utf8, 0x7D3, 0x7D2);
    put_bytes(v, 4, t.data(), t.size());
    return v;
}

// The text check request (interactive out, 0x7DC): u64 the text's size with its NUL, then the text.
Blob kb_check_request(const Applet& a) {
    Blob v(0x7DC, 0);
    const Blob t = kb_text_bytes(a.kb_text, a.kb_utf8, 0x7D3, 0x7D2);
    wr64(v, 0, a.kb_utf8 ? t.size() + 1 : (t.size() / 2 + 1) * 2);
    put_bytes(v, 8, t.data(), t.size());
    return v;
}

void kb_answer(Applet& a, bool ok, const std::string& text) {
    if (!ok) {
        push_out(a, kb_output(a, false));
        end_applet(a);
        return;
    }
    a.kb_text = cut(to_utf16(text), a.kb_max);
    if (a.kb_check) {
        push_reply(a, kb_check_request(a));   // the game checks the text first; the applet stays open
        return;
    }
    push_out(a, kb_output(a, true));
    end_applet(a);
}

// The on-screen keyboard: asked on a thread of its own (it waits for the player), the answer applied under am's lock when it comes.
void kb_ask(const AppletPtr& ap, const swkbdui::Request& r) {
    std::thread([applet = ap, question = r] {
        std::string text;
        const bool ok = swkbdui::ask(question, &text);
        Guard g(am_lock());
        if (applet->completed) return;
        kb_answer(*applet, ok, text);
    }).detach();
}

void kb_start(const AppletPtr& ap) {
    Applet& a = *ap;
    const KbConfig c = kb_config(a);
    a.kb_utf8 = c.utf8;
    a.kb_check = c.check;
    a.kb_max = c.request.max_len;
    a.kb_request = c.request;
    if (config_has("applet", "swkbd_text")) {
        kb_answer(a, true, config_get_str("applet", "swkbd_text", ""));
    } else if (swkbdui::prompt_enabled()) {
        kb_ask(ap, a.kb_request);
    } else {
        kb_answer(a, true, c.request.initial.empty() ? std::string("Player") : c.request.initial);
    }
}

// The game's message with a rejected text (@4), in the config's encoding.
std::string kb_game_message(const Applet& a, const Blob& st) {
    if (!a.kb_utf8) return read_utf16(st, 4, 0x3EA);
    std::string s;
    for (uint64_t i = 4; i < st.size() && i < 4 + 0x7D4; ++i) {
        const uint8_t c = st[i];
        if (c == 0) break;
        if (c < 0x20 && c != 0x0A) continue;
        s += static_cast<char>(c);
    }
    return s;
}

// The game's verdict on the text (PushInteractiveInData): Success / ShowConfirmDialog take the text; anything else asks again when the
// prompt can, else the keyboard ends cancelled.
void kb_check_answer(const AppletPtr& ap) {
    Applet& a = *ap;
    if (!a.started || !a.kb_check || a.completed || a.game_in.empty()) return;
    const BlobPtr st = a.game_in.front();
    a.game_in.pop_front();
    if (st->size() != 0x7D8) NXLOG_I(kTag, "software keyboard: text check answer of %zu bytes", st->size());
    const uint32_t verdict = st->size() < 4 ? 1 : rd32(*st, 0);
    if (verdict == 0 || verdict == 2) {
        push_out(a, kb_output(a, true));
        end_applet(a);
        return;
    }
    const std::string msg = kb_game_message(a, *st);
    NXLOG_I(kTag, "software keyboard: the game refused the text (%u): %s", verdict, msg.c_str());
    if (swkbdui::prompt_enabled() && !config_has("applet", "swkbd_text")) {
        swkbdui::Request r = a.kb_request;
        if (!msg.empty()) r.guide = msg;
        r.initial = to_utf8(a.kb_text);
        kb_ask(ap, r);
        return;
    }
    push_out(a, kb_output(a, false));
    end_applet(a);
}

// ---- the software keyboard, inline (resident; requests through PushInteractiveInData) ----
// Every reply: u32 state, u32 reply type, then its payload.
void ik_reply(Applet& a, uint32_t type, const Blob& payload = Blob()) {
    Blob v(8, 0);
    wr32(v, 0, a.ik_state);
    wr32(v, 4, type);
    v.insert(v.end(), payload.begin(), payload.end());
    push_reply(a, std::move(v));
}
void ik_set_state(Applet& a, uint32_t state) {
    a.ik_state = state;
    ik_reply(a, 1);   // Default
}

// The text area of a reply: UTF-16 0x3EC bytes (text up to 0x3EA), UTF-8 0x7D4 bytes (text up to 0x7D3), zero filled.
Blob ik_text_area(const Applet& a) {
    if (a.ik_utf8) {
        Blob v(0x7D4, 0);
        const std::string s = to_utf8(a.ik_text);
        put_bytes(v, 0, s.data(), std::min<size_t>(s.size(), 0x7D3));
        return v;
    }
    Blob v(0x3EC, 0);
    put_bytes(v, 0, a.ik_text.data(), std::min<size_t>(a.ik_text.size() * 2, 0x3EA));
    return v;
}
void ik_changed_string(Applet& a) {
    Blob p = ik_text_area(a);
    append32(p, static_cast<uint32_t>(a.ik_text.size()));
    append32(p, 0xFFFFFFFFu);
    append32(p, 0xFFFFFFFFu);
    append32(p, static_cast<uint32_t>(a.ik_cursor));
    if (a.ik_v2) p.push_back(0);
    ik_reply(a, a.ik_utf8 ? (a.ik_v2 ? 15u : 7u) : (a.ik_v2 ? 13u : 2u), p);
}
void ik_decided_enter(Applet& a) {
    Blob p = ik_text_area(a);
    append32(p, static_cast<uint32_t>(a.ik_text.size()));
    ik_reply(a, a.ik_utf8 ? 9u : 5u, p);
}
void ik_hide(Applet& a) {
    if (a.ik_state != 3) return;
    ik_set_state(a, 4);
    ik_set_state(a, 1);
}
void ik_answer(Applet& a, bool ok, const std::u16string& text) {
    if (!ok) {
        ik_reply(a, 6);   // DecidedCancel
        ik_hide(a);
        return;
    }
    a.ik_text = cut(text, a.ik_max);
    a.ik_cursor = static_cast<int32_t>(a.ik_text.size());
    ik_changed_string(a);
    ik_decided_enter(a);
    ik_hide(a);
}

// Appear (from the hidden state): shown, then answered with the text (or later, when the prompt answers).
void ik_appear(const AppletPtr& ap, const Blob& st, uint64_t appear) {
    Applet& a = *ap;
    ik_set_state(a, 2);
    ik_set_state(a, 3);
    const uint32_t want_max = rd32(st, appear + 0x20), want_min = rd32(st, appear + 0x24);
    a.ik_max = want_max >= 1 && want_max <= kTextMax ? want_max : kTextMax;
    swkbdui::Request q;
    q.title = profile().name;
    q.ok_text = read_utf16(st, appear + 4, 9);
    q.initial = to_utf8(a.ik_text);
    q.max_len = a.ik_max;
    q.min_len = want_min <= a.ik_max ? want_min : 0;
    const uint64_t serial = ++a.ik_serial;
    if (config_has("applet", "swkbd_text")) {
        ik_answer(a, true, to_utf16(config_get_str("applet", "swkbd_text", "")));
    } else if (swkbdui::prompt_enabled()) {
        std::thread([applet = ap, question = q, serial] {
            std::string text;
            const bool ok = swkbdui::ask(question, &text);
            Guard g(am_lock());
            if (applet->ik_serial != serial || applet->completed) return;   // a Finalize or a disappear came first
            ik_answer(*applet, ok, to_utf16(text));
        }).detach();
    } else {
        ik_answer(a, true, a.ik_text.empty() ? std::u16string(u"Player") : a.ik_text);
    }
}

// Calc: offsets in the storage (4 + the argument's own). The old argument (size 0x4A0) and the newer one (every other size).
void ik_calc(const AppletPtr& ap, const Blob& st) {
    Applet& a = *ap;
    const bool old_layout = rd16(st, 8) == 0x4A0;
    const uint64_t flags = rd64(st, 12);
    const uint64_t appear = old_layout ? 0x24 : 0x1C, cursor = old_layout ? 0x20 : 0x90, text = old_layout ? 0x6C : 0x94,
                   utf8 = old_layout ? 0x460 : 0x488;
    if (flags & 0x8) a.ik_text = to_utf16(read_utf16(st, text, 0x1FA));        // set the input text
    if (flags & 0x10) a.ik_cursor = static_cast<int32_t>(rd32(st, cursor));    // set the cursor
    if (flags & 0x20) a.ik_utf8 = rd8(st, utf8) != 0;                           // UTF-8 mode
    if (a.ik_state <= 1 && (flags & 0x40)) ik_reply(a, 10);                     // UnsetCustomizeDic
    if (a.ik_state <= 1 && (flags & 0x400)) ik_reply(a, 11);                    // ReleasedUserWordInfo
    if (a.ik_state == 0 && (flags & 0x1)) {                                     // initialize
        ik_set_state(a, 1);
        ik_reply(a, 0, Blob(1, 0));                                            // FinishedInitialize
    }
    if (a.ik_state == 1 && (flags & 0x4)) {
        ik_appear(ap, st, appear);
    } else if (a.ik_state == 3 && (flags & 0x80)) {                             // disappear
        ++a.ik_serial;
        ik_hide(a);
    }
}

// The inline keyboard's requests (u32 @0 of each storage the game pushes).
enum : uint32_t {
    kIkFinalize = 4,
    kIkSetUserWordInfo = 6,
    kIkSetCustomizeDic = 7,
    kIkCalc = 10,
    kIkSetCustomizedDictionaries = 11,
    kIkUnsetCustomizedDictionaries = 12,
    kIkSetChangedStringV2Flag = 13,
    kIkSetMovedCursorV2Flag = 14,
};

void ik_request(const AppletPtr& ap, const Blob& st) {
    Applet& a = *ap;
    const uint32_t cmd = rd32(st, 0);
    switch (cmd) {
        case kIkFinalize:
            ++a.ik_serial;
            ik_set_state(a, 0);
            end_applet(a);
            break;
        case kIkSetUserWordInfo:
        case kIkSetCustomizeDic:
        case kIkSetCustomizedDictionaries:
            break;   // nothing to keep
        case kIkUnsetCustomizedDictionaries:
            ik_reply(a, 12);
            break;
        case kIkSetChangedStringV2Flag:
            a.ik_v2 = rd8(st, 4) != 0;
            break;
        case kIkSetMovedCursorV2Flag:
            a.ik_cursor_v2 = rd8(st, 4) != 0;
            break;
        case kIkCalc:
            ik_calc(ap, st);
            break;
        default:
            NXLOG_I(kTag, "inline keyboard: request %u (%zu bytes) ignored", cmd, st.size());
            break;
    }
}

// Every queued request in order, until one ends the keyboard.
void ik_process(const AppletPtr& ap) {
    Applet& a = *ap;
    while (!a.completed && !a.game_in.empty()) {
        const BlobPtr st = a.game_in.front();
        a.game_in.pop_front();
        ik_request(ap, *st);
    }
}

// ---- player select ----
void player_select(Applet& a) {
    const Blob* ui = in_entry(a, 1);
    const uint32_t mode = ui && ui->size() >= 4 ? rd32(*ui, 0) : 0;
    Blob v(0x18, 0);
    if (mode == 0) {   // the user selector: the one user
        uint64_t hi = 0, lo = 0;
        accapi::user_uid(&hi, &lo);
        wr64(v, 8, hi);
        wr64(v, 16, lo);
    } else {           // a user creator or a network account check: the player backed out (124-1 CancelledByUser)
        wr64(v, 0, 0x27C);
    }
    push_out(a, std::move(v));
    end_applet(a);
}

// ---- error applet: logged, nothing shown ----
std::string c_text(const Blob& b, uint64_t off, uint64_t max) {
    std::string s;
    for (uint64_t i = off; i < b.size() && i < off + max && b[i]; ++i) s += static_cast<char>(b[i]);
    return s;
}
void error_applet(Applet& a) {
    const Blob* info = in_entry(a, 1);
    if (info == nullptr || info->size() == 0) {
        NXLOG_W(kTag, "error applet without an argument");
    } else {
        const Blob& e = *info;
        switch (e[0]) {
            case 0: NXLOG_W(kTag, "error applet: result 0x%X", rd32(e, 0x10)); break;
            case 4: NXLOG_W(kTag, "error applet: result 0x%X", rd32(e, 8)); break;
            case 2:
                NXLOG_W(kTag, "error applet: application error %u (%s): %s / %s", rd32(e, 8), c_text(e, 0xC, 8).c_str(),
                        c_text(e, 0x14, 0x800).c_str(), c_text(e, 0x814, 0x800).c_str());
                break;
            default: NXLOG_W(kTag, "error applet: type %u, %zu bytes", static_cast<unsigned>(e[0]), e.size()); break;
        }
    }
    end_applet(a);
}

// ---- controller applet ----
struct CtrlArg {
    int min = 0, max = 0;   // 0 when there is no header
    bool single = false;
};
struct CtrlHid {
    int max_seen = 0;       // the maximum given to hid (1 in single mode)
    int players = 0;
    uint32_t selected = 0;
    bool waits = false;
};

CtrlArg controller_arg(const Applet& a) {
    CtrlArg c;
    const Blob* h = in_entry(a, 1);
    if (h && h->size() >= 2) {
        c.min = static_cast<int8_t>((*h)[0]);
        c.max = static_cast<int8_t>((*h)[1]);
    }
    if (h && h->size() >= 6) c.single = (*h)[5] != 0;
    return c;
}

// hid's side, without am's lock (every Start).
void controller_hid(const CtrlArg& c, CtrlHid* h) {
    h->max_seen = c.single ? 1 : c.max;
    hid_applet_open(h->max_seen);
    h->players = hid_sdk_players(&h->selected);
    h->waits = !c.single && !ctrlui::enough(h->players, c.min) && ctrlui::prompt_enabled();
    if (!h->waits) h->players = hid_applet_close(h->max_seen, &h->selected);
}

// ControllerSupportResultInfo (12 bytes): s8 players, u32 selected npad id @4, u32 result @8 (0 success, 2 cancel).
void controller_answer(Applet& a, const CtrlArg& c, int players, uint32_t selected) {
    const int lo = std::max(1, c.min), hi = c.max;
    const bool cancel = !c.single && lo > players;
    int count = c.single ? std::min(players, 1) : players;
    if (hi > 0) count = std::min(count, hi);
    Blob v(12, 0);
    v[0] = static_cast<uint8_t>(static_cast<int8_t>(count));
    wr32(v, 4, selected);
    wr32(v, 8, cancel ? 2u : 0u);
    push_out(a, std::move(v));
    end_applet(a);
}

void controller_start(const AppletPtr& ap, const CtrlArg& c, const CtrlHid& h) {
    if (!h.waits) {
        controller_answer(*ap, c, h.players, h.selected);
        return;
    }
    const int max_seen = h.max_seen;
    std::thread([ap, c, max_seen] {   // the prompt is up until enough players are there or the player closes it
        ctrlui::wait_for_players(
            c.min, c.max, [] { return hid_sdk_players(nullptr); },
            [ap] {
                Guard g(am_lock());
                return ap->completed;
            });
        uint32_t selected = 0;
        const int players = hid_applet_close(max_seen, &selected);
        Guard g(am_lock());
        if (ap->completed) return;
        controller_answer(*ap, c, players, selected);
    }).detach();
}

// ---- the web family ----
// The argument (in-data[1]): u16 count @0, u32 shim kind @4, then entries {u16 type, u16 size, 4 bytes, data} from @8. The walk ends at an
// entry whose data the argument cuts short.
bool tlv_find(const Blob& arg, uint16_t type, uint64_t* off, uint16_t* size) {
    if (arg.size() < 8) return false;
    const uint16_t count = rd16(arg, 0);
    uint64_t at = 8;
    for (uint32_t i = 0; i < count && at + 8 <= arg.size(); ++i) {
        const uint16_t t = rd16(arg, at), n = rd16(arg, at + 2);
        if (at + 8 + n > arg.size()) return false;
        if (t == type) {
            *off = at + 8;
            *size = n;
            return true;
        }
        at += 8 + n;
    }
    return false;
}
uint32_t tlv_u32(const Blob* arg, uint16_t type, uint32_t def) {
    uint64_t off = 0;
    uint16_t n = 0;
    if (!arg || !tlv_find(*arg, type, &off, &n) || n == 0) return def;
    uint32_t v = 0;
    std::memcpy(&v, arg->data() + off, std::min<size_t>(n, 4));
    return v;
}
std::string tlv_str(const Blob* arg, uint16_t type) {
    uint64_t off = 0;
    uint16_t n = 0;
    if (!arg || !tlv_find(*arg, type, &off, &n)) return std::string();
    return c_text(*arg, off, n);
}

bool hidden_web_session(const Applet& a) {
    const Blob* arg = in_entry(a, 1);
    return a.kind == kOffline && arg && tlv_u32(arg, 0x42, 0) != 0 && tlv_u32(arg, 0x41, 0) == 1;
}

// A content message (kind 0): u32 kind, u32 size (the text with its NUL), 8 bytes, the text, NUL.
Blob content_message(const std::string& text) {
    Blob v(0x10 + text.size() + 1, 0);
    wr32(v, 4, static_cast<uint32_t>(text.size() + 1));
    put_bytes(v, 0x10, text.data(), text.size());
    return v;
}

// The return value, as out data; the page (if one is shown) gives the input back; the applet ends.
void web_return(Applet& a, uint32_t reason, const std::string& url) {
    const bool tlv_form = webview::view_enabled();
    Blob v;
    if (tlv_form) {
        const Blob* arg = in_entry(a, 1);
        const Blob* common = in_entry(a, 0);
        const uint32_t shim = arg && arg->size() >= 8 ? rd32(*arg, 4) : 0;
        const uint32_t library = common && common->size() >= 12 ? rd32(*common, 8) : 0;
        v = webview::return_storage(shim, library, reason, url);
    } else {   // WebCommonReturnValue
        v.assign(0x1010, 0);
        wr32(v, 0, reason);
        const size_t n = std::min<size_t>(url.size(), 0xFFF);
        put_bytes(v, 8, url.data(), n);
        wr64(v, 0x1008, n);
    }
    push_out(a, std::move(v));
    if (a.page) {
        a.page = false;
        webview::set_input_held(false);
    }
    end_applet(a);
}

// What the host reports, applied under am's lock; nothing once the applet ended or its page is gone.
webview::Events web_events(const AppletPtr& ap) {
    const std::weak_ptr<Applet> weak = ap;
    webview::Events e;
    e.message = [weak](const std::string& text) {
        Guard g(am_lock());
        const AppletPtr a = weak.lock();
        if (!a || a->completed || !a->page) return;
        push_reply(*a, content_message(text));
    };
    e.ended = [weak](uint32_t reason, const std::string& last_url) {
        Guard g(am_lock());
        const AppletPtr a = weak.lock();
        if (!a || a->completed || !a->page) return;
        web_return(*a, reason, last_url);
    };
    e.failed = [weak](const std::string& why) {
        Guard g(am_lock());
        const AppletPtr a = weak.lock();
        if (!a || a->completed || !a->page) return;
        NXLOG_W(kTag, "web applet: the web view could not show the page: %s", why.c_str());
        a->page = false;
        webview::set_input_held(false);
        if (a->session) push_reply(*a, content_message(std::string()));
        else web_return(*a, a->online ? webview::kExitErrorDialog : webview::kExitRequested, std::string());
    };
    return e;
}

// An offline page the game ships, in the host's web view. False: the stand-in answers instead.
bool web_offline_page(const AppletPtr& ap, bool start_hidden) {
    Applet& a = *ap;
    const Blob* arg = in_entry(a, 1);
    webview::Host* host = webview::host();
    const std::string doc = tlv_str(arg, 0x6);
    if (!host || doc.empty()) {
        NXLOG_I(kTag, "offline web applet (%s): %s, the stand-in answers", doc.c_str(), host ? "no document path" : "no web view");
        return false;
    }
    webview::Page page;
    if (!webview::find_page(webview::html_folder(), doc, &page)) {
        NXLOG_W(kTag, "offline web applet: %s is not in the title's html folder, the stand-in answers", doc.c_str());
        return false;
    }
    const std::string callback = tlv_str(arg, 0x3);
    page.callback_url = callback.empty() ? std::string("http://localhost/") : callback;
    page.hidden = start_hidden;
    page.repeat_delay = tlv_u32(arg, 0x1C, 0);
    page.repeat_pulse = tlv_u32(arg, 0x1D, 0);
    page.muted = config_get_str("audio", "sink", "") == "null";
    if (!host->open(page, web_events(ap))) {
        NXLOG_W(kTag, "offline web applet: the web view refused %s, the stand-in answers", doc.c_str());
        return false;
    }
    a.page = true;
    if (!start_hidden) webview::set_input_held(true);
    return true;
}

// An online page (the web applet with [web] view): the page, or the console's error dialog when it is refused.
void web_online_page(const AppletPtr& ap) {
    Applet& a = *ap;
    const Blob* arg = in_entry(a, 1);
    webview::Page page;
    page.online = true;
    page.url = tlv_str(arg, 0x1);
    page.callback_url = tlv_str(arg, 0x3);
    page.repeat_delay = tlv_u32(arg, 0x1C, 0);
    page.repeat_pulse = tlv_u32(arg, 0x1D, 0);
    page.muted = config_get_str("audio", "sink", "") == "null";
    const bool http = page.url.rfind("http://", 0) == 0 || page.url.rfind("https://", 0) == 0;
    const std::string site = webview::url_host(page.url);
    const bool online_ok = webview::online_enabled();
    if (http && webview::nintendo_url(page.url)) {
        page.error = "This page is on Nintendo's servers (" + site + "), which AnySwitch never connects to.";
    } else if (!online_ok) {
        page.error = "Unable to connect to the Internet.\n\nThis game wants to show a page from the Internet" +
                     (site.empty() ? std::string() : " (" + site + ")") + ". Online pages are off in AnySwitch's settings.";
    } else if (!http) {
        page.error = "This page cannot be shown.";
    }
    a.online = true;
    if (!page.error.empty()) NXLOG_I(kTag, "web applet: %s refused: %s", page.url.c_str(), page.error.c_str());
    webview::Host* host = webview::host();
    if (host && host->open(page, web_events(ap))) {
        a.page = true;
        webview::set_input_held(true);
        return;
    }
    web_return(a, webview::kExitErrorDialog, std::string());
}

void web_start(const AppletPtr& ap) {
    Applet& a = *ap;
    const Blob* arg = in_entry(a, 1);
    if (a.kind == kWeb && arg && webview::view_enabled()) {
        web_online_page(ap);
        return;
    }
    if (a.kind == kOffline && arg) {
        const bool session = tlv_u32(arg, 0x42, 0) != 0;
        const bool hidden = session && tlv_u32(arg, 0x41, 0) == 1;
        if (web_offline_page(ap, hidden)) {
            a.session = session;
            return;
        }
        if (session) {   // the stand-in session: the page's first content message, empty; Appear ends it
            a.session = true;
            push_reply(a, content_message(std::string()));
            return;
        }
    }
    web_return(a, a.kind == kWeb || a.kind == kOffline ? webview::kExitRequested : webview::kExitEndButton, std::string());
}

// A message of the game to the session's page (the storage stays queued).
void web_session_message(const AppletPtr& ap) {
    Applet& a = *ap;
    if (a.completed || a.game_in.empty()) return;
    const Blob& st = *a.game_in.back();
    if (st.size() < 0x10) return;
    const uint32_t kind = rd32(st, 0), size = rd32(st, 4);
    const bool view = webview::view_enabled();
    if (view && (kind == 0 || kind == 0x100)) {   // acknowledged at once: 0x1000 a content message, 0x1001 the Appear
        Blob ack(0x20, 0);
        wr32(ack, 0, kind == 0 ? 0x1000u : 0x1001u);
        wr32(ack, 4, 0xC);
        wr32(ack, 0x10, static_cast<uint32_t>(st.size()));
        push_reply(a, std::move(ack));
    }
    if (a.page) {
        webview::Host* host = webview::host();
        if (kind == 0x100) {
            if (!a.owns_focus) take_focus(a);
            webview::set_input_held(true);
            if (host) host->show();
        } else if (kind == 0) {
            std::string text;
            if (view) {
                text = c_text(st, 0x10, std::min<uint64_t>(st.size() - 0x10, size));
            } else if (size != 0 && st.size() >= 0x10 + static_cast<uint64_t>(size)) {
                text.assign(reinterpret_cast<const char*>(st.data()) + 0x10, size - 1);
            }
            if (host) host->send(text);
        }
        return;
    }
    if (kind == 0x100) {
        web_return(a, webview::kExitEndButton, std::string());
    } else if (kind == 0) {
        if (g_content_logged.fetch_add(1) < 8) NXLOG_I(kTag, "offline web stand-in: content message of %u bytes", size);
    } else if (kind != 0x1000) {
        NXLOG_D(kTag, "offline web stand-in: message kind 0x%X", kind);
    }
}

// ---- Start ----
uint32_t applet_start(const AppletPtr& ap) {
    CtrlArg carg;
    bool controller = false;
    {
        Guard g(am_lock());
        controller = ap->kind == kController;
        if (controller) carg = controller_arg(*ap);
    }
    CtrlHid hid;
    if (controller) controller_hid(carg, &hid);
    Guard g(am_lock());
    Applet& a = *ap;
    if (a.started) return 0;
    a.started = true;
    if (a.inline_kb) return 0;   // resident; the game keeps its focus
    if (!hidden_web_session(a)) take_focus(a);
    switch (a.kind) {
        case kKeyboard: kb_start(ap); break;
        case kPlayerSelect: player_select(a); break;
        case kErrorApplet: error_applet(a); break;
        case kController: controller_start(ap, carg, hid); break;
        case kWeb:
        case kOffline:
        case kLoginShare:
        case kWifiAuth: web_start(ap); break;
        default:
            if (!ends_cancelled(a.kind)) NXLOG_W(kTag, "library applet 0x%X (kind 0x%X) is not answered: it ends at once", a.id, a.kind);
            end_applet(a);
            break;
    }
    return 0;
}

std::shared_ptr<ipc::Service> make_applet_accessor(uint32_t id, uint32_t mode) {
    auto ap = std::make_shared<Applet>();
    ap->id = id;
    ap->kind = id & 0xFFFF;
    ap->mode = mode;
    ap->inline_kb = ap->kind == kKeyboard && mode != 0;
    NXLOG_I(kTag, "library applet 0x%X created (mode %u)", id, mode);
    return make_iface("ILibraryAppletAccessor", [ap](ipc::Service& s) {
        s.add_cmd(0, "GetAppletStateChangedEvent", [ap](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.out_copy.push_back(ap->end_event.new_handle());
            return 0;
        });
        s.add_cmd(1, "IsCompleted", [ap](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<uint8_t>(ap->completed ? 1 : 0);
            return 0;
        });
        s.add_cmd(10, "Start", [ap](Req&, Rsp&) -> uint32_t { return applet_start(ap); });
        s.add_cmd(20, "RequestExit", [ap](Req&, Rsp&) -> uint32_t {
            Guard g(am_lock());
            Applet& a = *ap;
            if (a.completed) return 0;
            if (a.page) {
                if (webview::Host* host = webview::host()) host->close();
                web_return(a, webview::kExitRequested, std::string());
            } else if (a.session) {
                web_return(a, webview::kExitRequested, std::string());
            } else {
                end_applet(a);
            }
            return 0;
        });
        s.add_cmd(25, "Terminate", [ap](Req&, Rsp&) -> uint32_t {
            Guard g(am_lock());
            Applet& a = *ap;
            if (a.page) {
                if (webview::Host* host = webview::host()) host->close();
                a.page = false;
                webview::set_input_held(false);
            }
            if (!a.completed) end_applet(a);
            return 0;
        });
        s.add_cmd(30, "GetResult", [ap](Req&, Rsp&) -> uint32_t { return ends_cancelled(ap->kind) ? kAppletCancelled : 0; });
        ignored(s, 50, "SetOutOfFocusApplicationSuspendingEnabled");
        ignored(s, 60, "PresetLibraryAppletGpuTimeSliceZero");
        ignored(s, 90, "Cmd90");
        s.add_cmd(100, "PushInData", [ap](Req& req, Rsp&) -> uint32_t {
            const auto st = storage_passed(req);
            if (!st) return kNotAStorage;
            Guard g(am_lock());
            ap->in_data.push_back(bytes_now(*st));
            return 0;
        });
        s.add_cmd(101, "PopOutData", [ap](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            return pop_storage(ap->out_data, ap->out_ready, rsp);
        });
        s.add_cmd(102, "PushExtraStorage", [](Req& req, Rsp&) -> uint32_t { return storage_passed(req) ? 0 : kNotAStorage; });
        s.add_cmd(103, "PushInteractiveInData", [ap](Req& req, Rsp&) -> uint32_t {
            const auto st = storage_passed(req);
            if (!st) return kNotAStorage;
            Guard g(am_lock());
            Applet& a = *ap;
            a.game_in.push_back(bytes_now(*st));
            if (!a.started) return 0;
            if (a.inline_kb) ik_process(ap);
            else if (a.kind == kKeyboard) kb_check_answer(ap);
            else if (a.session) web_session_message(ap);
            return 0;
        });
        s.add_cmd(104, "PopInteractiveOutData", [ap](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            return pop_storage(ap->interactive_out, ap->reply_ready, rsp);
        });
        s.add_cmd(105, "GetPopOutDataEvent", [ap](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.out_copy.push_back(ap->out_ready.new_handle());
            return 0;
        });
        s.add_cmd(106, "GetPopInteractiveOutDataEvent", [ap](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.out_copy.push_back(ap->reply_ready.new_handle());
            return 0;
        });
        ignored(s, 110, "NeedsToExitProcess", 1);
        s.add_cmd(120, "GetLibraryAppletInfo", [ap](Req&, Rsp& rsp) -> uint32_t {
            rsp.push<uint32_t>(ap->id);
            rsp.push<uint32_t>(ap->mode);
            return 0;
        });
        ignored(s, 150, "RequestForAppletToGetForeground");
    });
}

// ---- ExecuteProgram and the relaunch: the hand-off to the launcher (docs/EXECUTE_PROGRAM.md) -------------------------------------
// Both writers report a failed write or close.
bool write_channel_file(const std::string& path, const std::vector<Blob>& params) {
    FilePtr out = open_file(path, "wb");
    if (out == nullptr) return false;
    const uint32_t count = static_cast<uint32_t>(params.size());
    bool ok = std::fwrite(&count, 4, 1, out.get()) == 1;
    for (const Blob& p : params) {
        if (!ok) break;
        const uint64_t n = p.size();
        ok = std::fwrite(&n, 8, 1, out.get()) == 1 && (n == 0 || std::fwrite(p.data(), 1, static_cast<size_t>(n), out.get()) == n);
    }
    return std::fclose(out.release()) == 0 && ok;
}
bool write_text_file(const std::string& path, const char* text) {
    FilePtr out = open_file(path, "wb");
    if (out == nullptr) return false;
    const size_t len = std::strlen(text);
    const bool ok = std::fwrite(text, 1, len, out.get()) == len;
    return std::fclose(out.release()) == 0 && ok;
}

// The launch parameters to [am] execute_program_file + ".user_channel.bin", then the ini naming the next program; then the process ends
// with kExitExecuteProgram (through the exit hook when one is set: the call then returns). A file that cannot be written: the program
// goes on. Called without am's lock.
uint32_t hand_off(int64_t next, int64_t previous, const char* extra, const std::vector<Blob>& params, const std::string& what) {
    const std::string ini_path = config_get_path("am", "execute_program_file", "execute_program.ini");
    const std::string chan_path = ini_path + ".user_channel.bin";
    if (!write_channel_file(chan_path, params)) {
        NXLOG_E(kTag, "%s: cannot write %s; the program goes on", what.c_str(), chan_path.c_str());
        return 0;
    }
    char text[640];
    std::snprintf(text, sizeof(text),
                  "; written by the runtime at %s: the launcher starts this program next\n[execute_program]\nprogram_index = %lld\n"
                  "%sprevious_program_index = %lld\nuser_channel = %s\n",
                  what.c_str(), static_cast<long long>(next), extra, static_cast<long long>(previous), chan_path.c_str());
    if (!write_text_file(ini_path, text)) {
        NXLOG_E(kTag, "%s: cannot write %s; the program goes on", what.c_str(), ini_path.c_str());
        return 0;
    }
    NXLOG_I(kTag, "%s: program %lld next (%zu launch parameter(s)); this program ends", what.c_str(), static_cast<long long>(next),
            params.size());
    if (auto hook = g_on_exit.load()) {
        hook(amctl::kExitExecuteProgram, what.c_str());
        return 0;
    }
    log_flush();
    std::fflush(nullptr);
    std::_Exit(amctl::kExitExecuteProgram);
}

// CreateApplicationAndRequestToStart (12) / ...AndPushAndRequestToStart (10): this application again (id 0 or one of its programs).
uint32_t request_to_start(uint64_t id, const char* call, const std::vector<Blob>& params) {
    char what[160];
    std::snprintf(what, sizeof what, "IApplicationFunctions %s(%016llx)", call, static_cast<unsigned long long>(id));
    const uint64_t app_mask = ~uint64_t{0xFFF};
    const bool own = id == 0 || (id & app_mask) == (profile().application_id & app_mask) || (id & app_mask) == (program_id_now() & app_mask);
    if (!own) {
        NXLOG_E(kTag, "%s: another application cannot be started from here", what);
        return kLaunchFailed;
    }
    return hand_off(id == 0 ? 0 : static_cast<int64_t>(id & 0xFFF), -1, "relaunch = 1\n", params, what);
}

// ---- the other interfaces ------------------------------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_lock_accessor() {
    struct LockState {
        bool locked = false;
        ServiceEvent event{true};
    };
    auto st = std::make_shared<LockState>();
    return make_iface("ILockAccessor", [st](ipc::Service& s) {
        s.add_cmd(1, "TryLock", [st](Req& req, Rsp& rsp) -> uint32_t {
            const bool want_handle = req.arg<uint8_t>(0) != 0;
            Guard g(am_lock());
            const bool acquired = !st->locked;
            st->locked = true;
            rsp.push<uint8_t>(acquired ? 1 : 0);
            rsp.out_copy.push_back(want_handle ? st->event.new_handle() : 0);
            return 0;
        });
        s.add_cmd(2, "Unlock", [st](Req&, Rsp&) -> uint32_t {
            Guard g(am_lock());
            st->locked = false;
            st->event.signal();
            return 0;
        });
        s.add_cmd(3, "GetEvent", [st](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.out_copy.push_back(st->event.new_handle());
            return 0;
        });
        s.add_cmd(4, "IsLocked", [st](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<uint8_t>(st->locked ? 1 : 0);
            return 0;
        });
    });
}

// [am] resolution = WxH (decimal, a lower-case x, nothing after the height, each 1..65535).
bool resolution_override(int32_t* w, int32_t* h) {
    const std::string s = config_get_str("am", "resolution", "");
    if (s.empty()) return false;
    const char* p = s.c_str();
    char* end = nullptr;
    const unsigned long width = std::strtoul(p, &end, 10);
    if (end == p || *end != 'x') return false;
    const char* q = end + 1;
    const unsigned long height = std::strtoul(q, &end, 10);
    if (end == q || *end != '\0') return false;
    if (width < 1 || width > 0xFFFF || height < 1 || height > 0xFFFF) return false;
    *w = static_cast<int32_t>(width);
    *h = static_cast<int32_t>(height);
    return true;
}

std::shared_ptr<ipc::Service> make_common_state_getter() {
    return make_iface("ICommonStateGetter", [](ipc::Service& s) {
        process_event(s, 0, "GetEventHandle", &Proc::message_event);
        s.add_cmd(1, "ReceiveMessage", [](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            Proc& p = proc();
            if (p.messages.empty()) {
                p.message_event.clear();
                return kNoMessages;
            }
            const uint32_t msg = p.messages.front();
            p.messages.pop_front();
            if (p.messages.empty()) p.message_event.clear();
            rsp.push<uint32_t>(msg);
            return 0;
        });
        ignored(s, 2, "GetThisAppletKind", 8);
        s.add_cmd(5, "GetOperationMode", [](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<uint8_t>(proc().docked ? 1 : 0);   // 1 Console, 0 Handheld
            return 0;
        });
        s.add_cmd(6, "GetPerformanceMode", [](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<uint32_t>(proc().docked ? 1u : 0u);   // 1 Boost while docked
            return 0;
        });
        ignored(s, 8, "GetBootMode", 1);
        s.add_cmd(9, "GetCurrentFocusState", [](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<uint8_t>(proc().focus);
            return 0;
        });
        s.add_cmd(10, "RequestToAcquireSleepLock", [](Req&, Rsp&) -> uint32_t {
            Guard g(am_lock());
            proc().sleep_lock.signal();
            return 0;
        });
        ignored(s, 11, "ReleaseSleepLock");
        ignored(s, 12, "ReleaseSleepLockTransiently");
        process_event(s, 13, "GetAcquiredSleepLockEvent", &Proc::sleep_lock);
        const auto lock_accessor = [](Req&, Rsp& rsp) -> uint32_t {
            rsp.push_object(make_lock_accessor());
            return 0;
        };
        s.add_cmd(30, "GetHomeButtonReaderLockAccessor", lock_accessor);
        s.add_cmd(31, "GetReaderLockAccessorEx", lock_accessor);
        s.add_cmd(32, "GetWriterLockAccessorEx", lock_accessor);
        s.add_cmd(50, "IsVrModeEnabled", [](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<uint8_t>(proc().vr ? 1 : 0);
            return 0;
        });
        s.add_cmd(51, "SetVrModeEnabled", [](Req& req, Rsp&) -> uint32_t {
            Guard g(am_lock());
            proc().vr = req.arg<uint8_t>(0) != 0;
            return 0;
        });
        ignored(s, 52, "SetLcdBacklighOffEnabled");
        s.add_cmd(53, "BeginVrModeEx", [](Req&, Rsp&) -> uint32_t {
            Guard g(am_lock());
            proc().vr = true;
            return 0;
        });
        s.add_cmd(54, "EndVrModeEx", [](Req&, Rsp&) -> uint32_t {
            Guard g(am_lock());
            proc().vr = false;
            return 0;
        });
        ignored(s, 55, "IsInControllerFirmwareUpdateSection", 1);
        ignored(s, 59, "SetVrPositionForDebug");
        s.add_cmd(60, "GetDefaultDisplayResolution", [](Req&, Rsp& rsp) -> uint32_t {
            bool docked;
            {
                Guard g(am_lock());
                docked = proc().docked;
            }
            int32_t w = docked ? 1920 : 1280, h = docked ? 1080 : 720;
            resolution_override(&w, &h);
            rsp.push<int32_t>(w);
            rsp.push<int32_t>(h);
            return 0;
        });
        process_event(s, 61, "GetDefaultDisplayResolutionChangeEvent", &Proc::never);
        ignored(s, 62, "GetHdcpAuthenticationState", 4);
        process_event(s, 63, "GetHdcpAuthenticationStateChangeEvent", &Proc::never);
        ignored(s, 66, "SetCpuBoostMode");
        ignored(s, 67, "CancelCpuBoostMode");
        ignored(s, 68, "GetBuiltInDisplayType", 4);
        s.add_cmd(130, "EnableStartupLogoDisappearedMessage", [](Req&, Rsp&) -> uint32_t {
            Guard g(am_lock());
            queue_message(proc(), kMsgStartupLogoGone);
            return 0;
        });
        s.add_cmd(300, "GetSettingsPlatformRegion", [](Req&, Rsp& rsp) -> uint32_t {
            rsp.push<uint32_t>(1);   // Global
            return 0;
        });
        ignored(s, 900, "SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled");
        s.add_cmd(1000, "BeginVrMode3d", [](Req&, Rsp&) -> uint32_t {
            Guard g(am_lock());
            proc().vr3d = true;
            return 0;
        });
        s.add_cmd(1001, "EndVrMode3d", [](Req&, Rsp&) -> uint32_t {
            Guard g(am_lock());
            proc().vr3d = false;
            return 0;
        });
        s.add_cmd(1002, "IsVrModeEnabled3d", [](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<uint8_t>(proc().vr3d ? 1 : 0);
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_self_controller() {
    return make_iface("ISelfController", [](ipc::Service& s) {
        s.add_cmd(0, "Exit", [](Req&, Rsp&) -> uint32_t {
            NXLOG_W(kTag, "ISelfController Exit: the game asks to end (the run is ended by the shell, not here)");
            return 0;
        });
        ignored(s, 1, "LockExit");
        ignored(s, 2, "UnlockExit");
        ignored(s, 3, "EnterFatalSection");
        ignored(s, 4, "LeaveFatalSection");
        process_event(s, 9, "GetLibraryAppletLaunchableEvent", &Proc::launchable);
        ignored(s, 10, "SetScreenShotPermission");
        s.add_cmd(11, "SetOperationModeChangedNotification", [](Req& req, Rsp&) -> uint32_t {
            Guard g(am_lock());
            proc().mode_notify = req.arg<uint8_t>(0) != 0;
            return 0;
        });
        s.add_cmd(12, "SetPerformanceModeChangedNotification", [](Req& req, Rsp&) -> uint32_t {
            Guard g(am_lock());
            proc().perf_notify = req.arg<uint8_t>(0) != 0;
            return 0;
        });
        s.add_cmd(13, "SetFocusHandlingMode", [](Req& req, Rsp&) -> uint32_t {
            const uint8_t notify = req.arg<uint8_t>(0), b1 = req.arg<uint8_t>(1), b2 = req.arg<uint8_t>(2);
            NXLOG_D(kTag, "SetFocusHandlingMode(%u, %u, %u)", static_cast<unsigned>(notify), static_cast<unsigned>(b1),
                    static_cast<unsigned>(b2));
            Guard g(am_lock());
            proc().focus_notify = notify != 0;
            return 0;
        });
        ignored(s, 14, "SetRestartMessageEnabled");
        ignored(s, 16, "SetOutOfFocusSuspendingEnabled");
        s.add_cmd(40, "CreateManagedDisplayLayer", [](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<uint64_t>(proc().next_layer++);
            return 0;
        });
        ignored(s, 41, "IsSystemBufferSharingEnabled");
        s.add_cmd(44, "CreateManagedDisplaySeparableLayer", [](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            Proc& p = proc();
            rsp.push<uint64_t>(p.next_layer);
            rsp.push<uint64_t>(p.next_layer + 1);
            p.next_layer += 2;
            return 0;
        });
        s.add_cmd(62, "SetIdleTimeDetectionExtension", [](Req& req, Rsp&) -> uint32_t {
            Guard g(am_lock());
            proc().idle_extension = req.arg<uint32_t>(0);
            return 0;
        });
        s.add_cmd(63, "GetIdleTimeDetectionExtension", [](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<uint32_t>(proc().idle_extension);
            return 0;
        });
        s.add_cmd(68, "SetAutoSleepDisabled", [](Req& req, Rsp&) -> uint32_t {
            Guard g(am_lock());
            proc().auto_sleep_disabled = req.arg<uint8_t>(0) != 0;
            return 0;
        });
        s.add_cmd(69, "IsAutoSleepDisabled", [](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<uint8_t>(proc().auto_sleep_disabled ? 1 : 0);
            return 0;
        });
        ignored(s, 90, "GetAccumulatedSuspendedTickValue", 8);
        process_event(s, 91, "GetAccumulatedSuspendedTickChangedEvent", &Proc::suspended_tick);
        ignored(s, 100, "SetAlbumImageTakenNotificationEnabled");
        ignored(s, 110, "SetApplicationAlbumUserData");
        ignored(s, 120, "SaveCurrentScreenshot");
        s.add_cmd(2000, "GetGraphicsSupervisor", [](Req&, Rsp& rsp) -> uint32_t {
            const uint32_t session = ipc::create_session_handle(std::make_shared<ipc::StubService>("GraphicsSupervisor"));
            if (session == 0) return kernel::kStubService;   // no handle left
            rsp.out_move.push_back(session);
            return 0;
        });
        static constexpr uint32_t kSharingCmds[] = {2100, 2200, 2300, 2400, 2401, 2402};
        for (const uint32_t cmd : kSharingCmds)
            s.add_cmd(cmd, "SetLayerOrAudioSharingEnabled (S2)", [](Req&, Rsp&) -> uint32_t { return 0; });
    });
}

std::shared_ptr<ipc::Service> make_window_controller() {
    return make_iface("IWindowController", [](ipc::Service& s) {
        s.add_cmd(1, "GetAppletResourceUserId", [](Req&, Rsp& rsp) -> uint32_t {
            rsp.push<uint64_t>(0x51);
            return 0;
        });
        ignored(s, 10, "AcquireForegroundRights");
        ignored(s, 11, "ReleaseForegroundRights");
        ignored(s, 12, "RejectToChangeIntoBackground");
    });
}

float volume(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }   // a NaN stays as it is

std::shared_ptr<ipc::Service> make_audio_controller() {
    struct Volumes {
        float main = 1.0f, library = 1.0f;
    };
    auto vol = std::make_shared<Volumes>();
    return make_iface("IAudioController", [vol](ipc::Service& s) {
        s.add_cmd(0, "SetExpectedMasterVolume", [vol](Req& req, Rsp&) -> uint32_t {
            Guard g(am_lock());
            vol->main = volume(req.arg<float>(0));
            vol->library = volume(req.arg<float>(4));
            return 0;
        });
        s.add_cmd(1, "GetMainAppletExpectedMasterVolume", [vol](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<float>(vol->main);
            return 0;
        });
        s.add_cmd(2, "GetLibraryAppletExpectedMasterVolume", [vol](Req&, Rsp& rsp) -> uint32_t {
            Guard g(am_lock());
            rsp.push<float>(vol->library);
            return 0;
        });
        s.add_cmd(3, "ChangeMainAppletMasterVolume", [vol](Req& req, Rsp&) -> uint32_t {
            Guard g(am_lock());
            vol->main = volume(req.arg<float>(0));   // the fade time (@8) is not used
            return 0;
        });
        ignored(s, 4, "SetTransparentVolumeRate");
    });
}

std::shared_ptr<ipc::Service> make_library_applet_creator() {
    return make_iface("ILibraryAppletCreator", [](ipc::Service& s) {
        const auto create = [](Req& req, Rsp& rsp) -> uint32_t {
            rsp.push_object(make_applet_accessor(req.arg<uint32_t>(0), req.arg<uint32_t>(4)));
            return 0;
        };
        s.add_cmd(0, "CreateLibraryApplet", create);
        s.add_cmd(3, "CreateLibraryAppletEx", create);
        s.add_cmd(10, "CreateStorage", [](Req& req, Rsp& rsp) -> uint32_t {
            const int64_t size = req.arg<int64_t>(0);
            if (size < 0 || size > kStorageMax) return kNoMemory;
            rsp.push_object(storage_object(std::make_shared<Blob>(static_cast<size_t>(size), 0)));
            return 0;
        });
        s.add_cmd(11, "CreateTransferMemoryStorage", [](Req& req, Rsp& rsp) -> uint32_t {
            return memory_storage(req, rsp, req.arg<int64_t>(8));
        });
        s.add_cmd(12, "CreateHandleStorage", [](Req& req, Rsp& rsp) -> uint32_t { return memory_storage(req, rsp, req.arg<int64_t>(0)); });
    });
}

// control.nacp ([fs] nacp, read at each call): two i64 maxima at `off`; a missing one, or one at or below zero, is 0xFFFFFFF.
void nacp_maxima(long off, int64_t* first, int64_t* second) {
    int64_t v[2] = {0, 0};
    const std::string nacp_file = config_get_path("fs", "nacp", "");
    const FilePtr in = nacp_file.empty() ? FilePtr() : open_file(nacp_file, "rb");
    if (in && std::fseek(in.get(), off, SEEK_SET) == 0) {
        if (std::fread(&v[0], 8, 1, in.get()) != 1) v[0] = 0;   // a short read counts as missing
        else if (std::fread(&v[1], 8, 1, in.get()) != 1) v[1] = 0;
    }
    *first = v[0] > 0 ? v[0] : 0xFFFFFFF;
    *second = v[1] > 0 ? v[1] : 0xFFFFFFF;
}

// The language the game should use: the console's, when the game supports it (or says nothing); else the first one it supports.
Language desired_language() {
    const TitleProfile& p = profile();
    const uint32_t mine = static_cast<uint32_t>(p.language);
    const uint32_t supported = p.supported_languages;
    if (supported == 0 || (mine < 32 && ((supported >> mine) & 1))) return p.language;
    for (uint32_t i = 0; i < kLanguages; ++i)
        if ((supported >> i) & 1) return static_cast<Language>(i);
    return p.language;
}

std::shared_ptr<ipc::Service> make_application_functions() {
    return make_iface("IApplicationFunctions", [](ipc::Service& s) {
        s.add_cmd(1, "PopLaunchParameter", [](Req& req, Rsp& rsp) -> uint32_t {
            const uint32_t kind = req.arg<uint32_t>(0);
            Guard g(am_lock());
            Proc& p = proc();
            if (kind == 1) {   // UserChannel: the newest first
                if (p.user_channel.empty()) return kNoData;
                BlobPtr b = p.user_channel.back();
                p.user_channel.pop_back();
                rsp.push_object(storage_object(std::move(b)));
                return 0;
            }
            if (kind == 2 && !p.preselected_popped) {   // PreselectedUser, once
                p.preselected_popped = true;
                auto b = std::make_shared<Blob>(0x88, 0);
                uint64_t hi = 0, lo = 0;
                accapi::user_uid(&hi, &lo);
                wr32(*b, 0, 0xC79497CA);
                wr32(*b, 4, 1);
                wr64(*b, 8, hi);
                wr64(*b, 16, lo);
                rsp.push_object(storage_object(std::move(b)));
                return 0;
            }
            return kNoData;
        });
        s.add_cmd(10, "CreateApplicationAndPushAndRequestToStart", [](Req& req, Rsp&) -> uint32_t {
            std::vector<Blob> params;
            if (const auto st = storage_passed(req)) {
                Guard g(am_lock());
                params.push_back(*st->bytes());
            }
            return request_to_start(req.arg<uint64_t>(0), "CreateApplicationAndPushAndRequestToStart", params);
        });
        s.add_cmd(12, "CreateApplicationAndRequestToStart", [](Req& req, Rsp&) -> uint32_t {
            return request_to_start(req.arg<uint64_t>(0), "CreateApplicationAndRequestToStart", std::vector<Blob>());
        });
        s.add_cmd(20, "EnsureSaveData", [](Req& req, Rsp& rsp) -> uint32_t {
            bool created = false;
            const uint32_t r = fsapi::ensure_account_save(req.arg<uint64_t>(0), req.arg<uint64_t>(8), &created);
            if (r) return r;
            rsp.push<uint64_t>(0);   // no more space needed
            return 0;
        });
        s.add_cmd(21, "GetDesiredLanguage", [](Req&, Rsp& rsp) -> uint32_t {
            rsp.push<uint64_t>(language_code_u64(desired_language()));
            return 0;
        });
        ignored(s, 22, "SetTerminateResult");
        s.add_cmd(23, "GetDisplayVersion", [](Req&, Rsp& rsp) -> uint32_t {
            uint8_t v[16] = {};
            const std::string& dv = profile().display_version;
            std::memcpy(v, dv.data(), std::min<size_t>(dv.size(), 15));
            rsp.out.insert(rsp.out.end(), v, v + sizeof v);
            return 0;
        });
        s.add_cmd(25, "ExtendSaveData", [](Req& req, Rsp& rsp) -> uint32_t {
            if (req.arg<uint8_t>(0) == 1) {   // Account
                Guard g(am_lock());
                Proc& p = proc();
                p.save_extended = true;
                p.save_normal = req.arg<int64_t>(24);
                p.save_journal = req.arg<int64_t>(32);
            }
            rsp.push<int64_t>(0);
            return 0;
        });
        s.add_cmd(26, "GetSaveDataSize", [](Req& req, Rsp& rsp) -> uint32_t {
            int64_t normal = 0, journal = 0;
            if (req.arg<uint8_t>(0) == 1) {
                bool extended;
                {
                    Guard g(am_lock());
                    const Proc& p = proc();
                    extended = p.save_extended;
                    normal = p.save_normal;
                    journal = p.save_journal;
                }
                if (!extended) {
                    const uint32_t r = fsapi::account_save_size(req.arg<uint64_t>(8), req.arg<uint64_t>(16), &normal, &journal);
                    if (r) return r;
                }
            }
            rsp.push<int64_t>(normal);
            rsp.push<int64_t>(journal);
            return 0;
        });
        s.add_cmd(27, "CreateCacheStorage", [](Req& req, Rsp& rsp) -> uint32_t {
            const uint32_t r = fsapi::create_cache_storage(req.arg<uint16_t>(0), req.arg<int64_t>(8), req.arg<int64_t>(16));
            if (r) return r;
            rsp.push<uint32_t>(1);   // NAND
            rsp.push<uint32_t>(0);
            rsp.push<int64_t>(0);
            return 0;
        });
        s.add_cmd(28, "GetSaveDataSizeMax", [](Req&, Rsp& rsp) -> uint32_t {
            int64_t normal = 0, journal = 0;
            nacp_maxima(0x3148, &normal, &journal);   // UserAccountSaveDataSizeMax, UserAccountSaveDataJournalSizeMax
            rsp.push<int64_t>(normal);
            rsp.push<int64_t>(journal);
            return 0;
        });
        ignored(s, 32, "BeginBlockingHomeButton");
        ignored(s, 33, "EndBlockingHomeButton");
        ignored(s, 34, "SelectApplicationLicense", 1);
        s.add_cmd(35, "GetDeviceSaveDataSizeMax", [](Req&, Rsp& rsp) -> uint32_t {
            int64_t normal = 0, journal = 0;
            nacp_maxima(0x3158, &normal, &journal);   // DeviceSaveDataSizeMax, DeviceSaveDataJournalSizeMax
            rsp.push<int64_t>(normal);
            rsp.push<int64_t>(journal);
            return 0;
        });
        process_event(s, 37, "GetLimitedApplicationLicenseUpgradableEvent", &Proc::never);
        s.add_cmd(40, "NotifyRunning", [](Req&, Rsp& rsp) -> uint32_t {
            rsp.push<uint8_t>(1);
            return 0;
        });
        s.add_cmd(50, "GetPseudoDeviceId", [](Req&, Rsp& rsp) -> uint32_t {
            static constexpr char kDeviceId[] = "NXRT-DEVICE-0001";   // 16 bytes on the wire, no NUL
            rsp.out.insert(rsp.out.end(), kDeviceId, kDeviceId + 16);
            return 0;
        });
        ignored(s, 60, "SetMediaPlaybackStateForApplication");
        ignored(s, 65, "IsGamePlayRecordingSupported", 1);
        ignored(s, 66, "InitializeGamePlayRecording");
        ignored(s, 67, "SetGamePlayRecordingState");
        ignored(s, 68, "RequestFlushGamePlayingMovieForDebug");
        ignored(s, 90, "EnableApplicationCrashReport");
        ignored(s, 100, "InitializeApplicationCopyrightFrameBuffer");
        ignored(s, 101, "SetApplicationCopyrightImage");
        ignored(s, 110, "QueryApplicationPlayStatistics", 4);
        s.add_cmd(120, "ExecuteProgram", [](Req& req, Rsp&) -> uint32_t {
            const uint32_t kind = req.arg<uint32_t>(0);   // 0 program index, 1 sub-application, 2 restart
            const uint64_t value = req.arg<uint64_t>(8);
            const int current = program_index_now();
            const int64_t next = kind == 2 ? current : static_cast<int64_t>(value <= 0xFF ? value : (value & 0xFF));
            std::vector<Blob> params;   // the user channel goes with it (and stays)
            {
                Guard g(am_lock());
                for (const BlobPtr& b : proc().user_channel) params.push_back(*b);
            }
            char what[160], extra[32];
            std::snprintf(what, sizeof what, "IApplicationFunctions 120 ExecuteProgram(kind %u, %llu)", kind,
                          static_cast<unsigned long long>(value));
            std::snprintf(extra, sizeof extra, "kind = %u\n", kind);
            return hand_off(next, current, extra, params, what);
        });
        s.add_cmd(121, "ClearUserChannel", [](Req&, Rsp&) -> uint32_t {
            Guard g(am_lock());
            proc().user_channel.clear();
            return 0;
        });
        s.add_cmd(122, "UnpopToUserChannel", [](Req& req, Rsp&) -> uint32_t {
            if (!object_passed(req)) return 0;
            const auto st = storage_passed(req);
            if (!st) return kNotAStorage;
            Guard g(am_lock());
            proc().user_channel.push_back(bytes_now(*st));
            return 0;
        });
        s.add_cmd(123, "GetPreviousProgramIndex", [](Req&, Rsp& rsp) -> uint32_t {
            rsp.push<int32_t>(static_cast<int32_t>(config_get_int("am", "previous_program_index", -1)));
            return 0;
        });
        ignored(s, 124, "EnableApplicationAllThreadDumpOnCrash");
        process_event(s, 130, "GetGpuErrorDetectedSystemEvent", &Proc::never);
        process_event(s, 140, "GetFriendInvitationStorageChannelEvent", &Proc::never);
        s.add_cmd(141, "TryPopFromFriendInvitationStorageChannel", [](Req&, Rsp&) -> uint32_t { return kNoData; });
        process_event(s, 150, "GetNotificationStorageChannelEvent", &Proc::never);
        s.add_cmd(151, "TryPopFromNotificationStorageChannel", [](Req&, Rsp&) -> uint32_t { return kNoData; });
        process_event(s, 160, "GetHealthWarningDisappearedSystemEvent", &Proc::never);
        ignored(s, 170, "SetHdcpAuthenticationActivated");
        process_event(s, 210, "GetUnknownEvent210", &Proc::never);
        ignored(s, 320, "Cmd320", 2);
        ignored(s, 321, "Cmd321", 1);
    });
}

// [am] graphics_scale, read once per process: 20 or 30, anything else 10.
uint32_t graphics_scale() {
    static const uint32_t v = [] {
        const int64_t s = config_get_int("am", "graphics_scale", 10);
        return s == 20 || s == 30 ? static_cast<uint32_t>(s) : 10u;
    }();
    return v;
}

std::shared_ptr<ipc::Service> make_graphics_scaling() {
    return make_iface("IGraphicsScalingParametersFunctions", [](ipc::Service& s) {
        s.add_cmd(10, "GetGraphicsScalingParameters", [](Req&, Rsp& rsp) -> uint32_t {
            rsp.push<uint32_t>(graphics_scale());
            rsp.push<uint32_t>(0);
            rsp.push<uint64_t>(0);
            return 0;
        });
        s.add_cmd(1050, "SetGraphicsScalingNotification (S2)", [](Req&, Rsp&) -> uint32_t { return 0; });
    });
}

std::shared_ptr<ipc::Service> make_application_proxy() {
    return make_iface("IApplicationProxy", [](ipc::Service& s) {
        const auto getter = [&s](uint32_t id, const char* name, std::shared_ptr<ipc::Service> (*make)()) {
            s.add_cmd(id, name, [make](Req&, Rsp& rsp) -> uint32_t {
                rsp.push_object(make());
                return 0;
            });
        };
        getter(0, "GetCommonStateGetter", make_common_state_getter);
        getter(1, "GetSelfController", make_self_controller);
        getter(2, "GetWindowController", make_window_controller);
        getter(3, "GetAudioController", make_audio_controller);
        getter(4, "GetDisplayController", [] { return empty_iface("IDisplayController"); });
        getter(10, "GetProcessWindingController", [] { return empty_iface("IProcessWindingController"); });
        getter(11, "GetLibraryAppletCreator", make_library_applet_creator);
        getter(20, "GetApplicationFunctions", make_application_functions);
        getter(40, "GetPerformanceFunctions",
               [] { return std::static_pointer_cast<ipc::Service>(std::make_shared<ipc::StubService>("IPerformanceFunctions")); });
        getter(60, "GetGraphicsScalingParametersFunctions", make_graphics_scaling);
        getter(1000, "GetDebugFunctions", [] { return empty_iface("IDebugFunctions"); });
    });
}

// appletOE / appletAE: each open answers a new proxy (the process handle passed along is the client's: not used, not closed).
std::shared_ptr<ipc::Service> make_proxy_service(const char* name, std::initializer_list<std::pair<uint32_t, const char*>> opens) {
    return make_iface(name, [opens](ipc::Service& s) {
        for (const auto& o : opens)
            s.add_cmd(o.first, o.second, [](Req&, Rsp& rsp) -> uint32_t {
                rsp.push_object(make_application_proxy());
                return 0;
            });
    });
}

}  // namespace

void register_am() {
    add("appletOE", [] { return make_proxy_service("appletOE", {{0, "OpenApplicationProxy"}, {10, "OpenApplicationProxy2"}}); });
    add("appletAE", [] { return make_proxy_service("appletAE", {{350, "OpenSystemApplicationProxy"}}); });
}

void am_reset() {
    Guard g(am_lock());
    proc_slot() = start_state();
}

namespace amctl {

void set_docked(bool docked) {
    {
        Guard g(am_lock());
        proc().docked = docked;
    }
    hid_mode_changed();   // the npads' style events first (hid reads the mode back: not under am's lock)
    Guard g(am_lock());
    Proc& p = proc();
    if (p.mode_notify) queue_message(p, kMsgOperationMode);
    if (p.perf_notify) queue_message(p, kMsgPerformanceMode);
}

void set_focus(bool in_focus) {
    Guard g(am_lock());
    Proc& p = proc();
    p.host_focus = in_focus;
    refresh_focus(p);
}

void applet_open(bool open) {
    Guard g(am_lock());
    Proc& p = proc();
    if (open) applet_opened(p);
    else applet_closed(p);
}

void set_exit_hook(void (*fn)(int code, const char* why)) {
    g_on_exit.store(fn);   // null: a plain exit
}

bool docked() {
    Guard g(am_lock());
    return proc().docked;
}

bool in_focus() {
    Guard g(am_lock());
    return proc().focus == kInFocus;
}

}  // namespace amctl

namespace swkbdui {

void enable_prompt(bool on) { g_swkbd_prompt.store(on); }
bool prompt_enabled() { return g_swkbd_prompt.load(); }

}  // namespace swkbdui

}  // namespace nxrt::services
