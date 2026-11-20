#include "omsi/tt_man.hpp"
#include "omsi/tt_loader.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>

namespace omsi::tt {
namespace {

TimeTableMan g_man{};

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// Profile per-station arrays are stored sparsely; these keep them aligned with the station list.
template <class V>
void shift_up(V& v, int from) { // station inserted at `from`
    for (auto& p : v) if (p.first >= from) ++p.first;
}
template <class V>
void remove_and_shift(V& v, int idx) { // station idx removed
    v.erase(std::remove_if(v.begin(), v.end(), [&](const auto& p) { return p.first == idx; }), v.end());
    for (auto& p : v) if (p.first > idx) --p.first;
}

} // namespace

TimeTableMan& global_tt_man() { return g_man; }

void TimeTableMan::clear() {
    data = {};
    dirty = false;
    loaded = false;
    selected_track = selected_track_entry = selected_trip = selected_trip_station = -1;
    selected_stnlink = selected_stnlink_entry = selected_line = -1;
    stnlink_filter_id = 0;
    mode = TtEditMode::None;
    undo_stack_.clear();
    redo_stack_.clear();
    have_last_ = false;
}

// ── Undo / redo ─────────────────────────────────────────────────────────────

namespace {
inline void mix64(std::uint64_t& h, std::uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
}
inline void mixf(std::uint64_t& h, float f) {
    std::uint32_t u = 0;
    std::memcpy(&u, &f, sizeof(u));
    mix64(h, u);
}
inline void mixs(std::uint64_t& h, const std::string& s) { mix64(h, std::hash<std::string>{}(s)); }
} // namespace

std::uint64_t TimeTableMan::data_hash() const {
    std::uint64_t h = 1469598103934665603ULL;
    for (const auto& t : data.tracks) {
        mixs(h, t.name);
        for (const auto& e : t.entries) { mix64(h, static_cast<std::uint64_t>(e.id)); mix64(h, static_cast<std::uint64_t>(e.path_index)); mixf(h, e.length); }
        mix64(h, 0xA1);
    }
    for (const auto& t : data.trips) {
        mixs(h, t.name); mixs(h, t.track_name); mixs(h, t.terminus); mixs(h, t.line);
        mix64(h, t.train_reversed ? 1u : 0u);
        for (const auto& st : t.stations) { mix64(h, static_cast<std::uint64_t>(st.object_id)); mix64(h, static_cast<std::uint64_t>(st.track_entry_index)); }
        for (const auto& p : t.profiles) {
            mixs(h, p.name); mixf(h, p.minutes);
            for (const auto& q : p.man_arr) { mix64(h, static_cast<std::uint64_t>(q.first)); mixf(h, q.second); }
            for (const auto& q : p.man_dep) { mix64(h, static_cast<std::uint64_t>(q.first) + 77); mixf(h, q.second); }
            for (const auto& q : p.other_stopping) { mix64(h, static_cast<std::uint64_t>(q.first) + 991); mix64(h, static_cast<std::uint64_t>(q.second)); }
        }
        mix64(h, 0xB2);
    }
    for (const auto& l : data.stn_links) {
        mix64(h, static_cast<std::uint64_t>(l.from_id)); mix64(h, static_cast<std::uint64_t>(l.to_id));
        for (const auto& e : l.entries) { mix64(h, static_cast<std::uint64_t>(e.id)); mix64(h, static_cast<std::uint64_t>(e.path_index)); }
        mix64(h, 0xC3);
    }
    for (const auto& l : data.lines) {
        mixs(h, l.name); mix64(h, l.user_allowed ? 1u : 0u); mix64(h, static_cast<std::uint64_t>(l.priority));
        for (const auto& t : l.tours) {
            mixs(h, t.number); mixs(h, t.depot_name); mix64(h, static_cast<std::uint64_t>(t.day_mask));
            for (const auto& e : t.trips) { mixs(h, e.trip_name); mix64(h, static_cast<std::uint64_t>(e.profile_index)); mixf(h, e.departure_minutes); }
        }
        mix64(h, 0xD4);
    }
    return h;
}

TimeTableMan::Snap TimeTableMan::make_snap() const {
    Snap s;
    s.data = data;
    s.track = selected_track; s.entry = selected_track_entry; s.trip = selected_trip;
    s.station = selected_trip_station; s.link = selected_stnlink; s.line = selected_line;
    return s;
}

void TimeTableMan::apply_snap(const Snap& s) {
    data = s.data;
    selected_track = s.track; selected_track_entry = s.entry; selected_trip = s.trip;
    selected_trip_station = s.station; selected_stnlink = s.link; selected_line = s.line;
    dirty = true;
    refresh_all_trips();
    last_snap_ = make_snap();
    last_hash_ = data_hash();
    have_last_ = true;
}

void TimeTableMan::track_changes() {
    if (!loaded) return;
    const std::uint64_t h = data_hash();
    if (!have_last_) {
        last_snap_ = make_snap();
        last_hash_ = h;
        have_last_ = true;
        return;
    }
    if (h == last_hash_) {
        // selection moves are not undo steps but keep the snapshot's selection fresh
        last_snap_.track = selected_track; last_snap_.entry = selected_track_entry; last_snap_.trip = selected_trip;
        last_snap_.station = selected_trip_station; last_snap_.link = selected_stnlink; last_snap_.line = selected_line;
        return;
    }
    undo_stack_.push_back(std::move(last_snap_));
    if (undo_stack_.size() > 200) undo_stack_.erase(undo_stack_.begin());
    redo_stack_.clear();
    last_snap_ = make_snap();
    last_hash_ = h;
}

bool TimeTableMan::undo() {
    if (undo_stack_.empty()) return false;
    redo_stack_.push_back(make_snap());
    Snap s = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    apply_snap(s);
    return true;
}

bool TimeTableMan::redo() {
    if (redo_stack_.empty()) return false;
    undo_stack_.push_back(make_snap());
    Snap s = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    apply_snap(s);
    return true;
}

void TimeTableMan::refresh_all_trips() {
    for (int i = 0; i < static_cast<int>(data.trips.size()); ++i) {
        auto& tp = data.trips[static_cast<std::size_t>(i)];
        tp.track_index = find_track_by_name(tp.track_name); // -1 => type 2 semantics
        for (auto& st : tp.stations) st.typ2 = tp.track_index < 0;
    }
}

TimeTableMan::DiskResult TimeTableMan::read_disk(const std::filesystem::path& map_root) {
    DiskResult r;
    r.map_root = map_root;
    if (map_root.empty()) return r;
    r.data.root = map_root / "TTData";
    // Accept TTData / ttdata / Ttdata (the workspace sync copies any of those spellings).
    {
        std::error_code ec;
        if (std::filesystem::is_directory(r.data.root, ec)) r.dir_found = true;
        else {
            for (const auto& e : std::filesystem::directory_iterator(map_root, ec)) {
                if (ec || !e.is_directory()) continue;
                if (ieq(e.path().filename().string(), "TTData")) { r.data.root = e.path(); r.dir_found = true; break; }
            }
        }
    }
    const std::filesystem::path dir_copy = r.data.root;
    r.ok = r.dir_found && load_ttdata(dir_copy, r.data);
    if (r.dir_found) r.data.root = dir_copy;
    return r;
}

bool TimeTableMan::adopt(DiskResult&& r) {
    clear();
    if (r.map_root.empty()) return false;
    data = std::move(r.data);
    loaded_root = r.map_root;
    ttdata_dir_found = r.dir_found;
    loaded = true;
    dirty = false;
    // Load(): RefreshAllStnLinks, RefreshAllTracks, RefreshAllTrips, RefreshAllLines
    refresh_all_stnlinks();
    for (int t = 0; t < static_cast<int>(data.tracks.size()); ++t) calc_track_length(t);
    refresh_all_trips();
    if (!data.tracks.empty()) selected_track = 0;
    if (!data.trips.empty()) selected_trip = 0;
    if (!data.stn_links.empty()) selected_stnlink = 0;
    if (!data.lines.empty()) selected_line = 0;
    omsi::log::info("TTMan load tracks=" + std::to_string(data.tracks.size()) +
                    " trips=" + std::to_string(data.trips.size()) +
                    " busstops=" + std::to_string(data.busstops.size()) +
                    " stnlinks=" + std::to_string(data.stn_links.size()) +
                    " lines=" + std::to_string(data.lines.size()) +
                    " root=" + data.root.string());
    return r.ok;
}

bool TimeTableMan::load(const std::filesystem::path& map_root) {
    const auto new_root = map_root.empty() ? std::filesystem::path{} : (map_root / "TTData");
    if (loaded && dirty && !data.root.empty() && data.root == new_root) {
        omsi::log::info("TTMan load skipped (dirty in-memory edits for same TTData)");
        return true;
    }
    if (pending_.valid()) {
        // the background read of this map: wait for it instead of reading everything twice
        if (pending_root_ == map_root) return adopt(pending_.get());
        (void)pending_.get(); // another map's read: dropped
    }
    return adopt(read_disk(map_root));
}

bool TimeTableMan::save() {
    if (data.root.empty()) return false;
    // Save always rewrites everything (and drops stale files), even if nothing is left in memory.
    if (!loaded && !dirty) return false;
    refresh_all_trips(); // station layout (type1/type2) is chosen from the resolved track index
    const bool ok = save_ttdata(data);
    if (ok) dirty = false;
    omsi::log::info(std::string("TTMan save ") + (ok ? "ok" : "FAIL"));
    return ok;
}

// ── Tracks ──────────────────────────────────────────────────────────────────

int TimeTableMan::add_track(const std::string& name_stem, int copy_from) {
    if (!valid_file_name(name_stem)) return -1;
    if (find_track_by_name(name_stem) >= 0) return -1;
    Track tr;
    tr.name = name_stem;
    tr.path = data.root / (name_stem + ".ttr");
    if (copy_from >= 0 && copy_from < static_cast<int>(data.tracks.size())) {
        tr.entries = data.tracks[static_cast<std::size_t>(copy_from)].entries;
        tr.total_length = data.tracks[static_cast<std::size_t>(copy_from)].total_length;
    }
    data.tracks.push_back(std::move(tr));
    dirty = true;
    selected_track = static_cast<int>(data.tracks.size()) - 1;
    selected_track_entry = -1;
    calc_track_length(selected_track);
    return selected_track;
}

bool TimeTableMan::del_track(int track_idx) {
    if (track_idx < 0 || track_idx >= static_cast<int>(data.tracks.size())) return false;
    data.tracks.erase(data.tracks.begin() + track_idx);
    if (selected_track >= static_cast<int>(data.tracks.size())) selected_track = static_cast<int>(data.tracks.size()) - 1;
    selected_track_entry = -1;
    refresh_all_trips();
    dirty = true;
    return true;
}

bool TimeTableMan::add_track_entry(int track_idx, int insert_after, const PathPickInfo& pick) {
    if (track_idx < 0 || track_idx >= static_cast<int>(data.tracks.size())) return false;
    auto& tr = data.tracks[static_cast<std::size_t>(track_idx)];
    int pos = insert_after + 1; // OMSI UI passes selected_trackentry + 1
    if (pos < 0 || pos > static_cast<int>(tr.entries.size())) return false; // AddTrackEntry bounds check
    TrackEntry e;
    e.id = pick.owner_id;
    e.path_index = pick.path_index;
    // AddTrackEntry stores -1 in the tile cache and lets RefreshTrackEntry resolve it.
    e.tile_index = pick.tile_index;
    e.internal_path_no = pick.internal_path_no != 0 ? pick.internal_path_no : pick.path_index;
    e.length = pick.length;
    tr.entries.insert(tr.entries.begin() + pos, std::move(e));
    // Trips on this track: every station whose track entry >= pos moves one entry further.
    for (auto& tp : data.trips) {
        if (tp.track_index != track_idx) continue;
        for (auto& st : tp.stations)
            if (st.track_entry_index >= pos) ++st.track_entry_index;
    }
    selected_track_entry = pos;
    dirty = true;
    calc_track_length(track_idx);
    return true;
}

bool TimeTableMan::del_track_entry(int track_idx, int entry_idx) {
    if (track_idx < 0 || track_idx >= static_cast<int>(data.tracks.size())) return false;
    auto& tr = data.tracks[static_cast<std::size_t>(track_idx)];
    if (entry_idx < 0 || entry_idx >= static_cast<int>(tr.entries.size())) return false;
    tr.entries.erase(tr.entries.begin() + entry_idx);
    // Trips on this track: stations sitting on the deleted entry are deleted (DelTripBusstop),
    // stations behind it move one entry back.
    for (int t = 0; t < static_cast<int>(data.trips.size()); ++t) {
        auto& tp = data.trips[static_cast<std::size_t>(t)];
        if (tp.track_index != track_idx) continue;
        for (int s = 0; s < static_cast<int>(tp.stations.size());) {
            auto& st = tp.stations[static_cast<std::size_t>(s)];
            if (st.track_entry_index > entry_idx) { --st.track_entry_index; ++s; }
            else if (st.track_entry_index == entry_idx) { del_trip_busstop(t, s); }
            else ++s;
        }
    }
    if (selected_track_entry >= static_cast<int>(tr.entries.size()))
        selected_track_entry = static_cast<int>(tr.entries.size()) - 1;
    dirty = true;
    calc_track_length(track_idx);
    return true;
}

bool TimeTableMan::refresh_track_entry(int track_idx, int entry_idx, ResolveTileFn resolve_tile, void* user) {
    if (track_idx < 0 || track_idx >= static_cast<int>(data.tracks.size())) return false;
    auto& tr = data.tracks[static_cast<std::size_t>(track_idx)];
    if (entry_idx < 0 || entry_idx >= static_cast<int>(tr.entries.size())) return false;
    auto& e = tr.entries[static_cast<std::size_t>(entry_idx)];
    if (resolve_tile) {
        const auto ti = resolve_tile(e.id, true, user);
        if (ti >= 0) e.tile_index = ti;
    }
    if (e.internal_path_no < 0) e.internal_path_no = e.path_index;
    return e.tile_index >= 0;
}

void TimeTableMan::calc_track_length(int track_idx, ResolveLengthFn len, void* user) {
    if (track_idx < 0 || track_idx >= static_cast<int>(data.tracks.size())) return;
    auto& tr = data.tracks[static_cast<std::size_t>(track_idx)];
    // CalcTrackLaenge: total = 0; for each entry { length = path length; start_offset = total; total += length }
    float total = 0.f;
    for (auto& e : tr.entries) {
        if (len) {
            const float l = len(e.id, e.path_index, true, user);
            if (l >= 0.f) e.length = l;
        }
        e.start_offset = total;
        total += e.length;
    }
    tr.total_length = total;
}

void TimeTableMan::refresh_all_tracks(ResolveTileFn resolve_tile, void* user) {
    for (int t = 0; t < static_cast<int>(data.tracks.size()); ++t) {
        for (int e = 0; e < static_cast<int>(data.tracks[static_cast<std::size_t>(t)].entries.size()); ++e)
            refresh_track_entry(t, e, resolve_tile, user);
        calc_track_length(t);
    }
}

// ── Trips ───────────────────────────────────────────────────────────────────

int TimeTableMan::add_trip_type1(const std::string& name_stem, const std::string& track_name) {
    return add_trip(find_track_by_name(track_name), name_stem, -1);
}

int TimeTableMan::add_trip_type2(const std::string& name_stem) {
    return add_trip_type1(name_stem, std::string{});
}

bool TimeTableMan::del_trip(int trip_idx) {
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return false;
    data.trips.erase(data.trips.begin() + trip_idx);
    if (selected_trip >= static_cast<int>(data.trips.size())) selected_trip = static_cast<int>(data.trips.size()) - 1;
    dirty = true;
    return true;
}

bool TimeTableMan::add_trip_busstop(int trip_idx, std::int32_t object_id, const std::string& name,
                                    std::int32_t tile_index, int track_entry_index, int insert_hint) {
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return false;
    auto& tp = data.trips[static_cast<std::size_t>(trip_idx)];
    const bool type2 = tp.is_type2();
    int entry = track_entry_index >= 0 ? track_entry_index : selected_track_entry;
    int pos = 0;
    if (type2) {
        const int n = static_cast<int>(tp.stations.size());
        pos = insert_hint < 0 ? n : std::min(insert_hint, n);
    } else {
        const int tri = tp.track_index >= 0 ? tp.track_index : find_track_by_name(tp.track_name);
        if (tri < 0) return false;
        const auto& tr = data.tracks[static_cast<std::size_t>(tri)];
        if (entry < 0 || entry >= static_cast<int>(tr.entries.size())) return false;
        // scan from the end for the last station with track_entry <= entry; insert behind it
        pos = 0;
        for (int i = static_cast<int>(tp.stations.size()) - 1; i >= 0; --i)
            if (tp.stations[static_cast<std::size_t>(i)].track_entry_index <= entry) { pos = i + 1; break; }
    }
    TripStation st;
    st.object_id = object_id;
    st.typ2 = type2;
    st.name = name;
    st.tile_index = tile_index;
    st.track_entry_index = type2 ? -1 : entry;
    tp.stations.insert(tp.stations.begin() + pos, std::move(st));
    for (auto& pr : tp.profiles) {
        shift_up(pr.man_dep, pos);
        shift_up(pr.man_arr, pos);
        shift_up(pr.other_stopping, pos);
    }
    selected_trip_station = pos;
    dirty = true;
    return true;
}

bool TimeTableMan::del_trip_busstop(int trip_idx, int station_idx) {
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return false;
    auto& tp = data.trips[static_cast<std::size_t>(trip_idx)];
    if (station_idx < 0 || station_idx >= static_cast<int>(tp.stations.size())) return false;
    tp.stations.erase(tp.stations.begin() + station_idx);
    for (auto& pr : tp.profiles) {
        remove_and_shift(pr.man_dep, station_idx);
        remove_and_shift(pr.man_arr, station_idx);
        remove_and_shift(pr.other_stopping, station_idx);
    }
    if (selected_trip_station >= static_cast<int>(tp.stations.size()))
        selected_trip_station = static_cast<int>(tp.stations.size()) - 1;
    dirty = true;
    return true;
}

bool TimeTableMan::change_trip_to_type2(int trip_idx) {
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return false;
    auto& tp = data.trips[static_cast<std::size_t>(trip_idx)];
    for (auto& s : tp.stations) {
        s.typ2 = true;
        s.track_entry_index = -1;
    }
    tp.track_name.clear();
    tp.track_index = -1;
    dirty = true;
    return true;
}

bool TimeTableMan::set_trip_meta(int trip_idx, const std::string& terminus, const std::string& line,
                                 bool train_rev) {
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return false;
    auto& tp = data.trips[static_cast<std::size_t>(trip_idx)];
    if (!terminus.empty()) tp.terminus = terminus;
    if (!line.empty()) tp.line = line;
    tp.train_reversed = train_rev;
    dirty = true;
    return true;
}

// Editor convenience (not an OMSI function): seed an editable "standard" profile.
bool TimeTableMan::ensure_default_profile(int trip_idx) {
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return false;
    auto& tp = data.trips[static_cast<std::size_t>(trip_idx)];
    if (tp.profiles.empty()) {
        TripProfile pr;
        pr.name = "standard";
        pr.minutes = 1.f;
        tp.profiles.push_back(pr);
        dirty = true;
    }
    auto& pr = tp.profiles[0];
    if (pr.man_dep.empty() && !tp.stations.empty()) {
        for (int i = 0; i < static_cast<int>(tp.stations.size()); ++i)
            pr.man_dep.emplace_back(i, static_cast<float>(i));
        dirty = true;
    }
    return true;
}

void TimeTableMan::refresh_trip(int trip_idx) {
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return;
    auto& tp = data.trips[static_cast<std::size_t>(trip_idx)];
    tp.track_index = find_track_by_name(tp.track_name);
    if (tp.track_index < 0 && tp.stations.size() < 2)
        data.errors.push_back("Trip \"" + tp.name + ".ttp\"\r\nTrips having no track have to have at least two busstops!");
}

// ── Busstops ────────────────────────────────────────────────────────────────

int TimeTableMan::find_busstop_by_object(std::int32_t object_id) const {
    for (int i = 0; i < static_cast<int>(data.busstops.size()); ++i)
        if (data.busstops[static_cast<std::size_t>(i)].object_id == object_id) return i;
    return -1;
}

int TimeTableMan::add_busstop(const BusstopListEntry& e) {
    const int ex = find_busstop_by_object(e.object_id);
    if (ex >= 0) return ex;
    data.busstops.push_back(e);
    dirty = true;
    return static_cast<int>(data.busstops.size()) - 1;
}

// ── StnLinks ────────────────────────────────────────────────────────────────

int TimeTableMan::add_stnlink() {
    StnLink L{}; // AddStnLink: from = to = 0, caches -1
    data.stn_links.push_back(L);
    dirty = true;
    selected_stnlink = static_cast<int>(data.stn_links.size()) - 1;
    return selected_stnlink;
}

bool TimeTableMan::set_stnlink_busstop(int link_idx, std::int32_t object_id, bool is_start) {
    if (link_idx < 0 || link_idx >= static_cast<int>(data.stn_links.size())) return false;
    auto& L = data.stn_links[static_cast<std::size_t>(link_idx)];
    if (is_start) L.from_id = object_id;
    else L.to_id = object_id;
    dirty = true;
    refresh_stnlink(link_idx);
    return true;
}

bool TimeTableMan::add_stnlink_entry(int link_idx, int insert_after, const PathPickInfo& pick) {
    if (link_idx < 0 || link_idx >= static_cast<int>(data.stn_links.size())) return false;
    auto& L = data.stn_links[static_cast<std::size_t>(link_idx)];
    StnLinkEntry e;
    e.id = pick.owner_id;
    e.path_index = pick.path_index;
    e.tile_index = pick.tile_index;
    e.length = pick.length;
    int pos = insert_after + 1;
    if (pos < 0) pos = 0;
    if (pos > static_cast<int>(L.entries.size())) pos = static_cast<int>(L.entries.size());
    L.entries.insert(L.entries.begin() + pos, std::move(e));
    selected_stnlink_entry = pos;
    dirty = true;
    calc_stnlink_length(link_idx);
    return true;
}

bool TimeTableMan::del_stnlink_entry(int link_idx, int entry_idx) {
    if (link_idx < 0 || link_idx >= static_cast<int>(data.stn_links.size())) return false;
    auto& L = data.stn_links[static_cast<std::size_t>(link_idx)];
    if (entry_idx < 0 || entry_idx >= static_cast<int>(L.entries.size())) return false;
    L.entries.erase(L.entries.begin() + entry_idx);
    dirty = true;
    calc_stnlink_length(link_idx);
    return true;
}

bool TimeTableMan::del_stnlink(int link_idx) {
    if (link_idx < 0 || link_idx >= static_cast<int>(data.stn_links.size())) return false;
    data.stn_links.erase(data.stn_links.begin() + link_idx);
    if (selected_stnlink >= static_cast<int>(data.stn_links.size()))
        selected_stnlink = static_cast<int>(data.stn_links.size()) - 1;
    dirty = true;
    return true;
}

void TimeTableMan::calc_stnlink_length(int link_idx) {
    if (link_idx < 0 || link_idx >= static_cast<int>(data.stn_links.size())) return;
    auto& L = data.stn_links[static_cast<std::size_t>(link_idx)];
    // CalcStnLinkLaenge: sum over entries [i30, i34) when both are >= 0, otherwise over all entries.
    int first = 0, last = static_cast<int>(L.entries.size());
    if (L.i30 >= 0 && L.i34 >= 0) { first = L.i30; last = L.i34; }
    last = std::min(last, static_cast<int>(L.entries.size()));
    L.length = 0.f;
    for (int i = std::max(first, 0); i < last; ++i) {
        auto& e = L.entries[static_cast<std::size_t>(i)];
        e.rel_offset = L.length - L.p38; // entry +0x14 (runtime)
        L.length += e.length;
    }
}

void TimeTableMan::refresh_stnlink(int link_idx) { calc_stnlink_length(link_idx); }

void TimeTableMan::refresh_all_stnlinks() {
    for (int i = 0; i < static_cast<int>(data.stn_links.size()); ++i) refresh_stnlink(i);
}

int TimeTableMan::search_next_erroneous_stnlink(int start_after) const {
    const int n = static_cast<int>(data.stn_links.size());
    if (n <= 0) return -1;
    for (int k = 0; k < n; ++k) {
        const int i = (start_after + 1 + k) % n;
        const auto& L = data.stn_links[static_cast<std::size_t>(i)];
        // "StnLink has to contain at least two paths!"
        if (L.from_id == 0 || L.to_id == 0 || L.entries.size() < 2) return i;
    }
    return -1;
}

// ── Lines / tours ───────────────────────────────────────────────────────────

int TimeTableMan::add_line(const std::string& name_stem) {
    if (!valid_file_name(name_stem)) return -1;
    for (const auto& l : data.lines) if (l.name == name_stem) return -1;
    Line ln;
    ln.name = name_stem;
    ln.path = data.root / (name_stem + ".ttl");
    data.lines.push_back(std::move(ln));
    dirty = true;
    selected_line = static_cast<int>(data.lines.size()) - 1;
    return selected_line;
}

bool TimeTableMan::del_line(int line_idx) {
    if (line_idx < 0 || line_idx >= static_cast<int>(data.lines.size())) return false;
    data.lines.erase(data.lines.begin() + line_idx);
    if (selected_line >= static_cast<int>(data.lines.size())) selected_line = static_cast<int>(data.lines.size()) - 1;
    dirty = true;
    return true;
}

int TimeTableMan::add_tour(int line_idx, const std::string& number, const std::string& depot,
                           std::int32_t day_mask) {
    if (line_idx < 0 || line_idx >= static_cast<int>(data.lines.size())) return -1;
    Tour t;
    t.number = number;
    t.depot_name = depot;
    t.day_mask = day_mask & 0x3FF;
    data.lines[static_cast<std::size_t>(line_idx)].tours.push_back(std::move(t));
    dirty = true;
    return static_cast<int>(data.lines[static_cast<std::size_t>(line_idx)].tours.size()) - 1;
}

bool TimeTableMan::add_tour_entry(int line_idx, int tour_idx, const std::string& trip_name,
                                  int profile_index, float dep_minutes) {
    if (line_idx < 0 || line_idx >= static_cast<int>(data.lines.size())) return false;
    auto& ln = data.lines[static_cast<std::size_t>(line_idx)];
    if (tour_idx < 0 || tour_idx >= static_cast<int>(ln.tours.size())) return false;
    TourTrip at;
    at.trip_name = trip_name;
    at.profile_index = profile_index;
    at.departure_minutes = dep_minutes;
    ln.tours[static_cast<std::size_t>(tour_idx)].trips.push_back(std::move(at));
    dirty = true;
    return true;
}

void TimeTableMan::validate_all() {
    refresh_all_trips();
    validate_ttdata(data);
    for (int i = 0; i < static_cast<int>(data.tracks.size()); ++i) {
        const auto& tr = data.tracks[static_cast<std::size_t>(i)];
        for (std::size_t e = 0; e < tr.entries.size(); ++e)
            if (tr.entries[e].id <= 0)
                data.errors.push_back("There are is at least one invalid track entry: " + tr.name +
                                      ", Nr. " + std::to_string(e));
    }
}

namespace {
// Relative minute (from trip start) at which the trip leaves station `i` for the given profile.
// OMSI interpolates by path length; without map geometry we interpolate by station index between
// the manual anchors (start = 0, end = duration, plus every station with a manual flag).
float rel_time(const TripProfile& pr, int n, int i, bool dep) {
    if (n <= 1) return 0.f;
    struct A { int idx; float t; };
    std::vector<A> an;
    an.push_back({0, 0.f});
    for (int k = 1; k < n - 1; ++k) {
        for (const auto& d : pr.man_dep) if (d.first == k) { an.push_back({k, d.second}); goto next; }
        for (const auto& d : pr.man_arr) if (d.first == k) { an.push_back({k, d.second}); goto next; }
    next:;
    }
    an.push_back({n - 1, pr.minutes});
    for (std::size_t k = 0; k + 1 < an.size(); ++k) {
        if (i >= an[k].idx && i <= an[k + 1].idx) {
            const int span = std::max(1, an[k + 1].idx - an[k].idx);
            return an[k].t + (an[k + 1].t - an[k].t) * static_cast<float>(i - an[k].idx) / static_cast<float>(span);
        }
    }
    (void)dep;
    return 0.f;
}
} // namespace

std::string TimeTableMan::show_timetable_for_station(std::int32_t object_id) const {
    // Tform_timetable.CreateTimetable: header, one block per direction, three day groups,
    // an hour table (0..27) per group and footnotes for * % ~.
    struct Dep { std::string line, dest; int minute; bool allwd; bool nosch; bool onlysch; };
    std::vector<Dep> groups[3];
    const std::string sname = busstop_name(object_id);
    for (const auto& ln : data.lines) {
        for (const auto& tour : ln.tours) {
            const bool wd = (tour.day_mask & 0x1F) != 0, sa = (tour.day_mask & 0x20) != 0,
                       su = (tour.day_mask & 0xC0) != 0;
            const bool allwd = (tour.day_mask & 0x1F) == 0x1F;
            const bool sch_a = (tour.day_mask & 0x100) != 0, sch_b = (tour.day_mask & 0x200) != 0;
            for (const auto& at : tour.trips) {
                const int ti = find_trip_by_name(at.trip_name);
                if (ti < 0) continue;
                const auto& tp = data.trips[static_cast<std::size_t>(ti)];
                if (tp.profiles.empty()) continue;
                const int pi = std::clamp(at.profile_index, 0, static_cast<int>(tp.profiles.size()) - 1);
                const int n = static_cast<int>(tp.stations.size());
                for (int i = 0; i < n; ++i) {
                    if (tp.stations[static_cast<std::size_t>(i)].object_id != object_id) continue;
                    if (i == n - 1) continue; // arrivals only at the terminus are not departures
                    const float t = at.departure_minutes + rel_time(tp.profiles[static_cast<std::size_t>(pi)], n, i, true);
                    Dep d{ln.name, tp.terminus, static_cast<int>(t + 0.5f), allwd, sch_b && !sch_a, sch_a && !sch_b};
                    if (wd) groups[0].push_back(d);
                    if (sa) groups[1].push_back(d);
                    if (su) groups[2].push_back(d);
                }
            }
        }
    }
    std::ostringstream o;
    const char* bar = "====================================================================================";
    const char* thin = "------------------------------------------------------------------------------------";
    o << bar << "\n" << (sname.empty() ? "Busstop " + std::to_string(object_id) : sname) << "\n" << bar << "\n";
    const char* titles[3] = {"  Monday - Friday:", "  Saturday:", "  Sunday, Holiday:"};
    bool any_star = false, any_pct = false, any_tilde = false;
    for (int g = 0; g < 3; ++g) {
        auto& v = groups[g];
        std::sort(v.begin(), v.end(), [](const Dep& a, const Dep& b) { return a.minute < b.minute; });
        o << titles[g] << "\n";
        if (v.empty()) { o << "  -\n" << thin << "\n"; continue; }
        for (int h = 0; h < 28; ++h) {
            std::ostringstream row;
            bool has = false;
            for (const auto& d : v) {
                if (d.minute / 60 != h) continue;
                char b[16];
                std::snprintf(b, sizeof(b), "%02d", d.minute % 60);
                row << "\t" << b << (d.allwd || g != 0 ? "" : "*") << (d.nosch ? "%" : "") << (d.onlysch ? "~" : "");
                if (!d.allwd && g == 0) any_star = true;
                any_pct |= d.nosch;
                any_tilde |= d.onlysch;
                has = true;
            }
            if (has) { char b[8]; std::snprintf(b, sizeof(b), "%02d", h % 24); o << "  " << b << row.str() << "\n"; }
        }
        std::vector<std::string> seen;
        for (const auto& d : v) {
            const std::string key = d.line + " => " + d.dest;
            if (std::find(seen.begin(), seen.end(), key) == seen.end()) seen.push_back(key);
        }
        for (const auto& k : seen) o << "  " << k << "\n";
        o << thin << "\n";
    }
    if (any_star) o << "* : Not on all working days\n";
    if (any_pct) o << "% : Not on school holidays\n";
    if (any_tilde) o << "~ : Only on school holidays\n";
    o << bar << "\n";
    return o.str();
}


// ── Loading helpers ─────────────────────────────────────────────────────────

bool TimeTableMan::ensure_loaded(const std::filesystem::path& map_root) {
    if (map_root.empty()) return false;
    if (loaded && loaded_root == map_root) return true;
    if (pending_.valid()) {
        if (pending_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false; // still reading
        DiskResult r = pending_.get();
        if (pending_root_ == map_root) {
            dirty = false;
            loaded = false;
            return adopt(std::move(r));
        }
        // (finished for a map that is no longer loaded: start this one)
    }
    // A different map (or first call): drop everything and read TTData from disk on a worker thread.
    clear();
    dirty = false;
    loaded = false;
    pending_root_ = map_root;
    pending_ = std::async(std::launch::async, [map_root]() {
        try {
            return read_disk(map_root);
        } catch (...) {
            DiskResult r;
            r.map_root = map_root;
            return r;
        }
    });
    omsi::log::info("TTMan background read started: " + map_root.string());
    return false;
}

std::string TimeTableMan::status_text() const {
    if (!loaded) return "TTData: not loaded";
    if (!ttdata_dir_found)
        return "TTData: folder not found (" + data.root.string() + ") — Create... makes one on Save";
    return "TTData: " + std::to_string(data.tracks.size()) + " tracks, " + std::to_string(data.trips.size()) +
           " trips, " + std::to_string(data.lines.size()) + " lines, " + std::to_string(data.busstops.size()) +
           " busstops, " + std::to_string(data.stn_links.size()) + " StnLinks";
}

bool TimeTableMan::valid_file_name(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name)
        if (c == '\\' || c == '/' || c == ':' || c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    return true;
}

int TimeTableMan::add_trip(int track_idx, const std::string& name_stem, int copy_from) {
    if (!valid_file_name(name_stem)) return -1;
    if (find_trip_by_name(name_stem) >= 0) return -1; // AddTrip: same name => refused
    const bool type1 = track_idx >= 0 && track_idx < static_cast<int>(data.tracks.size());
    Trip tp;
    tp.name = name_stem;
    tp.path = data.root / (name_stem + ".ttp");
    if (type1) tp.track_name = data.tracks[static_cast<std::size_t>(track_idx)].name;
    if (copy_from >= 0 && copy_from < static_cast<int>(data.trips.size())) {
        const auto& src = data.trips[static_cast<std::size_t>(copy_from)];
        tp.track_name = src.track_name;
        tp.track_index = src.track_index;
        tp.terminus = src.terminus;
        tp.line = src.line;
        tp.train_reversed = src.train_reversed;
        tp.stations = src.stations;
        tp.profiles = src.profiles;
    }
    data.trips.push_back(std::move(tp));
    refresh_all_trips();
    dirty = true;
    selected_trip = static_cast<int>(data.trips.size()) - 1;
    selected_trip_station = -1;
    return selected_trip;
}

bool TimeTableMan::copy_track_and_trip(const std::string& new_name) {
    if (selected_track < 0 || selected_trip < 0) return false;
    if (!valid_file_name(new_name) || find_track_by_name(new_name) >= 0 || find_trip_by_name(new_name) >= 0)
        return false;
    const int src_trip = selected_trip;
    const int t = add_track(new_name, selected_track);
    if (t < 0) return false;
    const int tp = add_trip(t, new_name, src_trip);
    if (tp < 0) return false;
    data.trips[static_cast<std::size_t>(tp)].track_name = new_name;
    refresh_all_trips();
    return true;
}

// ── Profiles ────────────────────────────────────────────────────────────────

namespace {
const float* find_pair(const std::vector<std::pair<std::int32_t, float>>& v, int i) {
    for (const auto& p : v) if (p.first == i) return &p.second;
    return nullptr;
}
void set_pair(std::vector<std::pair<std::int32_t, float>>& v, int i, bool flag, float val) {
    auto it = std::find_if(v.begin(), v.end(), [&](const auto& p) { return p.first == i; });
    if (!flag) { if (it != v.end()) v.erase(it); return; }
    if (it != v.end()) it->second = val; else v.emplace_back(i, val);
}
} // namespace

TimeTableMan::ProfileSlot TimeTableMan::profile_slot(int trip_idx, int prof_idx, int st) const {
    ProfileSlot r;
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return r;
    const auto& tp = data.trips[static_cast<std::size_t>(trip_idx)];
    if (prof_idx < 0 || prof_idx >= static_cast<int>(tp.profiles.size())) return r;
    const auto& pr = tp.profiles[static_cast<std::size_t>(prof_idx)];
    if (const float* a = find_pair(pr.man_arr, st)) { r.arr_flag = true; r.arr = *a; }
    if (const float* d = find_pair(pr.man_dep, st)) { r.dep_flag = true; r.dep = *d; }
    for (const auto& o : pr.other_stopping) if (o.first == st) r.other = o.second;
    return r;
}

void TimeTableMan::set_profile_arr(int t, int p, int st, bool flag, float v) {
    if (t < 0 || t >= static_cast<int>(data.trips.size())) return;
    auto& tp = data.trips[static_cast<std::size_t>(t)];
    if (p < 0 || p >= static_cast<int>(tp.profiles.size()) || st < 0 || st >= static_cast<int>(tp.stations.size())) return;
    set_pair(tp.profiles[static_cast<std::size_t>(p)].man_arr, st, flag, v);
    dirty = true;
}

void TimeTableMan::set_profile_dep(int t, int p, int st, bool flag, float v) {
    if (t < 0 || t >= static_cast<int>(data.trips.size())) return;
    auto& tp = data.trips[static_cast<std::size_t>(t)];
    if (p < 0 || p >= static_cast<int>(tp.profiles.size()) || st < 0 || st >= static_cast<int>(tp.stations.size())) return;
    set_pair(tp.profiles[static_cast<std::size_t>(p)].man_dep, st, flag, v);
    dirty = true;
}

void TimeTableMan::set_profile_other(int t, int p, int st, int value) {
    if (t < 0 || t >= static_cast<int>(data.trips.size())) return;
    auto& tp = data.trips[static_cast<std::size_t>(t)];
    if (p < 0 || p >= static_cast<int>(tp.profiles.size()) || st < 0 || st >= static_cast<int>(tp.stations.size())) return;
    value = std::clamp(value, 0, 255);
    auto& v = tp.profiles[static_cast<std::size_t>(p)].other_stopping;
    auto it = std::find_if(v.begin(), v.end(), [&](const auto& q) { return q.first == st; });
    if (value == 0) { if (it != v.end()) v.erase(it); }
    else if (it != v.end()) it->second = value;
    else v.emplace_back(st, value);
    dirty = true;
}

void TimeTableMan::set_profile_duration(int t, int p, float minutes) {
    if (t < 0 || t >= static_cast<int>(data.trips.size())) return;
    auto& tp = data.trips[static_cast<std::size_t>(t)];
    if (p < 0 || p >= static_cast<int>(tp.profiles.size())) return;
    tp.profiles[static_cast<std::size_t>(p)].minutes = minutes;
    dirty = true;
}

bool TimeTableMan::add_profile(int trip_idx, const std::string& name) {
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size()) || name.empty()) return false;
    auto& tp = data.trips[static_cast<std::size_t>(trip_idx)];
    for (const auto& q : tp.profiles) if (q.name == name) return false; // AddProfile: unique names
    TripProfile pr;
    pr.name = name;
    pr.minutes = 1.f; // AddProfile stores 0x42700000 (60.0f) in OMSI's seconds == 1 minute in the file
    tp.profiles.push_back(std::move(pr));
    dirty = true;
    return true;
}

