#include "omsi/utf8_win32.hpp"
#include "omsi/sound/sound_runtime.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"
#include "miniaudio/miniaudio.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <mmsystem.h>
#include <audioclient.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <thread>
#include <unordered_set>

namespace omsi::sound {
namespace {

std::unordered_set<std::string> g_missing_trigger_warned;
std::unordered_set<std::string> g_missing_wav_warned;

constexpr auto kOneshotMinInterval = std::chrono::milliseconds(250);
constexpr auto kOneshotGearInterval = std::chrono::milliseconds(750);
constexpr float kAudibleEps = 0.01f;
constexpr float kAudibleStopEps = 0.002f;
// OMSI keeps voices open and sets volume=0; only free the device after a quiet hold.
constexpr float kMuteHoldSec = 0.85f;
// conditionSingle flicker (kompressor / hupe) must not restart the sample every ~100ms.
constexpr float kCondOnHoldSec = 0.08f;
constexpr float kCondOffHoldSec = 0.45f;
constexpr float kPitchUpdateEps = 0.002f;
constexpr float kGainUpdateEps = 0.004f;
// Soft approach toward curve targets (~100ms full-scale for gain).
constexpr float kGainSlewPerSec = 10.f;
constexpr float kPitchSlewPerSec = 15.f;

float approach(float current, float target, float rate_per_sec, float dt) {
    const float max_delta = rate_per_sec * dt;
    const float d = target - current;
    if (std::fabs(d) <= max_delta) return target;
    return current + (d > 0.f ? max_delta : -max_delta);
}

bool is_gear_oneshot_trigger(std::string_view id) {
    return id.find("gear_") != std::string_view::npos || id.find("upshift") != std::string_view::npos ||
           id.find("downshift") != std::string_view::npos || id.find("schaltruck") != std::string_view::npos;
}

std::string trim(std::string_view s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return std::string(s.substr(a, b - a + 1));
}

bool is_numeric_token(std::string_view line) {
    if (line.empty()) return false;
    for (char ch : line) {
        if (!(std::isdigit(static_cast<unsigned char>(ch)) || ch == '-' || ch == '+' || ch == '.'))
            return false;
    }
    return true;
}

// OMSI resolves bare filenames under Sound/ and one level of subfolders (e.g. Allison/).
std::filesystem::path resolve_sound_wav(const std::filesystem::path& sound_dir, const std::string& rel) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path raw(rel);
    const fs::path direct = sound_dir / raw;
    if (fs::exists(direct, ec)) return direct;
    if (raw.is_absolute() && fs::exists(raw, ec)) return raw;

    const fs::path name = raw.filename();
    if (name.empty()) return direct;

    const fs::path in_root = sound_dir / name;
    if (fs::exists(in_root, ec)) return in_root;

