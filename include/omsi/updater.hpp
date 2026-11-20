#pragma once
// Feditor's update check: at start-up the latest GitHub release of the project is looked up on a worker
// thread; when it is newer than this build the editor asks whether to update. Updating downloads the
// release zip, closes the editor, unpacks it over the install folder and opens the editor again.
// FEDITOR_NO_UPDATE_CHECK=1 turns the check off.
#include <string>

namespace omsi::updater {

inline constexpr const char* kRepo = "FelixAyram/Feditor"; // owner/name on GitHub

enum class State { Idle, Checking, UpToDate, Available, Downloading, ReadyToRestart, Failed };

struct Status {
    State state = State::Idle;
    std::string latest;     // release tag without the leading 'v'
    std::string notes;      // release notes (first lines)
    std::string error;
    float progress = 0.f;   // download 0..1
};

void start_check();                 // once per run (no-op after the first call)
[[nodiscard]] Status status();
void start_download_and_install();  // State::Available -> Downloading -> ReadyToRestart
// Runs the helper that waits for this process to exit, unpacks the downloaded release over the install
// folder and starts the editor again. The caller closes the editor right after.
bool launch_installer();
void dismiss();                     // "not now": no more prompts this run
[[nodiscard]] bool dismissed();

} // namespace omsi::updater
