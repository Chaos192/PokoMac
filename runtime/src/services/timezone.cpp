// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Time zone rules and calendar conversion (see timezone.h).
// Adapted from Ryubing (Ryujinx fork) src/Ryujinx.HLE/HOS/Services/Time/TimeZone/TimeZone.cs and TimeZoneRule.cs (MIT): the rule
// layout, the goback/goahead extension of ToCalendarTimeInternal and the time-type checks of ParseTimeZoneBinary. The behaviour
// they follow is tzcode localtime.c (public domain): localsub and time2.
// to_posix is written as a candidate search (time2 semantics), not Ryujinx's binary search, so an ambiguous wall time gives two results.
#include "timezone.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace nxrt::services::tz {
namespace {

constexpr int kMaxTimes = 1000;   // TZ_MAX_TIMES
constexpr int kMaxTypes = 128;    // TZ_MAX_TYPES
constexpr int kMaxChars = 512;    // 2 * (TZ_NAME_MAX + 1)
constexpr int64_t kSecondsPerDay = 86400;
constexpr int64_t kYearsPerRepeat = 400;
constexpr int64_t kAverageSecondsPerYear = 31556952;                    // 365.2425 days
constexpr int64_t kSecondsPerRepeat = kYearsPerRepeat * kAverageSecondsPerYear;   // == 146097 days

// One ttinfo: 16 bytes (int32 utoff, bool isdst, int32 desigidx, bool ttisstd, bool ttisut, 2 pad).
struct TtInfo {
    int32_t utoff;
    uint8_t isdst;
    uint8_t pad1[3];
    int32_t desigidx;
    uint8_t ttisstd;
    uint8_t ttisut;
    uint8_t pad2[2];
};

// The 0x4000-byte rule as C lays it out (tzcode struct state without the leap-second members). Natural alignment: the int64 array
// starts at 16 because goback/goahead end at 14.
struct RuleImage {
    int32_t timecnt;
    int32_t typecnt;
    int32_t charcnt;
    uint8_t goback;
    uint8_t goahead;
    uint8_t pad0[2];
    int64_t ats[kMaxTimes];
    uint8_t types[kMaxTimes];
    TtInfo ttis[kMaxTypes];
    char chars[kMaxChars];
    int32_t defaulttype;
    uint8_t reserved[kRuleSize - 11580];
};
static_assert(sizeof(TtInfo) == 16);
static_assert(offsetof(RuleImage, ats) == 16);
static_assert(offsetof(RuleImage, types) == 8016);
static_assert(offsetof(RuleImage, ttis) == 9016);
static_assert(offsetof(RuleImage, chars) == 11064);
static_assert(offsetof(RuleImage, defaulttype) == 11576);
static_assert(sizeof(RuleImage) == kRuleSize);

// ---- calendar arithmetic (proleptic Gregorian, days since 1970-01-01) ----
// days_from_civil / civil_from_days: Howard Hinnant's date algorithms (public domain).

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
int64_t floor_mod(int64_t a, int64_t b) { return a - floor_div(a, b) * b; }

int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2 ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t z, int64_t* y, int* m, int* d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    *y = yoe + era * 400 + (*m <= 2 ? 1 : 0);
}

int days_in_month(int64_t y, int m) {
    static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
    return (m == 2 && leap) ? 29 : kDays[m - 1];
}

// ---- rule access (tzcode localtime.c semantics) ----

bool valid_rule(const RuleImage& r) {
    if (r.timecnt < 0 || r.timecnt > kMaxTimes) return false;
    if (r.typecnt < 1 || r.typecnt > kMaxTypes) return false;
    if (r.charcnt < 0 || r.charcnt > kMaxChars) return false;
    if (r.defaulttype < 0 || r.defaulttype >= r.typecnt) return false;
    for (int i = 0; i < r.timecnt; ++i) {
        if (r.types[i] >= r.typecnt) return false;
        if (i > 0 && r.ats[i] <= r.ats[i - 1]) return false;
    }
    for (int j = 0; j < r.typecnt; ++j) {
        if (r.ttis[j].desigidx < 0 || r.ttis[j].desigidx >= r.charcnt) return false;
    }
    return true;
}

