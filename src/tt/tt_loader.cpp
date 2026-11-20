#include "omsi/vfs_overlay.hpp"
#include "omsi/fs_cow.hpp"
#include "omsi/tt_loader.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

// TTData reader/writer — mirrors TTimeTableMan.Load*/Save* in Unit121 line by line.
// OMSI reads these files as a stream of lines (ReadLString + ReadLn): a tag line selects the
// record, then the record's fields are consumed from the following lines in a FIXED order.
// Anything that is not a recognised tag is skipped. LineCursor reproduces exactly that.

namespace omsi::tt {
namespace {

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::int32_t to_i(const std::string& s) {
    try {
        return static_cast<std::int32_t>(std::stol(trim(s)));
    } catch (...) {
        return 0;
    }
}

float to_f(const std::string& s) {
    try {
        std::string t = trim(s);
        // Delphi StrToFloat in OMSI uses '.' ; tolerate a decimal comma from hand-edited files.
        std::replace(t.begin(), t.end(), ',', '.');
        return std::stof(t);
    } catch (...) {
        return 0.f;
    }
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
    // Prefer cfg reader (handles UTF-16 LE maps / TTData the same way as global.cfg).
    auto lines = cfg::read_cfg_lines(path);
    if (!lines.empty()) return lines;
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::string> out;
    std::string cur;
    for (char c : all) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        if (cur.back() == '\r') cur.pop_back();
        out.push_back(cur);
    }
    return out;
}

// Sequential line reader == Delphi "ReadLString(f, s); ReadLn(f);" pairs.
class LineCursor {
public:
    explicit LineCursor(std::vector<std::string> l) : lines_(std::move(l)) {}
    [[nodiscard]] bool eof() const { return i_ >= lines_.size(); }
    std::string next() { return i_ < lines_.size() ? lines_[i_++] : std::string{}; }
    std::int32_t next_i() { return to_i(next()); }
    float next_f() { return to_f(next()); }
    // Delphi stores the raw line; names are compared/stored untouched, only CR/LF stripped.
    std::string next_s() { return next(); }

private:
    std::vector<std::string> lines_;
    std::size_t i_ = 0;
};

bool write_lines(const std::filesystem::path& path, const std::vector<std::string>& lines) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    omsi::fsutil::break_link(path); // copy-on-write workspace: never write through a link
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    for (const auto& l : lines) {
        f.write(l.data(), static_cast<std::streamsize>(l.size()));
        f.put('\r');
        f.put('\n');
    }
    return static_cast<bool>(f);
}

// FloatToStrF(v, ffFixed, 10, 3) as used by SaveTrip/SaveLine (Unit7.sub_00424F2C(2,10,3)).
std::string fmt_f(float v) {
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
    return buf;
}

std::string today() {
    // Delphi DateToStr (German locale: dd.mm.yyyy). The loader ignores this line.
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%02d.%02d.%04d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year + 1900);
    return buf;
}

void write_header(std::vector<std::string>& out, const char* dashes, const char* title) {
    out.push_back(dashes);
    out.push_back(title);
    out.push_back(dashes);
    out.emplace_back();
    out.push_back("Created with Omsi Version 2.3.004");
    out.push_back("Date: " + today());
    out.emplace_back();
}

} // namespace

// ─── Busstops.cfg ───────────────────────────────────────────────────────────
bool load_busstops_cfg(const std::filesystem::path& path, std::vector<BusstopListEntry>& out) {
    out.clear();
    if (!omsi::vfs::exists(path)) return true; // empty ok
    LineCursor c(read_lines(path));
    while (!c.eof()) {
        if (trim(c.next()) != "[busstop]") continue;
        BusstopListEntry e{};
        e.name = c.next_s();
        e.tile_index = c.next_i();
        e.object_id = c.next_i();
        e.offset = c.next_f();
        const std::int32_t n = c.next_i();
        for (std::int32_t k = 0; k < n && !c.eof(); ++k) e.aliases.push_back(c.next_s());
        e.extra_int = c.next_i();
        e.tail = c.next_s();
        // TTimeTableMan.LoadBusstopList de-duplicates by id (+0xC): first one wins.
        const bool dup = std::any_of(out.begin(), out.end(),
                                     [&](const BusstopListEntry& o) { return o.object_id == e.object_id; });
        if (!dup) out.push_back(std::move(e));
    }
    return true;
}

