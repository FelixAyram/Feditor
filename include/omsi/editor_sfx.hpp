#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace omsi::editor {

enum class SfxCue : std::uint8_t {
    StonePlace = 0,  // place / dup / paste SCO
    PebbleSoft,      // delete SCO
    WoodThud,        // release G
    GravelScrape,    // release R
    WoodClick,       // place .sli / SA apply / path snap
    Hint,            // reminder confirm (Kenney soft)
    UiClick,         // button / tab / checkbox
    UndoWhoosh,      // undo (redo via pitch_mul)
    Error,           // bind fail / no parent SA
    StreamTick,      // tile stream ready
    Count
};

// 2D editor cues via miniaudio (not SoundRuntime / bus). Pool + per-cue cooldown.
class EditorSfx {
public:
    EditorSfx() = default;
    ~EditorSfx();
    EditorSfx(const EditorSfx&) = delete;
    EditorSfx& operator=(const EditorSfx&) = delete;

    bool init(const std::filesystem::path& assets_dir);
    void shutdown();

    // Switch pack under assets/editor_sfx/packs/<id>/ (fallback: root). Empty → "blocky".
    bool set_pack(const std::string& pack_id);
    [[nodiscard]] const std::string& pack() const { return pack_; }

    void set_enabled(bool on) { enabled_ = on; }
    [[nodiscard]] bool enabled() const { return enabled_; }
    void set_volume(float v01); // 0..1 (Vista slider 0–100)
    [[nodiscard]] float volume() const { return volume_; }

    // pan: -0.4..0.4 from screen X. pitch_mul: e.g. redo ≈ 1.04 (+1 semitone-ish).
    void play(SfxCue cue, float pan = 0.f, float pitch_mul = 1.f, float vol_mul = 1.f);

private:
    void resolve_paths_();

    bool ready_ = false;
    bool enabled_ = true;
    float volume_ = 0.7f;
    void* engine_ = nullptr; // ma_engine*
    std::filesystem::path assets_root_{};
    std::string pack_ = "blocky";
    std::filesystem::path paths_[static_cast<int>(SfxCue::Count)]{};
    double last_play_s_[static_cast<int>(SfxCue::Count)]{};
};

} // namespace omsi::editor
