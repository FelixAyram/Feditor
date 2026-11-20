#include "omsi/editor_sfx.hpp"
#include <filesystem>

namespace omsi::editor {

EditorSfx::~EditorSfx() {}
bool EditorSfx::init(const std::filesystem::path&) { return false; }
void EditorSfx::shutdown() {}
bool EditorSfx::set_pack(const std::string&) { return false; }
void EditorSfx::set_volume(float) {}
void EditorSfx::play(SfxCue, float, float, float) {}
void EditorSfx::resolve_paths_() {}

} // namespace omsi::editor