bool save_busstops_cfg(const std::filesystem::path& path, const std::vector<BusstopListEntry>& list) {
    std::vector<std::string> out;
    write_header(out, "---------------------------", "Time Table BusStopList File");
    for (const auto& e : list) {
        out.push_back("[busstop]");
        out.push_back(e.name);
        out.push_back(std::to_string(e.tile_index));
        out.push_back(std::to_string(e.object_id));
        out.push_back(fmt_f(e.offset));
        out.push_back(std::to_string(e.aliases.size()));
        for (const auto& a : e.aliases) out.push_back(a);
        out.push_back(std::to_string(e.extra_int));
        out.push_back(e.tail);
        out.emplace_back();
    }
    return write_lines(path, out);
}

// ─── StnLinks.cfg ───────────────────────────────────────────────────────────
bool load_stnlinks_cfg(const std::filesystem::path& path, std::vector<StnLink>& out) {
    out.clear();
    if (!omsi::vfs::exists(path)) return true;
    LineCursor c(read_lines(path));
    while (!c.eof()) {
        const std::string t = trim(c.next());
        if (t == "[StnLink]") {
            StnLink L{};
            L.length = c.next_f();
            L.from_id = c.next_i();
            L.to_id = c.next_i();
            L.p10 = c.next_f();
            L.p14 = c.next_f();
            L.p38 = c.next_f();
            L.p3c = c.next_f();
            L.i30 = c.next_i();
            L.i34 = c.next_i();
            out.push_back(std::move(L));
        } else if (t == "[StnLink_entry]" && !out.empty()) {
            StnLinkEntry e{};
            e.id = c.next_i();
            e.path_index = c.next_i();
            e.tile_index = c.next_i();
            e.length = c.next_f();
            e.i20 = c.next_i();
            const std::int32_t n = c.next_i();
            for (std::int32_t k = 0; k < n && !c.eof(); ++k) e.ints.push_back(c.next_i());
            const std::int32_t m = c.next_i();
            for (std::int32_t k = 0; k < m && !c.eof(); ++k) e.strings.push_back(c.next_s());
            out.back().entries.push_back(std::move(e));
        }
    }
    return true;
}

bool save_stnlinks_cfg(const std::filesystem::path& path, const std::vector<StnLink>& links) {
    std::vector<std::string> out;
    write_header(out, "---------------------------", "Time Table StnLinkList File");
    for (const auto& L : links) {
        out.push_back("[StnLink]");
        out.push_back(fmt_f(L.length));
        out.push_back(std::to_string(L.from_id));
        out.push_back(std::to_string(L.to_id));
        out.push_back(fmt_f(L.p10));
        out.push_back(fmt_f(L.p14));
        out.push_back(fmt_f(L.p38));
        out.push_back(fmt_f(L.p3c));
        out.push_back(std::to_string(L.i30));
        out.push_back(std::to_string(L.i34));
        out.emplace_back();
        for (std::size_t n = 0; n < L.entries.size(); ++n) {
            const auto& e = L.entries[n];
            out.push_back("   " + std::to_string(n) + ":");
            out.push_back("[StnLink_entry]");
            out.push_back(std::to_string(e.id));
            out.push_back(std::to_string(e.path_index));
            out.push_back(std::to_string(e.tile_index));
            out.push_back(fmt_f(e.length));
            out.push_back(std::to_string(e.i20));
            out.push_back(std::to_string(e.ints.size()));
            for (const auto v : e.ints) out.push_back(std::to_string(v));
            out.push_back(std::to_string(e.strings.size()));
            for (const auto& s : e.strings) out.push_back(s);
            out.emplace_back();
        }
    }
    return write_lines(path, out);
}

// ─── Track (.ttr) ───────────────────────────────────────────────────────────
bool load_track_ttr(const std::filesystem::path& path, Track& out, std::int32_t map_version) {
    out = {};
    out.path = path;
    out.name = path.stem().string();
    if (!omsi::vfs::exists(path)) return false;
    LineCursor c(read_lines(path));
    while (!c.eof()) {
        if (trim(c.next()) != "[track_entry]") continue;
        TrackEntry e{};
        e.id = c.next_i();
        e.path_index = c.next_i();
        if (map_version >= kTtExtendedVersion) {
            e.tile_index = c.next_i();
            e.internal_path_no = c.next_i();
            e.length = c.next_f();
            const std::int32_t n = c.next_i();
            for (std::int32_t k = 0; k < n && !c.eof(); ++k) e.extra.push_back(c.next_i());
        }
        out.entries.push_back(std::move(e));
    }
    return true;
}

