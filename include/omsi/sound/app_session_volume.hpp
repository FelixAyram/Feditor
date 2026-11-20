#pragma once

namespace omsi::sound {

// Set this process's audio sessions (every active output device) to 100 % and unmuted, logging what
// they were. Windows remembers a per-.exe session volume; returns how many sessions were changed.
int restore_app_session_volume();

} // namespace omsi::sound
