#include "omsi/editor_sfx.hpp"

#include "omsi/logger.hpp"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "miniaudio/miniaudio.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>
#include <vector>

namespace omsi::editor {
namespace {

constexpr int kVoicePool = 8;

struct SfxEngine {
    ma_engine eng{};
    ma_sound voices[kVoicePool]{};
    bool voice_alive[kVoicePool]{};
    int steal = 0;
};

const char* cue_filename(SfxCue c) {
    switch (c) {
    case SfxCue::StonePlace:
        return "stone_place.wav";
    case SfxCue::PebbleSoft:
        return "pebble_soft.wav";
    case SfxCue::WoodThud:
        return "wood_thud.wav";
    case SfxCue::GravelScrape:
        return "gravel_scrape.wav";
    case SfxCue::WoodClick:
        return "wood_click.wav";
    case SfxCue::Hint:
        return "hint.wav";
    case SfxCue::UiClick:
        return "ui_click.wav";
    case SfxCue::UndoWhoosh:
        return "undo_whoosh.wav";
    case SfxCue::Error:
        return "error.wav";
    case SfxCue::StreamTick:
        return "stream_tick.wav";
    default:
        return "hint.wav";
    }
}

double now_s() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void write_wav_pcm16(const std::filesystem::path& path, const std::vector<std::int16_t>& pcm, int sr) {
    const std::uint32_t data_bytes = static_cast<std::uint32_t>(pcm.size() * 2);
    std::ofstream out(path, std::ios::binary);
    if (!out) return;
    auto w16 = [&](std::uint16_t v) {
        char b[2]{static_cast<char>(v & 255), static_cast<char>((v >> 8) & 255)};
        out.write(b, 2);
    };
    auto w32 = [&](std::uint32_t v) {
        w16(static_cast<std::uint16_t>(v));
        w16(static_cast<std::uint16_t>(v >> 16));
    };
    out.write("RIFF", 4);
    w32(36 + data_bytes);
    out.write("WAVEfmt ", 8);
    w32(16);
    w16(1);
    w16(1);
    w32(static_cast<std::uint32_t>(sr));
    w32(static_cast<std::uint32_t>(sr * 2));
    w16(2);
    w16(16);
    out.write("data", 4);
    w32(data_bytes);
    out.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(data_bytes));
}

void ensure_placeholder_wav(const std::filesystem::path& path, SfxCue cue) {
    if (std::filesystem::exists(path)) return;
    constexpr int sr = 22050;
    std::vector<std::int16_t> pcm;
    std::mt19937 rng(static_cast<unsigned>(cue) * 9973u + 42u);
    std::uniform_real_distribution<float> nd(-1.f, 1.f);
    const float dur = cue == SfxCue::StonePlace       ? 0.22f
                      : cue == SfxCue::PebbleSoft     ? 0.12f
                      : cue == SfxCue::WoodThud       ? 0.18f
                      : cue == SfxCue::GravelScrape   ? 0.10f
                      : cue == SfxCue::WoodClick      ? 0.06f
                      : cue == SfxCue::UiClick        ? 0.04f
                      : cue == SfxCue::UndoWhoosh     ? 0.16f
                      : cue == SfxCue::Error          ? 0.14f
                      : cue == SfxCue::StreamTick     ? 0.05f
                                                      : 0.14f;
    const int n = static_cast<int>(dur * sr);
    pcm.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(sr);
        const float env = std::exp(-t * (cue == SfxCue::WoodThud ? 18.f : 28.f));
        float s = 0.f;
        switch (cue) {
        case SfxCue::StonePlace:
            s = (0.45f * nd(rng) + 0.25f * std::sin(2.f * 3.14159265f * 90.f * t)) * env;
            break;
        case SfxCue::PebbleSoft:
            s = (0.5f * nd(rng)) * env * 0.7f;
            break;
        case SfxCue::WoodThud:
            s = (0.65f * std::sin(2.f * 3.14159265f * 70.f * t) + 0.2f * nd(rng)) * env;
            break;
        case SfxCue::GravelScrape:
            s = nd(rng) * env * 0.5f;
            break;
        case SfxCue::WoodClick:
            s = (0.4f * nd(rng) + 0.25f * std::sin(2.f * 3.14159265f * 620.f * t)) * env;
            break;
        case SfxCue::Hint:
            s = (0.3f * std::sin(2.f * 3.14159265f * 480.f * t) + 0.15f * nd(rng)) * env * 0.8f;
            break;
        case SfxCue::UiClick:
            s = (0.35f * nd(rng) + 0.2f * std::sin(2.f * 3.14159265f * 1100.f * t)) * env;
            break;
        case SfxCue::UndoWhoosh: {
            const float f = 400.f - 220.f * (t / dur);
            s = (0.35f * std::sin(2.f * 3.14159265f * f * t) + 0.25f * nd(rng)) * env;
            break;
        }
        case SfxCue::Error:
            s = (0.4f * std::sin(2.f * 3.14159265f * 180.f * t) +
                 0.25f * std::sin(2.f * 3.14159265f * 240.f * t)) *
                env;
            break;
        case SfxCue::StreamTick:
            s = std::sin(2.f * 3.14159265f * 880.f * t) * env * 0.4f;
            break;
        default:
            break;
        }
        s = std::clamp(s, -1.f, 1.f);
        // Soft placeholders — was 28000 and felt harsh/repetitive in the editor.
        pcm.push_back(static_cast<std::int16_t>(s * 12000.f));
    }
    write_wav_pcm16(path, pcm, sr);
}

