// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Ported from AnySwitch by Chaos192 (github.com/Chaos192/AnySwitch).
// Copyright (c) 2026 Chaos192
// The running game's network rooms, for the launcher ("Play online with friends"): a status file the runtime writes and a control file it
// watches, both in the run's folder under --as (run_report_dir), else beside pkp.log (where room-code.txt goes). Game-independent.
//
//   net-status.json   written only once a part is set (a game that plays nothing over the network gets no file and no thread): on every
//                     change, at most 4 times a second, and every 5 s with a fresh updated_ms; written to a temporary file and renamed over
//                     the old one, so a reader never sees half of it. {"version":1,"pid":N,"updated_ms":T,"code":"XXXX-XXXX-XXXX-XXXX",
//                     "server":"auto", <part>: {...}, ...}. Parts: "lan_room" (services/lan_room.cpp), "link_play" (services/ldn.cpp).
//   net-control.txt   lines "<n> <command>" the launcher appends while the game runs (n increasing). The runtime looks at the file every
//                     500 ms while a handler is registered; each line whose n is higher than any applied before is given to every handler
//                     once, in order. The lines already in the file when the watcher starts (an earlier run's) are never applied, so the
//                     launcher either removes the file before it starts the game or keeps numbering upward. Commands: "leave", "join".
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace nxrt::services::net_status {

// Sets one part (a JSON object's text, e.g. "{\"state\":\"in_room\"}"); empty removes it. Starts the writer on first use.
void set_part(const std::string& name, const std::string& json_object);
// The top-level "code" (the room code in use, display form; empty: none) and "server" ([net] server as given).
void set_common(const std::string& code, const std::string& server);

// A handler for the control file's commands (called on the watcher thread). The same owner replaces its handler; the watcher starts with
// the first one.
using Command = std::function<void(const std::string& command)>;
void on_command(const std::string& owner, Command handler);

// "text" as a JSON string literal (quotes included).
std::string json_str(const std::string& text);

std::string status_path();    // <pkp.log's folder>/net-status.json
std::string control_path();   // <pkp.log's folder>/net-control.txt

// Tests: writes the status now, and forgets every part, handler and applied command (the threads keep running). The next commands
// count from a file changed after this call.
void flush();
void reset_for_tests();

}  // namespace nxrt::services::net_status
