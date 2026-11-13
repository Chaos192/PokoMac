// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// Offline HTML pages in a web view (nxnative services.md "Offline HTML pages in a web view"). A game shows pages it ships in its
// HtmlDocument content (manuals, menus: Super Mario 3D All-Stars' pause menu, Octopath Traveler 0) through the offline web applet
// (am.cpp web_applet_start). The page is found in the title's html folder (the HtmlDocument content's files, as html/html-document or
// html<n>/html-document) and shown by a web view of the platform over the game picture (a Host: WKWebView on the Mac, WebView2 on Windows),
// with the Switch browser's JavaScript extension (window.nx: content messages both ways, nx.endApplet). Without a host (a headless run) or
// without the page, the applet answers with the stand-in: one empty content message, ended when it appears.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nxrt::webview {

// Where a page's files are read: the title's extracted html-document folder, or its HtmlDocument content read in place from the player's
// package (html/romfs.nxsrc, tools/intake/nca_intake.py --romfs-in-place). `rel`: a path inside the html-document folder, '/' separators.
class Files {
public:
    virtual ~Files() = default;
    virtual bool read(const std::string& rel, std::vector<uint8_t>* data) const = 0;
};

// The page the game asks for, resolved to files on the host.
struct Page {
    std::string root;          // the html-document folder that holds it (for the log; its files are read through `files`)
    std::shared_ptr<const Files> files;   // its files (find_page); null: read from the folder `root`
    std::string path;          // its file, relative to root, with '/' separators ("launcher_pause_menu.htdocs/index.html")
    std::string query;         // what followed '?' or '#' in the document path ("" when nothing did)
    std::string callback_url;  // the page ends when it goes there (exit reason 3, the URL as the last URL); default http://localhost/
    bool hidden = false;       // a session started initially hidden: shown at Appear
    uint32_t repeat_delay = 0, repeat_pulse = 0;   // KeyRepeatFrame (0x1c / 0x1d): the arrow keys' repeat, in frames (0: 30, then every 6)
    // An online page (the web applet, ShowWebPage: [web] online_pages): `url` is its address, nothing is served from the title, every
    // address under Nintendo's domains is refused (nintendo_url). `error`: no page, the applet's error dialog with this text instead (an
    // online page refused: the console's "cannot connect" error, which ends the applet with exit reason 7).
    bool online = false;
    std::string url;
    std::string error;
    bool muted = false;        // the page's sounds are silenced (the game's audio goes to no speaker: NXRT_AUDIO_SINK / [audio] sink = null)
};

// Exit reasons of the web applets' return value (WebCommonReturnValue / OfflineHtmlPageReturnValue; libnx applets/web.h WebExitReason).
constexpr uint32_t kExitEndButton = 0, kExitBack = 1, kExitRequested = 2, kExitCallbackUrl = 3, kExitWindowClosed = 4, kExitErrorDialog = 7;

// [web] view (default off until the batched pass proves it): the platform web views show the title's offline pages and the applet answers
// as the console's does (session acknowledgements, the last URL's size with its NUL, the TLV return of the web applet on 8.0.0+, online
// pages refused with the error dialog). Off: the stand-in of 704e2a48 answers every page.
bool view_enabled();
// [web] online_pages (default off, the player's choice): an online page (the web applet) opens in the web view, unless its address is
// Nintendo's. Off: every online page is refused with the console's offline error.
bool online_enabled();
// `url`'s host is Nintendo's (net/redirect.h nintendo_host, or any host name with "nintendo" in it), or `url` has no host this can check
// (no scheme://host, a '%' placeholder such as the SDK's "%.cdn.nintendo.net"): it is never opened, nor is anything the page loads from it.
bool nintendo_url(const std::string& url);
// The host of `url` in lower case ("" when it has none: about:, data:, a relative URL).
std::string url_host(const std::string& url);

