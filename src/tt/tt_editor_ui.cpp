#include "omsi/tt_editor_ui.hpp"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_map>
#include <cstdio>
#include <cstring>

namespace omsi::tt::ui {
namespace {

void say(const Host& h, const std::string& s) {
    if (h.status) h.status(s);
}

void copy_buf(char* dst, std::size_t n, const std::string& s) {
    std::snprintf(dst, n, "%s", s.c_str());
}

bool in_range(int i, std::size_t n) { return i >= 0 && i < static_cast<int>(n); }

const char* kDayNames[10] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun", "Holiday",
                             "Only on school holidays (~)", "Not on school holidays (%)"};
// mask bit for each checkbox: bits 0..7 = days, 0x100 / 0x200 = school-holiday flags
const int kDayBits[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x100, 0x200};

std::string fmt_hm(float minutes) {
    const int t = static_cast<int>(minutes + 0.5f);
    char b[16];
    std::snprintf(b, sizeof(b), "%02d:%02d", (t / 60) % 100, ((t % 60) + 60) % 60);
    return b;
}

// Centre every sub-window on the editor viewport the first time it opens (they used to spawn under
// the 400 px side panel and looked "missing"). Focus is requested ONLY on the opening frame: asking for
// it every frame made ImGui close every combo / popup of the window the moment it opened.
void place_window(const char* key, float w, float h) {
    static std::unordered_map<std::string, int> last_frame;
    int& lf = last_frame[key];
    const int fr = ImGui::GetFrameCount();
    const bool opening = (fr - lf) > 1;
    lf = fr;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f - 60.f, vp->WorkPos.y + vp->WorkSize.y * 0.5f - 20.f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.f, 220.f), ImVec2(FLT_MAX, FLT_MAX));
    if (opening) ImGui::SetNextWindowFocus();
}

} // namespace

TtEditorUi& global_ui() {
    static TtEditorUi ui;
    return ui;
}

// ── Prompts ────────────────────────────────────────────────────────────────

void TtEditorUi::begin_prompt(Prompt kind, const std::string& title, const std::string& initial) {
    prompt_ = kind;
    prompt_title_ = title;
    prompt_error_.clear();
    copy_buf(prompt_buf_, sizeof(prompt_buf_), initial);
    prompt_open_request_ = true;
}

bool TtEditorUi::run_prompt(TimeTableMan& man, const Host& host, const std::string& name) {
    bool ok = false;
    switch (prompt_) {
    case Prompt::CreateTrack:
        ok = man.add_track(name) >= 0;
        if (ok) say(host, "Track created: " + name + " (written on Save)");
        break;
    case Prompt::CopyTrack:
        ok = in_range(man.selected_track, man.data.tracks.size()) && man.add_track(name, man.selected_track) >= 0;
        if (ok) say(host, "Track copied: " + name);
        break;
    case Prompt::CreateTripT1:
        ok = in_range(man.selected_track, man.data.tracks.size()) && man.add_trip(man.selected_track, name) >= 0;
        if (ok) say(host, "Trip (type 1) created: " + name);
        break;
    case Prompt::CreateTripT2:
        ok = man.add_trip(-1, name) >= 0;
        if (ok) say(host, "Trip (type 2) created: " + name);
        break;
    case Prompt::CopyTrip:
        ok = in_range(man.selected_trip, man.data.trips.size()) && man.add_trip(-1, name, man.selected_trip) >= 0;
        if (ok) say(host, "Trip copied: " + name);
        break;
    case Prompt::CopyBoth:
        ok = man.copy_track_and_trip(name);
        if (ok) say(host, "Track & trip copied: " + name);
        break;
    case Prompt::NewLine:
        ok = man.add_line(name) >= 0;
        if (ok) { tt_line_ = man.selected_line; tt_tour_ = -1; say(host, "Line created: " + name); }
        break;
    case Prompt::NewTour:
        ok = man.add_tour(tt_line_, name, "", kTourAllDays) >= 0;
        if (ok) {
            tt_tour_ = static_cast<int>(man.data.lines[static_cast<std::size_t>(tt_line_)].tours.size()) - 1;
            tour_buf_tour_ = -2;
        }
        break;
    case Prompt::NewProfile:
        ok = man.add_profile(man.selected_trip, name);
        if (ok) prof_sel_ = static_cast<int>(man.data.trips[static_cast<std::size_t>(man.selected_trip)].profiles.size()) - 1;
        break;
    case Prompt::None: break;
    }
    if (ok) buf_trip_ = -2;
    return ok;
}

bool TtEditorUi::submit_prompt(TimeTableMan& man, const Host& host, const std::string& text) {
    if (prompt_ == Prompt::None) return false;
    if (run_prompt(man, host, text)) { prompt_ = Prompt::None; prompt_error_.clear(); return true; }
    prompt_error_ = "Problem: filename exists or is invalid!";
    return false;
}

void TtEditorUi::draw_name_prompt(TimeTableMan& man, const Host& host) {
    if (prompt_ == Prompt::None) return;
    const std::string title = prompt_title_ + "##tt_prompt";
    if (prompt_open_request_) { ImGui::OpenPopup(title.c_str()); prompt_open_request_ = false; }
    ImGui::SetNextWindowSizeConstraints(ImVec2(320.f, 0.f), ImVec2(520.f, 400.f));
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::SetNextItemWidth(300.f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##name", prompt_buf_, sizeof(prompt_buf_), ImGuiInputTextFlags_EnterReturnsTrue);
        if (!prompt_error_.empty()) ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "%s", prompt_error_.c_str());
        bool ok = ImGui::Button("OK", ImVec2(90.f, 0.f)) || enter;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(90.f, 0.f))) {
            prompt_ = Prompt::None;
            ImGui::CloseCurrentPopup();
        } else if (ok) {
            if (submit_prompt(man, host, prompt_buf_)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void TtEditorUi::sync_trip_buffers(const TimeTableMan& man) {
    const int t = man.selected_trip;
    const std::string nm = in_range(t, man.data.trips.size()) ? man.data.trips[static_cast<std::size_t>(t)].name : "";
    if (t == buf_trip_ && nm == buf_trip_name_) return;
    buf_trip_ = t;
    buf_trip_name_ = nm;
    if (in_range(t, man.data.trips.size())) {
        const auto& tp = man.data.trips[static_cast<std::size_t>(t)];
        copy_buf(terminus_buf_, sizeof(terminus_buf_), tp.terminus);
        copy_buf(line_buf_, sizeof(line_buf_), tp.line);
    } else {
        terminus_buf_[0] = 0;
        line_buf_[0] = 0;
    }
}

// ── Tracks tab ─────────────────────────────────────────────────────────────

void TtEditorUi::draw_tracks_tab(TimeTableMan& man, const Host& host, int& mode) {
    sync_trip_buffers(man);
    ImGui::TextUnformatted("Mode");
    ImGui::BeginGroup();
    const char* modes[3] = {"Add Trackentry", "Add Station", "Show Timetable of Station"};
    mode = std::clamp(mode, 0, 2);
    for (int i = 0; i < 3; ++i)
        if (ImGui::RadioButton(modes[i], mode == i)) {
            mode = i;
            if (host.set_mode) host.set_mode(i);
        }
    ImGui::EndGroup();
    ImGui::SameLine();
    if (ImGui::Button("Find Errors\non loaded tiles", ImVec2(120.f, 54.f))) {
        man.validate_all();
        win_errors_ = true;
        if (host.find_errors_on_tiles) host.find_errors_on_tiles();
        say(host, man.data.errors.empty() ? "No errors found" : std::to_string(man.data.errors.size()) + " errors found");
    }
    ImGui::TextDisabled("%s", man.status_text().c_str());
    ImGui::Spacing();

    // ── Tracks ──
    ImGui::TextUnformatted("Tracks");
    ImGui::SameLine();
    if (ImGui::SmallButton("Create...")) begin_prompt(Prompt::CreateTrack, "New Track", "");
    ImGui::SameLine();
    ImGui::TextDisabled("%s", man.track_length_label().c_str());

    {
        const auto names = man.track_combo_strings();
        const std::string prev = names.empty() ? "(no tracks)"
            : (in_range(man.selected_track, names.size()) ? names[static_cast<std::size_t>(man.selected_track)] : names[0]);
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::BeginCombo("##tt_track_combo", prev.c_str())) {
            for (int i = 0; i < static_cast<int>(names.size()); ++i) {
                if (ImGui::Selectable(names[static_cast<std::size_t>(i)].c_str(), man.selected_track == i)) {
                    man.selected_track = i;
                    man.selected_track_entry = -1;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::BeginChild("##tt_track_entries", ImVec2(0.f, 100.f), true);
    {
        const auto rows = man.track_entry_list_strings();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            if (ImGui::Selectable(rows[static_cast<std::size_t>(i)].c_str(), man.selected_track_entry + 1 == i)) {
                man.on_track_entry_list_click(i);
                if (host.camera_jump) host.camera_jump();
            }
        }
        if (rows.size() <= 1) ImGui::TextDisabled("(empty — Add Trackentry + click path)");
    }
    ImGui::EndChild();
    if (ImGui::Button("Delete act. Track Entry")) {
        if (!man.del_track_entry(man.selected_track, man.selected_track_entry))
            say(host, "Delete Track Entry: select a track entry first");
        else say(host, "Track entry deleted (trips on this track were adjusted)");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ── Trips ──
    ImGui::TextUnformatted("Trips");
    if (ImGui::Button("Create New (type 1) with act. Track...")) {
        if (in_range(man.selected_track, man.data.tracks.size()))
            begin_prompt(Prompt::CreateTripT1, "New Trip (type 1) — track " +
                         man.data.tracks[static_cast<std::size_t>(man.selected_track)].name, "");
        else say(host, "No track selected!");
    }
    if (ImGui::Button("Create New (type 2) ...")) begin_prompt(Prompt::CreateTripT2, "New Trip (type 2)", "");
    {
        const auto names = man.trip_combo_strings();
        const std::string prev = names.empty() ? "(no trips)"
            : (in_range(man.selected_trip, names.size()) ? names[static_cast<std::size_t>(man.selected_trip)] : names[0]);
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::BeginCombo("##tt_trip_combo", prev.c_str())) {
            for (int i = 0; i < static_cast<int>(names.size()); ++i) {
                if (ImGui::Selectable(names[static_cast<std::size_t>(i)].c_str(), man.selected_trip == i)) {
                    man.selected_trip = i;
                    man.selected_trip_station = -1;
                    // ComboBox3Change: selecting a type 1 trip also selects its track
                    const auto& tp = man.data.trips[static_cast<std::size_t>(i)];
                    const int ti = man.find_track_by_name(tp.track_name);
                    if (ti >= 0 && ti != man.selected_track) { man.selected_track = ti; man.selected_track_entry = -1; }
                }
            }
            ImGui::EndCombo();
        }
    }
    sync_trip_buffers(man);
    const bool have_trip = in_range(man.selected_trip, man.data.trips.size());
    Trip* tp = have_trip ? &man.data.trips[static_cast<std::size_t>(man.selected_trip)] : nullptr;

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Terminus:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x - 56.f));
    if (ImGui::InputText("##terminus", terminus_buf_, sizeof(terminus_buf_)) && tp) {
        tp->terminus = terminus_buf_;
        man.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("->T2")) {
        if (!tp) say(host, "No trip selected!");
        else if (tp->is_type2()) say(host, "Trip is already type 2");
        else if (tp->stations.size() < 2) say(host, "Trips having no track have to have at least two busstops!");
        else { man.change_trip_to_type2(man.selected_trip); say(host, "Trip changed to type 2"); }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Line:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.f, ImGui::GetContentRegionAvail().x - 90.f));
    if (ImGui::InputText("##line", line_buf_, sizeof(line_buf_)) && tp) {
        tp->line = line_buf_;
        man.dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Assistent...")) {
        if (!tp) say(host, "No trip selected!");
        else win_assistant_ = true;
    }

    {
        const float list_w = std::max(100.f, ImGui::GetContentRegionAvail().x - 52.f);
        ImGui::BeginChild("##tt_stations", ImVec2(list_w, 110.f), true);
        const auto rows = man.trip_station_list_strings();
        for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
            if (ImGui::Selectable(rows[static_cast<std::size_t>(i)].c_str(), man.selected_trip_station + 1 == i)) {
                man.on_trip_station_list_click(i);
                if (host.camera_jump) host.camera_jump();
            }
        }
        if (rows.size() <= 1) ImGui::TextDisabled("(no stations)");
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::Button("Add\nfrom\nList...", ImVec2(48.f, 110.f))) {
            if (!tp) say(host, "No trip selected!");
            else { win_add_list_ = true; al_sel_ = -1; }
        }
    }
    if (ImGui::Button("Delete ac. Station")) {
        if (!man.del_trip_busstop(man.selected_trip, man.selected_trip_station))
            say(host, "Delete Station: select a station first");
    }
    ImGui::SameLine();
    if (ImGui::Button("Edit Profiles...")) {
        if (!tp) say(host, "No trip selected!");
        else { win_profiles_ = true; prof_sel_ = std::clamp(prof_sel_, 0, std::max(0, static_cast<int>(tp->profiles.size()) - 1)); dur_for_ = -1; }
    }
    bool rev = tp ? tp->train_reversed : false;
    if (ImGui::Checkbox("Train reversed by default", &rev) && tp) {
        tp->train_reversed = rev;
        man.dirty = true;
    }
    if (ImGui::Button("Copy Track")) {
        if (in_range(man.selected_track, man.data.tracks.size()))
            begin_prompt(Prompt::CopyTrack, "Copy Track " + man.data.tracks[static_cast<std::size_t>(man.selected_track)].name, "");
        else say(host, "No track selected!");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy Trip")) {
        if (tp) begin_prompt(Prompt::CopyTrip, "Copy Trip " + tp->name, "");
        else say(host, "No trip selected!");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy T & T")) {
        if (tp && in_range(man.selected_track, man.data.tracks.size()))
            begin_prompt(Prompt::CopyBoth, "Copy Track & Trip", "");
        else say(host, "Select a track and a trip first!");
    }
    if (ImGui::Button("Edit Timetable...", ImVec2(-1.f, 0.f))) {
        win_timetable_ = true;
        if (tt_line_ < 0 && !man.data.lines.empty()) tt_line_ = std::max(0, man.selected_line);
    }
}