std::vector<TimeTableMan::ParallelRef> TimeTableMan::parallel_refs(int trip_idx, int station_idx) const {
    std::vector<ParallelRef> out;
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return out;
    const auto& me = data.trips[static_cast<std::size_t>(trip_idx)];
    if (station_idx < 0 || station_idx >= static_cast<int>(me.stations.size())) return out;
    const auto id = me.stations[static_cast<std::size_t>(station_idx)].object_id;
    for (int t = 0; t < static_cast<int>(data.trips.size()); ++t) {
        if (t == trip_idx) continue;
        const auto& o = data.trips[static_cast<std::size_t>(t)];
        for (int s = 0; s < static_cast<int>(o.stations.size()); ++s) {
            if (o.stations[static_cast<std::size_t>(s)].object_id != id) continue;
            for (int p = 0; p < static_cast<int>(o.profiles.size()); ++p) out.push_back({t, p, s});
            break;
        }
    }
    return out;
}

bool TimeTableMan::apply_parallel(int trip_idx, int prof_idx, int from_station, const ParallelRef& ref, bool use_dep) {
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return false;
    if (ref.trip < 0 || ref.trip >= static_cast<int>(data.trips.size())) return false;
    auto& me = data.trips[static_cast<std::size_t>(trip_idx)];
    const auto& ot = data.trips[static_cast<std::size_t>(ref.trip)];
    if (prof_idx < 0 || prof_idx >= static_cast<int>(me.profiles.size())) return false;
    if (ref.profile < 0 || ref.profile >= static_cast<int>(ot.profiles.size())) return false;
    auto& mp = me.profiles[static_cast<std::size_t>(prof_idx)];
    const auto& op = ot.profiles[static_cast<std::size_t>(ref.profile)];
    const auto mine = profile_slot(trip_idx, prof_idx, from_station);
    const auto theirs = profile_slot(ref.trip, ref.profile, ref.station);
    // delta = (my time) - (their time) at the meeting station, arrival or departure column
    const float delta = use_dep ? (mine.dep - theirs.dep) : (mine.arr - theirs.arr);
    const int n_me = static_cast<int>(me.stations.size());
    for (int k = from_station; k < n_me; ++k) {
        const int j = ref.station + (k - from_station);
        if (j >= static_cast<int>(ot.stations.size())) break;
        if (me.stations[static_cast<std::size_t>(k)].object_id != ot.stations[static_cast<std::size_t>(j)].object_id) break;
        const auto t = profile_slot(ref.trip, ref.profile, j);
        set_pair(mp.man_arr, k, t.arr_flag, t.arr + delta);
        set_pair(mp.man_dep, k, t.dep_flag, t.dep + delta);
    }
    (void)op;
    dirty = true;
    return true;
}

