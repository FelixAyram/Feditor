#pragma once

// TTimeTableMan-style runtime (Unit121): memory-first, disk only on save().

#include "omsi/tt_types.hpp"

#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace omsi::tt {

struct PathPickInfo {
    std::int32_t owner_id = 0;
    std::int32_t path_index = 0;
    std::int32_t tile_index = 0;       // index in global [map] list if known
    std::int32_t internal_path_no = 0;
    float length = 0.f;
};

// Optional resolver: map editor supplies tile_index for a path owner id.
using ResolveTileFn = std::int32_t (*)(std::int32_t owner_id, bool from_object, void* user);
using ResolveLengthFn = float (*)(std::int32_t owner_id, std::int32_t path_index, bool from_object, void* user);

class TimeTableMan {
public:
    TimeTableData data{};
    bool dirty = false;
    bool loaded = false;

    // Selection (TForm_main equivalents)
    int selected_track = -1;
    int selected_track_entry = -1;
    int selected_trip = -1;
    int selected_trip_station = -1;
    int selected_stnlink = -1;
    int selected_stnlink_entry = -1;
    int selected_line = -1;
    std::int32_t stnlink_filter_id = 0; // editor_stnlink_filter_IDCode

    TtEditMode mode = TtEditMode::None;

private:
    struct Snap { TimeTableData data; int track = -1, entry = -1, trip = -1, station = -1, link = -1, line = -1; };
    [[nodiscard]] std::uint64_t data_hash() const;
    [[nodiscard]] Snap make_snap() const;
    void apply_snap(const Snap& s);
    std::vector<Snap> undo_stack_, redo_stack_;
    Snap last_snap_;
    std::uint64_t last_hash_ = 0;
    bool have_last_ = false;
    // TTData read from disk: on a worker thread for ensure_loaded (a big map's hundreds of .ttr / .ttp /
    // .ttl files read on the main thread froze the editor for most of a minute on a cold disk)
    struct DiskResult {
        bool ok = false;
        bool dir_found = false;
        TimeTableData data{};
        std::filesystem::path map_root;
    };
    static DiskResult read_disk(const std::filesystem::path& map_root);
    bool adopt(DiskResult&& r);
    std::future<DiskResult> pending_;
    std::filesystem::path pending_root_;
public:
    // true while ensure_loaded's background read is still running
    [[nodiscard]] bool loading() const { return pending_.valid(); }

    void clear();
    // Undo / redo of the in-memory TTData. Nothing touches the disk until save(): call track_changes() once per
    // frame and every edit (tracks, trips, profiles, stn links, lines) becomes one undo step.
    void track_changes();
    bool undo();
    bool redo();
    [[nodiscard]] bool can_undo() const { return !undo_stack_.empty(); }
    [[nodiscard]] bool can_redo() const { return !redo_stack_.empty(); }
    // Reload from disk when the map root changed (or nothing is loaded yet). Safe to call every
    // frame: it is a no-op while the root is unchanged. Unsaved in-memory edits of the SAME map are
    // kept.
    bool ensure_loaded(const std::filesystem::path& map_root);
    // As ensure_loaded, but the data is there when it returns (waits for / does the read): for the code
    // that needs the trips or tracks right away (track / trip drives, test hooks).
    bool ensure_loaded_now(const std::filesystem::path& map_root) {
        if (map_root.empty()) return false;
        if (loaded && loaded_root == map_root) return true;
        dirty = false;
        return load(map_root);
    }
    // One-line description of what was found on disk ("TTData: <path> — 3 tracks, ..." or why not).
    [[nodiscard]] std::string status_text() const;
    std::filesystem::path loaded_root; // map root passed to load()
    bool ttdata_dir_found = false;
    // Resolve Trip::track_index for every trip (RefreshAllTrips).
    void refresh_all_trips();
    // DelTrack / DelTrip / DelLine: remove by index (Save drops the stale files).
    bool del_track(int track_idx);
    bool del_trip(int trip_idx);
    bool del_line(int line_idx);
    bool add_profile(int trip_idx, const std::string& name);
    bool load(const std::filesystem::path& map_root);
    bool save(); // writes all dirty structures under data.root