// ── Add from List (Tform_tripAddStop) ──────────────────────────────────────

void TtEditorUi::draw_add_from_list(TimeTableMan& man, const Host& host) {
    if (!win_add_list_) return;
    place_window("Add Busstop to Trip", 560.f, 380.f);
    if (ImGui::Begin("Add Busstop to Trip", &win_add_list_)) {
        if (!in_range(man.selected_trip, man.data.trips.size())) {
            ImGui::TextUnformatted("No trip selected!");
        } else {
            const auto& tp = man.data.trips[static_cast<std::size_t>(man.selected_trip)];
            ImGui::Text("Trip: %s (%s)", tp.name.c_str(), tp.is_type2() ? "type 2" : "type 1");
            ImGui::BeginChild("##al_list", ImVec2(0.f, -34.f), true);
            for (int i = 0; i < static_cast<int>(man.data.busstops.size()); ++i) {
                const auto& b = man.data.busstops[static_cast<std::size_t>(i)];
                std::string label = b.tail + " " + b.name;
                const auto used = man.trips_using_busstop(b.object_id);
                if (used.empty()) label += "\t(not used)";
                else {
                    label += "\tused by: ";
                    for (std::size_t k = 0; k < used.size(); ++k) label += (k ? ", " : "") + used[k];
                }
                ImGui::PushID(i);
                if (ImGui::Selectable(label.c_str(), al_sel_ == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                    al_sel_ = i;
                }
                ImGui::PopID();
            }
            if (man.data.busstops.empty()) ImGui::TextDisabled("Busstops.cfg is empty or missing");
            ImGui::EndChild();
            const bool can = in_range(al_sel_, man.data.busstops.size());
            if (!can) ImGui::BeginDisabled();
            if (ImGui::Button("Add") && can) {
                const auto& b = man.data.busstops[static_cast<std::size_t>(al_sel_)];
                const bool t2 = tp.is_type2();
                if (!t2 && man.selected_track_entry < 0) {
                    say(host, "Select the track entry this station belongs to first");
                } else if (man.add_trip_busstop(man.selected_trip, b.object_id, b.name, b.tile_index,
                                                t2 ? -1 : man.selected_track_entry, man.selected_trip_station + 1)) {
                    say(host, "Station added: " + b.name);
                } else {
                    say(host, "Could not add station");
                }
            }
            if (!can) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Close")) win_add_list_ = false;
        }
    }
    ImGui::End();
}

// ── Trip assistant (Tform_tripassistant) ───────────────────────────────────

void TtEditorUi::draw_assistant(TimeTableMan& man, const Host& host) {
    if (!win_assistant_) return;
    place_window("Trip Assistant", 420.f, 360.f);
    if (ImGui::Begin("Trip Assistant", &win_assistant_)) {
        if (!in_range(man.selected_trip, man.data.trips.size())) {
            ImGui::TextUnformatted("No trip selected!");
        } else {
            const auto& tp = man.data.trips[static_cast<std::size_t>(man.selected_trip)];
            if (!tp.is_type2()) {
                ImGui::TextWrapped("The assistant builds type 2 trips (trips without a track) from the StnLinks. "
                                   "Use ->T2 or create a type 2 trip.");
            } else {
                if (tp.stations.empty()) ImGui::TextUnformatted("Pick the first busstop:");
                else ImGui::Text("Last busstop: %s (%d)", man.busstop_name(tp.stations.back().object_id).c_str(),
                                 static_cast<int>(tp.stations.back().object_id));
                ImGui::Separator();
                const auto next = man.assistant_next_busstops(man.selected_trip);
                ImGui::BeginChild("##asst_next", ImVec2(0.f, -34.f), true);
                for (const auto id : next) {
                    std::string nm = man.busstop_name(id);
                    if (nm.empty()) nm = "Busstop " + std::to_string(id);
                    ImGui::PushID(static_cast<int>(id));
                    if (ImGui::Button(nm.c_str(), ImVec2(-1.f, 0.f))) {
                        man.add_trip_busstop(man.selected_trip, id, man.busstop_name(id), 0, -1,
                                             static_cast<int>(tp.stations.size()));
                        say(host, "Station added: " + nm);
                    }
                    ImGui::PopID();
                }
                if (next.empty()) ImGui::TextDisabled("No StnLink starts here (see the Est. tab).");
                ImGui::EndChild();
                if (ImGui::Button("Remove last") && !tp.stations.empty())
                    man.del_trip_busstop(man.selected_trip, static_cast<int>(tp.stations.size()) - 1);
                ImGui::SameLine();
            }
            if (ImGui::Button("Close")) win_assistant_ = false;
        }
    }
    ImGui::End();
}

// ── Shared helpers of the Time Table Editor / Edit Profiles windows ───────────

namespace {

// Hover help: what the control does and when to use it (derived from Unit134 handlers).
void tip(const char* text) {
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void group_title(const char* title) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.45f, 1.f), "%s", title);
    ImGui::Separator();
}