std::string TimeTableMan::busstop_name(std::int32_t object_id) const {
    const int bi = find_busstop_by_object(object_id);
    return bi >= 0 ? data.busstops[static_cast<std::size_t>(bi)].name : std::string{};
}

std::vector<std::string> TimeTableMan::trips_using_busstop(std::int32_t object_id) const {
    std::vector<std::string> out;
    for (const auto& tp : data.trips)
        for (const auto& st : tp.stations)
            if (st.object_id == object_id) { out.push_back(tp.name); break; }
    return out;
}

std::vector<std::int32_t> TimeTableMan::assistant_next_busstops(int trip_idx) const {
    std::vector<std::int32_t> out;
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return out;
    const auto& tp = data.trips[static_cast<std::size_t>(trip_idx)];
    if (tp.stations.empty()) {
        for (const auto& L : data.stn_links)
            if (L.from_id != 0 && std::find(out.begin(), out.end(), L.from_id) == out.end()) out.push_back(L.from_id);
        return out;
    }
    const auto last = tp.stations.back().object_id;
    for (const auto& L : data.stn_links)
        if (L.from_id == last && L.to_id != 0 && std::find(out.begin(), out.end(), L.to_id) == out.end())
            out.push_back(L.to_id);
    return out;
}

// ── Lines (Edit Timetable) ──────────────────────────────────────────────────

