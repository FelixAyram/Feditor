#include "omsi/utf8_win32.hpp"
#include "omsi/cti.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"

namespace omsi::cti {
namespace {

bool parse_float(const std::string& text, float& out) {
    try {
        out = std::stof(text);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

std::optional<CtiDocument> load_cti(const std::filesystem::path& path) {
    const auto doc = cfg::load_cfg_file(path);
    if (!doc) return std::nullopt;

    CtiDocument out{};
    out.path = path;
    out.title = omsi::path_text(path.stem());

    // Title often appears as first non-section comment block; stem is enough for runtime.
    if (const auto it = doc->find("[item]"); it != doc->end()) {
        for (const auto& block : it->second) {
            CtiItem item{};
            if (block.size() >= 1) item.label = block[0];
            if (block.size() >= 2) item.texture_var = block[1];
            if (block.size() >= 3) item.texture_file = block[2];
            out.items.push_back(std::move(item));
        }
    }

    if (const auto it = doc->find("[setvar]"); it != doc->end()) {
        for (const auto& block : it->second) {
            if (block.size() < 2) continue;
            float v = 0.f;
            if (parse_float(block[1], v)) out.setvars[block[0]] = v;
        }
    }

    omsi::log::info("cti: " + omsi::path_text(path) + " items=" + std::to_string(out.items.size()) +
                    " setvars=" + std::to_string(out.setvars.size()));
    return out;
}

} // namespace omsi::cti
