#include "omsi/map_attachment.hpp"

#include "omsi/cfg_document.hpp"
#include "omsi/logger.hpp"
#include "omsi/sco_types.hpp"
#include "omsi/spline_types.hpp"
#include "omsi/utf8_win32.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace omsi::map {
namespace {

template <typename T>
T parse_value(const std::string& s) {
    try {
        if constexpr (std::is_same_v<T, std::int32_t>) return std::stoi(s);
        else if constexpr (std::is_same_v<T, f4f23ma::f32>) return std::stof(s);
        else return T{};
    } catch (...) {
        if constexpr (std::is_same_v<T, std::int32_t>) return 0;
        else if constexpr (std::is_same_v<T, f4f23ma::f32>) return 0.f;
        else return T{};
    }
}

std::string trim_line(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string clean_cfg_value(std::string s) {
    s = trim_line(std::move(s));
    const auto eq = s.find('=');
    if (eq != std::string::npos) return trim_line(s.substr(eq + 1));
    return s;
}

bool tag_ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

bool is_spline_attachment_tag(std::string_view tag) {
    auto starts = [&](std::string_view t, std::string_view prefix) {
        if (t.size() < prefix.size()) return false;
        return tag_ieq(t.substr(0, prefix.size()), prefix);
    };
    return starts(tag, "[splineattachement") || starts(tag, "[splineattachment");
}

bool is_object_nr_line(const std::string& t) {
    if (t.size() < 9) return false;
    std::size_t j = 0;
    while (j < t.size() && (t[j] == ' ' || t[j] == '\t')) ++j;
    if (j + 9 > t.size()) return false;
    const std::string_view rest{t.data() + j, t.size() - j};
    return tag_ieq(rest.substr(0, 9), "Object Nr") || tag_ieq(rest.substr(0, 9), "object nr");
}

bool is_section_tag_line(const std::string& t) {
    return t.size() >= 3 && t.front() == '[' && t.back() == ']';
}

std::vector<std::size_t> collect_nonempty_body_lines(const std::vector<std::string>& lines,
                                                     const std::size_t tag_line) {
    std::vector<std::size_t> bl;
    for (std::size_t j = tag_line + 1; j < lines.size(); ++j) {
        const std::string t = trim_line(lines[j]);
        if (t.empty()) continue;
        if (is_section_tag_line(t)) break;
        if (is_object_nr_line(t)) continue;
        bl.push_back(j);
    }
    return bl;
}

f4f23ma::ma4 translation_ma4(const float x, const float y, const float z) {
    auto t = f4f23ma::ma4::identity();
    t.m[3][0] = x;
    t.m[3][1] = y;
    t.m[3][2] = z;
    return t;
}

// Same intrinsic order as omsi_object_rotation_matrix (column M*v: Rz, Rx, Ry).
f4f23ma::ma4 omsi_object_rotation_ma4(const float yaw_deg, const float pitch_deg, const float bank_deg) {
    constexpr float kDeg = 3.14159265f / 180.f;
    const auto rz = [](float a) {
        const float c = std::cos(a), s = std::sin(a);
        f4f23ma::ma4 r = f4f23ma::ma4::identity();
        r.m[0][0] = c;
        r.m[1][0] = -s;
        r.m[0][1] = s;
        r.m[1][1] = c;
        return r;
    };
    const auto rx = [](float a) {
        const float c = std::cos(a), s = std::sin(a);
        f4f23ma::ma4 r = f4f23ma::ma4::identity();
        r.m[1][1] = c;
        r.m[2][1] = -s;
        r.m[1][2] = s;
        r.m[2][2] = c;
        return r;
    };
    const auto ry = [](float a) {
        const float c = std::cos(a), s = std::sin(a);
        f4f23ma::ma4 r = f4f23ma::ma4::identity();
        r.m[0][0] = c;
        r.m[2][0] = s;
        r.m[0][2] = -s;
        r.m[2][2] = c;
        return r;
    };
    return ry(-bank_deg * kDeg) * rx(-pitch_deg * kDeg) * rz(-yaw_deg * kDeg);
}

sco::AttachmentPoint resolve_parent_attach_point(const MapAttachObjectInst& attach,
                                                 const MapObjectInst& parent) {
    sco::AttachmentPoint pt{};
    if (const auto parent_sco = sco::load_sco(parent.sco_path)) {
        // a point the parent does not have is its origin (Omsi.exe / openOMSI), not the last point
        const int idx = attach.attach_point_index;
        if (idx >= 0 && idx < static_cast<int>(parent_sco->attachment_points.size()))
            pt = parent_sco->attachment_points[static_cast<std::size_t>(idx)];
    }
    return pt;
}

} // namespace

std::string extract_object_text_value(const std::string& cleaned) {
    const auto eq = cleaned.find('=');
    if (eq != std::string::npos) return trim_line(cleaned.substr(eq + 1));
    return cleaned;
}

f4f23ma::f32 spline_local_length(const SplineInst& sp) {
    // Chain prefix (sum of prior spline lengths). Absent or loader placeholder (≈ own length) → 0.
    // offsetAlong in .map is chain-absolute; along = offsetAlong - localLength.
    if (sp.local_length <= 1.e-3f) return 0.f;
    if (std::fabs(sp.local_length - sp.length) <= 1.e-3f) return 0.f;
    return sp.local_length;
}

std::vector<GlobalSplineIndexEntry> build_global_spline_index(const GlobalMap& global,
                                                              const std::filesystem::path& omsi_root) {
    std::vector<GlobalSplineIndexEntry> out;
    for (const auto& ref : global.tiles) {
        const auto tile = load_map_tile(ref.tile_path, omsi_root);
        if (!tile) continue;
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(tile->splines.size()); ++i)
            out.push_back({ref.grid_x, ref.grid_y, i});
    }
    return out;
}