std::string sanitize_pack_id(std::string id) {
    if (id.empty()) return "blocky";
    for (char& c : id) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) c = '_';
    }
    if (id.size() > 32) id.resize(32);
    return id;
}

SfxEngine* as_eng(void* p) { return static_cast<SfxEngine*>(p); }

} // namespace

EditorSfx::~EditorSfx() { shutdown(); }

void EditorSfx::resolve_paths_() {
    if (assets_root_.empty()) return;
    for (int i = 0; i < static_cast<int>(SfxCue::Count); ++i) {
        const auto cue = static_cast<SfxCue>(i);
        const char* name = cue_filename(cue);
        std::filesystem::path chosen = assets_root_ / name;
        if (pack_ != "classic") {
            const auto pack_file = assets_root_ / "packs" / pack_ / name;
            if (std::filesystem::exists(pack_file)) chosen = pack_file;
        } else {
            const auto pack_file = assets_root_ / "packs" / "classic" / name;
            if (std::filesystem::exists(pack_file)) chosen = pack_file;
        }
        // Also try named pack even if classic (covers soft/blocky/synth).
        if (!std::filesystem::exists(chosen) && !pack_.empty()) {
            const auto pack_file = assets_root_ / "packs" / pack_ / name;
            if (std::filesystem::exists(pack_file)) chosen = pack_file;
        }
        ensure_placeholder_wav(chosen, cue);
        paths_[i] = chosen;
        last_play_s_[i] = 0.;
    }
}

bool EditorSfx::init(const std::filesystem::path& assets_dir) {
    shutdown();
    assets_root_ = assets_dir;
    std::error_code ec;
    std::filesystem::create_directories(assets_dir, ec);
    std::filesystem::create_directories(assets_dir / "packs", ec);
    // Ensure root placeholders exist for fallback.
    for (int i = 0; i < static_cast<int>(SfxCue::Count); ++i) {
        ensure_placeholder_wav(assets_dir / cue_filename(static_cast<SfxCue>(i)), static_cast<SfxCue>(i));
    }
    resolve_paths_();
    auto* se = new SfxEngine{};
    if (ma_engine_init(nullptr, &se->eng) != MA_SUCCESS) {
        delete se;
        log::warn("EditorSfx: ma_engine_init failed");
        return false;
    }
    engine_ = se;
    ready_ = true;
    ma_engine_set_volume(&se->eng, volume_);
    log::info("EditorSfx ready dir=" + assets_dir.string() + " pack=" + pack_);
    return true;
}

