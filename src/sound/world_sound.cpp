#include "omsi/sound/world_sound.hpp"

#include "omsi/logger.hpp"
#include "omsi/sound/world_sound_cfg.hpp"

#include "miniaudio/miniaudio.h" // declarations only; the implementation lives in miniaudio_impl.cpp

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omsi::sound {
namespace {
// path::string() throws on characters outside the ANSI code page; UTF-8 never throws.
std::string path_u8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}


constexpr int kMaxVoices = 48;
// Files above this size are streamed from disk instead of being decoded into the resource cache.
constexpr std::uintmax_t kStreamThresholdBytes = 2ull * 1024ull * 1024ull;
constexpr auto kWorkerTick = std::chrono::milliseconds(10);

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

} // namespace

Vec3f object_offset_to_world(Vec3f o, float yaw_deg, Vec3f origin) {
    const float a = yaw_deg * 3.14159265358979f / 180.f;
    const float c = std::cos(a);
    const float s = std::sin(a);
    return {origin.x + o.x * c - o.y * s, origin.y + o.x * s + o.y * c, origin.z + o.z};
}

namespace {

struct Command {
    enum class Kind { Start, Stop, Pos, Gain, Pitch };
    Kind kind = Kind::Start;
    SoundId id = 0;
    std::filesystem::path path; // Start
    EmitterDesc desc{};         // Start
    float value = 0.f;          // Gain / Pitch
    Vec3f pos{};                // Pos
};

struct Voice {
    SoundId id = 0;
    bool used = false;
    bool loop = false;
    bool started = false;
    float volume = 1.f;
    std::unique_ptr<ma_sound> snd;
};

} // namespace

struct WorldSound::Impl {
    // Main thread -> worker.
    std::mutex cmd_mu;
    std::condition_variable cmd_cv;
    std::vector<Command> cmds;

    std::thread worker;
    std::atomic<bool> quit{false};
    bool running = false;

    ma_engine engine{};
    bool engine_ok = false;

    // Shared scalars (written by main thread, read by worker / callers).
    std::atomic<bool> enabled{true};
    std::atomic<float> master{0.7f};
    std::atomic<float> lx{0.f}, ly{0.f}, lz{0.f};
    std::atomic<float> fx{0.f}, fy{0.f}, fz{1.f};
    std::atomic<int> active{0};
    std::atomic<SoundId> next_id{1};

    // Script / sound variables (main + script threads).
    mutable std::mutex var_mu;
    std::unordered_map<std::string, float> vars;

    // Play-cfg groups: group id -> voice ids (main thread side only).
    std::mutex group_mu;
    std::unordered_map<SoundId, std::vector<SoundId>> groups;

    // Worker-owned state.
    std::array<Voice, kMaxVoices> voices{};
    std::unordered_set<std::string> warned;

    void warn_once(const std::string& key, const std::string& msg) {
        if (warned.insert(key).second) log::warn(msg);
    }

    int find_voice(SoundId id) {
        for (int i = 0; i < kMaxVoices; ++i)
            if (voices[i].used && voices[i].id == id) return i;
        return -1;
    }

    void free_voice(int i) {
        Voice& v = voices[i];
        if (v.snd) {
            ma_sound_stop(v.snd.get());
            ma_sound_uninit(v.snd.get());
            v.snd.reset();
        }
        v = Voice{};
    }