bool save_track_ttr(const Track& track) {
    if (track.path.empty()) return false;
    std::vector<std::string> out;
    write_header(out, "-----------------------", "Time Table Track File");
    for (std::size_t n = 0; n < track.entries.size(); ++n) {
        const auto& e = track.entries[n];
        out.push_back("   " + std::to_string(n) + ":");
        out.push_back("[track_entry]");
        out.push_back(std::to_string(e.id));
        out.push_back(std::to_string(e.path_index));
        out.push_back(std::to_string(e.tile_index));
        out.push_back(std::to_string(e.internal_path_no));
        out.push_back(fmt_f(e.length));
        out.push_back(std::to_string(e.extra.size()));
        for (const auto v : e.extra) out.push_back(std::to_string(v));
        out.emplace_back();
    }
    return write_lines(track.path, out);
}

// ─── Trip (.ttp) ────────────────────────────────────────────────────────────
bool load_trip_ttp(const std::filesystem::path& path, Trip& out, std::int32_t map_version) {
    out = {};
    out.path = path;
    out.name = path.stem().string();
    if (!omsi::vfs::exists(path)) return false;
    LineCursor c(read_lines(path));
    TripProfile* cur_prof = nullptr;
    const auto n_st = [&]() { return static_cast<std::int32_t>(out.stations.size()); };
    while (!c.eof()) {
        const std::string t = trim(c.next());
        if (t == "[trip]") {
            out.track_name = c.next_s();
            out.terminus = c.next_s();
            out.line = c.next_s();
        } else if (t == "[trainreverse]") {
            out.train_reversed = true;
        } else if (t == "[station_typ2]") {
            TripStation s{};
            s.typ2 = true;
            s.object_id = c.next_i();
            s.track_entry_index = -1;
            s.tile_index = -1;
            out.stations.push_back(s);
        } else if (t == "[station]") {
            TripStation s{};
            s.object_id = c.next_i();
            s.track_entry_index = c.next_i();
            s.tile_index = -1;
            if (map_version >= kTtExtendedVersion) {
                s.name = c.next_s();
                s.tile_index = c.next_i();
                s.dist = c.next_f(); // +0x34
                s.f1 = c.next_f();   // +0x3C
                s.f2 = c.next_f();   // +0x38
                s.f3 = c.next_f();   // +0x20
            }
            out.stations.push_back(std::move(s));
        } else if (t == "[profile]") {
            out.profiles.emplace_back();
            cur_prof = &out.profiles.back();
            cur_prof->name = c.next_s();
            cur_prof->minutes = c.next_f();
        } else if (t == "[profile_man_arr_time]" || t == "[profile_man_dep_time]") {
            const std::int32_t idx = c.next_i();
            const float v = c.next_f();
            if (!cur_prof || idx < 0 || idx >= n_st()) continue; // OMSI: only if idx <= High(array)
            auto& vec = (t == "[profile_man_arr_time]") ? cur_prof->man_arr : cur_prof->man_dep;
            auto it = std::find_if(vec.begin(), vec.end(), [&](const auto& p) { return p.first == idx; });
            if (it != vec.end()) it->second = v; else vec.emplace_back(idx, v);
        } else if (t == "[profile_otherstopping]") {
            const std::int32_t idx = c.next_i();
            std::int32_t v = c.next_i();
            if (!cur_prof || idx < 0 || idx >= n_st()) continue;
            if (v > 0xFF) v = 0xFF; // stored in a byte
            if (v < 0) v = 0;
            auto& vec = cur_prof->other_stopping;
            auto it = std::find_if(vec.begin(), vec.end(), [&](const auto& p) { return p.first == idx; });
            if (it != vec.end()) it->second = v; else vec.emplace_back(idx, v);
        }
    }
    return true;
}