std::vector<GlobalSplineIndexEntry> build_global_spline_index_from_tiles(
    const std::vector<MapTileData>& tiles) {
    std::vector<GlobalSplineIndexEntry> out;
    for (const auto& tile : tiles) {
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(tile.splines.size()); ++i)
            out.push_back({tile.grid_x, tile.grid_y, i});
    }
    return out;
}

std::optional<TileSplineRef> resolve_spline_attachment_index(
    const std::int32_t spline_index, const std::int32_t attachment_tile_gx,
    const std::int32_t attachment_tile_gy, const std::int32_t attachment_tile_spline_count,
    const std::vector<GlobalSplineIndexEntry>& global_index) {
    // Tile-local index is authoritative for [splineAttachement] when the attachment
    // belongs to a known tile. Never fall through to global_index while the tile is
    // known: global_index[i] is "i-th spline among currently loaded tiles", which is
    // NOT the .map file index (caused Eaos idx=218 → wrong id 3692426).
    if (spline_index >= 0 && attachment_tile_spline_count > 0) {
        if (spline_index < attachment_tile_spline_count)
            return TileSplineRef{attachment_tile_gx, attachment_tile_gy, spline_index};
        return std::nullopt; // out of range for this tile — do not use global
    }
    // Unknown tile (count==0): legacy global table only.
    if (spline_index >= 0 && static_cast<std::size_t>(spline_index) < global_index.size()) {
        const auto& e = global_index[static_cast<std::size_t>(spline_index)];
        return TileSplineRef{e.tile_gx, e.tile_gy, e.local_index};
    }
    return std::nullopt;
}

const SplineInst* find_spline_in_tiles(const std::vector<MapTileData>& tiles, const TileSplineRef& ref) {
    for (const auto& tile : tiles) {
        if (tile.grid_x != ref.tile_gx || tile.grid_y != ref.tile_gy) continue;
        if (ref.local_index < 0 || static_cast<std::size_t>(ref.local_index) >= tile.splines.size()) return nullptr;
        return &tile.splines[static_cast<std::size_t>(ref.local_index)];
    }
    return nullptr;
}

std::vector<float> enumerate_spline_attachment_along(const MapSplineAttachmentInst& att,
                                                     const SplineInst& spline) {
    std::vector<float> out;
    const float length = std::max(spline.length, 1.e-3f);
    float start = att.offset_along - spline_local_length(spline);
    const float step = att.repeat_step;
    const float range = att.repeat_range;

    if (start < 0.f) {
        if (start > -0.05f) start = 0.f;
        else return out;
    }
    if (start > length) return out;

    if (step <= 0.001f || range <= 0.001f) {
        out.push_back(start);
        return out;
    }

    const float end = start + range;
    float along = start;
    for (int guard = 0; guard < 4096; ++guard) {
        if (along < 0.f || along > length) break;
        out.push_back(along);
        const float next = along + step;
        if (next > end + 0.001f) break;
        along = next;
    }
    return out;
}

