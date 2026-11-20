#include "omsi/spline_types.hpp"

#include "gfx/dx12/dx12_texture.hpp"
#include "omsi/logger.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace omsi::spline {
namespace {

void write_line(std::ostream& out, const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out << buf << '\n';
}

void append_sli(std::ostream& out, const SplineDefinition& def) {
    if (!def.height_profiles.empty()) {
        out << "---------------------------\n      Height Profiles\n---------------------------\n\n";
        for (const auto& hp : def.height_profiles) {
            out << "[heightprofile]\n";
            write_line(out, "%.3f", hp.left);
            write_line(out, "%.3f", hp.right);
            write_line(out, "%.3f", hp.height);
            write_line(out, "%.3f", hp.skew);
            out << '\n';
        }
    }

    if (!def.textures.empty()) {
        out << "---------------------------\n         Textures\n---------------------------\n\n";
        for (std::size_t ti = 0; ti < def.textures.size(); ++ti) {
            out << "[texture]\n" << def.textures[ti] << "\n\n";
            if (ti < def.texture_alpha.size() && def.texture_alpha[ti] != 0) {
                out << "[matl_alpha]\n" << static_cast<int>(def.texture_alpha[ti]) << "\n\n";
            }
        }
    }

    if (!def.profiles.empty()) {
        out << "---------------------------\n     Graphical Lanes\n---------------------------\n\n";
        for (std::size_t pi = 0; pi < def.profiles.size(); ++pi) {
            const auto& strip = def.profiles[pi];
            if (strip.empty()) continue;
            if (pi < def.profile_comments.size() && !def.profile_comments[pi].empty())
                out << "### " << def.profile_comments[pi] << " ###\n\n";
            out << "[profile]\n" << strip.front().material_id << "\n\n";
            for (const auto& pt : strip) {
                out << "[profilepnt]\n";
                write_line(out, "%.3f", pt.x);
                write_line(out, "%.3f", pt.z);
                write_line(out, "%.3f", pt.uv_x);
                write_line(out, "%.3f", pt.y_tiling);
                out << '\n';
            }
        }
    }

    if (!def.paths.empty()) {
        out << "---------------------------\n          Paths\n---------------------------\n\n";
        for (const auto& p : def.paths) {
            out << (p.secondary ? "[path_2]\n" : "[path]\n");
            write_line(out, "%d", p.typ);
            write_line(out, "%.3f", p.lateral);
            write_line(out, "%.3f", p.height);
            write_line(out, "%.3f", p.width);
            write_line(out, "%d", p.direction);
            out << '\n';
        }
    }

    if (def.only_editor) out << "[onlyeditor]\n\n";

    if (def.patchwork.valid) {
        out << "[patchwork_chain]\n";
        write_line(out, "%.3f", def.patchwork.seg_length);
        out << def.patchwork.transitions << '\n';
        out << def.patchwork.factors << '\n';
        out << def.patchwork.inverse << '\n';
    }
}

std::optional<std::filesystem::path> resolve_tex_src(
    const std::filesystem::path& sli_base, const std::string& name, const std::filesystem::path& omsi_root,
    const std::unordered_map<std::string, std::filesystem::path>& sources) {
    if (const auto it = sources.find(name); it != sources.end() && !it->second.empty()) return it->second;
    return gfx::dx12::resolve_texture_path(sli_base, name, omsi_root);
}

} // namespace

std::string format_sli(const SplineDefinition& def) {
    std::ostringstream out;
    append_sli(out, def);
    return out.str();
}

std::vector<std::string> validate_sli(const SplineDefinition& def) {
    std::vector<std::string> warnings;
    if (def.textures.empty()) warnings.push_back("Sin [texture] — OMSI no dibujará la spline.");
    if (def.profiles.empty()) warnings.push_back("Sin [profile] — perfil vacío.");
    for (std::size_t pi = 0; pi < def.profiles.size(); ++pi) {
        if (def.profiles[pi].size() < 2)
            warnings.push_back("Perfil " + std::to_string(pi) + " tiene menos de 2 puntos.");
    }
    f4f23ma::f32 min_x = 0.f, max_x = 0.f;
    bool have_x = false;
    for (const auto& strip : def.profiles) {
        for (const auto& pt : strip) {
            if (!have_x) {
                min_x = max_x = pt.x;
                have_x = true;
            } else {
                min_x = (std::min)(min_x, pt.x);
                max_x = (std::max)(max_x, pt.x);
            }
        }
    }
    for (std::size_t i = 0; i < def.paths.size(); ++i) {
        const auto& p = def.paths[i];
        if (have_x && (p.lateral < min_x - 0.05f || p.lateral > max_x + 0.05f))
            warnings.push_back("Path " + std::to_string(i) + " lateral fuera del perfil (" +
                               std::to_string(p.lateral) + " m).");
    }
    return warnings;
}

bool write_sli(const SplineDefinition& def, const std::filesystem::path& sli_path) {
    std::ofstream out(sli_path);
    if (!out) {
        log::warn("write_sli: no se pudo abrir " + sli_path.string());
        return false;
    }
    append_sli(out, def);
    log::info("write_sli ok " + sli_path.filename().string());
    return true;
}

bool export_sli_with_textures(const SplineDefinition& def, const std::filesystem::path& sli_path,
                              const std::filesystem::path& omsi_root,
                              const std::unordered_map<std::string, std::filesystem::path>& tex_sources) {
    if (!write_sli(def, sli_path)) return false;
    if (def.textures.empty()) return true;

    const auto tex_dir = sli_path.parent_path() / "texture";
    std::error_code ec;
    std::filesystem::create_directories(tex_dir, ec);

    for (const auto& name : def.textures) {
        const auto src = resolve_tex_src(sli_path, name, omsi_root, tex_sources);
        if (!src) {
            log::warn("export_sli: textura no encontrada " + name);
            continue;
        }
        const auto dst = tex_dir / src->filename();
        std::filesystem::copy_file(*src, dst, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) log::warn("export_sli: copy fail " + src->string() + " -> " + dst.string());
        const std::size_t ti = static_cast<std::size_t>(
            std::distance(def.textures.begin(), std::find(def.textures.begin(), def.textures.end(), name)));
        if (ti < def.texture_extra.size()) {
            const auto& ex = def.texture_extra[ti];
            if (ex.puddles || ex.moisture) {
                std::ostringstream cfg;
                if (ex.puddles) cfg << "[puddles]\n";
                if (ex.moisture) cfg << "[moisture]\n";
                const auto cfg_path = dst.string() + ".cfg";
                std::ofstream cfg_out(cfg_path);
                if (cfg_out) cfg_out << cfg.str();
            }
            if (ex.damage_surf)
                log::info("export_sli: damage .surf pendiente para " + name + " (marca activa; copiar manual si hace falta)");
        }
    }
    return true;
}

} // namespace omsi::spline
