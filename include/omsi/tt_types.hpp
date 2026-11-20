#pragma once

// OMSI 2 Timetable / Tracks & Trips / StationLinks (TTimeTableMan — Unit121).
// Layouts re-audited against the decompilation (Unit121: LoadTrack/SaveTrack @0x00728930/0x0072da58,
// LoadTrip/SaveTrip @0x00729154/0x0072e194, LoadStnLinkList/SaveStnLinkList @0x0072a4c0/0x0072f2b8,
// LoadBusstopList/SaveBusstopList @0x0072b2f0/0x00730000, LoadLine/SaveLine @0x0072bc3c/0x00730744).
// See docs/REVERSE_ENGINEERING_Unit121.md for the offset map and the list of fixes.
// TTData lives under maps/<MapName>/TTData/

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace omsi::tt {

// Map file version at which the extended TTData layouts appear (gvar_00859D94->version > 10).
// Below this: [track_entry] has only 2 lines, [station] has only 2 lines, and
// Busstops.cfg / StnLinks.cfg are not read at all.
inline constexpr std::int32_t kTtExtendedVersion = 11;

// ─── Track (.ttr) ───────────────────────────────────────────────────────────
// TTTTrackEntry, record size 0x28. File order (version >= 11):
//   id(+0) path_index(+4) tile_index(+8) internal_path_no(+0xC) length(+0x10)
//   N, then N ints (dyn array at +0x1C)
// +0x14 (start_offset) is a runtime cumulative length written by CalcTrackLaenge, never saved.
// tile_index / internal_path_no are a runtime cache (-1 until RefreshTrackEntry resolves them).
struct TrackEntry {
    std::int32_t id = 0;                 // path owner id (spline / object instance)
    std::int32_t path_index = 0;         // [path] index inside .sli / .sco
    std::int32_t tile_index = -1;        // +8  cache: index in loaded-tile list
    std::int32_t internal_path_no = -1;  // +0xC cache: index in that tile's path table
    float length = 0.f;                  // +0x10 path length (metres), refreshed by calc_track_length
    std::vector<std::int32_t> extra;     // +0x1C int array, round-tripped verbatim
    float start_offset = 0.f;            // +0x14 runtime only
};

struct Track {
    std::string name; // file stem (e.g. "1_Klisa-Liman1")
    std::filesystem::path path; // full .ttr path
    std::vector<TrackEntry> entries;
    float total_length = 0.f; // +0xC runtime (sum of entry lengths)
};

// ─── Trip (.ttp) ────────────────────────────────────────────────────────────
// [trip] 3 lines: track name, terminus, line.  Track is resolved by name CASE-INSENSITIVELY;
// an unresolved name gives track index -1 == "type 2" semantics.
// [trainreverse] flag.
// Type 1: [station] id, track_entry_index, [version>=11: name, int(+8), f(+0x34), f(+0x3C), f(+0x38), f(+0x20)]
// Type 2: [station_typ2] id
struct TripStation {
    bool typ2 = false; // true = [station_typ2]
    std::int32_t object_id = 0;         // +0xC busstop id
    std::int32_t track_entry_index = 0; // +0x2C (-1 for type 2)
    std::string name;                   // +0
    std::int32_t tile_index = 0;        // +8
    float dist = 0.f;                   // +0x34
    float f1 = 0.f;                     // +0x3C
    float f2 = 0.f;                     // +0x38
    float f3 = 0.f;                     // +0x20
};

// TTTProfile (0x14) = name(+0), duration(+4), per-station array(+8) of {arr_time, dep_time, flags}.
// The per-station array always has exactly one slot per station of the trip (kept in sync by
// AddTripBusstop / DelTripBusstop). Times are kept in FILE units (minutes); OMSI scales them on
// load (x _DAT_0072a400) and unscales on save (/ _DAT_0072f200), so this round-trips exactly.
struct TripProfile {
    std::string name;    // e.g. "standard"
    float minutes = 0.f; // duration
    std::vector<std::pair<std::int32_t, float>> man_dep;  // [profile_man_dep_time]
    std::vector<std::pair<std::int32_t, float>> man_arr;  // [profile_man_arr_time]
    // [profile_otherstopping] station_index, BYTE value 0..255 (0 == not written)
    std::vector<std::pair<std::int32_t, std::int32_t>> other_stopping;
};

struct Trip {
    std::string name; // file stem
    std::filesystem::path path;
    std::string track_name; // [trip] line 0
    std::string terminus;   // [trip] line 1
    std::string line;       // [trip] line 2
    bool train_reversed = false;
    std::vector<TripStation> stations;
    std::vector<TripProfile> profiles;
    // Resolved track index (case-insensitive name lookup), set by TimeTableMan::refresh_trip.
    // kUnresolved => fall back to name-based typing.
    static constexpr std::int32_t kUnresolved = -2;
    std::int32_t track_index = kUnresolved;

    [[nodiscard]] bool is_type2() const {
        return track_index == kUnresolved ? track_name.empty() : track_index < 0;
    }
    [[nodiscard]] bool is_type1() const { return !is_type2(); }
};

// ─── Busstops.cfg (version >= 11 only) ──────────────────────────────────────
// [busstop] name, int(+8), id(+0xC), float, N, N strings(+0x18), int(+0x10), string(+0)
struct BusstopListEntry {
    std::string name;
    std::int32_t tile_index = 0;
    std::int32_t object_id = 0;
    float offset = 0.f;
    std::vector<std::string> aliases; // +0x18 string array, N lines
    std::int32_t extra_int = 0;       // +0x10
    std::string tail;                 // +0   trailing string line
};

// ─── StnLinks.cfg (version >= 11 only) ──────────────────────────────────────
// [StnLink] float(+4) from(+8) to(+0xC) f(+0x10) f(+0x14) f(+0x38) f(+0x3C) i(+0x30) i(+0x34)
// [StnLink_entry] id path tile(+8) length(+0x10) i(+0x20) N ints(+0x1C) M strings(+0x24)
struct StnLinkEntry {
    std::int32_t id = 0;
    std::int32_t path_index = 0;
    std::int32_t tile_index = -1; // +8 cache
    float length = 0.f;           // +0x10
    std::int32_t i20 = 0;         // +0x20
    std::vector<std::int32_t> ints;        // +0x1C
    std::vector<std::string> strings;      // +0x24
    float rel_offset = 0.f;                // +0x14 runtime only
};

struct StnLink {
    float length = 0.f;       // +4
    std::int32_t from_id = 0; // +8  busstop object id
    std::int32_t to_id = 0;   // +0xC
    float p10 = 0.f, p14 = 0.f, p38 = 0.f, p3c = 0.f;
    std::int32_t i30 = 0, i34 = 0;
    std::vector<StnLinkEntry> entries;
};

// ─── Line (.ttl) ────────────────────────────────────────────────────────────
constexpr std::int32_t kTourAllDays = 0x3FF; // empty 3rd line of [newtour] => all 10 day flags set

struct TourTrip {
    std::string trip_name;     // .ttp stem
    std::int32_t profile_index = 0; // +8
    float departure_minutes = 0.f;  // +0xC (file units)
};

struct Tour {
    // [newtour]: number, depot/name, day-validity bit mask (NOT an AI group):
    //   bit0..bit7 -> +0x20..+0x27, bit8(0x100) -> +0x29, bit9(0x200) -> +0x28.
    std::string number;
    std::string depot_name;
    std::int32_t day_mask = kTourAllDays;
    std::vector<TourTrip> trips;
};

struct Line {
    std::string name; // file stem
    std::filesystem::path path;
    bool user_allowed = false; // OMSI default is false; [userallowed] sets true
    std::int32_t priority = 1; // OMSI default is 1 (byte); [priority] overrides (0..255)
    std::vector<Tour> tours;
};

// ─── Whole TTData folder ────────────────────────────────────────────────────
struct TimeTableData {
    std::filesystem::path root; // .../TTData
    std::int32_t map_version = kTtExtendedVersion; // map file version (gvar+0x20)
    std::vector<BusstopListEntry> busstops;
    std::vector<StnLink> stn_links;
    std::vector<Track> tracks;
    std::vector<Trip> trips;
    std::vector<Line> lines;

    // Validation messages (Find Errors)
    std::vector<std::string> errors;
};

// Editor interaction modes (mirror TForm_main radio buttons)
enum class TtEditMode : std::uint8_t {
    None = 0,
    AddTrackEntry,    // Tracks & Trips: Add Trackentry
    AddStation,       // Tracks & Trips: Add Station
    ShowStationTT,    // Show Timetable of Station
    SetStartStation,  // Station Links
    SetEndStation,
    AddPathSegments,
};

} // namespace omsi::tt