bool save_trip_ttp(const Trip& trip) {
    if (trip.path.empty()) return false;
    std::vector<std::string> out;
    write_header(out, "-----------------------", "Time Table Trip File");
    out.push_back("[trip]");
    out.push_back(trip.track_name);
    out.push_back(trip.terminus);
    out.push_back(trip.line);
    out.emplace_back();
    if (trip.train_reversed) {
        out.push_back("[trainreverse]");
        out.emplace_back();
    }
    out.push_back(".........................");
    out.push_back("        Stations");
    out.push_back(".........................");
    out.emplace_back();
    // OMSI picks the station layout from the RESOLVED track index (<0 => type 2), not from the
    // stations' own flags.
    const bool type2 = trip.is_type2();
    for (const auto& s : trip.stations) {
        if (type2) {
            out.push_back("[station_typ2]");
            out.push_back(std::to_string(s.object_id));
        } else {
            out.push_back("[station]");
            out.push_back(std::to_string(s.object_id));
            out.push_back(std::to_string(s.track_entry_index));
            out.push_back(s.name);
            out.push_back(std::to_string(s.tile_index));
            out.push_back(fmt_f(s.dist)); // +0x34
            out.push_back(fmt_f(s.f1));   // +0x3C
            out.push_back(fmt_f(s.f2));   // +0x38
            out.push_back(fmt_f(s.f3));   // +0x20
        }
        out.emplace_back();
    }
    out.emplace_back();
    out.push_back(".........................");
    out.push_back("        Profiles");
    out.push_back(".........................");
    out.emplace_back();
    const auto find = [](const std::vector<std::pair<std::int32_t, float>>& v, std::int32_t i) -> const float* {
        for (const auto& p : v) if (p.first == i) return &p.second;
        return nullptr;
    };
    for (const auto& p : trip.profiles) {
        out.push_back("[profile]");
        out.push_back(p.name);
        out.push_back(fmt_f(p.minutes));
        out.emplace_back();
        // Per-station array, in station order: arr, dep, otherstopping (no blank lines between).
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(trip.stations.size()); ++i) {
            if (const float* a = find(p.man_arr, i)) {
                out.push_back("[profile_man_arr_time]");
                out.push_back(std::to_string(i));
                out.push_back(fmt_f(*a));
            }
            if (const float* d = find(p.man_dep, i)) {
                out.push_back("[profile_man_dep_time]");
                out.push_back(std::to_string(i));
                out.push_back(fmt_f(*d));
            }
            for (const auto& o : p.other_stopping) {
                if (o.first == i && o.second != 0) {
                    out.push_back("[profile_otherstopping]");
                    out.push_back(std::to_string(i));
                    out.push_back(std::to_string(o.second));
                }
            }
        }
    }
    return write_lines(trip.path, out);
}

// ─── Line (.ttl) ────────────────────────────────────────────────────────────
bool load_line_ttl(const std::filesystem::path& path, Line& out) {
    out = {};
    out.path = path;
    out.name = path.stem().string();
    if (!omsi::vfs::exists(path)) return false;
    LineCursor c(read_lines(path));
    while (!c.eof()) {
        const std::string t = trim(c.next());
        if (t == "[newtour]") {
            Tour tour{};
            tour.number = c.next_s();
            tour.depot_name = c.next_s();
            const std::string mask = c.next_s();
            tour.day_mask = trim(mask).empty() ? kTourAllDays : to_i(mask);
            out.tours.push_back(std::move(tour));
        } else if (t == "[addtrip]") {
            TourTrip tt{};
            tt.trip_name = c.next_s();
            tt.profile_index = c.next_i();
            tt.departure_minutes = c.next_f();
            if (!out.tours.empty()) out.tours.back().trips.push_back(std::move(tt));
        } else if (t == "[userallowed]") {
            out.user_allowed = true;
        } else if (t == "[priority]") {
            std::int32_t p = c.next_i();
            if (p > 0xFF) p = 0xFF;
            if (p < 0) p = 0;
            out.priority = p;
        }
    }
    return true;
}

bool save_line_ttl(const Line& line) {
    if (line.path.empty()) return false;
    std::vector<std::string> out;
    write_header(out, "-----------------------", "Time Table Line File");
    if (line.user_allowed) {
        out.push_back("[userallowed]");
        out.emplace_back();
    }
    out.push_back("[priority]");
    out.push_back(std::to_string(line.priority));
    for (const auto& tour : line.tours) {
        out.push_back("------------------------------------");
        out.emplace_back();
        out.push_back("[newtour]");
        out.push_back(tour.number);
        out.push_back(tour.depot_name);
        out.push_back(std::to_string(tour.day_mask & 0x3FF));
        out.emplace_back();
        out.push_back("------------------------------------");
        out.emplace_back();
        for (const auto& tt : tour.trips) {
            out.push_back("[addtrip]");
            out.push_back(tt.trip_name);
            out.push_back(std::to_string(tt.profile_index));
            out.push_back(fmt_f(tt.departure_minutes));
            out.emplace_back();
        }
    }
    return write_lines(line.path, out);
}