bool TimeTableMan::set_line_props(int l, bool user_allowed, int priority) {
    if (l < 0 || l >= static_cast<int>(data.lines.size())) return false;
    data.lines[static_cast<std::size_t>(l)].user_allowed = user_allowed;
    data.lines[static_cast<std::size_t>(l)].priority = std::clamp(priority, 0, 255);
    dirty = true;
    return true;
}

bool TimeTableMan::del_tour(int l, int t) {
    if (l < 0 || l >= static_cast<int>(data.lines.size())) return false;
    auto& tours = data.lines[static_cast<std::size_t>(l)].tours;
    if (t < 0 || t >= static_cast<int>(tours.size())) return false;
    tours.erase(tours.begin() + t);
    dirty = true;
    return true;
}

bool TimeTableMan::set_tour(int l, int t, const std::string& number, const std::string& depot, std::int32_t mask) {
    if (l < 0 || l >= static_cast<int>(data.lines.size())) return false;
    auto& tours = data.lines[static_cast<std::size_t>(l)].tours;
    if (t < 0 || t >= static_cast<int>(tours.size())) return false;
    tours[static_cast<std::size_t>(t)].number = number;
    tours[static_cast<std::size_t>(t)].depot_name = depot;
    tours[static_cast<std::size_t>(t)].day_mask = mask & 0x3FF;
    dirty = true;
    return true;
}

