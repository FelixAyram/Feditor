#pragma once

// Self-contained ImGui window "Sonido 3D": mute toggle + master volume for world sound.
// Kept separate from the options dialog so the settings session can merge without conflicts.

namespace omsi::sound {

struct WorldSoundUi {
    bool enabled = true;
    float volume_pct = 70.f; // 0..100
};

enum WorldSoundUiResult : int {
    kWorldSoundUnchanged = 0,
    kWorldSoundChanged = 1, // apply to WorldSound now
    kWorldSoundCommit = 2,  // also persist (checkbox toggled, or slider released)
};

// Draws the window and returns a bit mask of WorldSoundUiResult values.
int draw_world_sound_window(WorldSoundUi& s, int candidates, int live_objects, int live_voices);

} // namespace omsi::sound
