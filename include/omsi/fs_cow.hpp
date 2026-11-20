#pragma once

// Copy-on-write map workspaces: *_original / *_actual are built from links to the live map's files
// instead of full copies (fast on big maps, almost no disk space). A file becomes a real file of its
// own only when something writes it: every writer of map files calls break_link() first.
//
// link_or_copy: NTFS hard link (no admin rights needed, same volume) -> symbolic link (Developer
// Mode) -> plain copy.
// break_link: when the path shares its data with another link (hard link count > 1, or it is a
// symbolic link), it is replaced by an independent copy of the same bytes, so the write that follows
// can never change the live map or the snapshot it is linked to. A no-op for ordinary files.

#include <filesystem>

namespace omsi::fsutil {

// dst is replaced (existing file removed first). Returns false only when even the copy failed.
bool link_or_copy(const std::filesystem::path& src, const std::filesystem::path& dst);
// true when src and dst already are the same file (same volume + file id)
bool same_file(const std::filesystem::path& a, const std::filesystem::path& b);
// Make `path` an independent file before writing it (see above). Safe to call on missing files.
void break_link(const std::filesystem::path& path);

} // namespace omsi::fsutil