    void start_voice(const Command& c) {
        if (!engine_ok) return;
        int slot = -1;
        for (int i = 0; i < kMaxVoices; ++i)
            if (!voices[i].used) {
                slot = i;
                break;
            }
        if (slot < 0) {
            warn_once("pool", "world_sound: voice pool full (" + std::to_string(kMaxVoices) + "), request dropped");
            return;
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(c.path, ec);
        if (ec) {
            warn_once("missing:" + path_u8(c.path), "world_sound: missing audio file " + path_u8(c.path));
            return;
        }
        auto snd = std::make_unique<ma_sound>();
        ma_uint32 flags = MA_SOUND_FLAG_ASYNC;
        if (size > kStreamThresholdBytes) flags |= MA_SOUND_FLAG_STREAM;
        const ma_result r = ma_sound_init_from_file_w(&engine, c.path.wstring().c_str(), flags, nullptr, nullptr,
                                                      snd.get());
        if (r != MA_SUCCESS) {
            warn_once("decode:" + path_u8(c.path),
                      "world_sound: cannot open " + path_u8(c.path) + " (ma_result " + std::to_string(r) + ")");
            return;
        }
        const float range = std::max(1.f, c.desc.range_m);
        ma_sound_set_looping(snd.get(), c.desc.loop ? MA_TRUE : MA_FALSE);
        ma_sound_set_attenuation_model(snd.get(), ma_attenuation_model_linear);
        ma_sound_set_min_distance(snd.get(), std::max(0.5f, range * 0.1f));
        ma_sound_set_max_distance(snd.get(), range);
        ma_sound_set_rolloff(snd.get(), 1.f);
        ma_sound_set_min_gain(snd.get(), 0.f);
        ma_sound_set_max_gain(snd.get(), 1.f);
        ma_sound_set_position(snd.get(), c.desc.pos.x, c.desc.pos.y, c.desc.pos.z);
        ma_sound_set_volume(snd.get(), clampf(c.desc.volume, 0.f, 4.f));
        ma_sound_set_pitch(snd.get(), clampf(c.desc.pitch, 0.05f, 4.f));
        ma_sound_start(snd.get());

        Voice& v = voices[slot];
        v.id = c.id;
        v.used = true;
        v.loop = c.desc.loop;
        v.started = true;
        v.volume = c.desc.volume;
        v.snd = std::move(snd);
    }

    void apply(const Command& c) {
        switch (c.kind) {
        case Command::Kind::Start: start_voice(c); break;
        case Command::Kind::Stop: {
            const int i = find_voice(c.id);
            if (i >= 0) free_voice(i);
            break;
        }
        case Command::Kind::Pos: {
            const int i = find_voice(c.id);
            if (i >= 0) ma_sound_set_position(voices[i].snd.get(), c.pos.x, c.pos.y, c.pos.z);
            break;
        }
        case Command::Kind::Gain: {
            const int i = find_voice(c.id);
            if (i >= 0) {
                voices[i].volume = c.value;
                ma_sound_set_volume(voices[i].snd.get(), clampf(c.value, 0.f, 4.f));
            }
            break;
        }
        case Command::Kind::Pitch: {
            const int i = find_voice(c.id);
            if (i >= 0) ma_sound_set_pitch(voices[i].snd.get(), clampf(c.value, 0.05f, 4.f));
            break;
        }
        }
    }

    void tick_engine() {
        if (!engine_ok) return;
        ma_engine_listener_set_position(&engine, 0, lx.load(), ly.load(), lz.load());
        ma_engine_listener_set_direction(&engine, 0, fx.load(), fy.load(), fz.load());
        ma_engine_set_volume(&engine, enabled.load() ? master.load() : 0.f);
    }

    void reap() {
        int n = 0;
        for (int i = 0; i < kMaxVoices; ++i) {
            Voice& v = voices[i];
            if (!v.used) continue;
            ++n;
            if (!v.loop && v.started && v.snd && ma_sound_at_end(v.snd.get())) {
                free_voice(i);
                --n;
            }
        }
        active.store(n, std::memory_order_relaxed);
    }

    void run() {
        std::vector<Command> batch;
        while (true) {
            {
                std::unique_lock lk(cmd_mu);
                cmd_cv.wait_for(lk, kWorkerTick, [&] { return quit.load() || !cmds.empty(); });
                batch.swap(cmds);
            }
            // An exception escaping this thread is std::terminate (the whole editor closes): log and drop the command.
            for (const auto& c : batch) {
                try {
                    apply(c);
                } catch (const std::exception& ex) {
                    log::warn(std::string("world_sound: command failed: ") + ex.what());
                } catch (...) {
                    log::warn("world_sound: command failed (unknown exception)");
                }
            }
            batch.clear();
            try {
                tick_engine();
                reap();
            } catch (...) {
                log::warn("world_sound: tick failed (exception)");
            }
            if (quit.load()) break;
        }
        for (int i = 0; i < kMaxVoices; ++i)
            if (voices[i].used) free_voice(i);
    }

    void push(Command c) {
        {
            std::lock_guard lk(cmd_mu);
            cmds.push_back(std::move(c));
        }
        cmd_cv.notify_one();
    }
};

WorldSound::WorldSound() : impl_(new Impl()) {}

WorldSound::~WorldSound() {
    shutdown();
    delete impl_;
    impl_ = nullptr;
}

WorldSound& WorldSound::instance() {
    static WorldSound inst;
    return inst;
}

bool WorldSound::init() {
    Impl& m = *impl_;
    if (m.running) return true;

    ma_engine_config cfg = ma_engine_config_init();
    cfg.listenerCount = 1;
    cfg.channels = 2;
    if (ma_engine_init(&cfg, &m.engine) != MA_SUCCESS) {
        log::warn("WorldSound: ma_engine_init failed — 3D sound disabled");
        return false;
    }
    m.engine_ok = true;
    // OMSI editor space is Z-up (see world_sound.hpp).
    ma_engine_listener_set_world_up(&m.engine, 0, 0.f, 0.f, 1.f);
    m.quit.store(false);
    m.worker = std::thread([&m] { m.run(); });
    m.running = true;
    log::info("WorldSound ready (miniaudio, voices=" + std::to_string(kMaxVoices) + ")");
    return true;
}

void WorldSound::shutdown() {
    Impl& m = *impl_;
    if (!m.running) return;
    m.quit.store(true);
    m.cmd_cv.notify_all();
    if (m.worker.joinable()) m.worker.join();
    if (m.engine_ok) {
        ma_engine_uninit(&m.engine);
        m.engine_ok = false;
    }
    m.running = false;
    std::lock_guard lk(m.group_mu);
    m.groups.clear();
}

bool WorldSound::ready() const { return impl_->running && impl_->engine_ok; }

void WorldSound::set_enabled(bool on) {
    impl_->enabled.store(on);
    if (!on) {
        // Silence everything right away; callers re-trigger when enabled again.
        std::lock_guard lk(impl_->group_mu);
        for (auto& [gid, ids] : impl_->groups)
            for (SoundId id : ids) {
                Command c;
                c.kind = Command::Kind::Stop;
                c.id = id;
                impl_->push(std::move(c));
            }
        impl_->groups.clear();
    }
}

bool WorldSound::enabled() const { return impl_->enabled.load(); }

void WorldSound::set_master_volume(float v01) { impl_->master.store(clampf(v01, 0.f, 1.f)); }

float WorldSound::master_volume() const { return impl_->master.load(); }

void WorldSound::set_listener(Vec3f pos, Vec3f forward) {
    impl_->lx.store(pos.x);
    impl_->ly.store(pos.y);
    impl_->lz.store(pos.z);
    impl_->fx.store(forward.x);
    impl_->fy.store(forward.y);
    impl_->fz.store(forward.z);
}

SoundId WorldSound::play(const std::filesystem::path& wav, const EmitterDesc& desc) {
    Impl& m = *impl_;
    if (!ready() || !m.enabled.load()) return 0;
    Command c;
    c.kind = Command::Kind::Start;
    c.id = m.next_id.fetch_add(1);
    c.path = wav;
    c.desc = desc;
    m.push(std::move(c));
    return c.id;
}

SoundId WorldSound::play_cfg(const std::filesystem::path& sound_cfg, const EmitterDesc& base,
                             std::string_view trigger) {
    Impl& m = *impl_;
    if (!ready() || !m.enabled.load()) return 0;
    const auto cfg = load_sound_cfg(sound_cfg);
    if (!cfg) return 0;

    SoundId gid = 0;
    std::vector<SoundId> ids;
    for (const auto& d : cfg->defs) {
        if (std::string_view(d.trigger) != trigger) continue;
        bool holds = true;
        for (const auto& cond : d.conditions) {
            float v = 0.f;
            {
                std::lock_guard lk(m.var_mu);
                if (const auto it = m.vars.find(cond.var); it != m.vars.end()) v = it->second;
            }
            if (!sound_condition_holds(cond, v)) {
                holds = false;
                break;
            }
        }
        if (!holds) continue;

        EmitterDesc e = base;
        if (d.has_3d) {
            e.pos = object_offset_to_world(d.offset, base.yaw_deg, base.pos);
            e.range_m = d.range_m;
        }
        e.volume = d.volume;
        e.loop = d.kind == SoundDef::Kind::Loop && !d.no_loop;
        e.pitch = 1.f;
        if (!d.pitch_var.empty() && d.pitch_ref > 0.f) {
            float v = d.pitch_ref;
            {
                std::lock_guard lk(m.var_mu);
                if (const auto it = m.vars.find(d.pitch_var); it != m.vars.end()) v = it->second;
            }
            e.pitch = clampf(v / d.pitch_ref, 0.05f, 4.f);
        }
        Command c;
        c.kind = Command::Kind::Start;
        c.id = m.next_id.fetch_add(1);
        c.path = d.wav;
        c.desc = e;
        ids.push_back(c.id);
        m.push(std::move(c));
    }
    if (ids.empty()) return 0;
    gid = m.next_id.fetch_add(1);
    std::lock_guard lk(m.group_mu);
    m.groups.emplace(gid, std::move(ids));
    return gid;
}

void WorldSound::stop(SoundId id) {
    Impl& m = *impl_;
    if (!ready()) return;
    std::vector<SoundId> targets{id};
    {
        std::lock_guard lk(m.group_mu);
        if (const auto it = m.groups.find(id); it != m.groups.end()) {
            targets = it->second;
            m.groups.erase(it);
        }
    }
    for (SoundId t : targets) {
        Command c;
        c.kind = Command::Kind::Stop;
        c.id = t;
        m.push(std::move(c));
    }
}

void WorldSound::set_position(SoundId id, Vec3f pos) {
    Impl& m = *impl_;
    if (!ready() || id == 0) return;
    std::lock_guard lk(m.group_mu);
    std::vector<SoundId> targets{id};
    if (const auto it = m.groups.find(id); it != m.groups.end()) targets = it->second;
    for (SoundId t : targets) {
        Command c;
        c.kind = Command::Kind::Pos;
        c.id = t;
        c.pos = pos;
        m.push(std::move(c));
    }
}

void WorldSound::set_gain(SoundId id, float volume) {
    Impl& m = *impl_;
    if (!ready() || id == 0) return;
    Command c;
    c.kind = Command::Kind::Gain;
    c.id = id;
    c.value = volume;
    m.push(std::move(c));
}

void WorldSound::set_pitch(SoundId id, float pitch) {
    Impl& m = *impl_;
    if (!ready() || id == 0) return;
    Command c;
    c.kind = Command::Kind::Pitch;
    c.id = id;
    c.value = pitch;
    m.push(std::move(c));
}

void WorldSound::set_var(std::string_view name, float value) {
    std::lock_guard lk(impl_->var_mu);
    impl_->vars[std::string(name)] = value;
}

float WorldSound::var(std::string_view name, float fallback) const {
    std::lock_guard lk(impl_->var_mu);
    const auto it = impl_->vars.find(std::string(name));
    return it != impl_->vars.end() ? it->second : fallback;
}

int WorldSound::active_voices() const { return impl_->active.load(std::memory_order_relaxed); }

} // namespace omsi::sound