bool TimeTableMan::del_tour_entry(int l, int t, int e) {
    if (l < 0 || l >= static_cast<int>(data.lines.size())) return false;
    auto& tours = data.lines[static_cast<std::size_t>(l)].tours;
    if (t < 0 || t >= static_cast<int>(tours.size())) return false;
    auto& v = tours[static_cast<std::size_t>(t)].trips;
    if (e < 0 || e >= static_cast<int>(v.size())) return false;
    v.erase(v.begin() + e);
    dirty = true;
    return true;
}

bool TimeTableMan::tour_valid(int l, int t) const {
    if (l < 0 || l >= static_cast<int>(data.lines.size())) return false;
    const auto& tours = data.lines[static_cast<std::size_t>(l)].tours;
    if (t < 0 || t >= static_cast<int>(tours.size())) return false;
    for (const auto& e : tours[static_cast<std::size_t>(t)].trips)
        if (find_trip_by_name(e.trip_name) < 0) return false;
    return true;
}

std::vector<float> TimeTableMan::station_times(int trip_idx, int prof_idx) const {
    std::vector<float> out;
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return out;
    const auto& tp = data.trips[static_cast<std::size_t>(trip_idx)];
    if (prof_idx < 0 || prof_idx >= static_cast<int>(tp.profiles.size())) return out;
    const int n = static_cast<int>(tp.stations.size());
    for (int i = 0; i < n; ++i) out.push_back(rel_time(tp.profiles[static_cast<std::size_t>(prof_idx)], n, i, true));
    return out;
}