    for (fs::directory_iterator it(sound_dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const fs::path cand = it->path() / name;
        if (fs::exists(cand, ec)) return cand;
    }
    return direct;
}

float sample_curve(const std::vector<std::pair<float, float>>& pts, float x) {
    if (pts.empty()) return 1.f;
    if (pts.size() == 1) return pts.front().second;
    // OMSI: outside the defined x-range the curve is 0.
    if (x < pts.front().first || x > pts.back().first) return 0.f;
    for (std::size_t i = 1; i < pts.size(); ++i) {
        const float x0 = pts[i - 1].first;
        const float y0 = pts[i - 1].second;
        const float x1 = pts[i].first;
        const float y1 = pts[i].second;
        if (x <= x1 || i + 1 == pts.size()) {
            if (std::fabs(x1 - x0) < 1e-8f) return y1;
            const float t = (x - x0) / (x1 - x0);
            return y0 + t * (y1 - y0);
        }
    }
    return pts.back().second;
}

// [volcurve] -1: before the first point stay at 0 (delay); after the last point hold y.
float sample_age_curve(const std::vector<std::pair<float, float>>& pts, float age) {
    if (pts.empty()) return 1.f;
    if (age < pts.front().first) return 0.f;
    if (age >= pts.back().first) return pts.back().second;
    return sample_curve(pts, age);
}

} // namespace

// One software mixer per vehicle: a single miniaudio playback device, every voice resampled
// (linear interpolation, so [pitch] really changes the pitch) and mixed in float, gain and pitch
// glide per sample (no zipper noise, no clicks on loop wrap), soft clip on the sum.
// Replaces the old waveOut-per-voice backend (one device per sound, playback-rate pitch that most
// drivers ignore, re-opened devices on every loop start: the source of the choppy engine sound).
struct SoundRuntime::Mixer {
    struct Clip {
        std::vector<float> samples; // interleaved
        std::uint32_t channels = 1;
        std::uint32_t rate = 44100;
        std::size_t frames = 0;
    };
    struct Voice {
        std::shared_ptr<const Clip> clip;
        double pos = 0.0;
        float cur_gain = 0.f, tgt_gain = 0.f;
        float cur_pitch = 1.f, tgt_pitch = 1.f;
        bool loop = false;
        bool stopping = false;
        bool done = false;
        int id = -1;
    };
    static constexpr std::size_t kMaxVoices = 64;

    ma_device device{};
    bool ok = false;
    std::uint32_t rate = 48000;
    float gain_k = 0.004f;  // ~5 ms one-pole
    float pitch_k = 0.001f; // ~20 ms one-pole
    std::mutex mu;
    std::vector<Voice> voices;
    // diagnostics (read by SoundRuntime::update's 5 s summary)
    std::atomic<std::uint64_t> frames_rendered{0};
    std::atomic<int> voice_count{0};
    std::atomic<float> peak{0.f};

    static float soft_clip(float x) {
        const float a = std::fabs(x);
        if (a <= 0.9f) return x;
        const float y = 0.9f + 0.1f * std::tanh((a - 0.9f) / 0.1f);
        return x < 0.f ? -y : y;
    }

    static void data_cb(ma_device* dev, void* out_v, const void*, ma_uint32 frames) {
        auto* self = static_cast<Mixer*>(dev->pUserData);
        auto* out = static_cast<float*>(out_v);
        const std::uint32_t ch = dev->playback.channels;
        std::fill(out, out + static_cast<std::size_t>(frames) * ch, 0.f);
        std::lock_guard lk(self->mu);
        for (auto& v : self->voices) {
            if (v.done || !v.clip || v.clip->frames == 0) {
                v.done = true;
                continue;
            }
            const Clip& c = *v.clip;
            const std::size_t n = c.frames;
            const std::uint32_t cch = c.channels;
            const double base_step = static_cast<double>(c.rate) / static_cast<double>(self->rate);
            const float tgt_gain = v.stopping ? 0.f : v.tgt_gain;
            for (ma_uint32 f = 0; f < frames; ++f) {
                v.cur_gain += (tgt_gain - v.cur_gain) * self->gain_k;
                v.cur_pitch += (v.tgt_pitch - v.cur_pitch) * self->pitch_k;
                std::size_t i0 = static_cast<std::size_t>(v.pos);
                if (i0 >= n) {
                    if (!v.loop) {
                        v.done = true;
                        break;
                    }
                    v.pos = std::fmod(v.pos, static_cast<double>(n));
                    i0 = (std::min)(static_cast<std::size_t>(v.pos), n - 1);
                }
                const std::size_t i1 = i0 + 1 < n ? i0 + 1 : (v.loop ? 0 : i0);
                const float t = static_cast<float>(v.pos - static_cast<double>(i0));
                const float* a = &c.samples[i0 * cch];
                const float* b = &c.samples[i1 * cch];
                const float l = a[0] + (b[0] - a[0]) * t;
                const float r = cch > 1 ? a[1] + (b[1] - a[1]) * t : l;
                const float g = v.cur_gain;
                out[f * ch] += l * g;
                if (ch > 1) out[f * ch + 1] += r * g;
                v.pos += base_step * (std::max)(v.cur_pitch, 0.01f);
            }
            if (v.stopping && v.cur_gain < 1e-4f) v.done = true;
        }
        std::erase_if(self->voices, [](const Voice& v) { return v.done; });
        const std::size_t total = static_cast<std::size_t>(frames) * ch;
        float pk = 0.f;
        for (std::size_t i = 0; i < total; ++i) {
            out[i] = soft_clip(out[i]);
            pk = (std::max)(pk, std::fabs(out[i]));
        }
        self->frames_rendered.fetch_add(frames, std::memory_order_relaxed);
        self->voice_count.store(static_cast<int>(self->voices.size()), std::memory_order_relaxed);
        if (pk > self->peak.load(std::memory_order_relaxed)) self->peak.store(pk, std::memory_order_relaxed);
    }