std::string fmt_hms(float minutes) {
    const int t = static_cast<int>(minutes * 60.f + 0.5f);
    char b[24];
    std::snprintf(b, sizeof(b), "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    return b;
}

// Hour:minute input over a value kept in minutes. Returns true when edited.
bool time_input(const char* id, int& minutes, float w = 46.f) {
    int h = minutes / 60, m = minutes % 60;
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(w);
    bool c = ImGui::InputInt("##h", &h, 0, 0);
    ImGui::SameLine(0.f, 3.f);
    ImGui::TextUnformatted(":");
    ImGui::SameLine(0.f, 3.f);
    ImGui::SetNextItemWidth(w);
    c |= ImGui::InputInt("##m", &m, 0, 0);
    ImGui::PopID();
    if (c) minutes = std::clamp(h, 0, 99) * 60 + std::clamp(m, 0, 59);
    return c;
}

// Day / holiday condition checkboxes in OMSI's 3-column layout (bit values of Tour::day_mask).
struct CondCell { const char* name; int bit; };
const CondCell kCondGrid[5][3] = {
    {{"Mon", 0x01}, {"Sat", 0x20}, {"No Hols", 0x200}},
    {{"Tue", 0x02}, {"Sun", 0x40}, {"Hols", 0x100}},
    {{"Wed", 0x04}, {"Holiday", 0x80}, {nullptr, 0}},
    {{"Thur", 0x08}, {nullptr, 0}, {nullptr, 0}},
    {{"Fri", 0x10}, {nullptr, 0}, {nullptr, 0}},
};
const char* kStopTypes[3] = {"on demand", "always", "never"};

void sort_tour_by_time(Tour& t) {
    std::stable_sort(t.trips.begin(), t.trips.end(),
                     [](const TourTrip& a, const TourTrip& b) { return a.departure_minutes < b.departure_minutes; });
}

ImU32 trip_color(const std::string& name) {
    const std::size_t h = std::hash<std::string>{}(name);
    static const ImU32 pal[] = {IM_COL32(60, 190, 190, 255), IM_COL32(40, 40, 40, 255), IM_COL32(200, 170, 40, 255),
                                IM_COL32(210, 90, 120, 255), IM_COL32(120, 170, 70, 255), IM_COL32(110, 130, 220, 255)};
    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
}

float trip_duration(const TimeTableMan& man, const TourTrip& e) {
    const int tix = man.find_trip_by_name(e.trip_name);
    if (tix < 0) return 10.f;
    const auto& pfs = man.data.trips[static_cast<std::size_t>(tix)].profiles;
    if (in_range(e.profile_index, pfs.size())) return pfs[static_cast<std::size_t>(e.profile_index)].minutes;
    return 10.f;
}

} // namespace

// Line + tour pickers and the tour properties (left column, shared by the OMSI and Modern tabs).
void TtEditorUi::draw_tt_line_tour(TimeTableMan& man, bool modern) {
    (void)modern;
    group_title("Line");
    const std::string lprev = in_range(tt_line_, man.data.lines.size())
        ? man.data.lines[static_cast<std::size_t>(tt_line_)].name : "(no lines)";
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo("##tt_line", lprev.c_str())) {
        for (int i = 0; i < static_cast<int>(man.data.lines.size()); ++i)
            if (ImGui::Selectable((man.data.lines[static_cast<std::size_t>(i)].name + "##l" + std::to_string(i)).c_str(), tt_line_ == i)) {
                tt_line_ = i; man.selected_line = i;
                tt_tour_ = man.data.lines[static_cast<std::size_t>(i)].tours.empty() ? -1 : 0;
                tt_sel_trip_ = -1; tour_buf_tour_ = -2;
            }
        ImGui::EndCombo();
    }
    tip("Linea (.ttl) a editar. Cada linea agrupa sus recorridos (tours) y los viajes (trips) que hace cada uno.");
    if (ImGui::Button("Create", ImVec2(110.f, 0.f))) begin_prompt(Prompt::NewLine, "New Line", "");
    tip("Crea una linea nueva (archivo .ttl). El nombre debe ser un nombre de archivo valido y no repetido.");
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(70.f, 0.f)) && in_range(tt_line_, man.data.lines.size())) {
        man.del_line(tt_line_);
        tt_line_ = std::min(tt_line_, static_cast<int>(man.data.lines.size()) - 1);
        tt_tour_ = -1; tt_sel_trip_ = -1; tour_buf_tour_ = -2;
        return;
    }
    tip("Elimina la linea completa con todos sus tours. El archivo se borra al guardar el mapa.");
    if (!in_range(tt_line_, man.data.lines.size())) { ImGui::TextDisabled("Create a line first."); return; }
    auto& ln = man.data.lines[static_cast<std::size_t>(tt_line_)];
    bool ua = ln.user_allowed;
    if (ImGui::Checkbox("User is allowed to drive this line", &ua)) man.set_line_props(tt_line_, ua, ln.priority);
    tip("Si esta marcada, el jugador puede elegir esta linea al iniciar un viaje. Desmarcala para lineas solo de trafico IA.");
    int prio = std::clamp(ln.priority, 1, 4);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Priority:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::SliderInt("##prio", &prio, 1, 4)) man.set_line_props(tt_line_, ln.user_allowed, prio);
    tip("Prioridad de la linea frente a otras al generar trafico IA (1 = baja, 4 = alta).");

    group_title("Tour");
    const std::string tprev = in_range(tt_tour_, ln.tours.size()) ? ln.tours[static_cast<std::size_t>(tt_tour_)].number : "(no tours)";
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo("##tt_tour", tprev.c_str())) {
        for (int i = 0; i < static_cast<int>(ln.tours.size()); ++i)
            if (ImGui::Selectable((ln.tours[static_cast<std::size_t>(i)].number + "##t" + std::to_string(i)).c_str(), tt_tour_ == i)) {
                tt_tour_ = i; tour_buf_tour_ = -2; tt_sel_trip_ = -1;
            }
        ImGui::EndCombo();
    }
    tip("Recorrido (tour): una secuencia de viajes que hace un mismo vehiculo durante el dia.");
    if (ImGui::Button("Create##tour", ImVec2(110.f, 0.f))) begin_prompt(Prompt::NewTour, "New Tour", "");
    tip("Crea un tour nuevo dentro de la linea. Se crea valido todos los dias.");
    ImGui::SameLine();
    if (ImGui::Button("Delete##tour", ImVec2(70.f, 0.f)) && in_range(tt_tour_, ln.tours.size())) {
        man.del_tour(tt_line_, tt_tour_);
        tt_tour_ = std::min(tt_tour_, static_cast<int>(ln.tours.size()) - 1);
        tour_buf_tour_ = -2; tt_sel_trip_ = -1;
        return;
    }
    tip("Elimina el tour seleccionado y todos sus viajes.");
    if (!in_range(tt_tour_, ln.tours.size())) return;
    auto& tour = ln.tours[static_cast<std::size_t>(tt_tour_)];
    if (tour_buf_line_ != tt_line_ || tour_buf_tour_ != tt_tour_) {
        copy_buf(tour_num_, sizeof(tour_num_), tour.number);
        copy_buf(tour_dep_, sizeof(tour_dep_), tour.depot_name);
        tour_buf_line_ = tt_line_; tour_buf_tour_ = tt_tour_;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Number:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.f);
    const bool e1 = ImGui::InputText("##tnum", tour_num_, sizeof(tour_num_), ImGuiInputTextFlags_EnterReturnsTrue);
    tip("Numero / nombre del tour (ej. 'Mo-So 1'). Enter para aplicar.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("AI-Veh.-Group:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.f);
    bool e2 = ImGui::InputText("##tdep", tour_dep_, sizeof(tour_dep_), ImGuiInputTextFlags_EnterReturnsTrue);
    tip("Grupo de vehiculos IA / cochera que opera este tour (se guarda como depot del tour). Enter para aplicar.");
    {
        std::vector<std::string> depots;
        for (const auto& l : man.data.lines)
            for (const auto& t : l.tours)
                if (!t.depot_name.empty() && std::find(depots.begin(), depots.end(), t.depot_name) == depots.end())
                    depots.push_back(t.depot_name);
        if (!depots.empty()) {
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::BeginCombo("##depots", "pick known group...")) {
                for (const auto& d : depots)
                    if (ImGui::Selectable(d.c_str())) { copy_buf(tour_dep_, sizeof(tour_dep_), d); e2 = true; }
                ImGui::EndCombo();
            }
            tip("Elige un grupo ya usado en otros tours del mapa.");
        }
    }
    if (e1 || e2) man.set_tour(tt_line_, tt_tour_, tour_num_, tour_dep_, tour.day_mask);

    group_title("Conditions");
    int mask = tour.day_mask;
    bool mchg = false;
    if (ImGui::BeginTable("##cond", 3, ImGuiTableFlags_SizingStretchSame)) {
        for (int r = 0; r < 5; ++r) {
            ImGui::TableNextRow();
            for (int c = 0; c < 3; ++c) {
                ImGui::TableSetColumnIndex(c);
                const auto& cell = kCondGrid[r][c];
                if (!cell.name) continue;
                bool f = (mask & cell.bit) != 0;
                if (ImGui::Checkbox(cell.name, &f)) { mask = f ? (mask | cell.bit) : (mask & ~cell.bit); mchg = true; }
                if (ImGui::IsItemHovered()) {
                    const char* t = cell.bit == 0x100 ? "Solo en vacaciones escolares ('Hols')."
                                  : cell.bit == 0x200 ? "No circula en vacaciones escolares ('No Hols')."
                                  : cell.bit == 0x80 ? "Circula en dias festivos ('Holiday')."
                                  : "El tour circula este dia de la semana.";
                    ImGui::SetTooltip("%s", t);
                }
            }
        }
        ImGui::EndTable();
    }
    if (mchg) man.set_tour(tt_line_, tt_tour_, tour.number, tour.depot_name, mask);
}

