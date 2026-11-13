// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Time zone rules and calendar conversion for the time services (time.cpp uses it; timezone.cpp implements it).
// A rule is the 0x4000-byte nn::time::TimeZoneRule (the tzcode `struct state` as Horizon lays it out; Ryujinx.HLE Time/TimeZone/TimeZoneRule.cs
// is the layout reference). The guest's SDK may read it itself, so the layout is exact.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace nxrt::services::tz {

constexpr size_t kRuleSize = 0x4000;

struct CalendarTime {            // nn::time::CalendarTime (8 bytes): Switchbrew "Services/Time" CalendarTime
    int16_t year;
    int8_t month, day, hour, minute, second;   // month 1..12
    uint8_t pad;
};
struct CalendarAdditionalInfo {  // nn::time::CalendarAdditionalInfo (0x18 bytes)
    uint32_t day_of_week;        // 0 = Sunday
    uint32_t day_of_year;        // 0-based
    char tz_name[8];             // abbreviation ("UTC", "CET" ...), NUL padded
    int32_t is_dst;
    int32_t utc_offset;          // seconds east of UTC
};
static_assert(sizeof(CalendarTime) == 8);
static_assert(sizeof(CalendarAdditionalInfo) == 0x18);

// UTC rule: no transitions, one type ("UTC", offset 0).
void make_utc_rule(uint8_t* out /*kRuleSize bytes*/);
// A rule for the host's Windows time zone with explicit transitions for the years 1970..2037 (falls back to the UTC rule and returns
// false when the host zone cannot be read). *name_out receives a location name for GetDeviceLocationName ("Europe/Berlin" when the
// Windows zone maps to one, else the Windows zone id; "UTC" for the fallback).
bool make_host_rule(uint8_t* out, std::string* name_out);

// tzcode localtime for `posix` seconds under `rule`. False for an invalid rule.
bool to_calendar(const uint8_t* rule, int64_t posix, CalendarTime* cal, CalendarAdditionalInfo* info);
// tzcode time2: the posix times that map to `cal` in `rule` (0 = none, 1, or 2 for an ambiguous DST hour). out[] receives them.
int to_posix(const uint8_t* rule, const CalendarTime& cal, int64_t out[2]);

}  // namespace nxrt::services::tz