    bool init() {
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 2;
        cfg.sampleRate = 48000;
        cfg.periodSizeInMilliseconds = 10;
        cfg.dataCallback = &Mixer::data_cb;
        cfg.pUserData = this;
        if (ma_device_init(nullptr, &cfg, &device) != MA_SUCCESS) return false;
        rate = device.sampleRate ? device.sampleRate : 48000;
        gain_k = 1.f - std::exp(-1.f / (0.005f * static_cast<float>(rate)));
        pitch_k = 1.f - std::exp(-1.f / (0.020f * static_cast<float>(rate)));
#ifdef _WIN32
        // The old waveOut backend called waveOutSetVolume, which on Vista+ sets this *application's*
        // session volume in the Windows mixer - and Windows remembers it per .exe. Muted loops left
        // it at 0 %, so every later run (any backend) was silent. Put the session back to 100 %.
        if (device.pContext && device.pContext->backend == ma_backend_wasapi && device.wasapi.pAudioClientPlayback) {
            auto* client = static_cast<IAudioClient*>(device.wasapi.pAudioClientPlayback);
            ISimpleAudioVolume* vol = nullptr;
            if (SUCCEEDED(client->GetService(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&vol))) && vol) {
                float level = -1.f;
                BOOL mute = FALSE;
                vol->GetMasterVolume(&level);
                vol->GetMute(&mute);
                if (level < 0.99f || mute) {
                    omsi::log::warn("OMSI Sound: app session volume was " + std::to_string(level) +
                                    (mute ? " (muted)" : "") + " - restored to 100%");
                    vol->SetMute(FALSE, nullptr);
                    vol->SetMasterVolume(1.f, nullptr);
                }
                vol->Release();
            }
        }
#endif
        if (ma_device_start(&device) != MA_SUCCESS) {
            ma_device_uninit(&device);
            return false;
        }
        ok = true;
        return true;
    }

    void shutdown() {
        if (ok) ma_device_uninit(&device);
        ok = false;
        std::lock_guard lk(mu);
        voices.clear();
    }