std::vector<SplineAttachAlongHit> enumerate_spline_attachment_along_chain(
    const MapSplineAttachmentInst& att, const SplineInst& start, const std::vector<SplineInst>& splines) {
    std::vector<SplineAttachAlongHit> hits;
    std::unordered_map<std::int32_t, const SplineInst*> by_id;
    by_id.reserve(splines.size() * 2);
    for (const auto& s : splines)
        if (s.id != 0) by_id[s.id] = &s;

    const float step = att.repeat_step;
    const float range = att.repeat_range;
    const SplineInst* cur = &start;
    float along = att.offset_along - spline_local_length(*cur);
    std::unordered_set<std::int32_t> seen_prev;
    for (int guard = 0; along < -0.05f && cur && cur->prev_id != 0 && guard < 256; ++guard) {
        if (!seen_prev.insert(cur->id).second) break;
        const auto it = by_id.find(cur->prev_id);
        if (it == by_id.end()) break;
        cur = it->second;
        along += std::max(cur->length, 1.e-3f);
    }
    if (along < 0.f) {
        if (along > -0.05f) along = 0.f;
        else return hits;
    }

    auto hop_forward_past_end = [&]() {
        for (int guard = 0; cur && guard < 256; ++guard) {
            const float len = std::max(cur->length, 1.e-3f);
            if (along <= len + 0.001f) return;
            if (cur->next_id == 0) return;
            const auto it = by_id.find(cur->next_id);
            if (it == by_id.end()) return;
            along -= len;
            cur = it->second;
        }
    };
    hop_forward_past_end();
    if (!cur) return hits;

    auto remaining_chain = [&](const SplineInst* p, float local_along) {
        float rem = std::max(0.f, std::max(p->length, 1.e-3f) - local_along);
        std::unordered_set<std::int32_t> seen;
        seen.insert(p->id);
        while (p->next_id != 0) {
            const auto it = by_id.find(p->next_id);
            if (it == by_id.end()) break;
            p = it->second;
            if (!seen.insert(p->id).second) break;
            rem += std::max(p->length, 0.f);
        }
        return rem;
    };

    const float remaining = remaining_chain(cur, along);

    if (step <= 0.001f || range <= 0.001f) {
        const float len = std::max(cur->length, 1.e-3f);
        if (along <= len) hits.push_back({cur->id, along});
        return hits;
    }

    const float span = std::min(range, remaining);

    const float chain_end = along + span;
    float chain_pos = along;
    std::unordered_set<std::int32_t> seen;
    for (int hop = 0; hop < 256 && cur; ++hop) {
        if (!seen.insert(cur->id).second) break;
        const float len = std::max(cur->length, 1.e-3f);
        while (along >= 0.f && along <= len + 0.001f && chain_pos <= chain_end + 0.001f) {
            hits.push_back({cur->id, along});
            along += step;
            chain_pos += step;
            if (hits.size() >= 4096) return hits;
        }
        if (chain_pos > chain_end + 0.001f) break;
        if (cur->next_id == 0) break;
        const auto it = by_id.find(cur->next_id);
        if (it == by_id.end()) break;
        along -= len;
        cur = it->second;
        // Next segment: ignore its localLength; along is already local 0..length.
        // Pose uses this spline's length/radius/heading.
    }
    return hits;
}

SplineAttachPanelCoords spline_attach_to_panel(const SplineInst& spline, const MapSplineAttachmentInst& att) {
    (void)spline;
    SplineAttachPanelCoords p{};
    p.x = att.offset_x;
    const float local_len = spline_local_length(spline);
    const float len = std::max(spline.length, 0.f);
    p.y = std::clamp(att.offset_along - local_len, 0.f, len);
    p.z = att.offset_y;
    p.rot = att.rotation_y;
    return p;
}

