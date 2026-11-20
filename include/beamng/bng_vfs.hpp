#pragma once
// BeamNG virtual filesystem: mounts the install's zips (content/**, gameengine.zip), mod zips and loose folders
// (user folder) into one case-insensitive tree of "/levels/...", "/assets/...", "/art/..." paths.
// Priority (highest first): user folder loose files > unpacked mods > mod zips > install content zips > install loose.
// Reads are thread-safe (one mutex per zip; loose files need none).
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bng {

struct VfsMountInfo {
    std::filesystem::path source; // zip file or folder
    bool is_zip = false;
    int priority = 0;
    std::size_t file_count = 0;
};

struct VfsEntry {
    std::uint32_t mount = 0;      // index into mounts
    std::uint32_t zip_index = 0;  // file index inside the zip
    std::uint64_t size = 0;
    std::string real_name;        // original-case virtual path ("/levels/Foo/bar.dae")
    std::string loose_rel;        // path relative to the mounted folder (loose files only)
};

class Vfs {
public:
    Vfs();
    ~Vfs();
    Vfs(const Vfs&) = delete;
    Vfs& operator=(const Vfs&) = delete;

    // Mounts the standard BeamNG layout. user_folder may be empty (then only the install is mounted).
    // progress(done, total, current) is called while scanning zips.
    bool mount_beamng(const std::filesystem::path& install_root, const std::filesystem::path& user_folder,
                      const std::function<void(int, int, const std::string&)>& progress = {});
    bool mount_zip(const std::filesystem::path& zip, int priority);
    bool mount_folder(const std::filesystem::path& folder, int priority, std::string_view virtual_prefix = "/");

    static std::string normalize(std::string_view path); // lower-case, '/' separators, leading '/', no "./"

    bool exists(std::string_view path) const;
    const VfsEntry* entry(std::string_view path) const;
    std::optional<std::vector<std::uint8_t>> read(std::string_view path) const;
    std::optional<std::string> read_text(std::string_view path) const;
    // Every file whose normalized path starts with dir_prefix and ends with suffix (both case-insensitive).
    std::vector<std::string> list(std::string_view dir_prefix, std::string_view suffix = {}) const;

    // Texture lookup the way the game does it: follows "*.link" json redirects, swaps .png/.jpg/.tga for .dds,
    // tries the path relative to base_dir (material file folder). Returns the normalized path or empty.
    std::string resolve_texture(std::string_view path, std::string_view base_dir = {}) const;
    // Shape lookup (.dae; also accepts .cdae/.dts names and level-relative paths).
    std::string resolve_shape(std::string_view path, std::string_view base_dir = {}) const;

    // Real file on disk for a loose entry (empty for zip entries).
    std::filesystem::path loose_path(std::string_view path) const;

    const std::vector<VfsMountInfo>& mounts() const { return mounts_; }
    std::size_t file_count() const { return index_.size(); }
    std::vector<std::string> failed_mounts; // zips that could not be opened (path + miniz error)

private:
    struct ZipHandle;
    void add_entry(std::string norm, VfsEntry e, int priority);
    std::vector<VfsMountInfo> mounts_;
    std::vector<std::unique_ptr<ZipHandle>> zips_;            // parallel to mounts_ (null for folders)
    std::vector<std::filesystem::path> folder_roots_;         // parallel to mounts_
    std::unordered_map<std::string, VfsEntry> index_;
    std::unordered_map<std::string, int> index_priority_;
};

// Install / user folder discovery.
std::filesystem::path find_beamng_install();      // Steam libraries
std::filesystem::path find_beamng_user_folder();  // %LOCALAPPDATA%\BeamNG\BeamNG.drive.ini userFolder (+ "current")

} // namespace bng