    static std::shared_ptr<const Clip> decode(const std::filesystem::path& path) {
        ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, 0);
        ma_decoder dec{};
#ifdef _WIN32
        if (ma_decoder_init_file_w(path.c_str(), &dc, &dec) != MA_SUCCESS) return nullptr;
#else
        if (ma_decoder_init_file(path.c_str(), &dc, &dec) != MA_SUCCESS) return nullptr;
#endif
        auto clip = std::make_shared<Clip>();
        clip->channels = (std::max)(dec.outputChannels, 1u);
        clip->rate = dec.outputSampleRate ? dec.outputSampleRate : 44100;
        ma_uint64 len = 0;
        if (ma_decoder_get_length_in_pcm_frames(&dec, &len) != MA_SUCCESS || len == 0) len = 0;
        std::vector<float> buf;
        if (len > 0) {
            buf.resize(static_cast<std::size_t>(len) * clip->channels);
            ma_uint64 got = 0;
            ma_decoder_read_pcm_frames(&dec, buf.data(), len, &got);
            buf.resize(static_cast<std::size_t>(got) * clip->channels);
        } else {
            float tmp[4096];
            const ma_uint64 chunk = 4096 / clip->channels;
            for (;;) {
                ma_uint64 got = 0;
                if (ma_decoder_read_pcm_frames(&dec, tmp, chunk, &got) != MA_SUCCESS || got == 0) break;
                buf.insert(buf.end(), tmp, tmp + got * clip->channels);
            }
        }
        ma_decoder_uninit(&dec);
        clip->frames = buf.size() / clip->channels;
        clip->samples = std::move(buf);
        if (clip->frames == 0) return nullptr;
        return clip;
    }

    void add(std::shared_ptr<const Clip> clip, float gain, float pitch, bool loop, int id) {
        std::lock_guard lk(mu);
        if (loop && id >= 0) {
            for (auto& v : voices)
                if (v.id == id) v.stopping = true; // fade the previous take of this loop
        }
        if (voices.size() >= kMaxVoices) {
            // Drop the quietest finished-soon oneshot to make room.
            auto it = std::min_element(voices.begin(), voices.end(), [](const Voice& a, const Voice& b) {
                return (a.loop ? 1e9f : a.cur_gain) < (b.loop ? 1e9f : b.cur_gain);
            });
            if (it == voices.end() || it->loop) return;
            voices.erase(it);
        }
        Voice v;
        v.clip = std::move(clip);
        v.tgt_gain = (std::max)(gain, 0.f);
        // Oneshots start at full gain (attack is in the sample); loops fade in over ~5 ms.
        v.cur_gain = loop ? 0.f : v.tgt_gain;
        v.tgt_pitch = v.cur_pitch = (std::max)(pitch, 0.01f);
        v.loop = loop;
        v.id = loop ? id : -1;
        voices.push_back(std::move(v));
    }

    void params(int id, float gain, float pitch) {
        std::lock_guard lk(mu);
        for (auto& v : voices) {
            if (v.id != id || v.stopping) continue;
            v.tgt_gain = (std::max)(gain, 0.f);
            v.tgt_pitch = (std::max)(pitch, 0.01f);
        }
    }

    void stop(int id) {
        std::lock_guard lk(mu);
        for (auto& v : voices)
            if (v.id == id) v.stopping = true;
    }
};

SoundRuntime::SoundRuntime() = default;

SoundRuntime::~SoundRuntime() { stop_worker(); }

void SoundRuntime::start_worker() {
    if (worker_.joinable()) return;
    if (!mixer_) {
        mixer_ = std::make_unique<Mixer>();
        if (!mixer_->init()) omsi::log::warn("OMSI Sound: miniaudio device init failed - vehicle sound disabled");
        else omsi::log::info("OMSI Sound: mixer ready (miniaudio, " + std::to_string(mixer_->rate) + " Hz)");
    }
    stop_.store(false);
    worker_ = std::thread([this] {
        try {
            worker_main();
        } catch (const std::exception& ex) {
            omsi::log::error(std::string("OMSI Sound worker exception: ") + ex.what());
        } catch (...) {
            omsi::log::error("OMSI Sound worker exception");
        }
    });
}

void SoundRuntime::stop_worker() {
    stop_.store(true);
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (mixer_) {
        mixer_->shutdown();
        mixer_.reset();
    }
    std::lock_guard lock(queue_mutex_);
    queue_.clear();
}

void SoundRuntime::enqueue(PlayCommand cmd) {
    {
        std::lock_guard lock(queue_mutex_);
        // Coalesce pending pitch/volume updates so the worker never falls behind.
        if (cmd.kind == PlayCommand::Kind::loop_params) {
            for (auto& q : queue_) {
                if (q.kind == PlayCommand::Kind::loop_params && q.loop_id == cmd.loop_id) {
                    q = std::move(cmd);
                    queue_cv_.notify_one();
                    return;
                }
            }
        }
        queue_.push_back(std::move(cmd));
    }
    queue_cv_.notify_one();
}