// What a host reports, from any thread: a content message the page sent (window.nx.sendMessage), the page's end (reason and last URL),
// or that it could not show the page (the applet then answers with the stand-in).
struct Events {
    std::function<void(const std::string& text)> message;
    std::function<void(uint32_t reason, const std::string& last_url)> ended;
    std::function<void(const std::string& why)> failed;
};

// A platform's web view. One page at a time, as the applet. open() starts loading it (hidden or shown, from the Page); show() makes a
// hidden page appear; send() hands the page a content message from the game; close() ends it at the game's request (no `ended` event).
class Host {
public:
    virtual ~Host() = default;
    virtual bool open(const Page& page, Events events) = 0;
    virtual void show() = 0;
    virtual void send(const std::string& text) = 0;
    virtual void close() = 0;
    // The controller while the page holds the input (every connected pad ORed: NpadButton bits, the left stick -32767..32767), from the hid
    // sampler (about every 5 ms); the host turns it into the page's key events and gamepad. Default: ignored.
    virtual void pad(uint64_t buttons, int32_t lx, int32_t ly) {
        (void)buttons;
        (void)lx;
        (void)ly;
    }
};

// The platform's host, registered when the game window exists (nullptr: none, the stand-in answers).
void set_host(Host* h);
Host* host();

// While a page is shown the game reads no input (as an application out of focus); the host feeds the controller to the page instead.
void set_input_held(bool held);
bool input_held();
// hid's read of the controllers while the input is held: hands it to the host (Host::pad).
void feed_pad(uint64_t buttons, int32_t lx, int32_t ly);

// The title's html folder (its HtmlDocument content: nca_intake.py writes it beside the RomFS): [web] html (a path of the title's) when set,
// else the folder "html" beside the RomFS folder ([fs] romfs: intake/romfs -> intake/html, as nxnative's DM_HTML default), else the game
// folder's html; the first that exists ("" when none does).
std::string html_folder();

// The page for `document_path` (the argument's DocumentPath: a path below the html-document folder, maybe with a query or fragment) in
// `html_root` (the title's html folder): its html-document folder, then html0/html-document, html1/html-document, ... Each of those html
// folders may instead hold romfs.nxsrc, the HtmlDocument content read in place from the package (its RomFS holds html-document/). A path
// that climbs out of its folder ("../") is looked up inside it. False when no folder holds the file.
bool find_page(const std::string& html_root, const std::string& document_path, Page* out);

// ---- What the platform hosts share (platform/mac/gfx/webview_mac.mm; Windows' WebView2 host): the page's addresses, its files, its script and
// the controller. Plain C++, tested on every platform (test_applet_output.cpp).

// The scheme both platform views serve the pages through (a scheme of their own, so the page's XMLHttpRequests and fetches of its own
// files work as on the console) and the page's origin under it.
constexpr const char* kScheme = "nxweb";
constexpr const char* kOrigin = "nxweb://doc/";

// The page's address: `origin` (the host's own scheme or served host, ending in '/': kOrigin) + its path, percent-encoded where a URL needs
// it, + its query. An online page: its URL. The error dialog: origin + "__nx/error.html".
std::string page_url(const std::string& origin, const Page& page);

// A navigation of the page to `url` (also a server redirect). An offline page: one to localhost (any port, http or https: the offline applet
// compares the domain with "localhost", libnx webConfigSetCallbackUrl) or under its callback URL ends the page (exit reason 3, `url` the
// last URL); within its origin (or about:, data:, blob:) it goes on; any other is refused (an offline page stays in its document). An online
// page: under its callback URL it ends; http(s) addresses go on unless they are Nintendo's (nintendo_url); anything else is refused.
enum class Navigation { Go, End, Refuse };
Navigation navigation(const std::string& origin, const Page& page, const std::string& url);
// A request the page makes for `url` (an image, a script, an XMLHttpRequest, a WebSocket) may leave the host: never to Nintendo's
// addresses, and from an offline page never off its origin (about:, data:, blob: stay allowed).
bool request_allowed(const std::string& origin, const Page& page, const std::string& url);