// Timeline (Image1): one row per tour of the line, one bar per departure.
void TtEditorUi::draw_tt_timeline(TimeTableMan& man, float height) {
    if (!in_range(tt_line_, man.data.lines.size())) { ImGui::TextDisabled("Create a line first."); return; }
    auto& ln = man.data.lines[static_cast<std::size_t>(tt_line_)];
    const float sliders_h = ImGui::GetFrameHeightWithSpacing() * 2.f + 6.f;
    const float avail_h = height > 0.f ? height : ImGui::GetContentRegionAvail().y;
    const ImVec2 size(std::max(200.f, ImGui::GetContentRegionAvail().x), std::max(110.f, avail_h - sliders_h));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##tl", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const bool area_hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float span = std::clamp(tl_zoom_hrs_, 1.f, 28.f);
    const float h0 = tl_mid_hrs_ - span * 0.5f;
    auto xof = [&](float minutes) { return p0.x + size.x * ((minutes / 60.f - h0) / span); };
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(250, 250, 250, 255));
    const float head = 18.f;
    const int ntour = static_cast<int>(ln.tours.size());
    const float row_h = ntour > 0 ? std::min(52.f, (size.y - head - 4.f) / static_cast<float>(ntour)) : 40.f;
    const float rows_y = p0.y + std::max(head, (size.y - row_h * static_cast<float>(ntour)) * 0.5f);
    for (int i = 0; i < ntour; ++i) {
        const float y = rows_y + row_h * static_cast<float>(i);
        const ImU32 bg = (i == tt_tour_) ? IM_COL32(200, 200, 232, 255) : IM_COL32(226, 226, 226, 255);
        dl->AddRectFilled(ImVec2(p0.x, y), ImVec2(p0.x + size.x, y + row_h - 1.f), bg);
        dl->AddText(ImVec2(p0.x + 3.f, y + 1.f), IM_COL32(40, 40, 40, 255), ln.tours[static_cast<std::size_t>(i)].number.c_str());
    }
    for (int hr = static_cast<int>(std::floor(h0)); hr <= static_cast<int>(std::ceil(h0 + span)); ++hr) {
        const float x = xof(static_cast<float>(hr) * 60.f);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + size.y), IM_COL32(150, 150, 150, 255));
        char b[16];
        std::snprintf(b, sizeof(b), "%d:00", ((hr % 24) + 24) % 24);
        dl->AddText(ImVec2(x + 3.f, p0.y + 1.f), IM_COL32(110, 110, 110, 255), b);
    }
    int hit_tour = -1, hit_trip = -1;
    for (int i = 0; i < ntour; ++i) {
        const auto& tr = ln.tours[static_cast<std::size_t>(i)];
        const float y = rows_y + row_h * static_cast<float>(i);
        if (area_hov && mp.y >= y && mp.y < y + row_h) hit_tour = i;
        for (int k = 0; k < static_cast<int>(tr.trips.size()); ++k) {
            const auto& e = tr.trips[static_cast<std::size_t>(k)];
            const float x0 = xof(e.departure_minutes);
            const float x1 = std::max(x0 + 2.f, xof(e.departure_minutes + trip_duration(man, e)));
            if (x1 < p0.x || x0 > p0.x + size.x) continue;
            const bool known = man.find_trip_by_name(e.trip_name) >= 0;
            const bool sel = (i == tt_tour_ && k == tt_sel_trip_);
            dl->AddRectFilled(ImVec2(x0, y + 2.f), ImVec2(x1, y + row_h - 3.f), known ? trip_color(e.trip_name) : IM_COL32(200, 60, 60, 255));
            dl->AddRect(ImVec2(x0, y + 2.f), ImVec2(x1, y + row_h - 3.f), sel ? IM_COL32(230, 20, 20, 255) : IM_COL32(20, 20, 20, 255), 0.f, 0, sel ? 2.5f : 1.f);
            if (x1 - x0 > 26.f) dl->AddText(ImVec2(x0 + 2.f, y + 3.f), IM_COL32(255, 255, 255, 255), e.trip_name.c_str());
            if (area_hov && mp.x >= x0 && mp.x <= x1 && mp.y >= y && mp.y < y + row_h) { hit_tour = i; hit_trip = k; }
        }
    }
    dl->PopClipRect();
    if (hit_tour >= 0 && hit_trip >= 0) {
        const auto& e = ln.tours[static_cast<std::size_t>(hit_tour)].trips[static_cast<std::size_t>(hit_trip)];
        ImGui::SetTooltip("%s  (profile %d)\nDeparture %s  -  duration %.0f min\nClick = seleccionar este viaje del tour",
                          e.trip_name.c_str(), e.profile_index, fmt_hms(e.departure_minutes).c_str(), static_cast<double>(trip_duration(man, e)));
    }
    if (clicked && hit_tour >= 0) {
        if (tt_tour_ != hit_tour) { tt_tour_ = hit_tour; tour_buf_tour_ = -2; }
        tt_sel_trip_ = hit_trip;
    }
    ImGui::SetNextItemWidth(-90.f);
    ImGui::SliderFloat("Zoom (hrs)", &tl_zoom_hrs_, 2.f, 28.f, "%.1f");
    tip("Cuantas horas se ven a la vez en la grafica. Menos horas = mas detalle.");
    ImGui::SetNextItemWidth(-90.f);
    ImGui::SliderFloat("Midpoint (hrs)", &tl_mid_hrs_, 0.f, 28.f, "%.1f");
    tip("Hora que queda en el centro de la grafica. Muevelo para desplazarte por el dia.");
}

// ── Time Table Editor, OMSI tab (Tform_tt) ─────────────────────────────────

