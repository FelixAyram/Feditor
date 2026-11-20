#pragma once

// ImGui front-end of the "Tracks" tab and its sub-windows (Unit134 TForm_main Tracks & Trips page,
// Tform_profiles, Tform_tt, Tform_tripAddStop, Tform_tripassistant, Tform_timetable).
// Pure ImGui + TimeTableMan: no Win32, no GPU, so it can be exercised headless in tests.

#include "omsi/tt_man.hpp"

#include <functional>
#include <string>
#include <vector>

namespace omsi::tt::ui {

struct Host {
    std::function<void(const std::string&)> status;   // status line of the editor
    std::function<void()> camera_jump;                // ListBox1Click / ListBox2Click camera move
    std::function<void(int)> set_mode;                // 0 Add Trackentry, 1 Add Station, 2 Show Timetable
    std::function<void()> find_errors_on_tiles;       // optional: world-side search after validate
};

class TtEditorUi {
public:
    // Body of the "Tracks" tab (mode radios, tracks, trips, buttons).
    void draw_tracks_tab(TimeTableMan& man, const Host& host, int& mode);
    // Floating windows / modals. Call once per frame from anywhere (not only while the tab is open).
    void draw_windows(TimeTableMan& man, const Host& host);

    // "Show Timetable of Station" clicked on a busstop object.
    void show_station_timetable(TimeTableMan& man, std::int32_t object_id);

    // Programmatic access (also used by the tests).
    void open_profiles() { win_profiles_ = true; }
    void open_timetable() { win_timetable_ = true; }
    void open_assistant() { win_assistant_ = true; }
    void open_add_from_list() { win_add_list_ = true; }
    void open_errors() { win_errors_ = true; }

    enum class Prompt {
        None, CreateTrack, CreateTripT1, CreateTripT2, CopyTrack, CopyTrip, CopyBoth, NewLine, NewTour,
        NewProfile
    };
    // Starts a name prompt; `on_ok(name)` returns false to keep the dialog open with the
    // "Problem: filename exists or is invalid!" message (AddTrack/AddTrip/AddLine/AddTour).
    void begin_prompt(Prompt kind, const std::string& title, const std::string& initial);
    bool prompt_active() const { return prompt_ != Prompt::None; }
    // Test hook: submit the active prompt with a text as if the user pressed OK.
    bool submit_prompt(TimeTableMan& man, const Host& host, const std::string& text);

private:
    bool run_prompt(TimeTableMan& man, const Host& host, const std::string& text);
    void sync_trip_buffers(const TimeTableMan& man);
    void draw_name_prompt(TimeTableMan& man, const Host& host);
    void draw_add_from_list(TimeTableMan& man, const Host& host);
    void draw_profiles(TimeTableMan& man, const Host& host);
    void draw_timetable(TimeTableMan& man, const Host& host);
    void draw_tt_line_tour(TimeTableMan& man, bool modern);
    void draw_tt_omsi_left(TimeTableMan& man);
    void draw_tt_modern(TimeTableMan& man);
    void draw_tt_timeline(TimeTableMan& man, float height);
    void draw_profiles_omsi(TimeTableMan& man, const Host& host, int ti, int pi);
    void draw_profiles_modern(TimeTableMan& man, const Host& host, int ti, int pi);
    void draw_assistant(TimeTableMan& man, const Host& host);
    void draw_errors(TimeTableMan& man);
    void draw_station_tt(TimeTableMan& man);

    Prompt prompt_ = Prompt::None;
    bool prompt_open_request_ = false;
    std::string prompt_title_;
    char prompt_buf_[160]{};
    std::string prompt_error_;

    char terminus_buf_[128]{};
    char line_buf_[64]{};
    int buf_trip_ = -2;
    std::string buf_trip_name_;

    bool win_profiles_ = false, win_timetable_ = false, win_assistant_ = false, win_add_list_ = false,
         win_errors_ = false, win_station_tt_ = false;
    int prof_sel_ = 0;
    char dur_buf_[32]{};
    int dur_for_ = -1;
    int al_sel_ = -1;
    int tt_line_ = -1, tt_tour_ = -1;
    char tour_num_[64]{}, tour_dep_[64]{};
    int tour_buf_line_ = -2, tour_buf_tour_ = -2;
    int add_trip_pick_ = 0;
    int tour_trip_sel_ = 0;
    std::string station_tt_text_;
    std::int32_t station_tt_id_ = 0;
    // pending "Are you sure?" for parallel copy
    struct PendingParallel { bool active = false; int station = 0; TimeTableMan::ParallelRef ref; bool dep = false; };
    PendingParallel pending_par_;
    // Time Table Editor / Edit Profiles shared state
    float tl_zoom_hrs_ = 14.f;   // visible span of the timeline (hours)
    float tl_mid_hrs_ = 12.f;    // hour at the centre of the timeline
    bool tt_view_graphic_ = false;
    int tt_sel_trip_ = -1;       // selected departure of the current tour
    int gen_trip_ = 0, gen_prof_ = 0, gen_from_ = 8 * 60, gen_to_ = 19 * 60, gen_every_ = 15;
    bool gen_replace_ = false;
    int rep_last_ = 2, rep_times_ = 1, rep_interval_ = 60;
    int shift_min_ = 5;
    int new_dep_h_ = 6, new_dep_m_ = 0;
    std::vector<char> row_sel_;  // Modern: multi-selected departures
    int bulk_prof_ = 0;
    int pf_dwell_sec_ = 20, pf_shift_from_ = 0;
    float pf_shift_min_ = 1.f;
    int pf_copy_trip_ = 0;
};

TtEditorUi& global_ui();

} // namespace omsi::tt::ui