    // ── Tracks ─────────────────────────────────────────────────────────────
    // AddTrack: the name must be a valid file name (none of \\ / : ? * " < > |) and unique.
    // copy_from >= 0 copies that track's entries (Copy Track). Returns the new index or -1.
    int add_track(const std::string& name_stem, int copy_from = -1);
    [[nodiscard]] static bool valid_file_name(const std::string& name);
    bool add_track_entry(int track_idx, int insert_after, const PathPickInfo& pick);
    bool del_track_entry(int track_idx, int entry_idx);
    bool refresh_track_entry(int track_idx, int entry_idx, ResolveTileFn resolve_tile, void* user);
    // CalcTrackLaenge: refreshes entry lengths through `len` (if given), fills the runtime
    // cumulative start_offset of every entry and Track::total_length.
    void calc_track_length(int track_idx, ResolveLengthFn len = nullptr, void* user = nullptr);
    void refresh_all_tracks(ResolveTileFn resolve_tile, void* user);

    // ── Trips ──────────────────────────────────────────────────────────────
    int add_trip_type1(const std::string& name_stem, const std::string& track_name);
    int add_trip_type2(const std::string& name_stem);
    // AddTrip: track_idx out of range => type 2. copy_from >= 0 copies a whole trip (Copy Trip).
    int add_trip(int track_idx, const std::string& name_stem, int copy_from = -1);
    // "Copy T & T": copy the selected track and the selected trip under one new name; the copied
    // trip points to the copied track.
    bool copy_track_and_trip(const std::string& new_name);
    // AddTripBusstop: type 1 inserts AFTER the last station whose track entry is <= track_entry_index
    // (stations stay sorted along the track); type 2 inserts at insert_hint (clamped, -1 => append).
    // Every profile's per-station arrays are shifted accordingly. Returns the new station position
    // through selected_trip_station.
    bool add_trip_busstop(int trip_idx, std::int32_t object_id, const std::string& name,
                          std::int32_t tile_index, int track_entry_index = -1, int insert_hint = -1);
    bool del_trip_busstop(int trip_idx, int station_idx);
    bool change_trip_to_type2(int trip_idx);
    bool set_trip_meta(int trip_idx, const std::string& terminus, const std::string& line, bool train_rev);
    bool ensure_default_profile(int trip_idx);

    // ── Profiles (Tform_profiles) ──────────────────────────────────────────
    struct ProfileSlot { bool arr_flag = false, dep_flag = false; float arr = 0.f, dep = 0.f; int other = 0; };
    [[nodiscard]] ProfileSlot profile_slot(int trip_idx, int prof_idx, int station_idx) const;
    // Setting a time (EditKeyDown); `flag` is the "manual" bit (+8 arrival / +9 departure).
    void set_profile_arr(int trip_idx, int prof_idx, int station_idx, bool flag, float minutes);
    void set_profile_dep(int trip_idx, int prof_idx, int station_idx, bool flag, float minutes);
    void set_profile_other(int trip_idx, int prof_idx, int station_idx, int value);
    void set_profile_duration(int trip_idx, int prof_idx, float minutes);
    // Parallel trips: other trips that call at the same busstop, used to copy their timing.
    struct ParallelRef { int trip = -1, profile = -1, station = -1; };
    [[nodiscard]] std::vector<ParallelRef> parallel_refs(int trip_idx, int station_idx) const;
    // Copy times from a parallel profile starting at station `from_station`, shifting them so that
    // the arrival (or departure when `use_dep`) at that station keeps its current value.
    bool apply_parallel(int trip_idx, int prof_idx, int from_station, const ParallelRef& ref, bool use_dep);
    // Trip assistant: busstops reachable from the last station through StnLinks (type 2 trips).
    [[nodiscard]] std::vector<std::int32_t> assistant_next_busstops(int trip_idx) const;
    [[nodiscard]] std::string busstop_name(std::int32_t object_id) const;
    // Minutes from trip start at which each station is reached for a profile (manual times + interpolation).
    [[nodiscard]] std::vector<float> station_times(int trip_idx, int prof_idx) const;
    bool del_profile(int trip_idx, int prof_idx);
    // "used by: trip, trip" (TripAddStop).
    [[nodiscard]] std::vector<std::string> trips_using_busstop(std::int32_t object_id) const;
    void refresh_trip(int trip_idx); // fills data.errors for this trip

