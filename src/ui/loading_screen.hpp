#pragma once

#include <string>

namespace omsi::ui {

// Win32 modal progress window; pump_messages() keeps UI responsive during blocking work.
class LoadingScreen {
public:
    bool open(const wchar_t* title);
    void update(float fraction, const std::string& status_utf8);
    void close();
    void pump_messages();

private:
    void* hwnd_ = nullptr;
    void* bar_ = nullptr;
    void* label_ = nullptr;
};

} // namespace omsi::ui