bool TimeTableMan::del_profile(int trip_idx, int prof_idx) {
    if (trip_idx < 0 || trip_idx >= static_cast<int>(data.trips.size())) return false;
    auto& tp = data.trips[static_cast<std::size_t>(trip_idx)];
    if (prof_idx < 0 || prof_idx >= static_cast<int>(tp.profiles.size())) return false;
    tp.profiles.erase(tp.profiles.begin() + prof_idx);
    // tour entries that used a later profile slide down by one
    for (auto& ln : data.lines)
        for (auto& tour : ln.tours)
            for (auto& e : tour.trips)
                if (e.trip_name == tp.name && e.profile_index > prof_idx) --e.profile_index;
    dirty = true;
    return true;
}

int TimeTableMan::find_track_by_name(const std::string& name) const {
    for (int i = 0; i < static_cast<int>(data.tracks.size()); ++i)
        if (ieq(data.tracks[static_cast<std::size_t>(i)].name, name)) return i;
    return -1;
}

int TimeTableMan::find_trip_by_name(const std::string& name) const {
    for (int i = 0; i < static_cast<int>(data.trips.size()); ++i)
        if (data.trips[static_cast<std::size_t>(i)].name == name) return i;
    return -1;
}

// ── UI bridge (Unit134) ─────────────────────────────────────────────────────

