#include "omsi/vfs_overlay.hpp"
#include "omsi/cfg_document.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "omsi/utf8_win32.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace omsi::cfg {
namespace {

[[nodiscard]] std::string read_cfg_bytes(const std::filesystem::path& path) {
    // co-op guest editing the host's version in RAM: those files never exist on disk
    if (omsi::vfs::active()) {
        if (const auto ram = omsi::vfs::get(path)) return *ram;
        if (omsi::vfs::hidden(path)) return {};
    }
#ifdef _WIN32
    // FILE_FLAG_SEQUENTIAL_SCAN: OS prefetches ahead — critical on HDD when reading many .map/.sco.
    // Large ReadFile chunks (1 MiB): NTFS hates tiny buffered reads.
    const HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 0 || li.QuadPart > 0x7fffffff) {
        CloseHandle(h);
        return {};
    }
    const auto sz = static_cast<std::size_t>(li.QuadPart);
    std::string bytes(sz, '\0');
    std::size_t off = 0;
    constexpr DWORD kChunk = 1u << 20; // 1 MiB
    while (off < sz) {
        const DWORD want = static_cast<DWORD>((std::min)(static_cast<std::size_t>(kChunk), sz - off));
        DWORD got = 0;
        if (!ReadFile(h, bytes.data() + off, want, &got, nullptr) || got == 0) break;
        off += got;
    }
    CloseHandle(h);
    if (off != sz) return {};
    return bytes;
#else
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const auto sz = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::string bytes(sz, '\0');
    if (sz && !in.read(bytes.data(), static_cast<std::streamsize>(sz))) return {};
    return bytes;
#endif
}

std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool is_section_header(const std::string& line) {
    return line.size() >= 3 && line.front() == '[' && line.back() == ']';
}

bool is_object_nr_label(const std::string& line) {
    const auto t = trim(line);
    if (t.size() < 9) return false;
    std::size_t j = 0;
    while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
    const char* pref = "object nr";
    for (std::size_t k = 0; k < 9; ++k) {
        if (j + k >= t.size()) return false;
        const auto c = static_cast<unsigned char>(t[j + k]);
        if (std::tolower(c) != static_cast<unsigned char>(pref[k])) return false;
    }
    j += 9;
    if (j < t.size() && t[j] == '.') ++j;
    while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
    if (j >= t.size()) return false;
    if (t[j] == '-' || t[j] == '+') ++j;
    if (j >= t.size() || t[j] < '0' || t[j] > '9') return false;
    for (; j < t.size(); ++j)
        if (t[j] < '0' || t[j] > '9') return false;
    return true;
}

bool looks_utf16le_raw(const std::string& bytes) {
    if (bytes.size() < 4 || (bytes.size() % 2) != 0) return false;
    const std::size_t n = (std::min)(bytes.size(), std::size_t{256});
    std::size_t zero_odd = 0;
    std::size_t pairs = 0;
    for (std::size_t i = 1; i < n; i += 2) {
        ++pairs;
        if (static_cast<unsigned char>(bytes[i]) == 0) ++zero_odd;
    }
    return pairs >= 4 && zero_odd >= pairs * 3 / 4;
}

std::vector<std::string> read_utf16le_lines(const std::string& bytes, std::size_t start) {
    std::vector<std::string> lines;
    std::wstring current;
    for (std::size_t i = start; i + 1 < bytes.size(); i += 2) {
        const wchar_t wc = static_cast<unsigned char>(bytes[i]) |
                           (static_cast<wchar_t>(static_cast<unsigned char>(bytes[i + 1])) << 8);
        if (wc == L'\r') continue;
        if (wc == L'\n') {
            lines.push_back(omsi::wide_to_utf8(current));
            current.clear();
        } else {
            current.push_back(wc);
        }
    }
    if (!current.empty()) lines.push_back(omsi::wide_to_utf8(current));
    return lines;
}

std::optional<CfgDocument> parse_lines(const std::vector<std::string>& lines) {
    CfgDocument doc;
    std::string current;
    bool pending_instance = false;

    for (const auto& raw : lines) {
        const auto line = trim(raw);
        if (line.empty()) continue;
        if (is_object_nr_label(line)) continue;
        if (is_section_header(line)) {
            // OMSI repeats section headers (e.g. one [map] block per tile).
            if (current == line) {
                doc[current].push_back({});
                pending_instance = false;
            } else {
                current = line;
                if (!doc.contains(current)) doc[current] = {};
                pending_instance = true;
            }
            continue;
        }
        if (current.empty()) continue;
        if (pending_instance || doc[current].empty()) {
            doc[current].push_back({});
            pending_instance = false;
        }
        doc[current].back().push_back(line);
    }
    return doc;
}

void split_text_line_views(std::string_view text, std::vector<std::string_view>& lines) {
    lines.clear();
    std::size_t i = 0;
    while (i <= text.size()) {
        const std::size_t eol = text.find('\n', i);
        const std::size_t end = (eol == std::string_view::npos) ? text.size() : eol;
        std::size_t len = end - i;
        if (len > 0 && text[i + len - 1] == '\r') --len;
        lines.emplace_back(text.data() + i, len);
        if (eol == std::string_view::npos) break;
        i = eol + 1;
    }
}

std::vector<std::string> split_text_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t i = 0;
    while (i <= text.size()) {
        const std::size_t eol = text.find('\n', i);
        const std::size_t end = (eol == std::string_view::npos) ? text.size() : eol;
        std::string line(text.substr(i, end - i));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (eol == std::string_view::npos) break;
        i = eol + 1;
    }
    return lines;
}

std::vector<std::string> decode_cfg_text_bytes(std::string bytes) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    if (!omsi::is_valid_utf8(bytes)) bytes = omsi::acp_to_utf8(bytes);
    return split_text_lines(bytes);
}

} // namespace

std::uint64_t fnv1a_file_bytes(const std::filesystem::path& path) {
    const std::string bytes = read_cfg_bytes(path);
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : bytes) {
        h ^= static_cast<std::uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

CfgFileBuffer read_cfg_file_buffer(const std::filesystem::path& path) {
    CfgFileBuffer out;
    std::string bytes = read_cfg_bytes(path);
    if (bytes.empty()) return out;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        const auto lines = read_utf16le_lines(bytes, 2);
        for (const auto& ln : lines) {
            if (!out.text.empty()) out.text.push_back('\n');
            out.text += ln;
        }
    } else if (looks_utf16le_raw(bytes)) {
        const auto lines = read_utf16le_lines(bytes, 0);
        for (const auto& ln : lines) {
            if (!out.text.empty()) out.text.push_back('\n');
            out.text += ln;
        }
    } else {
        if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
            bytes.erase(0, 3);
        }
        if (!omsi::is_valid_utf8(bytes)) bytes = omsi::acp_to_utf8(bytes);
        out.text = std::move(bytes);
    }
    split_text_line_views(out.text, out.lines);
    return out;
}

std::vector<std::string> read_cfg_lines(const std::filesystem::path& path) {
    std::string bytes = read_cfg_bytes(path);
    if (bytes.empty()) return {};
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        return read_utf16le_lines(bytes, 2);
    }
    if (looks_utf16le_raw(bytes)) return read_utf16le_lines(bytes, 0);

    return decode_cfg_text_bytes(std::move(bytes));
}

std::optional<CfgDocument> load_cfg_file(const std::filesystem::path& path) {
    const auto lines = read_cfg_lines(path);
    if (lines.empty()) return std::nullopt;
    return parse_lines(lines);
}

} // namespace omsi::cfg