// What a request of the page for `url` gets (its query and fragment ignored, percent-escapes decoded): the file from the page's folder; when a
// relative URL climbed out of the document's first folder (a stylesheet in menu.htdocs/css/ naming ../../img/), the file inside that folder,
// as the Switch finds it; at "__nx/ext.ttf" the shared extension font (the button icons, pl:u font 5, as the player's controller shows them).
// False: not served (404).
bool page_file(const std::string& origin, const Page& page, const std::string& url, std::vector<uint8_t>* data, std::string* mime);

// The script each document of the page gets before its own: the Switch browser's JavaScript extension and controller. window.nx
// (addEventListener / removeEventListener "message", sendMessage, endApplet, footer.setAssign / unsetAssign / setFixed, setCursorScrollSpeed;
// any other member a no-op, logged once), navigator.getGamepads in the standard mapping, the D-pad and the left stick as the arrow keys with key
// repeat (`repeat_delay` frames, then every `repeat_pulse`; 0: 30 and 6), A as keyCode 32 that clicks the focused link or button when the
// page does not take it, B back, or from the first page its end (exit reason 1), unless the page took B over; X, Y, + and - the page's own
// footer functions; a click focuses what it lands on and presses A there; the page's :focus rules kept on the focused element whatever the
// window's focus. `post` is a JavaScript function expression that hands the host one string (parse_post). `x_ends`: X ends the page with
// exit reason 0 unless the page took X over (the web applet's X button, libnx WebExitReason_ExitButton). `muted`: the page's sounds still
// play and end (pages that wait for them) but reach no speaker.
std::string page_script(const std::string& post, uint32_t repeat_delay, uint32_t repeat_pulse, bool x_ends = false, bool muted = false);
// The script for `page` (its repeat frames, X on an online page, muted).
std::string page_script(const std::string& post, const Page& page);

// A string the page script posted.
struct Post {
    enum Kind { None, Message, End, Log } kind = None;
    std::string text;      // Message: the content message for the game; Log: a line for the log (the page's console, its errors)
    uint32_t reason = 0;   // End: the exit reason
};
Post parse_post(const std::string& s);

// The applet's output storage at its end. The TLV storage (0x2000 bytes: header {u16 count, u16 0, u32 shim kind}, then entries {u16 type,
// u16 size, u32 0} + data packed one after another, as the SDK walks them: ExitReason 0x1 u32, LastUrl 0x2 with its NUL, LastUrlSize 0x3
// u64) for the Share shim on 3.0.0+ and the Web shim on 8.0.0+ (switchbrew Internet_Browser "Output TLVs"); else the WebCommonReturnValue
// (0x1010 bytes: u32 exit reason, the last URL at +8, its size at +0x1008). A last URL's size counts its NUL (libnx WebCommonReturnValue);
// no last URL: size 0. `library_version`: the common arguments' (+8).
constexpr uint32_t kShimShop = 1, kShimLogin = 2, kShimOffline = 3, kShimShare = 4, kShimWeb = 5;
std::vector<uint8_t> return_storage(uint32_t shim_kind, uint32_t library_version, uint32_t reason, const std::string& last_url);

// Script to run in the page: hand it the game's content message; the page appeared (its controller starts afresh).
std::string deliver_call(const std::string& message);
std::string show_call();
// `s` as a JavaScript string literal (invalid UTF-8 read as U+FFFD).
std::string js_string(const std::string& s);

// The controller for the page, from Host::pad: buttons held as the page appears count only once released, a resting stick (within 1/8 of
// its range) is 0, only the buttons the page uses are passed (A B X Y, the stick presses, L R ZL ZR, + -, the D-pad). `call` gets the script
// to run in the page when something changed (false: nothing to send).
class PadRelay {
public:
    void start(uint64_t held_buttons);
    bool next(uint64_t buttons, int32_t lx, int32_t ly, std::string* call);

private:
    uint64_t mask_ = 0, last_ = 0;
    int32_t lx_ = 0, ly_ = 0;
};

}  // namespace nxrt::webview
