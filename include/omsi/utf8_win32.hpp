#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace omsi {

// True when `s` is well-formed UTF-8 (OMSI .map/.cfg without UTF-16 BOM are often system ANSI instead).
[[nodiscard]] inline bool is_valid_utf8(std::string_view s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = p[i];
        if (c <= 0x7F) {
            ++i;
            continue;
        }
        std::size_t need = 0;
        if ((c & 0xE0) == 0xC0)
            need = 1;
        else if ((c & 0xF0) == 0xE0)
            need = 2;
        else if ((c & 0xF8) == 0xF0)
            need = 3;
        else
            return false;
        if (i + need >= s.size()) return false;
        for (std::size_t j = 1; j <= need; ++j) {
            if ((p[i + j] & 0xC0) != 0x80) return false;
        }
        i += need + 1;
    }
    return true;
}

// UTF-8 (source / logs / map cfg) → UTF-16 for Win32 controls. Falls back to system ACP.
[[nodiscard]] inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    auto convert = [](UINT cp, DWORD flags, const std::string& in) -> std::wstring {
        const int n = MultiByteToWideChar(cp, flags, in.c_str(), static_cast<int>(in.size()), nullptr, 0);
        if (n <= 0) return {};
        std::wstring out(static_cast<std::size_t>(n), L'\0');
        MultiByteToWideChar(cp, 0, in.c_str(), static_cast<int>(in.size()), out.data(), n);
        return out;
    };
    if (auto w = convert(CP_UTF8, MB_ERR_INVALID_CHARS, s); !w.empty()) return w;
    if (auto w = convert(CP_ACP, 0, s); !w.empty()) return w;
    return std::wstring(s.begin(), s.end());
}

[[nodiscard]] inline std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr,
                                      nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Non-throwing replacement for std::filesystem::path::string(). path::string() converts to the ANSI code page and
// THROWS std::system_error when a character has no mapping (Cyrillic/CJK/... file names in OMSI installs) — on a
// std::thread or outside a try that closes the editor. Returns ACP text when every character fits (same bytes as
// path::string(), so strings fed back into ANSI APIs keep working), otherwise UTF-8.
[[nodiscard]] inline std::string path_text(const std::filesystem::path& p) {
    const std::wstring& w = p.native();
    if (w.empty()) return {};
    BOOL lossy = FALSE;
    const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, &lossy);
    if (n > 0 && !lossy) {
        std::string out(static_cast<std::size_t>(n), '\0');
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), static_cast<int>(w.size()), out.data(), n,
                            nullptr, nullptr);
        return out;
    }
    return wide_to_utf8(w);
}

// Windows-1252 / ACP bytes (typical OMSI ANSI cfg) → UTF-8 for /utf-8 builds and std::filesystem.
[[nodiscard]] inline std::string acp_to_utf8(const std::string& acp) {
    if (acp.empty()) return {};
    const int wlen =
        MultiByteToWideChar(CP_ACP, 0, acp.c_str(), static_cast<int>(acp.size()), nullptr, 0);
    if (wlen <= 0) return acp;
    std::wstring w(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, acp.c_str(), static_cast<int>(acp.size()), w.data(), wlen);
    return wide_to_utf8(w);
}

// OMSI UTF-16 maps sometimes store UTF-8 bytes as separate U+00XX codepoints (e.g. C3+B1 instead of U+00F1).
[[nodiscard]] inline std::string repair_omsi_path_mojibake(const std::string& path) {
    const std::wstring w = utf8_to_wide(path);
    if (w.empty() && !path.empty()) return path;
    for (const wchar_t ch : w) {
        if (ch > 0xFF) return path;
    }
    std::string raw;
    raw.reserve(w.size());
    for (const wchar_t ch : w) raw.push_back(static_cast<char>(static_cast<unsigned char>(ch)));
    if (raw != path && is_valid_utf8(raw)) return raw;
    return path;
}

// MSVC /utf-8: `root / rel_utf8` keeps UTF-8 bytes as separate wchar_t (e.g. jua + c3 + b1). Join via UTF-16.
[[nodiscard]] inline std::filesystem::path join_omsi_path(const std::filesystem::path& root,
                                                          const std::string& rel_utf8) {
    if (rel_utf8.empty()) return root;
    const std::wstring wrel = utf8_to_wide(rel_utf8);
    if (wrel.empty()) return root / rel_utf8;
    const std::filesystem::path rel(wrel);
    if (rel.is_absolute()) return rel;
    return std::filesystem::path(root.wstring()) / rel;
}

} // namespace omsi