// goback/goahead extension: times outside the table are moved by whole 400-year repeats into the table.
bool needs_extension(const RuleImage& r, int64_t t) {
    if (r.timecnt <= 0) return false;
    return (r.goahead && t < r.ats[0]) || (r.goback && t > r.ats[r.timecnt - 1]);
}

// Index into ttis for t, without the extension.
int type_index(const RuleImage& r, int64_t t) {
    if (r.timecnt == 0 || t < r.ats[0]) return r.defaulttype;
    int low = 1;
    int high = r.timecnt;
    while (low < high) {
        const int mid = (low + high) >> 1;
        if (t < r.ats[mid]) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return r.types[low - 1];
}

// UTC offset in effect at t (seconds east). Same answer as the extended path for shifted times.
int32_t offset_at(const RuleImage& r, int64_t t, int depth = 0) {
    if (needs_extension(r, t) && depth < 2) {
        const bool before = t < r.ats[0];
        const int64_t edge = before ? r.ats[0] : r.ats[r.timecnt - 1];
        const int64_t seconds = (before ? edge - t : t - edge) - 1;
        const int64_t years = (seconds / kSecondsPerRepeat + 1) * kYearsPerRepeat;
        const int64_t shift = years * kAverageSecondsPerYear;
        return offset_at(r, before ? t + shift : t - shift, depth + 1);
    }
    return r.ttis[type_index(r, t)].utoff;
}

bool calendar_of(const RuleImage& r, int64_t t, CalendarTime* cal, CalendarAdditionalInfo* info, int depth = 0) {
    if (needs_extension(r, t)) {
        if (depth >= 2) return false;
        const bool before = t < r.ats[0];
        const int64_t edge = before ? r.ats[0] : r.ats[r.timecnt - 1];
        const int64_t seconds = (before ? edge - t : t - edge) - 1;
        const int64_t years = (seconds / kSecondsPerRepeat + 1) * kYearsPerRepeat;
        const int64_t shift = years * kAverageSecondsPerYear;
        if (!calendar_of(r, before ? t + shift : t - shift, cal, info, depth + 1)) return false;
        const int64_t y = cal->year + (before ? -years : years);
        if (y < INT16_MIN || y > INT16_MAX) return false;
        cal->year = static_cast<int16_t>(y);
        return true;
    }

    const int idx = type_index(r, t);
    const TtInfo& tt = r.ttis[idx];
    const int64_t local = t + tt.utoff;
    const int64_t days = floor_div(local, kSecondsPerDay);
    const int64_t secs = local - days * kSecondsPerDay;

    int64_t year = 0;
    int month = 0;
    int day = 0;
    civil_from_days(days, &year, &month, &day);
    if (year < INT16_MIN || year > INT16_MAX) return false;

    cal->year = static_cast<int16_t>(year);
    cal->month = static_cast<int8_t>(month);
    cal->day = static_cast<int8_t>(day);
    cal->hour = static_cast<int8_t>(secs / 3600);
    cal->minute = static_cast<int8_t>((secs % 3600) / 60);
    cal->second = static_cast<int8_t>(secs % 60);
    cal->pad = 0;

    info->day_of_week = static_cast<uint32_t>(floor_mod(days + 4, 7));   // 1970-01-01 was a Thursday; 0 = Sunday
    info->day_of_year = static_cast<uint32_t>(days - days_from_civil(year, 1, 1));
    std::memset(info->tz_name, 0, sizeof info->tz_name);
    for (int i = 0; i < 8 && tt.desigidx + i < r.charcnt; ++i) {
        const char c = r.chars[tt.desigidx + i];
        if (c == '\0') break;
        info->tz_name[i] = c;
    }
    info->is_dst = tt.isdst ? 1 : 0;
    info->utc_offset = tt.utoff;
    return true;
}

// Posix seconds of a wall time with the given offset, if that offset is the one in effect there (tzcode time2 check).
// Returns the number of results (0..2) written to out, earliest first.
int to_posix_impl(const RuleImage& r, const CalendarTime& cal, int64_t out[2]) {
    if (cal.month < 1 || cal.month > 12 || cal.day < 1 || cal.day > 31) return 0;
    // Fields may be out of range; the arithmetic normalises them the way mktime does.
    const int64_t days = days_from_civil(cal.year, cal.month, 1) + (cal.day - 1);
    const int64_t naive = days * kSecondsPerDay + cal.hour * 3600 + cal.minute * 60 + cal.second;

    int64_t found[kMaxTypes];
    int n = 0;
    for (int j = 0; j < r.typecnt; ++j) {
        const int32_t utoff = r.ttis[j].utoff;
        bool seen = false;
        for (int k = 0; k < j && !seen; ++k) seen = r.ttis[k].utoff == utoff;
        if (seen) continue;
        const int64_t t = naive - utoff;
        if (offset_at(r, t) == utoff) found[n++] = t;
    }
    std::sort(found, found + n);
    int count = std::min(n, 2);
    for (int i = 0; i < count; ++i) out[i] = found[i];
    return count;
}

// ---- host rule (Windows) ----

struct AbbrMap {
    const char* windows_id;
    const char* iana;
    const char* std_abbr;
    const char* dst_abbr;
};

// Windows time zone id -> IANA name and the abbreviations tzdata uses for it today. Zones not listed use the Windows id and
// numeric abbreviations ("+03", "+0530").
constexpr AbbrMap kZoneMap[] = {
    {"UTC", "UTC", "UTC", "UTC"},
    {"GMT Standard Time", "Europe/London", "GMT", "BST"},
    {"Greenwich Standard Time", "Africa/Abidjan", "GMT", "GMT"},
    {"W. Europe Standard Time", "Europe/Berlin", "CET", "CEST"},
    {"Central Europe Standard Time", "Europe/Budapest", "CET", "CEST"},
    {"Central European Standard Time", "Europe/Warsaw", "CET", "CEST"},
    {"Romance Standard Time", "Europe/Paris", "CET", "CEST"},
    {"W. Central Africa Standard Time", "Africa/Lagos", "WAT", "WAT"},
    {"GTB Standard Time", "Europe/Bucharest", "EET", "EEST"},
    {"FLE Standard Time", "Europe/Helsinki", "EET", "EEST"},
    {"E. Europe Standard Time", "Europe/Chisinau", "EET", "EEST"},
    {"Egypt Standard Time", "Africa/Cairo", "EET", "EEST"},
    {"Russian Standard Time", "Europe/Moscow", "MSK", "MSK"},
    {"Turkey Standard Time", "Europe/Istanbul", "+03", "+03"},
    {"Israel Standard Time", "Asia/Jerusalem", "IST", "IDT"},
    {"Arabian Standard Time", "Asia/Dubai", "+04", "+04"},
    {"India Standard Time", "Asia/Kolkata", "IST", "IST"},
    {"China Standard Time", "Asia/Shanghai", "CST", "CST"},
    {"Tokyo Standard Time", "Asia/Tokyo", "JST", "JST"},
    {"Korea Standard Time", "Asia/Seoul", "KST", "KST"},
    {"Singapore Standard Time", "Asia/Singapore", "+08", "+08"},
    {"W. Australia Standard Time", "Australia/Perth", "AWST", "AWST"},
    {"AUS Eastern Standard Time", "Australia/Sydney", "AEST", "AEDT"},
    {"New Zealand Standard Time", "Pacific/Auckland", "NZST", "NZDT"},
    {"Hawaiian Standard Time", "Pacific/Honolulu", "HST", "HST"},
    {"Alaskan Standard Time", "America/Anchorage", "AKST", "AKDT"},
    {"Pacific Standard Time", "America/Los_Angeles", "PST", "PDT"},
    {"US Mountain Standard Time", "America/Phoenix", "MST", "MST"},
    {"Mountain Standard Time", "America/Denver", "MST", "MDT"},
    {"Central Standard Time", "America/Chicago", "CST", "CDT"},
    {"Mexico Standard Time", "America/Mexico_City", "CST", "CST"},
    {"Eastern Standard Time", "America/New_York", "EST", "EDT"},
    {"Atlantic Standard Time", "America/Halifax", "AST", "ADT"},
    {"Newfoundland Standard Time", "America/St_Johns", "NST", "NDT"},
    {"E. South America Standard Time", "America/Sao_Paulo", "-03", "-03"},
    {"SA Pacific Standard Time", "America/Bogota", "-05", "-05"},
};

const AbbrMap* find_zone(const std::string& id) {
    for (const AbbrMap& m : kZoneMap) {
        if (id == m.windows_id) return &m;
    }
    return nullptr;
}

std::string numeric_abbr(int32_t off) {
    const int32_t a = off < 0 ? -off : off;
    char b[16];
    if (a % 3600 == 0) {
        std::snprintf(b, sizeof b, "%c%02d", off < 0 ? '-' : '+', a / 3600);
    } else {
        std::snprintf(b, sizeof b, "%c%02d%02d", off < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
    }
    return b;
}

struct HostType {
    int32_t utoff;
    bool isdst;
    std::string abbr;
};

struct HostTransition {
    int64_t at;
    int before;   // type index in effect before `at`
    int after;    // type index from `at` on
};

int find_or_add(std::vector<HostType>& types, int32_t utoff, bool isdst, const std::string& abbr) {
    for (size_t i = 0; i < types.size(); ++i) {
        if (types[i].utoff == utoff && types[i].isdst == isdst && types[i].abbr == abbr) return static_cast<int>(i);
    }
    types.push_back({utoff, isdst, abbr});
    return static_cast<int>(types.size() - 1);
}

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Local wall time of a SYSTEMTIME as seconds since the epoch in that wall clock. A wYear of 0 is a recurring rule:
// wDay is the n-th wDayOfWeek of the month, 5 meaning the last.
int64_t resolve_systemtime(const SYSTEMTIME& st, int year) {
    const int64_t y = st.wYear != 0 ? st.wYear : year;
    const int64_t first = days_from_civil(y, st.wMonth, 1);
    int64_t day_of_month;
    if (st.wYear != 0) {
        day_of_month = st.wDay;
    } else {
        const int64_t first_wd = floor_mod(first + 4, 7);
        day_of_month = 1 + floor_mod(st.wDayOfWeek - first_wd, 7) + 7 * (st.wDay - 1);
        while (day_of_month > days_in_month(y, st.wMonth)) day_of_month -= 7;
    }
    return (first + day_of_month - 1) * kSecondsPerDay + st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
}

// Builds the rule from the host's dynamic zone: for each year 1970..2037 the DST transitions of GetTimeZoneInformationForYear.
// Returns false when anything cannot be read or does not fit a rule.
bool build_host_rule(RuleImage& img, std::string& name) {
    DYNAMIC_TIME_ZONE_INFORMATION dtzi{};
    if (GetDynamicTimeZoneInformation(&dtzi) == TIME_ZONE_ID_INVALID) return false;
    const std::string key = wide_to_utf8(dtzi.TimeZoneKeyName);
    if (key.empty()) return false;
    const AbbrMap* map = find_zone(key);
    name = map ? map->iana : key;

    auto abbr_for = [&](bool isdst, int32_t off) -> std::string {
        if (!map) return numeric_abbr(off);
        return isdst ? map->dst_abbr : map->std_abbr;
    };

    std::vector<HostType> types;
    std::vector<HostTransition> trans;
    int std_1970 = -1;
    for (int year = 1970; year <= 2037; ++year) {
        TIME_ZONE_INFORMATION tzi{};
        if (!GetTimeZoneInformationForYear(static_cast<USHORT>(year), &dtzi, &tzi)) return false;
        const int32_t std_off = -(tzi.Bias + tzi.StandardBias) * 60;
        const int32_t dst_off = -(tzi.Bias + tzi.DaylightBias) * 60;
        const int std_ty = find_or_add(types, std_off, false, abbr_for(false, std_off));
        if (year == 1970) std_1970 = std_ty;
        if (tzi.DaylightDate.wMonth == 0 || tzi.StandardDate.wMonth == 0) continue;   // no DST this year
        const int dst_ty = find_or_add(types, dst_off, true, abbr_for(true, dst_off));
        // DaylightDate is given in standard wall time, StandardDate in daylight wall time.
        const int64_t start = resolve_systemtime(tzi.DaylightDate, year) - std_off;
        const int64_t end = resolve_systemtime(tzi.StandardDate, year) - dst_off;
        trans.push_back({start, std_ty, dst_ty});
        trans.push_back({end, dst_ty, std_ty});
    }
    if (std_1970 < 0) return false;

    std::sort(trans.begin(), trans.end(), [](const HostTransition& a, const HostTransition& b) { return a.at < b.at; });

    // The type in effect before the first transition is the table's default; it must be type 0.
    const int default_ty = trans.empty() ? std_1970 : trans[0].before;
    if (types.size() > static_cast<size_t>(kMaxTypes)) return false;
    auto remap = [&](int i) {
        if (i == 0) return default_ty;
        if (i == default_ty) return 0;
        return i;
    };
    std::swap(types[0], types[default_ty]);

    // Drop transitions that do not change the type (keeps the table minimal and strictly increasing).
    std::vector<HostTransition> table;
    int state = remap(default_ty);
    for (const HostTransition& t : trans) {
        if (!table.empty() && t.at == table.back().at) return false;
        const int after = remap(t.after);
        if (after == state) continue;
        table.push_back({t.at, state, after});
        state = after;
    }
    if (static_cast<int>(table.size()) > kMaxTimes) return false;

    // Abbreviations into chars, NUL separated, each name stored once.
    std::string chars;
    std::vector<int32_t> desig(types.size(), 0);
    for (size_t i = 0; i < types.size(); ++i) {
        const std::string& a = types[i].abbr;
        const size_t pos = chars.find(a + '\0');
        if (pos != std::string::npos && (pos == 0 || chars[pos - 1] == '\0')) {
            desig[i] = static_cast<int32_t>(pos);
        } else {
            desig[i] = static_cast<int32_t>(chars.size());
            chars += a;
            chars.push_back('\0');
        }
    }
    if (chars.size() > static_cast<size_t>(kMaxChars)) return false;

    img.timecnt = static_cast<int32_t>(table.size());
    img.typecnt = static_cast<int32_t>(types.size());
    img.charcnt = static_cast<int32_t>(chars.size());
    img.goback = 0;
    img.goahead = 0;
    for (int i = 0; i < img.timecnt; ++i) {
        img.ats[i] = table[static_cast<size_t>(i)].at;
        img.types[i] = static_cast<uint8_t>(table[static_cast<size_t>(i)].after);
    }
    for (size_t j = 0; j < types.size(); ++j) {
        TtInfo& tt = img.ttis[j];
        tt.utoff = types[j].utoff;
        tt.isdst = types[j].isdst ? 1 : 0;
        tt.desigidx = desig[j];
        tt.ttisstd = 0;
        tt.ttisut = 0;
    }
    std::memcpy(img.chars, chars.data(), chars.size());
    img.defaulttype = 0;
    return valid_rule(img);
}
#endif

}  // namespace

void make_utc_rule(uint8_t* out) {
    RuleImage img{};
    img.timecnt = 0;
    img.typecnt = 1;
    img.charcnt = 4;
    std::memcpy(img.chars, "UTC", 4);   // "UTC" and its NUL
    img.ttis[0].utoff = 0;
    img.ttis[0].isdst = 0;
    img.ttis[0].desigidx = 0;
    img.defaulttype = 0;
    std::memcpy(out, &img, kRuleSize);
}

bool make_host_rule(uint8_t* out, std::string* name_out) {
#ifdef _WIN32
    auto img = std::make_unique<RuleImage>();
    std::string name;
    if (build_host_rule(*img, name)) {
        std::memcpy(out, img.get(), kRuleSize);
        if (name_out) *name_out = name;
        return true;
    }
#endif
    make_utc_rule(out);
    if (name_out) *name_out = "UTC";
    return false;
}

bool to_calendar(const uint8_t* rule, int64_t posix, CalendarTime* cal, CalendarAdditionalInfo* info) {
    if (!rule || !cal || !info) return false;
    auto img = std::make_unique<RuleImage>();
    std::memcpy(img.get(), rule, kRuleSize);
    if (!valid_rule(*img)) return false;
    return calendar_of(*img, posix, cal, info);
}

int to_posix(const uint8_t* rule, const CalendarTime& cal, int64_t out[2]) {
    if (!rule || !out) return 0;
    auto img = std::make_unique<RuleImage>();
    std::memcpy(img.get(), rule, kRuleSize);
    if (!valid_rule(*img)) return 0;
    return to_posix_impl(*img, cal, out);
}

}  // namespace nxrt::services::tz