void TtEditorUi::draw_tt_omsi_left(TimeTableMan& man) {
    if (!in_range(tt_line_, man.data.lines.size())) return;
    auto& ln = man.data.lines[static_cast<std::size_t>(tt_line_)];
    if (!in_range(tt_tour_, ln.tours.size())) return;
    auto& tour = ln.tours[static_cast<std::size_t>(tt_tour_)];
    const auto tnames = man.trip_combo_strings();
    add_trip_pick_ = std::clamp(add_trip_pick_, 0, std::max(0, static_cast<int>(tnames.size()) - 1));
    const bool have_sel = in_range(tt_sel_trip_, tour.trips.size());

    group_title(have_sel ? ("Trip Nr. " + std::to_string(tt_sel_trip_)).c_str() : "Trip Nr. #");
    if (ImGui::Button("Delete##trip", ImVec2(90.f, 0.f)) && have_sel) {
        man.del_tour_entry(tt_line_, tt_tour_, tt_sel_trip_);
        tt_sel_trip_ = std::min(tt_sel_trip_, static_cast<int>(tour.trips.size()) - 1);
        return;
    }
    tip("Borra el viaje seleccionado de este tour (no borra el archivo .ttp del viaje).");
    ImGui::SameLine();
    if (ImGui::Button("NEW", ImVec2(90.f, 0.f)) && !tnames.empty()) {
        float last = 0.f;
        if (!tour.trips.empty()) last = tour.trips.back().departure_minutes + 30.f;
        man.add_tour_entry(tt_line_, tt_tour_, tnames[static_cast<std::size_t>(add_trip_pick_)], 0, last);
        tt_sel_trip_ = static_cast<int>(tour.trips.size()) - 1;
    }
    tip("Agrega un viaje nuevo al final del tour (30 min despues del ultimo), con el viaje elegido abajo y el primer perfil. Luego ajusta hora y viaje.");
    if (!have_sel) {
        ImGui::TextDisabled("Click a bar in the graphic.");
    } else {
        auto& e = tour.trips[static_cast<std::size_t>(tt_sel_trip_)];
        int tot = static_cast<int>(e.departure_minutes * 60.f + 0.5f);
        int h = tot / 3600, m = (tot / 60) % 60, s = tot % 60;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Dep. Time:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(34.f); bool c = ImGui::InputInt("##dh", &h, 0, 0);
        ImGui::SameLine(0, 2); ImGui::TextUnformatted(":"); ImGui::SameLine(0, 2);
        ImGui::SetNextItemWidth(34.f); c |= ImGui::InputInt("##dm", &m, 0, 0);
        ImGui::SameLine(0, 2); ImGui::TextUnformatted(":"); ImGui::SameLine(0, 2);
        ImGui::SetNextItemWidth(34.f); c |= ImGui::InputInt("##ds", &s, 0, 0);
        tip("Hora de salida del viaje: horas : minutos : segundos (puede pasar de 24 h para viajes de madrugada).");
        if (c) {
            e.departure_minutes = static_cast<float>(std::clamp(h, 0, 99) * 3600 + std::clamp(m, 0, 59) * 60 + std::clamp(s, 0, 59)) / 60.f;
            man.dirty = true;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Trip:");
        ImGui::SameLine();
        const int tix = man.find_trip_by_name(e.trip_name);
        ImGui::SetNextItemWidth(-62.f);
        if (ImGui::BeginCombo("##trip", e.trip_name.c_str())) {
            for (const auto& n : tnames)
                if (ImGui::Selectable(n.c_str(), n == e.trip_name)) { e.trip_name = n; e.profile_index = 0; man.dirty = true; }
            ImGui::EndCombo();
        }
        tip("Viaje (.ttp) que hace este tour a esa hora. Cambia el viaje sin perder la hora de salida.");
        ImGui::SameLine();
        ImGui::BeginDisabled();
        ImGui::Button("ShowAI");
        ImGui::EndDisabled();
        tip("ShowAI: en OMSI muestra los vehiculos IA de este viaje en el mapa. No disponible en este editor.");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Time Profile:");
        ImGui::SameLine();
        if (tix >= 0) {
            const auto& tpf = man.data.trips[static_cast<std::size_t>(tix)].profiles;
            const std::string pp = in_range(e.profile_index, tpf.size()) ? tpf[static_cast<std::size_t>(e.profile_index)].name : "?";
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::BeginCombo("##pf", pp.c_str())) {
                for (int k = 0; k < static_cast<int>(tpf.size()); ++k)
                    if (ImGui::Selectable(tpf[static_cast<std::size_t>(k)].name.c_str(), e.profile_index == k)) { e.profile_index = k; man.dirty = true; }
                ImGui::EndCombo();
            }
            tip("Perfil de tiempos del viaje (rapido, lento, hora pico...). Se edita en Edit Profiles.");
            const auto& tpx = man.data.trips[static_cast<std::size_t>(tix)];
            const std::string first = tpx.stations.empty() ? "-" : (tpx.stations.front().name.empty() ? man.busstop_name(tpx.stations.front().object_id) : tpx.stations.front().name);
            const std::string last = tpx.stations.empty() ? "-" : (tpx.stations.back().name.empty() ? man.busstop_name(tpx.stations.back().object_id) : tpx.stations.back().name);
            ImGui::TextWrapped("First Stop:  %s", first.c_str());
            ImGui::TextWrapped("Last Stop:  %s", last.c_str());
            ImGui::Text("End Time:  %s", fmt_hms(e.departure_minutes + trip_duration(man, e)).c_str());
        } else {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "Trip file missing");
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("New trips use:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::BeginCombo("##addtrip", tnames.empty() ? "(no trips)" : tnames[static_cast<std::size_t>(add_trip_pick_)].c_str())) {
        for (int i = 0; i < static_cast<int>(tnames.size()); ++i)
            if (ImGui::Selectable(tnames[static_cast<std::size_t>(i)].c_str(), add_trip_pick_ == i)) add_trip_pick_ = i;
        ImGui::EndCombo();
    }
    tip("Viaje que se usara al pulsar NEW.");

    group_title("Repeat");
    ImGui::TextUnformatted("...last");
    ImGui::SameLine(); ImGui::SetNextItemWidth(40.f); ImGui::InputInt("##rl", &rep_last_, 0, 0);
    tip("Cuantos viajes del final del tour se repiten (los ultimos N).");
    ImGui::SameLine(); ImGui::TextUnformatted("entries");
    ImGui::TextUnformatted("...");
    ImGui::SameLine(); ImGui::SetNextItemWidth(40.f); ImGui::InputInt("##rt", &rep_times_, 0, 0);
    tip("Cuantas veces se repite ese bloque.");
    ImGui::SameLine(); ImGui::TextUnformatted("time(s)...");
    ImGui::TextUnformatted("...with interval:");
    ImGui::SetNextItemWidth(70.f); ImGui::InputInt("##ri", &rep_interval_, 0, 0);
    tip("Minutos de desfase entre cada repeticion del bloque (el bloque k se desplaza k x intervalo).");
    ImGui::SameLine(); ImGui::TextUnformatted("minutes");
    if (ImGui::Button("Do It!", ImVec2(-1.f, 0.f)) && !tour.trips.empty()) {
        const int n = std::clamp(rep_last_, 1, static_cast<int>(tour.trips.size()));
        const int times = std::clamp(rep_times_, 1, 200);
        const std::vector<TourTrip> base(tour.trips.end() - n, tour.trips.end());
        for (int k = 1; k <= times; ++k)
            for (const auto& b : base)
                man.add_tour_entry(tt_line_, tt_tour_, b.trip_name, b.profile_index,
                                   b.departure_minutes + static_cast<float>(k * rep_interval_));
    }
    tip("Copia los ultimos N viajes del tour K veces, cada copia desplazada por el intervalo. Util para repetir un patron horario.");
}

// ── Time Table Editor, Modern tab ──────────────────────────────────────────

void TtEditorUi::draw_tt_modern(TimeTableMan& man) {
    if (!in_range(tt_line_, man.data.lines.size()) ||
        !in_range(tt_tour_, man.data.lines[static_cast<std::size_t>(tt_line_)].tours.size())) {
        ImGui::TextDisabled("Pick or create a line and a tour on the left first.");
        return;
    }
    const auto tnames = man.trip_combo_strings();
    auto tour = [&]() -> Tour& { return man.data.lines[static_cast<std::size_t>(tt_line_)].tours[static_cast<std::size_t>(tt_tour_)]; };
    auto trip_pick = [&](const char* id, int& idx) {
        idx = std::clamp(idx, 0, std::max(0, static_cast<int>(tnames.size()) - 1));
        ImGui::SetNextItemWidth(220.f);
        if (ImGui::BeginCombo(id, tnames.empty() ? "(no trips)" : tnames[static_cast<std::size_t>(idx)].c_str())) {
            for (int i = 0; i < static_cast<int>(tnames.size()); ++i)
                if (ImGui::Selectable(tnames[static_cast<std::size_t>(i)].c_str(), idx == i)) idx = i;
            ImGui::EndCombo();
        }
    };
    auto prof_pick = [&](const char* id, int trip_i, int& pidx) {
        const std::vector<TripProfile>* pf = (trip_i >= 0 && trip_i < static_cast<int>(man.data.trips.size()))
            ? &man.data.trips[static_cast<std::size_t>(trip_i)].profiles : nullptr;
        const int n = pf ? static_cast<int>(pf->size()) : 0;
        pidx = std::clamp(pidx, 0, std::max(0, n - 1));
        ImGui::SetNextItemWidth(150.f);
        if (ImGui::BeginCombo(id, n ? (*pf)[static_cast<std::size_t>(pidx)].name.c_str() : "(none)")) {
            for (int i = 0; i < n; ++i)
                if (ImGui::Selectable((*pf)[static_cast<std::size_t>(i)].name.c_str(), pidx == i)) pidx = i;
            ImGui::EndCombo();
        }
    };

    // 1) departures between two hours at a fixed interval — the slow part of OMSI's editor
    if (ImGui::CollapsingHeader("1 · Generate departures (from - to, every N min)", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Trip:"); ImGui::SameLine(); trip_pick("##gtrip", gen_trip_);
        tip("Viaje (.ttp) que saldra en cada horario generado.");
        ImGui::SameLine(); ImGui::TextUnformatted("Profile:"); ImGui::SameLine(); prof_pick("##gprof", gen_trip_, gen_prof_);
        tip("Perfil de tiempos que usaran todas las salidas generadas.");
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("From:"); ImGui::SameLine(); time_input("gfrom", gen_from_);
        tip("Hora de la primera salida (HH : MM).");
        ImGui::SameLine(); ImGui::TextUnformatted("  To:"); ImGui::SameLine(); time_input("gto", gen_to_);
        tip("Hora limite: se genera una salida cada intervalo hasta esta hora (incluida).");
        ImGui::SameLine(); ImGui::TextUnformatted("  Every:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(50.f); ImGui::InputInt("##gev", &gen_every_, 0, 0); gen_every_ = std::clamp(gen_every_, 1, 600);
        ImGui::SameLine(); ImGui::TextUnformatted("min");
        tip("Frecuencia en minutos entre salidas.");
        for (int v : {5, 10, 15, 20, 30, 60}) {
            ImGui::SameLine();
            if (ImGui::SmallButton((std::to_string(v) + "##ev").c_str())) gen_every_ = v;
        }
        tip("Atajos de frecuencia: 5, 10, 15, 20, 30 o 60 minutos.");
        const int count = gen_to_ >= gen_from_ ? (gen_to_ - gen_from_) / gen_every_ + 1 : 0;
        ImGui::Checkbox("Replace the existing departures of this trip in this tour", &gen_replace_);
        tip("Si esta marcada, antes de generar se borran las salidas de este mismo viaje en el tour; si no, se agregan a las existentes.");
        ImGui::Text("= %d departures", count);
        ImGui::SameLine();
        if (ImGui::Button("Generate") && count > 0 && !tnames.empty()) {
            const std::string tn = tnames[static_cast<std::size_t>(gen_trip_)];
            if (gen_replace_) {
                auto& v = tour().trips;
                v.erase(std::remove_if(v.begin(), v.end(), [&](const TourTrip& t) { return t.trip_name == tn; }), v.end());
            }
            for (int k = 0; k < count; ++k)
                man.add_tour_entry(tt_line_, tt_tour_, tn, gen_prof_, static_cast<float>(gen_from_ + k * gen_every_));
            sort_tour_by_time(tour());
            tt_sel_trip_ = -1;
            row_sel_.assign(tour().trips.size(), 0);
            man.dirty = true;
        }
        tip("Crea todas las salidas de una vez. Ej.: 08:00 a 19:00 cada 15 min = 45 salidas, ordenadas por hora.");
    }

    // 2) repeat a block
    if (ImGui::CollapsingHeader("2 · Repeat a block of departures", ImGuiTreeNodeFlags_DefaultOpen)) {
        const int have = static_cast<int>(tour().trips.size());
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Last"); ImGui::SameLine();
        ImGui::SetNextItemWidth(46.f); ImGui::InputInt("##rl2", &rep_last_, 0, 0);
        ImGui::SameLine(); ImGui::TextUnformatted("entries, repeated"); ImGui::SameLine();
        ImGui::SetNextItemWidth(46.f); ImGui::InputInt("##rt2", &rep_times_, 0, 0);
        ImGui::SameLine(); ImGui::TextUnformatted("times, shifted"); ImGui::SameLine();
        ImGui::SetNextItemWidth(60.f); ImGui::InputInt("##ri2", &rep_interval_, 0, 0);
        ImGui::SameLine(); ImGui::TextUnformatted("min each");
        tip("Toma los ultimos N viajes del tour y los copia K veces; la copia numero k sale k x intervalo minutos despues.");
        rep_last_ = std::clamp(rep_last_, 1, std::max(1, have));
        rep_times_ = std::clamp(rep_times_, 1, 200);
        ImGui::SameLine();
        if (ImGui::Button("Repeat##blk") && have > 0) {
            const int n = rep_last_;
            const std::vector<TourTrip> base(tour().trips.end() - n, tour().trips.end());
            for (int k = 1; k <= rep_times_; ++k)
                for (const auto& b : base)
                    man.add_tour_entry(tt_line_, tt_tour_, b.trip_name, b.profile_index, b.departure_minutes + static_cast<float>(k * rep_interval_));
            sort_tour_by_time(tour());
            row_sel_.assign(tour().trips.size(), 0);
        }
    }

    // 3) move everything (or the selection)
    if (ImGui::CollapsingHeader("3 · Shift times", ImGuiTreeNodeFlags_DefaultOpen)) {
        row_sel_.resize(tour().trips.size(), 0);
        int nsel = 0;
        for (char c : row_sel_) nsel += c ? 1 : 0;
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s:", nsel > 0 ? "Selected departures" : "All departures");
        for (int d : {-60, -15, -5, -1, 1, 5, 15, 60}) {
            ImGui::SameLine();
            char lbl[16]; std::snprintf(lbl, sizeof(lbl), "%+d##sh", d);
            if (ImGui::SmallButton(lbl))
                for (std::size_t i = 0; i < tour().trips.size(); ++i)
                    if (nsel == 0 || row_sel_[i]) {
                        auto& t = tour().trips[i];
                        t.departure_minutes = std::max(0.f, t.departure_minutes + static_cast<float>(d));
                        man.dirty = true;
                    }
        }
        tip("Adelanta o atrasa los minutos indicados. Si hay filas marcadas en la tabla solo mueve esas; si no, mueve todo el tour.");
    }

    // 4) table of departures
    if (ImGui::CollapsingHeader("4 · Departures of the tour", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& tr = tour();
        row_sel_.resize(tr.trips.size(), 0);
        if (ImGui::SmallButton("All")) std::fill(row_sel_.begin(), row_sel_.end(), 1);
        tip("Marca todas las salidas.");
        ImGui::SameLine(); if (ImGui::SmallButton("None")) std::fill(row_sel_.begin(), row_sel_.end(), 0);
        tip("Desmarca todo.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Sort by time")) {
            sort_tour_by_time(tr);
            std::fill(row_sel_.begin(), row_sel_.end(), 0);
            tt_sel_trip_ = -1;
            man.dirty = true;
        }
        tip("Ordena las salidas por hora (conviene tras editar horas a mano).");
        ImGui::SameLine(); ImGui::TextUnformatted("   Set trip:"); ImGui::SameLine(); trip_pick("##btrip", add_trip_pick_);
        tip("Viaje que se asignara a las salidas marcadas al pulsar 'Apply trip'.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply trip") && !tnames.empty())
            for (std::size_t i = 0; i < tr.trips.size(); ++i)
                if (row_sel_[i]) {
                    tr.trips[i].trip_name = tnames[static_cast<std::size_t>(add_trip_pick_)];
                    tr.trips[i].profile_index = 0;
                    man.dirty = true;
                }
        tip("Cambia el viaje de todas las salidas marcadas (el perfil vuelve al primero).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete selected")) {
            for (int i = static_cast<int>(tr.trips.size()) - 1; i >= 0; --i)
                if (row_sel_[static_cast<std::size_t>(i)]) tr.trips.erase(tr.trips.begin() + i);
            row_sel_.assign(tr.trips.size(), 0);
            tt_sel_trip_ = -1;
            man.dirty = true;
        }
        tip("Borra las salidas marcadas del tour.");
        if (ImGui::BeginTable("##mdeps", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.f, 200.f))) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.f);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.f);
            ImGui::TableSetupColumn("Trip");
            ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthFixed, 170.f);
            ImGui::TableSetupColumn("Departure (h : m)", ImGuiTableColumnFlags_WidthFixed, 130.f);
            ImGui::TableSetupColumn("Ends", ImGuiTableColumnFlags_WidthFixed, 70.f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (int i = 0; i < static_cast<int>(tr.trips.size()); ++i) {
                auto& e = tr.trips[static_cast<std::size_t>(i)];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool b = row_sel_[static_cast<std::size_t>(i)] != 0;
                if (ImGui::Checkbox("##s", &b)) row_sel_[static_cast<std::size_t>(i)] = b ? 1 : 0;
                ImGui::TableSetColumnIndex(1);
                if (ImGui::Selectable(std::to_string(i).c_str(), tt_sel_trip_ == i)) tt_sel_trip_ = i;
                ImGui::TableSetColumnIndex(2);
                const int tix = man.find_trip_by_name(e.trip_name);
                if (tix < 0) ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "%s (missing)", e.trip_name.c_str());
                else ImGui::TextUnformatted(e.trip_name.c_str());
                ImGui::TableSetColumnIndex(3);
                if (tix >= 0) {
                    int p = e.profile_index;
                    prof_pick("##p", tix, p);
                    if (p != e.profile_index) { e.profile_index = p; man.dirty = true; }
                }
                ImGui::TableSetColumnIndex(4);
                int mins = static_cast<int>(e.departure_minutes + 0.0001f);
                const float frac = e.departure_minutes - static_cast<float>(mins);
                if (time_input("d", mins, 38.f)) { e.departure_minutes = static_cast<float>(mins) + frac; man.dirty = true; }
                ImGui::TableSetColumnIndex(5);
                ImGui::TextUnformatted(fmt_hm(e.departure_minutes + trip_duration(man, e)).c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
}

// ── Time Table Editor window ───────────────────────────────────────────────

void TtEditorUi::draw_timetable(TimeTableMan& man, const Host& host) {
    (void)host;
    if (!win_timetable_) return;
    place_window("Time Table Editor", 1120.f, 720.f);
    if (ImGui::Begin("Time Table Editor", &win_timetable_)) {
        if (ImGui::BeginTabBar("##tt_tabs")) {
            if (ImGui::BeginTabItem("OMSI")) {
                ImGui::BeginChild("##tt_left", ImVec2(235.f, -34.f), true);
                draw_tt_line_tour(man, false);
                draw_tt_omsi_left(man);
                ImGui::EndChild();
                ImGui::SameLine();
                ImGui::BeginChild("##tt_right", ImVec2(0.f, -34.f), false);
                const float h = ImGui::GetContentRegionAvail().y;
                draw_tt_timeline(man, tt_view_graphic_ ? h * 0.55f : h);
                if (tt_view_graphic_ && in_range(tt_line_, man.data.lines.size())) {
                    auto& ln = man.data.lines[static_cast<std::size_t>(tt_line_)];
                    if (in_range(tt_tour_, ln.tours.size()) &&
                        in_range(tt_sel_trip_, ln.tours[static_cast<std::size_t>(tt_tour_)].trips.size())) {
                        const auto& te = ln.tours[static_cast<std::size_t>(tt_tour_)].trips[static_cast<std::size_t>(tt_sel_trip_)];
                        const int tix2 = man.find_trip_by_name(te.trip_name);
                        if (tix2 >= 0) {
                            const auto times = man.station_times(tix2, te.profile_index);
                            const auto& tpx = man.data.trips[static_cast<std::size_t>(tix2)];
                            ImGui::Text("%s -> %s (line %s)", tpx.name.c_str(), tpx.terminus.c_str(), tpx.line.c_str());
                            if (ImGui::BeginTable("##tripno_tbl", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.f, 0.f))) {
                                ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.f);
                                ImGui::TableSetupColumn("Busstop");
                                ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 80.f);
                                ImGui::TableHeadersRow();
                                for (std::size_t k = 0; k < tpx.stations.size(); ++k) {
                                    ImGui::TableNextRow();
                                    ImGui::TableSetColumnIndex(0); ImGui::Text("%d", static_cast<int>(k));
                                    ImGui::TableSetColumnIndex(1);
                                    std::string nm = tpx.stations[k].name;
                                    if (nm.empty()) nm = man.busstop_name(tpx.stations[k].object_id);
                                    ImGui::TextUnformatted(nm.empty() ? std::to_string(tpx.stations[k].object_id).c_str() : nm.c_str());
                                    ImGui::TableSetColumnIndex(2);
                                    ImGui::TextUnformatted(fmt_hms(te.departure_minutes + (k < times.size() ? times[k] : 0.f)).c_str());
                                }
                                ImGui::EndTable();
                            }
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Modern")) {
                ImGui::BeginChild("##tm_left", ImVec2(235.f, -34.f), true);
                draw_tt_line_tour(man, true);
                ImGui::EndChild();
                ImGui::SameLine();
                ImGui::BeginChild("##tm_right", ImVec2(0.f, -34.f), false);
                ImGui::BeginChild("##tm_tools", ImVec2(0.f, -250.f), false);
                draw_tt_modern(man);
                ImGui::EndChild();
                draw_tt_timeline(man, 245.f);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (ImGui::Button("View Graphic", ImVec2(120.f, 0.f))) tt_view_graphic_ = !tt_view_graphic_;
        tip("Muestra / oculta la lista de horas por parada del viaje seleccionado debajo de la grafica (pestana OMSI).");
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(90.f, 0.f))) win_timetable_ = false;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.2f, 1.f), "ATTENTION: There is no Cancel! All Changes will be assigned immediately!");
        tip("Igual que en OMSI: cada cambio se aplica al instante en memoria. Se escribe a disco al guardar el mapa.");
    }
    ImGui::End();
}