bool spline_attach_from_panel(const SplineInst& spline, const f4f23ma::f32 px, const f4f23ma::f32 py,
                              const f4f23ma::f32 pz, MapSplineAttachmentInst& att) {
    const float len = std::max(spline.length, 0.f);
    if (py < -1.e-4f || py > len + 1.e-4f) return false;
    att.offset_x = px;
    att.offset_y = pz;
    att.offset_along = spline_local_length(spline) + std::clamp(py, 0.f, len);
    return true;
}

SplinePointProjection project_point_onto_spline(const SplineInst& spline, const f4f23ma::f32 wx,
                                                const f4f23ma::f32 wy,
                                                const std::optional<f4f23ma::f32> chain_end_z) {
    SplinePointProjection out{};
    const float len = std::max(spline.length, 1.e-4f);
    const int steps = (std::max)(16, static_cast<int>(len * 4.f) + 1);
    float best_along = 0.f;
    float best_lat = 0.f;
    float best_d2 = 1.e30f;
    for (int i = 0; i <= steps; ++i) {
        const float along = len * static_cast<float>(i) / static_cast<float>(steps);
        const auto samp = spline::evaluate_spline(spline, along, 0.f, 0.f, chain_end_z);
        const float dx = wx - samp.x;
        const float dy = wy - samp.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= best_d2) continue;
        best_d2 = d2;
        best_along = along;
        const float nx = std::cos(samp.heading_rad);
        const float ny = -std::sin(samp.heading_rad);
        const float world_lat = dx * nx + dy * ny;
        best_lat = world_lat;
    }
    out.along_m = best_along;
    out.lateral_m = best_lat;
    out.dist_sq = best_d2;
    return out;
}

SplineAttachmentPose compute_spline_attachment_pose(const SplineInst& spline,
                                                    const MapSplineAttachmentInst& att, const float along_m,
                                                    const std::optional<f4f23ma::f32> chain_end_z,
                                                    const bool tangential_follow) {
    const float len = std::max(spline.length, 1.e-4f);
    const float along = std::clamp(along_m, 0.f, len);
    // [splineAttachement] offset_x is map-authored; mirror flips strip mesh, not SA lateral.
    const auto samp = spline::evaluate_spline(spline, along, att.offset_x, att.offset_y, chain_end_z, false);
    SplineAttachmentPose pose{};
    pose.x = samp.x;
    pose.y = samp.y;
    pose.z = samp.z;
    constexpr f4f23ma::f32 kRad2Deg = 180.f / 3.14159265f;
    pose.yaw_deg = samp.heading_rad * kRad2Deg + (tangential_follow ? 0.f : att.rotation_y);
    if (tangential_follow) {
        pose.pitch_deg = samp.pitch_rad * kRad2Deg;
        pose.bank_deg = samp.cant_rad * kRad2Deg;
    } else {
        pose.pitch_deg = att.rotation_x;
        pose.bank_deg = att.rotation_z;
    }
    return pose;
}

ObjectAttachmentPose attach_obj_local_euler(const MapAttachObjectInst& attach,
                                            const sco::AttachmentPoint& pt) {
    ObjectAttachmentPose e{};
    // inst[6] extra_rot_x is [object]-style heading (90/270 on LED_Ped), not pitch.
    // (inst[4] is the spline-row instance the parent is, not an angle)
    e.yaw_deg = pt.rot_z + attach.extra_rot_x;
    e.pitch_deg = pt.rot_x + attach.extra_rot_y2;
    e.bank_deg = pt.rot_y + attach.extra_rot_z;
    return e;
}

std::optional<ObjectAttachmentPose> compute_attach_obj_pose(const MapAttachObjectInst& attach,
                                                            const MapObjectInst& parent) {
    const auto pt = resolve_parent_attach_point(attach, parent);
    const auto local = attach_obj_local_euler(attach, pt);
    const f4f23ma::ma4 parent_r =
        omsi_object_rotation_ma4(parent.rotation_deg.x, parent.rotation_deg.y, parent.rotation_deg.z);
    const f4f23ma::ma4 parent_t = translation_ma4(parent.position.x, parent.position.y, parent.position.z);
    // the point's own matrix (attach_trans / attach_rot_* in file order; same memory layout as ma4),
    // then the record's heading / pitch / bank
    f4f23ma::ma4 local_t = f4f23ma::ma4::identity();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) local_t.m[r][c] = pt.m[r * 4 + c];
    const f4f23ma::ma4 local_r =
        omsi_object_rotation_ma4(attach.extra_rot_x, attach.extra_rot_y2, attach.extra_rot_z);
    // Column: rotate about attach origin, then offset, then parent R*T.
    const f4f23ma::ma4 world = parent_t * parent_r * local_t * local_r;
    ObjectAttachmentPose pose = local;
    pose.x = world.m[3][0];
    pose.y = world.m[3][1];
    pose.z = world.m[3][2];
    pose.yaw_deg = parent.rotation_deg.x + local.yaw_deg;
    pose.pitch_deg = parent.rotation_deg.y + local.pitch_deg;
    pose.bank_deg = parent.rotation_deg.z + local.bank_deg;
    return pose;
}