std::vector<std::string> TimeTableMan::track_entry_list_strings() const {
    // RefreshTrackentryList: first line "<  -  >", then "index: id"
    std::vector<std::string> out;
    out.emplace_back("<  -  >");
    if (selected_track < 0 || selected_track >= static_cast<int>(data.tracks.size()))
        return out;
    const auto& tr = data.tracks[static_cast<std::size_t>(selected_track)];
    for (std::size_t i = 0; i < tr.entries.size(); ++i)
        out.push_back(std::to_string(i) + ": " + std::to_string(tr.entries[i].id));
    return out;
}

std::vector<std::string> TimeTableMan::trip_station_list_strings() const {
    // RefreshTripBusstopList: "<  -  >" then "index: object_id/track_entry_index - name"
    // For type2 track_entry_index is -1 in UI.
    std::vector<std::string> out;
    out.emplace_back("<  -  >");
    if (selected_trip < 0 || selected_trip >= static_cast<int>(data.trips.size()))
        return out;
    const auto& tp = data.trips[static_cast<std::size_t>(selected_trip)];
    for (std::size_t i = 0; i < tp.stations.size(); ++i) {
        const auto& st = tp.stations[i];
        int te = st.typ2 ? -1 : st.track_entry_index;
        std::string name = st.name;
        if (name.empty() && st.object_id != 0) {
            int bi = find_busstop_by_object(st.object_id);
            if (bi >= 0) name = data.busstops[static_cast<std::size_t>(bi)].name;
        }
        out.push_back(std::to_string(i) + ": " + std::to_string(st.object_id) + "/" +
                      std::to_string(te) + " - " + name);
    }
    return out;
}