    // ── Busstops ───────────────────────────────────────────────────────────
    int find_busstop_by_object(std::int32_t object_id) const;
    int add_busstop(const BusstopListEntry& e);

    // ── StnLinks ───────────────────────────────────────────────────────────
    int add_stnlink();
    bool set_stnlink_busstop(int link_idx, std::int32_t object_id, bool is_start);
    bool add_stnlink_entry(int link_idx, int insert_after, const PathPickInfo& pick);
    bool del_stnlink_entry(int link_idx, int entry_idx);
    bool del_stnlink(int link_idx);
    void calc_stnlink_length(int link_idx);
    void refresh_stnlink(int link_idx);
    void refresh_all_stnlinks();
    int search_next_erroneous_stnlink(int start_after) const;

    // ── Lines / tours ──────────────────────────────────────────────────────
    int add_line(const std::string& name_stem); // name must be a valid file name and unique
    bool set_line_props(int line_idx, bool user_allowed, int priority);
    bool del_tour(int line_idx, int tour_idx);
    bool set_tour(int line_idx, int tour_idx, const std::string& number, const std::string& depot,
                  std::int32_t day_mask);
    bool del_tour_entry(int line_idx, int tour_idx, int entry_idx);
    // True when the tour has a valid first departure etc. (CheckTourActualValid).
    [[nodiscard]] bool tour_valid(int line_idx, int tour_idx) const;
    int add_tour(int line_idx, const std::string& number, const std::string& depot,
                 std::int32_t day_mask = kTourAllDays);
    bool add_tour_entry(int line_idx, int tour_idx, const std::string& trip_name, int profile_index,
                         float dep_minutes);

    // ── Query / validate ───────────────────────────────────────────────────
    void validate_all();
    std::string show_timetable_for_station(std::int32_t object_id) const;

    // Case-insensitive, like RefreshTrip (LowerCase on both sides).
    int find_track_by_name(const std::string& name) const;
    int find_trip_by_name(const std::string& name) const;

    // ── UI bridge (Unit134 TForm_main list / camera) ───────────────────────
    // List strings match RefreshTrackentryList / RefreshTripBusstopList exactly.
    // First item is always "<  -  >" (sentinel index -1).
    std::vector<std::string> track_entry_list_strings() const;
    std::vector<std::string> trip_station_list_strings() const;
    std::vector<std::string> track_combo_strings() const;
    std::vector<std::string> trip_combo_strings() const;

    // Length label: "Length = X.XXX km" (Label68)
    std::string track_length_label() const;

    // Camera target helpers (ListBox1Click / ListBox2Click / SetMapCamToStation)
    // Returns false if selection invalid. out_xyz filled with world position.
    // Caller supplies a resolver that turns (owner_id, path_index, tile) into world XYZ.
    using ResolveWorldPosFn = bool (*)(std::int32_t owner_id, std::int32_t path_index,
                                       std::int32_t tile_index, float* out_xyz, void* user);
    bool selected_track_entry_world_pos(ResolveWorldPosFn resolve, void* user, float out_xyz[3]) const;
    bool selected_station_world_pos(ResolveWorldPosFn resolve_busstop, void* user, float out_xyz[3]) const;

    // Path highlight: true if this path belongs to the active track (and optionally
    // matches selected entry for stronger highlight).
    bool is_path_in_selected_track(std::int32_t owner_id, std::int32_t path_index) const;
    bool is_path_selected_entry(std::int32_t owner_id, std::int32_t path_index) const;

    // Called by side-panel list click handlers (mirrors ListBox1Click / ListBox2Click)
    void on_track_entry_list_click(int list_item_index); // includes the "< - >" row as 0
    void on_trip_station_list_click(int list_item_index);
};


// Global used by map editor (like gvar TimeTableMan).
TimeTableMan& global_tt_man();

} // namespace omsi::tt