// The worker only does disk I/O and decoding (never on the audio or render thread); voices live
// in the mixer.
void SoundRuntime::worker_main() {
    std::unordered_map<std::string, std::shared_ptr<const Mixer::Clip>> clip_cache;
    while (!stop_.load(std::memory_order_acquire)) {
        PlayCommand cmd{};
        {
            std::unique_lock lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return stop_.load() || !queue_.empty(); });
            if (stop_.load() && queue_.empty()) break;
            cmd = std::move(queue_.front());
            queue_.pop_front();
        }
        if (!mixer_ || !mixer_->ok) continue;

        if (cmd.kind == PlayCommand::Kind::loop_stop) {
            mixer_->stop(cmd.loop_id);
            continue;
        }
        if (cmd.kind == PlayCommand::Kind::loop_params) {
            mixer_->params(cmd.loop_id, cmd.volume, cmd.pitch);
            continue;
        }

        const std::string cache_key = omsi::path_text(cmd.path);
        auto it = clip_cache.find(cache_key);
        if (it == clip_cache.end()) {
            std::error_code ec;
            if (!std::filesystem::exists(cmd.path, ec)) {
                if (g_missing_wav_warned.insert(cache_key).second)
                    omsi::log::warn("OMSI Sound: missing '" + cache_key + "'");
                clip_cache.emplace(cache_key, nullptr);
                continue;
            }
            auto clip = Mixer::decode(cmd.path);
            if (!clip) omsi::log::warn("OMSI Sound: could not decode '" + cache_key + "'");
            it = clip_cache.emplace(cache_key, std::move(clip)).first;
        }
        if (!it->second) continue;

        if (cmd.kind == PlayCommand::Kind::one_shot)
            mixer_->add(it->second, std::clamp(cmd.volume, 0.f, 1.f), cmd.pitch, false, -1);
        else if (cmd.kind == PlayCommand::Kind::loop_start)
            mixer_->add(it->second, std::clamp(cmd.volume, 0.f, 1.f), cmd.pitch, true, cmd.loop_id);
    }
}