// ─── Whole folder ───────────────────────────────────────────────────────────
bool load_ttdata(const std::filesystem::path& ttdata_dir_in, TimeTableData& out, std::int32_t map_version) {
    // Take a COPY first: callers pass `out.root` itself (TimeTableMan::load), and `out = {}` below
    // would otherwise wipe the very path we are about to read — the original code did exactly that
    // and never loaded a single file.
    const std::filesystem::path ttdata_dir = ttdata_dir_in;
    out = {};
    out.root = ttdata_dir;
    out.map_version = map_version;
    if (ttdata_dir.empty() || !std::filesystem::is_directory(ttdata_dir)) {
        omsi::log::warn("TTData missing: " + ttdata_dir.string());
        return false;
    }
    // Busstops.cfg / StnLinks.cfg exist only for map version > 10 (LoadBusstopList / LoadStnLinkList).
    if (map_version >= kTtExtendedVersion) {
        (void)load_busstops_cfg(ttdata_dir / "Busstops.cfg", out.busstops);
        (void)load_stnlinks_cfg(ttdata_dir / "StnLinks.cfg", out.stn_links);
    }

    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(ttdata_dir, ec)) {
        if (ec || !e.is_regular_file()) continue;
        std::string low = e.path().extension().string();
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (low == ".ttr") {
            Track tr;
            if (load_track_ttr(e.path(), tr, map_version)) out.tracks.push_back(std::move(tr));
        } else if (low == ".ttp") {
            Trip tp;
            if (load_trip_ttp(e.path(), tp, map_version)) out.trips.push_back(std::move(tp));
        } else if (low == ".ttl") {
            Line ln;
            if (load_line_ttl(e.path(), ln)) out.lines.push_back(std::move(ln));
        }
    }
    // FindAllFiles returns directory order; keep a stable order so track/trip indices are
    // reproducible between sessions (indices are persisted nowhere but used by the UI).
    std::sort(out.tracks.begin(), out.tracks.end(), [](const Track& a, const Track& b) { return a.name < b.name; });
    std::sort(out.trips.begin(), out.trips.end(), [](const Trip& a, const Trip& b) { return a.name < b.name; });
    std::sort(out.lines.begin(), out.lines.end(), [](const Line& a, const Line& b) { return a.name < b.name; });
    omsi::log::info("TTData loaded busstops=" + std::to_string(out.busstops.size()) +
                    " stnlinks=" + std::to_string(out.stn_links.size()) +
                    " tracks=" + std::to_string(out.tracks.size()) +
                    " trips=" + std::to_string(out.trips.size()) +
                    " lines=" + std::to_string(out.lines.size()));
    return true;
}