// ── Edit Profiles (Tform_profiles) ─────────────────────────────────────────

void TtEditorUi::draw_profiles_omsi(TimeTableMan& man, const Host& host, int ti, int pi) {
    (void)host;
    auto& tp = man.data.trips[static_cast<std::size_t>(ti)];
    auto& pr = tp.profiles[static_cast<std::size_t>(pi)];
    if (dur_for_ != ti * 1000 + pi) {
        std::snprintf(dur_buf_, sizeof(dur_buf_), "%.3f", static_cast<double>(pr.minutes));
        dur_for_ = ti * 1000 + pi;
    }
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Time from start to terminus:");
    ImGui::SetNextItemWidth(90.f);
    if (ImGui::InputText("##dur", dur_buf_, sizeof(dur_buf_), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal))
        man.set_profile_duration(ti, pi, static_cast<float>(std::atof(dur_buf_)));
    ImGui::SameLine();
    ImGui::TextUnformatted("min");
    ImGui::EndGroup();
    tip("Duracion total del viaje desde la primera parada hasta la terminal, en minutos. Los tiempos automaticos se reparten proporcionalmente. Enter para aplicar.");
    const auto times = man.station_times(ti, pi);
    const int ns = static_cast<int>(tp.stations.size());
    if (ImGui::BeginTable("##prof_tbl", 6, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX,
                          ImVec2(0.f, -34.f))) {
        ImGui::TableSetupColumn("Busstop", ImGuiTableColumnFlags_WidthFixed, 170.f);
        ImGui::TableSetupColumn("Arrival", ImGuiTableColumnFlags_WidthFixed, 150.f);
        ImGui::TableSetupColumn("Departure", ImGuiTableColumnFlags_WidthFixed, 150.f);
        ImGui::TableSetupColumn("Stops", ImGuiTableColumnFlags_WidthFixed, 110.f);
        ImGui::TableSetupColumn("Copy from older profiles", ImGuiTableColumnFlags_WidthFixed, 340.f);
        ImGui::TableSetupColumn("##pad", ImGuiTableColumnFlags_WidthFixed, 10.f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        // master row: applies to every station at once
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted("All stations");
        tip("Fila maestra: lo que cambies aqui se aplica a todas las paradas a la vez.");
        int man_arr = 0, man_dep = 0;
        for (int s = 0; s < ns; ++s) {
            const auto sl = man.profile_slot(ti, pi, s);
            man_arr += sl.arr_flag ? 1 : 0;
            man_dep += sl.dep_flag ? 1 : 0;
        }
        ImGui::TableSetColumnIndex(1);
        {
            bool autom = man_arr == 0;
            if (ImGui::Checkbox("Autom.##aall", &autom))
                for (int s = 0; s < ns; ++s) {
                    const auto sl = man.profile_slot(ti, pi, s);
                    man.set_profile_arr(ti, pi, s, !autom, sl.arr_flag ? sl.arr : (s < static_cast<int>(times.size()) ? times[static_cast<std::size_t>(s)] : 0.f));
                }
            tip("Autom. = OMSI calcula la hora de llegada repartiendo la duracion total. Desmarcada = hora fija que escribes tu. Aqui cambia todas las paradas.");
        }
        ImGui::TableSetColumnIndex(2);
        {
            bool autom = man_dep == 0;
            if (ImGui::Checkbox("Autom.##dall", &autom))
                for (int s = 0; s < ns; ++s) {
                    const auto sl = man.profile_slot(ti, pi, s);
                    man.set_profile_dep(ti, pi, s, !autom, sl.dep_flag ? sl.dep : (s < static_cast<int>(times.size()) ? times[static_cast<std::size_t>(s)] : 0.f));
                }
            tip("Igual que en llegada pero para la hora de salida de cada parada.");
        }
        ImGui::TableSetColumnIndex(3);
        {
            int pick = -1;
            ImGui::SetNextItemWidth(100.f);
            if (ImGui::BeginCombo("##oall", "")) {
                for (int k = 0; k < 3; ++k)
                    if (ImGui::Selectable(kStopTypes[k])) pick = k;
                ImGui::EndCombo();
            }
            tip("Elige un tipo de parada para aplicarlo a todas las paradas de golpe.");
            if (pick >= 0)
                for (int s = 0; s < ns; ++s) man.set_profile_other(ti, pi, s, pick);
        }
        for (int s = 0; s < ns; ++s) {
            const auto slot = man.profile_slot(ti, pi, s);
            const float auto_t = s < static_cast<int>(times.size()) ? times[static_cast<std::size_t>(s)] : 0.f;
            ImGui::TableNextRow();
            ImGui::PushID(s);
            ImGui::TableSetColumnIndex(0);
            std::string nm = tp.stations[static_cast<std::size_t>(s)].name;
            if (nm.empty()) nm = man.busstop_name(tp.stations[static_cast<std::size_t>(s)].object_id);
            if (nm.empty()) nm = std::to_string(tp.stations[static_cast<std::size_t>(s)].object_id);
            ImGui::TextUnformatted(nm.c_str());
            ImGui::TableSetColumnIndex(1);
            {
                bool autom = !slot.arr_flag;
                float v = slot.arr_flag ? slot.arr : auto_t;
                bool changed = ImGui::Checkbox("##af", &autom);
                tip("Autom. marcada: la hora de llegada la calcula OMSI. Desmarca para fijarla a mano.");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.f);
                if (autom) ImGui::BeginDisabled();
                changed |= ImGui::InputFloat("##av", &v, 0.f, 0.f, "%.3f");
                if (autom) ImGui::EndDisabled();
                tip("Minutos desde la salida del viaje hasta llegar a esta parada.");
                if (changed) man.set_profile_arr(ti, pi, s, !autom, v);
            }
            ImGui::TableSetColumnIndex(2);
            {
                bool autom = !slot.dep_flag;
                float v = slot.dep_flag ? slot.dep : auto_t;
                bool changed = ImGui::Checkbox("##df", &autom);
                tip("Autom. marcada: la hora de salida la calcula OMSI. Desmarca para fijarla (fuerza espera en la parada).");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.f);
                if (autom) ImGui::BeginDisabled();
                changed |= ImGui::InputFloat("##dv", &v, 0.f, 0.f, "%.3f");
                if (autom) ImGui::EndDisabled();
                tip("Minutos desde la salida del viaje hasta salir de esta parada. Si es mayor que la llegada el bus espera.");
                if (changed) man.set_profile_dep(ti, pi, s, !autom, v);
            }
            ImGui::TableSetColumnIndex(3);
            {
                const int o = std::clamp(slot.other, 0, 2);
                ImGui::SetNextItemWidth(100.f);
                if (ImGui::BeginCombo("##ot", kStopTypes[o])) {
                    for (int k = 0; k < 3; ++k)
                        if (ImGui::Selectable(kStopTypes[k], o == k)) man.set_profile_other(ti, pi, s, k);
                    ImGui::EndCombo();
                }
                tip("on demand: para solo si hay pasajeros o solicitud. always: siempre para. never: pasa de largo sin parar.");
            }
            ImGui::TableSetColumnIndex(4);
            {
                const auto refs = man.parallel_refs(ti, s);
                int shown = 0;
                for (const auto& r : refs) {
                    if (shown >= 4) break;
                    const auto& ot = man.data.trips[static_cast<std::size_t>(r.trip)];
                    if (shown) ImGui::SameLine();
                    if (ImGui::SmallButton((ot.name + "/" + ot.profiles[static_cast<std::size_t>(r.profile)].name + "##k" + std::to_string(shown)).c_str()))
                        pending_par_ = {true, s, r, true};
                    tip("Copia los tiempos de este otro viaje/perfil (que pasa por la misma parada) desde esta parada en adelante. Util para viajes paralelos o de vuelta.");
                    ++shown;
                }
                if (shown == 0) ImGui::TextDisabled("-");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void TtEditorUi::draw_profiles_modern(TimeTableMan& man, const Host& host, int ti, int pi) {
    auto& tp = man.data.trips[static_cast<std::size_t>(ti)];
    auto& pr = tp.profiles[static_cast<std::size_t>(pi)];
    const int ns = static_cast<int>(tp.stations.size());
    const auto times = man.station_times(ti, pi);
    auto auto_t = [&](int s) { return s < static_cast<int>(times.size()) ? times[static_cast<std::size_t>(s)] : 0.f; };
    if (dur_for_ != ti * 1000 + pi) {
        std::snprintf(dur_buf_, sizeof(dur_buf_), "%.3f", static_cast<double>(pr.minutes));
        dur_for_ = ti * 1000 + pi;
    }
    group_title("Quick tools");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Total duration (min):");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.f);
    if (ImGui::InputText("##mdur", dur_buf_, sizeof(dur_buf_), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal))
        man.set_profile_duration(ti, pi, static_cast<float>(std::atof(dur_buf_)));
    tip("Duracion total del viaje en minutos. Enter para aplicar.");
    ImGui::SameLine();
    if (ImGui::Button("All automatic")) {
        for (int s = 0; s < ns; ++s) {
            man.set_profile_arr(ti, pi, s, false, 0.f);
            man.set_profile_dep(ti, pi, s, false, 0.f);
        }
        say(host, "All times automatic");
    }
    tip("Quita todas las horas fijas del perfil: OMSI vuelve a repartir los tiempos solo con la duracion total.");
    ImGui::SameLine();
    ImGui::TextUnformatted("  Dwell at every stop:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60.f);
    ImGui::InputInt("##dw", &pf_dwell_sec_, 0, 0);
    pf_dwell_sec_ = std::clamp(pf_dwell_sec_, 0, 3600);
    ImGui::SameLine();
    ImGui::TextUnformatted("s");
    ImGui::SameLine();
    if (ImGui::Button("Apply##dwell"))
        for (int s = 1; s + 1 < ns; ++s)
            man.set_profile_dep(ti, pi, s, true, auto_t(s) + static_cast<float>(pf_dwell_sec_) / 60.f);
    tip("Fija la espera en todas las paradas intermedias: salida = llegada + estos segundos.");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Stop type for all:");
    for (int k = 0; k < 3; ++k) {
        ImGui::SameLine();
        if (ImGui::SmallButton((std::string(kStopTypes[k]) + "##all").c_str()))
            for (int s = 0; s < ns; ++s) man.set_profile_other(ti, pi, s, k);
    }
    tip("Aplica 'on demand', 'always' o 'never' a todas las paradas de una vez.");
    ImGui::SameLine();
    ImGui::TextUnformatted("   Shift fixed times from stop");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(50.f);
    ImGui::InputInt("##sfrom", &pf_shift_from_, 0, 0);
    pf_shift_from_ = std::clamp(pf_shift_from_, 0, std::max(0, ns - 1));
    ImGui::SameLine();
    ImGui::TextUnformatted("by");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.f);
    ImGui::InputFloat("##sby", &pf_shift_min_, 0.f, 0.f, "%.2f");
    ImGui::SameLine();
    ImGui::TextUnformatted("min");
    ImGui::SameLine();
    if (ImGui::Button("Shift"))
        for (int s = pf_shift_from_; s < ns; ++s) {
            const auto sl = man.profile_slot(ti, pi, s);
            if (sl.arr_flag) man.set_profile_arr(ti, pi, s, true, sl.arr + pf_shift_min_);
            if (sl.dep_flag) man.set_profile_dep(ti, pi, s, true, sl.dep + pf_shift_min_);
        }
    tip("Suma (o resta, con valor negativo) minutos a todas las horas fijas desde la parada indicada hasta el final.");
    {
        struct Opt { std::string label; int station; TimeTableMan::ParallelRef ref; };
        std::vector<Opt> opts;
        for (int s = 0; s < ns; ++s)
            for (const auto& r : man.parallel_refs(ti, s)) {
                const auto& ot = man.data.trips[static_cast<std::size_t>(r.trip)];
                const std::string lb = ot.name + " / " + ot.profiles[static_cast<std::size_t>(r.profile)].name;
                if (std::none_of(opts.begin(), opts.end(), [&](const Opt& o) { return o.label == lb; })) opts.push_back({lb, s, r});
            }
        pf_copy_trip_ = std::clamp(pf_copy_trip_, 0, std::max(0, static_cast<int>(opts.size()) - 1));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Copy timing of:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260.f);
        if (ImGui::BeginCombo("##cpt", opts.empty() ? "(no parallel trips)" : opts[static_cast<std::size_t>(pf_copy_trip_)].label.c_str())) {
            for (int i = 0; i < static_cast<int>(opts.size()); ++i)
                if (ImGui::Selectable(opts[static_cast<std::size_t>(i)].label.c_str(), pf_copy_trip_ == i)) pf_copy_trip_ = i;
            ImGui::EndCombo();
        }
        tip("Viajes que comparten paradas con este. Al aplicar se copian sus tiempos desde la primera parada en comun.");
        ImGui::SameLine();
        if (ImGui::Button("Apply##cp") && !opts.empty()) {
            const auto& o = opts[static_cast<std::size_t>(pf_copy_trip_)];
            if (man.apply_parallel(ti, pi, o.station, o.ref, true)) say(host, "Timing copied from " + o.label);
        }
    }
    group_title("Stops (arrival / dwell / stop type)");
    if (ImGui::BeginTable("##prof_mod", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.f, -34.f))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28.f);
        ImGui::TableSetupColumn("Busstop");
        ImGui::TableSetupColumn("Arrival (min)", ImGuiTableColumnFlags_WidthFixed, 130.f);
        ImGui::TableSetupColumn("Dwell (s)", ImGuiTableColumnFlags_WidthFixed, 100.f);
        ImGui::TableSetupColumn("Departs", ImGuiTableColumnFlags_WidthFixed, 90.f);
        ImGui::TableSetupColumn("Stops", ImGuiTableColumnFlags_WidthFixed, 110.f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (int s = 0; s < ns; ++s) {
            const auto sl = man.profile_slot(ti, pi, s);
            const float a = sl.arr_flag ? sl.arr : auto_t(s);
            const float d = sl.dep_flag ? sl.dep : a;
            ImGui::PushID(s);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", s);
            ImGui::TableSetColumnIndex(1);
            std::string nm = tp.stations[static_cast<std::size_t>(s)].name;
            if (nm.empty()) nm = man.busstop_name(tp.stations[static_cast<std::size_t>(s)].object_id);
            ImGui::TextUnformatted(nm.empty() ? std::to_string(tp.stations[static_cast<std::size_t>(s)].object_id).c_str() : nm.c_str());
            ImGui::TableSetColumnIndex(2);
            {
                float v = a;
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::InputFloat("##a", &v, 0.f, 0.f, sl.arr_flag ? "%.3f (fixed)" : "%.3f")) man.set_profile_arr(ti, pi, s, true, v);
                tip("Llegada a la parada en minutos desde la salida. Escribe un valor para fijarla; 'All automatic' la libera.");
            }
            ImGui::TableSetColumnIndex(3);
            {
                int sec = static_cast<int>((d - a) * 60.f + 0.5f);
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::InputInt("##w", &sec, 0, 0)) man.set_profile_dep(ti, pi, s, true, a + static_cast<float>(std::max(0, sec)) / 60.f);
                tip("Segundos que espera el bus en esta parada (salida - llegada).");
            }
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(fmt_hms(d).c_str());
            ImGui::TableSetColumnIndex(5);
            {
                const int o = std::clamp(sl.other, 0, 2);
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::BeginCombo("##o", kStopTypes[o])) {
                    for (int k = 0; k < 3; ++k)
                        if (ImGui::Selectable(kStopTypes[k], o == k)) man.set_profile_other(ti, pi, s, k);
                    ImGui::EndCombo();
                }
                tip("on demand / always / never.");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void TtEditorUi::draw_profiles(TimeTableMan& man, const Host& host) {
    if (!win_profiles_) return;
    place_window("Edit Profiles", 900.f, 620.f);
    if (ImGui::Begin("Edit Profiles", &win_profiles_)) {
        if (!in_range(man.selected_trip, man.data.trips.size())) {
            ImGui::TextUnformatted("No trip selected!");
            ImGui::End();
            return;
        }
        const int ti = man.selected_trip;
        auto& tp = man.data.trips[static_cast<std::size_t>(ti)];
        ImGui::Text("Editing profiles of Trip N° %d: %s", ti, tp.name.c_str());
        ImGui::Text("Profile Cnt: %d", static_cast<int>(tp.profiles.size()));
        ImGui::SameLine(300.f);
        if (ImGui::Button("New", ImVec2(110.f, 0.f))) begin_prompt(Prompt::NewProfile, "New Profile", "standard");
        tip("Crea un perfil nuevo para este viaje (ej. 'standard', 'rush hour'). Cada perfil guarda sus propios tiempos por parada.");
        prof_sel_ = std::clamp(prof_sel_, 0, std::max(0, static_cast<int>(tp.profiles.size()) - 1));
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Profile:");
        ImGui::SameLine();
        const std::string prev = tp.profiles.empty() ? "(no profiles)" : tp.profiles[static_cast<std::size_t>(prof_sel_)].name;
        ImGui::SetNextItemWidth(220.f);
        if (ImGui::BeginCombo("##prof_combo", prev.c_str())) {
            for (int i = 0; i < static_cast<int>(tp.profiles.size()); ++i)
                if (ImGui::Selectable((tp.profiles[static_cast<std::size_t>(i)].name + "##p" + std::to_string(i)).c_str(), prof_sel_ == i)) {
                    prof_sel_ = i;
                    dur_for_ = -1;
                }
            ImGui::EndCombo();
        }
        tip("Perfil que estas editando. El tour elige cual usar para cada salida en el Time Table Editor.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete profile") && !tp.profiles.empty()) {
            man.del_profile(ti, prof_sel_);
            prof_sel_ = std::max(0, prof_sel_ - 1);
            dur_for_ = -1;
            ImGui::End();
            return;
        }
        tip("Borra el perfil seleccionado.");
        if (tp.profiles.empty()) {
            ImGui::TextDisabled("Create a profile first.");
            ImGui::End();
            return;
        }
        const int pi = prof_sel_;
        if (ImGui::BeginTabBar("##prof_tabs")) {
            if (ImGui::BeginTabItem("OMSI")) { draw_profiles_omsi(man, host, ti, pi); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Modern")) { draw_profiles_modern(man, host, ti, pi); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        if (ImGui::Button("Close", ImVec2(90.f, 0.f))) win_profiles_ = false;
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.2f, 1.f), "ATTENTION: There is no Cancel! All Changes will be assigned immediately!");
        if (pending_par_.active) ImGui::OpenPopup("Are you sure?##par");
    }
    if (ImGui::BeginPopupModal("Are you sure?##par", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Copy the other trip's times from this station onward?");
        if (ImGui::Button("Yes", ImVec2(90.f, 0.f))) {
            if (man.apply_parallel(man.selected_trip, prof_sel_, pending_par_.station, pending_par_.ref, pending_par_.dep))
                say(host, "Parallel timing applied");
            pending_par_.active = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(90.f, 0.f))) {
            pending_par_.active = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

// ── Errors / station timetable ─────────────────────────────────────────────

void TtEditorUi::draw_errors(TimeTableMan& man) {
    if (!win_errors_) return;
    place_window("Find Errors", 560.f, 300.f);
    if (ImGui::Begin("Find Errors", &win_errors_)) {
        if (ImGui::Button("Check again")) man.validate_all();
        ImGui::SameLine();
        ImGui::Text("%d problem(s)", static_cast<int>(man.data.errors.size()));
        ImGui::Separator();
        ImGui::BeginChild("##errs", ImVec2(0.f, 0.f), true);
        for (const auto& e : man.data.errors) ImGui::TextWrapped("%s", e.c_str());
        if (man.data.errors.empty()) ImGui::TextUnformatted("Everything is ok.");
        ImGui::EndChild();
    }
    ImGui::End();
}

void TtEditorUi::show_station_timetable(TimeTableMan& man, std::int32_t object_id) {
    station_tt_id_ = object_id;
    station_tt_text_ = man.show_timetable_for_station(object_id);
    win_station_tt_ = true;
}

void TtEditorUi::draw_station_tt(TimeTableMan&) {
    if (!win_station_tt_) return;
    place_window("Timetable of Station", 620.f, 460.f);
    if (ImGui::Begin("Timetable of Station", &win_station_tt_)) {
        ImGui::InputTextMultiline("##stt", station_tt_text_.data(), station_tt_text_.size() + 1, ImVec2(-1.f, -1.f),
                                  ImGuiInputTextFlags_ReadOnly);
    }
    ImGui::End();
}

void TtEditorUi::draw_windows(TimeTableMan& man, const Host& host) {
    draw_add_from_list(man, host);
    draw_assistant(man, host);
    draw_profiles(man, host);
    draw_timetable(man, host);
    draw_errors(man);
    draw_station_tt(man);
    draw_name_prompt(man, host);
}

} // namespace omsi::tt::ui