bool SoundRuntime::load(const std::filesystem::path& bus_dir, const std::filesystem::path& sound_cfg_rel) {
    stop_worker();
    triggers_.clear();
    loops_.clear();
    oneshot_last_fire_.clear();
    oneshot_dbg_count_.clear();
    g_missing_trigger_warned.clear();
    g_missing_wav_warned.clear();
    dbg_summary_age_ = 0.f;
    if (sound_cfg_rel.empty()) return false;

    const auto path = bus_dir / sound_cfg_rel;
    sound_dir_ = path.parent_path();
    const auto lines = cfg::read_cfg_lines(path);
    if (lines.empty()) {
        omsi::log::error("OMSI Sound: could not read '" + omsi::path_text(path) + "'");
        return false;
    }

    std::filesystem::path pending_wav;
    std::vector<VolCurve> pending_curves;
    std::vector<CondClause> pending_conds;
    std::string pending_var;
    std::string pending_pitch_var;
    std::string pending_linked_trigger;
    float pending_pitch_ref = 0.f;
    float pending_base_vol = 1.f;
    int pending_op = 3;
    float pending_value = 0.f;
    bool pending_is_loopsound = false;
    bool pending_had_trigger = false;
    bool pending_noloop = false;
    bool ignore_pnts = false;

    enum class Mode {
        none,
        sound_wav,
        sound_vol,
        loopsound_wav,
        loopsound_rate,
        loopsound_var,
        loopsound_ref,
        loopsound_vol,
        trigger_name,
        cond_var,
        cond_value,
        cond_op,
        volcurve_var,
        pnt_x,
        pnt_y
    } mode = Mode::none;

    auto sort_curves = [](std::vector<VolCurve>& curves) {
        for (auto& vc : curves) {
            std::sort(vc.points.begin(), vc.points.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
        }
    };

    auto clear_pending = [&] {
        pending_wav.clear();
        pending_curves.clear();
        pending_conds.clear();
        pending_var.clear();
        pending_pitch_var.clear();
        pending_linked_trigger.clear();
        pending_pitch_ref = 0.f;
        pending_base_vol = 1.f;
        pending_op = 3;
        pending_value = 0.f;
        pending_is_loopsound = false;
        pending_had_trigger = false;
        pending_noloop = false;
        ignore_pnts = false;
    };

    auto flush_pending_loop = [&] {
        if (pending_wav.empty()) {
            clear_pending();
            return;
        }
        // OMSI cfg: "[sound]+[trigger] looped automatisch NICHT" — oneshot only; volcurves
        // are sampled once when the trigger fires (stored on TriggerSound).
        if (pending_had_trigger && !pending_is_loopsound) {
            clear_pending();
            return;
        }
        // [trigger]+[loopsound] without condition (gear up/downshift): already registered as
        // T.L. oneshot. Continuous update would LOOP+/LOOP- while driving and chop the mix.
        if (!pending_linked_trigger.empty() && pending_is_loopsound && pending_conds.empty() &&
            !pending_noloop) {
            clear_pending();
            return;
        }
        const bool want_loop =
            pending_is_loopsound || !pending_conds.empty() || !pending_curves.empty();
        if (!want_loop) {
            clear_pending();
            return;
        }

        ConditionalLoop cl{};
        cl.conditions = pending_conds;
        cl.has_condition = !pending_conds.empty();
        cl.vol_curves = pending_curves;
        sort_curves(cl.vol_curves);
        cl.wav_path = pending_wav;
        cl.base_volume = pending_base_vol;
        cl.pitch_var = pending_pitch_var;
        cl.pitch_ref = pending_pitch_ref;
        cl.kind = pending_is_loopsound ? "loopsound" : "sound";
        cl.linked_trigger = pending_linked_trigger;
        cl.no_loop = pending_noloop;
        cl.loop_id = next_loop_id_++;
        loops_.push_back(std::move(cl));
        clear_pending();
    };

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto line = trim(lines[i]);
        if (line.empty() || line.front() == '\'') continue;

        if (line == "[sound]") {
            flush_pending_loop();
            mode = Mode::sound_wav;
            continue;
        }
        if (line == "[loopsound]") {
            flush_pending_loop();
            mode = Mode::loopsound_wav;
            pending_is_loopsound = true;
            continue;
        }
        if (line == "[trigger]") {
            mode = Mode::trigger_name;
            continue;
        }
        if (line == "[noloop]") {
            pending_noloop = true;
            mode = Mode::none;
            continue;
        }
        if (line == "[conditionSingle]") {
            mode = Mode::cond_var;
            pending_var.clear();
            continue;
        }
        if (line == "[volcurve]") {
            mode = Mode::volcurve_var;
            ignore_pnts = false;
            continue;
        }
        if (line == "[pnt]") {
            if (!ignore_pnts && !pending_curves.empty()) mode = Mode::pnt_x;
            else mode = Mode::none;
            continue;
        }
        if (line.size() >= 3 && line.front() == '[') {
            mode = Mode::none;
            continue;
        }

        if (mode == Mode::sound_wav) {
            pending_wav = resolve_sound_wav(sound_dir_, line);
            mode = Mode::sound_vol;
            continue;
        }
        if (mode == Mode::sound_vol) {
            try {
                pending_base_vol = std::stof(line);
            } catch (...) {
                pending_base_vol = 1.f;
            }
            mode = Mode::none;
            continue;
        }
        if (mode == Mode::loopsound_wav) {
            pending_wav = resolve_sound_wav(sound_dir_, line);
            mode = Mode::loopsound_rate;
            continue;
        }
        if (mode == Mode::loopsound_rate) {
            // Sample-rate hint from cfg; pitch uses pitch_ref vs live var instead.
            mode = Mode::loopsound_var;
            continue;
        }
        if (mode == Mode::loopsound_var) {
            pending_pitch_var = line;
            mode = Mode::loopsound_ref;
            continue;
        }
        if (mode == Mode::loopsound_ref) {
            try {
                pending_pitch_ref = std::stof(line);
            } catch (...) {
                pending_pitch_ref = 1.f;
            }
            mode = Mode::loopsound_vol;
            continue;
        }
        if (mode == Mode::loopsound_vol) {
            try {
                pending_base_vol = std::stof(line);
            } catch (...) {
                pending_base_vol = 1.f;
            }
            mode = Mode::none;
            continue;
        }
        if (mode == Mode::trigger_name) {
            TriggerSound ts{};
            ts.trigger_id = line;
            ts.wav_path = pending_wav;
            ts.base_volume = pending_base_vol;
            ts.vol_curves = pending_curves;
            sort_curves(ts.vol_curves);
            triggers_.push_back(std::move(ts));
            pending_had_trigger = true;
            pending_linked_trigger = line;
            mode = Mode::none;
            continue;
        }
        if (mode == Mode::volcurve_var) {
            // "-1" = loudness vs voice age (fade-in). Other numeric tokens (e.g. -2) skip.
            if (is_numeric_token(line)) {
                try {
                    const float n = std::stof(line);
                    if (std::fabs(n + 1.f) < 1e-6f) {
                        pending_curves.push_back(VolCurve{.var_name = "age", .is_age = true});
                        ignore_pnts = false;
                    } else {
                        ignore_pnts = true;
                    }
                } catch (...) {
                    ignore_pnts = true;
                }
            } else {
                pending_curves.push_back(VolCurve{.var_name = line});
                ignore_pnts = false;
            }
            mode = Mode::none;
            continue;
        }
        if (mode == Mode::pnt_x) {
            try {
                const float x = std::stof(line);
                if (!pending_curves.empty()) {
                    pending_curves.back().points.emplace_back(x, 0.f);
                }
            } catch (...) {
            }
            mode = Mode::pnt_y;
            continue;
        }
        if (mode == Mode::pnt_y) {
            try {
                const float y = std::stof(line);
                if (!pending_curves.empty() && !pending_curves.back().points.empty()) {
                    pending_curves.back().points.back().second = y;
                }
            } catch (...) {
            }
            mode = Mode::none;
            continue;
        }
        if (mode == Mode::cond_var) {
            pending_var = line;
            mode = Mode::cond_value;
            continue;
        }
        if (mode == Mode::cond_value) {
            try {
                pending_value = std::stof(line);
            } catch (...) {
                pending_value = 0.f;
            }
            mode = Mode::cond_op;
            continue;
        }
        if (mode == Mode::cond_op) {
            try {
                pending_op = std::stoi(line);
            } catch (...) {
                pending_op = 1;
            }
            // OMSI ANDs every [conditionSingle] on the same [sound]/[loopsound] block.
            pending_conds.push_back(
                CondClause{.var_name = pending_var, .compare_op = pending_op, .compare_value = pending_value});
            pending_var.clear();
            mode = Mode::none;
        }
    }
    flush_pending_loop();

    int age_curve_n = 0;
    for (const auto& loop : loops_) {
        for (const auto& vc : loop.vol_curves) {
            if (vc.is_age) ++age_curve_n;
        }
    }

    start_worker();
    omsi::log::info("OMSI Sound: " + omsi::path_text(path.filename()) + " triggers=" + std::to_string(triggers_.size()) +
                    " loops=" + std::to_string(loops_.size()) + " age_volcurves=" + std::to_string(age_curve_n) +
                    " (miniaudio mixer)");
    return true;
}

#include "sound_runtime_part2.inc.cpp"

} // namespace omsi::sound