bool save_ttdata(const TimeTableData& data) {
    if (data.root.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(data.root, ec);
    bool ok = true;
    std::vector<std::filesystem::path> written;
    for (const auto& t : data.tracks) {
        Track tr = t;
        if (tr.path.empty()) tr.path = data.root / (tr.name + ".ttr");
        ok = save_track_ttr(tr) && ok;
        written.push_back(tr.path);
    }
    for (const auto& t : data.trips) {
        Trip tp = t;
        if (tp.path.empty()) tp.path = data.root / (tp.name + ".ttp");
        ok = save_trip_ttp(tp) && ok;
        written.push_back(tp.path);
    }
    for (const auto& l : data.lines) {
        Line ln = l;
        if (ln.path.empty()) ln.path = data.root / (ln.name + ".ttl");
        ok = save_line_ttl(ln) && ok;
        written.push_back(ln.path);
    }
    // TTimeTableMan.Save deletes every *.ttr/*.ttp/*.ttl of the TTData dir before rewriting the
    // ones still in memory; otherwise deleted/renamed tracks, trips and lines come back on the next
    // load. We rewrite first and only then drop the leftovers, so a failed write loses nothing.
    if (ok) {
        const auto norm = [](std::filesystem::path p) {
            std::error_code e2;
            auto c = std::filesystem::weakly_canonical(p, e2);
            return e2 ? p.lexically_normal() : c;
        };
        std::vector<std::filesystem::path> keep;
        for (const auto& w : written) keep.push_back(norm(w));
        for (const auto& e : std::filesystem::directory_iterator(data.root, ec)) {
            if (ec || !e.is_regular_file()) continue;
            std::string low = e.path().extension().string();
            for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (low != ".ttr" && low != ".ttp" && low != ".ttl") continue;
            if (std::find(keep.begin(), keep.end(), norm(e.path())) == keep.end()) {
                std::error_code rec;
                std::filesystem::remove(e.path(), rec);
            }
        }
    }
    if (data.map_version >= kTtExtendedVersion) {
        ok = save_stnlinks_cfg(data.root / "StnLinks.cfg", data.stn_links) && ok;
        ok = save_busstops_cfg(data.root / "Busstops.cfg", data.busstops) && ok;
    }
    return ok;
}

bool mirror_ttdata(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::error_code ec;
    if (src.empty() || dst.empty() || !std::filesystem::is_directory(src, ec)) return false;
    if (std::filesystem::equivalent(src, dst, ec)) return true;
    std::filesystem::create_directories(dst, ec);
    const auto is_tt = [](const std::filesystem::path& p) {
        std::string low = p.extension().string();
        for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return low == ".ttr" || low == ".ttp" || low == ".ttl";
    };
    bool ok = true;
    std::vector<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator(src, ec)) {
        if (ec || !e.is_regular_file()) continue;
        const auto fn = e.path().filename().string();
        if (!is_tt(e.path()) && fn != "Busstops.cfg" && fn != "StnLinks.cfg") continue;
        std::error_code cec;
        omsi::fsutil::break_link(dst / fn); // copy-on-write workspace: never write through a link
        std::filesystem::copy_file(e.path(), dst / fn, std::filesystem::copy_options::overwrite_existing, cec);
        if (cec) ok = false;
        names.push_back(fn);
    }
    if (ok) { // drop files that no longer exist in the saved set (Save deletes *.ttr/*.ttp/*.ttl first)
        for (const auto& e : std::filesystem::directory_iterator(dst, ec)) {
            if (ec || !e.is_regular_file() || !is_tt(e.path())) continue;
            if (std::find(names.begin(), names.end(), e.path().filename().string()) == names.end()) {
                std::error_code rec;
                std::filesystem::remove(e.path(), rec);
            }
        }
    }
    return ok;
}

TrackEntry make_track_entry_from_path(std::int32_t owner_id, std::int32_t path_index, std::int32_t tile_index,
                                      std::int32_t internal_path_no, float length) {
    TrackEntry e;
    e.id = owner_id;
    e.path_index = path_index;
    e.tile_index = tile_index;
    e.internal_path_no = internal_path_no;
    e.length = length;
    return e;
}


namespace {
bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}
} // namespace

// Mirrors TTimeTableMan.RefreshTrip / RefreshAllTracks message texts (Unit121).
void validate_ttdata(TimeTableData& data) {
    data.errors.clear();
    for (const auto& tr : data.tracks) {
        if (tr.entries.empty())
            data.errors.push_back("Track \"" + tr.name + "\".ttr has no track segments.");
    }
    for (const auto& tp : data.trips) {
        // Track lookup is case-insensitive; an unknown track silently turns the trip into type 2.
        bool track_found = false;
        for (const auto& tr : data.tracks)
            if (ieq(tr.name, tp.track_name)) { track_found = true; break; }
        if (!track_found && tp.stations.size() < 2)
            data.errors.push_back("Trip \"" + tp.name + ".ttp\"\r\nTrips having no track have to have at least two busstops!");
        for (const auto& st : tp.stations) {
            const bool known = std::any_of(data.busstops.begin(), data.busstops.end(),
                [&](const BusstopListEntry& b) { return b.object_id == st.object_id; });
            if (!known)
                data.errors.push_back("Trip \"" + tp.name + ".ttp\"\r\nBusstop Nr. " + std::to_string(st.object_id) + " unknown!");
        }
        if (!track_found) {
            for (std::size_t i = 0; i + 1 < tp.stations.size(); ++i) {
                const auto a = tp.stations[i].object_id, b = tp.stations[i + 1].object_id;
                const bool link = std::any_of(data.stn_links.begin(), data.stn_links.end(),
                    [&](const StnLink& L) { return L.from_id == a && L.to_id == b; });
                if (!link)
                    data.errors.push_back("Trip \"" + tp.name + ".ttp\"\r\nNo StnLink between " + std::to_string(a) +
                                          " and " + std::to_string(b) + " found!");
            }
        }
    }
    for (const auto& L : data.stn_links) {
        if (L.from_id == 0 || L.to_id == 0)
            data.errors.push_back("StnLink missing start/end busstop id.");
        if (L.entries.size() < 2)
            data.errors.push_back("StnLink has to contain at least two paths!");
    }
}

} // namespace omsi::tt
