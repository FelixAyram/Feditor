#pragma once

// Cooperative map editing session (no external server).
//
// One editor hosts a room (TCP, default port 47123); the others join with an invite code that carries
// the host address, port and a random room secret. The host relays everything (star topology):
// map edits (object / spline upserts and deletes, in tile-local coordinates), avatar poses and chat.
// Edits are applied by every peer as they come (asynchronous, last writer wins per id); the host keeps
// the room's op log and replays it to whoever joins late. Only the host saves the map.
//
// Map versions: only the map folder (maps\<map>\) is ever transferred - never objects, splines,
// vehicles or textures outside it. A guest whose map folder differs from the host's chooses to update
// its own copy (with a backup) or to edit in a copy of the host's version (maps\<map>_coop\).
// Whatever .sco / .sli the guest lacks shows as a "missing" cube in its editor.
//
// LAN: presence is broadcast over UDP (port 47124) so friends and rooms on the same network show
// online. Over the internet the host's port must be reachable (port forwarding or a VPN such as
// Tailscale / ZeroTier / Radmin); the host picks the address that goes into the code.
//
// The session object is a process-wide singleton: it lives across map reloads of the editor.

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace omsi::coop {

struct Profile {
    std::string guid;
    std::string name = "Editor";
    float color[3] = {0.2f, 0.6f, 1.f};
};

struct Friend {
    std::string guid;
    std::string name;
    float color[3] = {0.7f, 0.7f, 0.7f};
};

struct ChatLine {
    std::int64_t t = 0; // unix seconds
    std::string from_guid;
    std::string from_name;
    std::string text;
    bool system = false;
};

struct User {
    int id = -1;
    std::string guid;
    std::string name;
    float color[3] = {1.f, 1.f, 1.f};
    bool host = false;
};

enum class OpKind : std::uint8_t { ObjUpsert = 1, ObjDelete = 2, SplUpsert = 3, SplDelete = 4 };

struct MapOp {
    OpKind kind = OpKind::ObjUpsert;
    std::int32_t id = 0;
    std::string data; // editor-defined serialization (tile-local coordinates)
    int from_user = -1;
};

struct Pose {
    int user = -1;
    std::int32_t gx = 0, gy = 0; // map tile of the position
    float lx = 0.f, ly = 0.f, lz = 0.f; // feet, tile-local
    float yaw = 0.f, pitch = 0.f;       // look direction (radians; yaw from +Y clockwise)
    bool walking = false;               // pedestrian mode
    bool airborne = false;              // jumping / falling (avatar pose)
    double recv_time = 0.0;
};

struct LanPeer {
    std::string guid;
    std::string name;
    std::string code; // non-empty when hosting
    std::string ip;
    double last_seen = 0.0;
};

enum class Role { None, Host, Guest };

enum class VersionState {
    None,         // not a guest / nothing to compare yet
    Comparing,    // hashing the local map folder
    Same,         // identical (or no map folder sent)
    Differs,      // waiting for the guest's choice
    Transferring, // receiving map files
    Reloading,    // files written; the editor must open `reload_map`
    Ready         // map loaded, room state received
};

class Session {
public:
    static Session& get();

    // ---- profile / friends / chat history (local files under %APPDATA%\OMSI_F4\coop) ----
    Profile& profile();
    void save_profile();
    std::vector<Friend> friends();
    void add_friend(const std::string& guid, const std::string& name, const float color[3]);
    void remove_friend(const std::string& guid);
    bool is_friend(const std::string& guid);
    bool friend_online(const std::string& guid); // in the room or seen on the LAN
    // channel "" = room chat, otherwise a friend's guid (direct messages, delivered when both are in the room)
    std::vector<ChatLine> chat(const std::string& channel);
    void send_chat(const std::string& channel, const std::string& text);
    int unread(const std::string& channel);
    void mark_read(const std::string& channel);
    bool export_profile(const std::filesystem::path& zip_or_dir);
    bool import_profile(const std::filesystem::path& dir);

    // ---- room ----
    bool host(const std::filesystem::path& omsi_root, const std::filesystem::path& live_map_dir, std::uint16_t port,
              std::string& err);
    bool join(const std::string& code, const std::filesystem::path& omsi_root, std::string& err);
    void leave();
    [[nodiscard]] Role role();
    [[nodiscard]] int my_id();
    [[nodiscard]] std::vector<User> users();
    void kick(int user_id);
    [[nodiscard]] std::string invite_code(const std::string& address);
    [[nodiscard]] std::vector<std::string> local_addresses();
    [[nodiscard]] std::uint16_t port();
    [[nodiscard]] std::string status();
    [[nodiscard]] std::vector<LanPeer> lan_peers();
    [[nodiscard]] std::string map_name(); // the room's map folder name
    // Internet: when hosting, the port is opened on the router by UPnP and the router's public IP is
    // read from it (no external server). Status text for the UI; empty public_address = not available.
    [[nodiscard]] std::string internet_status();
    [[nodiscard]] std::string public_address();

    // Main thread, once per frame.
    void update();
    // The editor opens no socket (no Windows firewall question) until the user agreed, once: the LAN discovery
    // socket opens then, and on every later start. host()/join() also open it.
    [[nodiscard]] bool network_consent();
    void give_network_consent();

    // ---- map edits ----
    void send_op(OpKind kind, std::int32_t id, const std::string& data);
    [[nodiscard]] std::vector<MapOp> take_ops();
    // Ids this peer may allocate for new objects / splines (no clashes between peers).
    void id_range(std::int32_t& lo, std::int32_t& hi);

    // ---- avatars ----
    void send_pose(const Pose& p);
    [[nodiscard]] std::vector<Pose> poses();

    // ---- map version (guest) ----
    [[nodiscard]] VersionState version_state();
    [[nodiscard]] std::string version_summary();
    [[nodiscard]] float transfer_progress();
    void choose_update_my_map();
    void choose_host_version();
    // like choose_host_version, but the host's files are kept only in RAM (omsi::vfs): nothing is
    // written to disk and the guest's own map is locked against writes while the session lasts
    void choose_host_version_ram();
    // The editor calls this after (re)loading a map; returns a global.cfg to open instead, if the
    // session needs another map folder (version choice made).
    // force: reload even when that map folder is already open (its contents changed in RAM)
    bool take_reload(std::filesystem::path& global_cfg_out, bool* force = nullptr);
    void notify_map_loaded(const std::filesystem::path& omsi_root, const std::filesystem::path& live_map_dir);
    [[nodiscard]] bool editing_host_copy(); // guest working in maps\<map>_coop (saves go to the host)
    [[nodiscard]] bool editing_in_ram();    // guest working on the host's version held in RAM

    // ---- assets the guest lacks (objects, splines, their meshes / textures) ----
    // The guest asks for the .sco / .sli files it could not load; the host sends each one with every
    // file it needs to be drawn, straight into the guest's RAM (omsi::vfs), never to disk.
    void request_assets(const std::vector<std::filesystem::path>& files);
    [[nodiscard]] int assets_epoch();         // changes when a batch arrived: retry the missing ones
    [[nodiscard]] std::string assets_status(); // empty when nothing was received

private:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    struct Impl;
    Impl* d_;
};

} // namespace omsi::coop