std::vector<std::string> TimeTableMan::track_combo_strings() const {
    std::vector<std::string> out;
    for (const auto& t : data.tracks) out.push_back(t.name);
    return out;
}

std::vector<std::string> TimeTableMan::trip_combo_strings() const {
    std::vector<std::string> out;
    for (const auto& t : data.trips) out.push_back(t.name);
    return out;
}

std::string TimeTableMan::track_length_label() const {
    // Label68: "Length = X.XXX km" — Track::total_length (CalcTrackLaenge) / 1000
    if (selected_track < 0 || selected_track >= static_cast<int>(data.tracks.size()))
        return "Length = 0.000 km";
    const auto& tr = data.tracks[static_cast<std::size_t>(selected_track)];
    float sum = tr.total_length;
    if (sum <= 0.f) for (const auto& e : tr.entries) sum += e.length;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Length = %.3f km", static_cast<double>(sum / 1000.f));
    return buf;
}

bool TimeTableMan::selected_track_entry_world_pos(ResolveWorldPosFn resolve, void* user,
                                                   float out_xyz[3]) const {
    if (!resolve || !out_xyz) return false;
    if (selected_track < 0 || selected_track >= static_cast<int>(data.tracks.size())) return false;
    const auto& tr = data.tracks[static_cast<std::size_t>(selected_track)];
    if (selected_track_entry < 0 ||
        selected_track_entry >= static_cast<int>(tr.entries.size()))
        return false;
    const auto& e = tr.entries[static_cast<std::size_t>(selected_track_entry)];
    return resolve(e.id, e.path_index, e.tile_index, out_xyz, user);
}

bool TimeTableMan::selected_station_world_pos(ResolveWorldPosFn resolve_busstop, void* user,
                                               float out_xyz[3]) const {
    if (!resolve_busstop || !out_xyz) return false;
    if (selected_trip < 0 || selected_trip >= static_cast<int>(data.trips.size())) return false;
    const auto& tp = data.trips[static_cast<std::size_t>(selected_trip)];
    if (selected_trip_station < 0 ||
        selected_trip_station >= static_cast<int>(tp.stations.size()))
        return false;
    const auto& st = tp.stations[static_cast<std::size_t>(selected_trip_station)];
    // For type1 we can also jump via track entry; prefer busstop world pos.
    return resolve_busstop(st.object_id, 0, st.tile_index, out_xyz, user);
}

bool TimeTableMan::is_path_in_selected_track(std::int32_t owner_id, std::int32_t path_index) const {
    if (selected_track < 0 || selected_track >= static_cast<int>(data.tracks.size())) return false;
    const auto& tr = data.tracks[static_cast<std::size_t>(selected_track)];
    for (const auto& e : tr.entries)
        if (e.id == owner_id && e.path_index == path_index) return true;
    return false;
}

bool TimeTableMan::is_path_selected_entry(std::int32_t owner_id, std::int32_t path_index) const {
    if (selected_track < 0 || selected_track >= static_cast<int>(data.tracks.size())) return false;
    if (selected_track_entry < 0) return false;
    const auto& tr = data.tracks[static_cast<std::size_t>(selected_track)];
    if (selected_track_entry >= static_cast<int>(tr.entries.size())) return false;
    const auto& e = tr.entries[static_cast<std::size_t>(selected_track_entry)];
    return e.id == owner_id && e.path_index == path_index;
}

void TimeTableMan::on_track_entry_list_click(int list_item_index) {
    // ListBox1Click: ItemIndex includes "< - >" at 0 → selected = ItemIndex - 1
    selected_track_entry = list_item_index - 1;
    if (selected_track_entry < -1) selected_track_entry = -1;
}

void TimeTableMan::on_trip_station_list_click(int list_item_index) {
    // ListBox2Click: sets selected_tripentry, then also selects the linked track entry
    selected_trip_station = list_item_index - 1;
    if (selected_trip_station < -1) selected_trip_station = -1;
    if (selected_trip < 0 || selected_trip >= static_cast<int>(data.trips.size())) return;
    const auto& tp = data.trips[static_cast<std::size_t>(selected_trip)];
    if (selected_trip_station < 0 ||
        selected_trip_station >= static_cast<int>(tp.stations.size()))
        return;
    const auto& st = tp.stations[static_cast<std::size_t>(selected_trip_station)];
    if (!st.typ2 && st.track_entry_index >= 0) {
        // Also select the corresponding track entry (OMSI does this)
        selected_track_entry = st.track_entry_index;
        // If trip has a track_name, ensure selected_track matches
        if (!tp.track_name.empty()) {
            int ti = find_track_by_name(tp.track_name);
            if (ti >= 0) selected_track = ti;
        }
    }
}

} // namespace omsi::tt
