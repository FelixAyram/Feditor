#include "omsi/sound/world_sound_window.hpp"

#include <imgui.h>

namespace omsi::sound {

int draw_world_sound_window(WorldSoundUi& s, int candidates, int live_objects, int live_voices) {
    int result = kWorldSoundUnchanged;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 16.f, vp->WorkPos.y + vp->WorkSize.y - 150.f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(260.f, 0.f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Sonido 3D##world_sound")) {
        if (ImGui::Checkbox("Activar sonido", &s.enabled))
            result |= kWorldSoundChanged | kWorldSoundCommit;
        if (ImGui::SliderFloat("Volumen", &s.volume_pct, 0.f, 100.f, "%.0f %%")) result |= kWorldSoundChanged;
        if (ImGui::IsItemDeactivatedAfterEdit()) result |= kWorldSoundCommit;
        ImGui::Separator();
        ImGui::TextDisabled("Objetos con sonido: %d", candidates);
        ImGui::TextDisabled("En rango: %d  Voces: %d", live_objects, live_voices);
    }
    ImGui::End();
    return result;
}

} // namespace omsi::sound
