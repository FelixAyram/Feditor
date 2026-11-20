#include "omsi/vfs_overlay.hpp"
#include "omsi/fs_cow.hpp"

#include "omsi/logger.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <system_error>

namespace omsi::fsutil {
namespace fs = std::filesystem;

static std::string u8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

#ifdef _WIN32
namespace {
bool file_info(const fs::path& p, BY_HANDLE_FILE_INFORMATION& out) {
    HANDLE h = CreateFileW(p.wstring().c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const BOOL ok = GetFileInformationByHandle(h, &out);
    CloseHandle(h);
    return ok != FALSE;
}
} // namespace
#endif

bool same_file(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION ia{}, ib{};
    if (!file_info(a, ia) || !file_info(b, ib)) return false;
    return ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber && ia.nFileIndexHigh == ib.nFileIndexHigh &&
           ia.nFileIndexLow == ib.nFileIndexLow;
#else
    std::error_code ec;
    return fs::equivalent(a, b, ec);
#endif
}

bool link_or_copy(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    if (same_file(src, dst)) return true;
#ifdef _WIN32
    const std::wstring s = src.wstring(), d = dst.wstring();
    SetFileAttributesW(d.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(d.c_str()); // only this directory entry; other links keep their data
    if (CreateHardLinkW(d.c_str(), s.c_str(), nullptr)) return true;
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
    if (CreateSymbolicLinkW(d.c_str(), s.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) return true;
    return CopyFileW(s.c_str(), d.c_str(), FALSE) != FALSE;
#else
    fs::remove(dst, ec);
    fs::create_hard_link(src, dst, ec);
    if (!ec) return true;
    ec.clear();
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
#endif
}

void break_link(const fs::path& path) {
    if (omsi::vfs::under_root(path)) return; // RAM overlay: the guest's files are locked, never copied
#ifdef _WIN32
    const std::wstring p = path.wstring();
    const DWORD attr = GetFileAttributesW(p.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    bool shared = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0; // symbolic link
    if (!shared) {
        BY_HANDLE_FILE_INFORMATION fi{};
        if (!file_info(path, fi)) return;
        shared = fi.nNumberOfLinks > 1;
    }
    if (!shared) return;
    // copy the bytes into a new file, then put it in place of this link (rename replaces only the
    // directory entry: the other links and their data are untouched)
    const std::wstring tmp = p + L".cow_tmp";
    if (!CopyFileW(p.c_str(), tmp.c_str(), FALSE)) {
        log::warn("cow: could not detach " + u8(path));
        return;
    }
    SetFileAttributesW(tmp.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        // a symbolic link: remove the link itself, then rename
        DeleteFileW(p.c_str());
        if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            DeleteFileW(tmp.c_str());
            log::warn("cow: could not replace link " + u8(path));
        }
    }
#else
    (void)path;
#endif
}

} // namespace omsi::fsutil