f4f23ma::ma4 attach_obj_tile_transform(const MapAttachObjectInst& attach, const MapObjectInst& parent) {
    const auto pt = resolve_parent_attach_point(attach, parent);
    const auto local = attach_obj_local_euler(attach, pt);
    const f4f23ma::ma4 parent_r =
        omsi_object_rotation_ma4(parent.rotation_deg.x, parent.rotation_deg.y, parent.rotation_deg.z);
    const f4f23ma::ma4 parent_t = translation_ma4(parent.position.x, parent.position.y, parent.position.z);
    // the point's own matrix (attach_trans / attach_rot_* in file order; same memory layout as ma4),
    // then the record's heading / pitch / bank
    f4f23ma::ma4 local_t = f4f23ma::ma4::identity();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) local_t.m[r][c] = pt.m[r * 4 + c];
    const f4f23ma::ma4 local_r =
        omsi_object_rotation_ma4(attach.extra_rot_x, attach.extra_rot_y2, attach.extra_rot_z);
    return parent_t * parent_r * local_t * local_r;
}

std::optional<MapSplineAttachmentInst> parse_spline_attachment_at_tag_line(
    const std::vector<std::string>& lines, const std::size_t tag_line,
    const std::filesystem::path& omsi_root) {
    if (tag_line >= lines.size()) return std::nullopt;
    const std::string tag = trim_line(lines[tag_line]);
    if (!is_spline_attachment_tag(tag)) return std::nullopt;

    const bool is_repeater =
        tag.find("repeater") != std::string::npos || tag.find("Repeater") != std::string::npos;
    const auto bl = collect_nonempty_body_lines(lines, tag_line);
    if (bl.size() < 8) return std::nullopt;

    MapSplineAttachmentInst att{};
    att.is_repeater = is_repeater;

    std::size_t path_bl = 0;
    std::size_t instance_id_bl = 0;
    std::size_t spline_index_bl = 0;
    std::size_t offset_x_bl = 0;
    std::size_t offset_y_bl = 0;
    std::size_t offset_along_bl = 0;
    std::size_t rotation_y_bl = 0;
    if (is_repeater) {
        if (bl.size() < 9) return std::nullopt;
        spline_index_bl = 2;
        path_bl = 3;
        instance_id_bl = 4;
        offset_x_bl = 5;
        offset_y_bl = 6;
        offset_along_bl = 7;
        rotation_y_bl = 8;
    } else {
        path_bl = 1;
        instance_id_bl = 2;
        spline_index_bl = 3;
        offset_x_bl = 4;
        offset_y_bl = 5;
        offset_along_bl = 6;
        rotation_y_bl = 7;
    }

    const std::string path = omsi::repair_omsi_path_mojibake(clean_cfg_value(lines[bl[path_bl]]));
    if (path.empty()) return std::nullopt;

    att.sco_path =
        omsi_root.empty() ? std::filesystem::path(omsi::utf8_to_wide(path)) : omsi::join_omsi_path(omsi_root, path);
    att.instance_id = parse_value<std::int32_t>(clean_cfg_value(lines[bl[instance_id_bl]]));
    att.spline_index = parse_value<std::int32_t>(clean_cfg_value(lines[bl[spline_index_bl]]));
    att.offset_x = parse_value<f4f23ma::f32>(clean_cfg_value(lines[bl[offset_x_bl]]));
    att.offset_y = parse_value<f4f23ma::f32>(clean_cfg_value(lines[bl[offset_y_bl]]));
    att.offset_along = parse_value<f4f23ma::f32>(clean_cfg_value(lines[bl[offset_along_bl]]));
    att.rotation_y = parse_value<f4f23ma::f32>(clean_cfg_value(lines[bl[rotation_y_bl]]));
    const std::size_t rotation_x_bl = rotation_y_bl + 1;
    const std::size_t rotation_z_bl = rotation_y_bl + 2;
    if (bl.size() > rotation_z_bl) {
        att.rotation_x = parse_value<f4f23ma::f32>(clean_cfg_value(lines[bl[rotation_x_bl]]));
        att.rotation_z = parse_value<f4f23ma::f32>(clean_cfg_value(lines[bl[rotation_z_bl]]));
    }

    const std::size_t repeat_step_bl = rotation_y_bl + 3;
    const std::size_t repeat_range_bl = rotation_y_bl + 4;
    if (bl.size() > repeat_range_bl) {
        att.repeat_step = parse_value<f4f23ma::f32>(clean_cfg_value(lines[bl[repeat_step_bl]]));
        att.repeat_range = parse_value<f4f23ma::f32>(clean_cfg_value(lines[bl[repeat_range_bl]]));
    }

    // Body slot after repeat_range: Tangential to spline (1=yes, 0=no).
    const std::size_t tangential_bl = rotation_y_bl + 5;
    if (bl.size() > tangential_bl) {
        const auto tv = clean_cfg_value(lines[bl[tangential_bl]]);
        att.tangential = (parse_value<std::int32_t>(tv) != 0);
    }

    const std::size_t extra_count_bl = rotation_y_bl + 6;
    int extra_line_count = 0;
    if (bl.size() > extra_count_bl) {
        const std::string count_text = clean_cfg_value(lines[bl[extra_count_bl]]);
        const auto sp = count_text.find_first_of(" \t");
        const std::string count_token = sp == std::string::npos ? count_text : count_text.substr(0, sp);
        extra_line_count = parse_value<std::int32_t>(count_token);
        att.object_text_line_count = extra_line_count;
        if (extra_line_count > 0) {
            const std::size_t count_file_line = bl[extra_count_bl];
            for (int extra = 0; extra < extra_line_count; ++extra) {
                const std::size_t extra_index = count_file_line + 1 + static_cast<std::size_t>(extra);
                if (extra_index >= lines.size()) {
                    att.object_text_lines.push_back("");
                    continue;
                }
                const std::string cleaned = clean_cfg_value(lines[extra_index]);
                att.object_text_lines.push_back(cleaned.empty() ? "" : extract_object_text_value(cleaned));
            }
        }
    }

    // Tags after body until next section: [spline_terrain_align]
    {
        std::size_t j = tag_line + 1;
        while (j < lines.size()) {
            const std::string t = trim_line(lines[j]);
            if (t.empty()) { ++j; continue; }
            if (t.front() == '[') {
                if (tag_ieq(t, "[spline_terrain_align]")) att.align_terrain = true;
                break; // stop at any tag (align is immediately under the SA block)
            }
            ++j;
        }
        // Also scan a few tags after body for align that sits after extra text lines
        for (std::size_t k = tag_line + 1; k < lines.size() && k < tag_line + 40; ++k) {
            const std::string t = trim_line(lines[k]);
            if (tag_ieq(t, "[object]") || tag_ieq(t, "[spline]") || tag_ieq(t, "[spline_h]") ||
                is_spline_attachment_tag(t) || tag_ieq(t, "[attachObj]"))
                break;
            if (tag_ieq(t, "[spline_terrain_align]")) {
                att.align_terrain = true;
                break;
            }
        }
    }

    return att;
}

std::vector<MapSplineAttachmentInst> parse_map_spline_attachment_entries(
    const std::filesystem::path& tile_map) {
    std::vector<MapSplineAttachmentInst> attachments;
    const auto lines = cfg::read_cfg_lines(tile_map);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::string tag = trim_line(lines[i]);
        if (!is_spline_attachment_tag(tag)) continue;
        if (auto att = parse_spline_attachment_at_tag_line(lines, i, {})) attachments.push_back(std::move(*att));
    }
    return attachments;
}

} // namespace omsi::map