bool EditorSfx::set_pack(const std::string& pack_id) {
    const std::string next = sanitize_pack_id(pack_id.empty() ? "blocky" : pack_id);
    // Stop all voices before swapping file paths (async decode can UAF old WAV).
    if (auto* se = as_eng(engine_)) {
        for (int i = 0; i < kVoicePool; ++i) {
            if (se->voice_alive[i]) {
                ma_sound_stop(&se->voices[i]);
                ma_sound_uninit(&se->voices[i]);
                se->voice_alive[i] = false;
            }
        }
    }
    pack_ = next;
    if (assets_root_.empty()) return false;
    resolve_paths_();
    log::info("EditorSfx pack=" + pack_);
    return true;
}

void EditorSfx::shutdown() {
    if (auto* se = as_eng(engine_)) {
        for (int i = 0; i < kVoicePool; ++i) {
            if (se->voice_alive[i]) {
                ma_sound_uninit(&se->voices[i]);
                se->voice_alive[i] = false;
            }
        }
        ma_engine_uninit(&se->eng);
        delete se;
        engine_ = nullptr;
    }
    ready_ = false;
}

void EditorSfx::set_volume(float v01) {
    volume_ = std::clamp(v01, 0.f, 1.f);
    if (auto* se = as_eng(engine_)) ma_engine_set_volume(&se->eng, volume_);
}

void EditorSfx::play(SfxCue cue, float pan, float pitch_mul, float vol_mul) {
    if (!ready_ || !enabled_ || !engine_) return;
    const int ci = static_cast<int>(cue);
    if (ci < 0 || ci >= static_cast<int>(SfxCue::Count)) return;
    const double t = now_s();
    // Longer cooldowns so place/edit bursts do not machine-gun.
    const double cool = (cue == SfxCue::UiClick)       ? 0.08
                        : (cue == SfxCue::StreamTick)  ? 0.35
                        : (cue == SfxCue::WoodClick)   ? 0.18
                        : (cue == SfxCue::StonePlace)  ? 0.16
                                                      : 0.14;
    if (t - last_play_s_[ci] < cool) return;
    last_play_s_[ci] = t;
    pan = std::clamp(pan, -0.4f, 0.4f);
    pitch_mul = std::clamp(pitch_mul, 0.85f, 1.2f);
    vol_mul = std::clamp(vol_mul * 0.55f, 0.f, 1.0f);

    auto* se = as_eng(engine_);
    int slot = -1;
    for (int i = 0; i < kVoicePool; ++i) {
        if (!se->voice_alive[i]) {
            slot = i;
            break;
        }
        if (!ma_sound_is_playing(&se->voices[i])) {
            ma_sound_uninit(&se->voices[i]);
            se->voice_alive[i] = false;
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = se->steal++ % kVoicePool;
        if (se->voice_alive[slot]) {
            ma_sound_stop(&se->voices[slot]);
            ma_sound_uninit(&se->voices[slot]);
            se->voice_alive[slot] = false;
        }
    }

    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> pitch_d(0.96f, 1.04f);
    const auto path_u8 = paths_[ci].u8string();
    const std::string path(reinterpret_cast<const char*>(path_u8.data()), path_u8.size());
    // Sync decode: avoids race with set_pack / shutdown while ASYNC decode is in flight.
    if (ma_sound_init_from_file(&se->eng, path.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr,
                                &se->voices[slot]) != MA_SUCCESS) {
        (void)ma_engine_play_sound(&se->eng, path.c_str(), nullptr);
        return;
    }
    se->voice_alive[slot] = true;
    ma_sound_set_pitch(&se->voices[slot], pitch_d(rng) * pitch_mul);
    ma_sound_set_pan(&se->voices[slot], pan);
    ma_sound_set_volume(&se->voices[slot], vol_mul);
    ma_sound_start(&se->voices[slot]);
}

} // namespace omsi::editor
