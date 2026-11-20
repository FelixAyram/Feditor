#include "beamng/bng_assets.hpp"

#include "beamng/bng_dae.hpp"
#include "beamng/bng_vfs.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace bng {

// ── euler conversion ──────────────────────────────────────────────────────────────────────────────────────────
void mat3_to_editor_euler(const Mat3& r, float& yaw_deg, float& pitch_deg, float& bank_deg) {
    // R = Rz(a) Rx(c) Ry(d) with a=-yaw, c=-pitch, d=-bank (see header)
    constexpr float kRad = 57.29577951f;
    const float r21 = (std::clamp)(r[7], -1.f, 1.f);
    const float c = std::asin(r21);
    float a, d;
    if (std::hypot(r[6], r[8]) > 1e-4f) {
        d = std::atan2(-r[6], r[8]);
        a = std::atan2(-r[1], r[4]);
    } else {
        d = 0.f;
        a = std::atan2(r[3], r[0]);
    }
    yaw_deg = -a * kRad;
    pitch_deg = -c * kRad;
    bank_deg = -d * kRad;
}

Mat3 editor_euler_to_mat3(float yaw_deg, float pitch_deg, float bank_deg) {
    constexpr float kDeg = 0.01745329252f;
    const float a = -yaw_deg * kDeg, c = -pitch_deg * kDeg, d = -bank_deg * kDeg;
    const float ca = std::cos(a), sa = std::sin(a), cc = std::cos(c), sc = std::sin(c), cd = std::cos(d), sd = std::sin(d);
    return {ca * cd - sa * sc * sd, -sa * cc, ca * sd + sa * sc * cd,
            sa * cd + ca * sc * sd, ca * cc,  sa * sd - ca * sc * cd,
            -cc * sd,               sc,       cc * cd};
}

// ── shared materials ──────────────────────────────────────────────────────────────────────────────────────────
void MaterialIndex::build(const Vfs& vfs) {
    Level tmp;
    // Shared folders only; level folders are handled by the level itself.
    for (const char* dir : {"/assets/", "/art/", "/core/", "/vehicles/common/"}) load_materials_under(vfs, dir, tmp);
    mats_ = std::move(tmp.materials);
    by_map_to_ = std::move(tmp.material_by_map_to);
    by_base_ = std::move(tmp.material_by_base);
    ready_.store(true);
}

const Material* MaterialIndex::find(const std::string& n) const {
    if (!ready_.load()) return nullptr;
    if (auto it = mats_.find(n); it != mats_.end()) return &it->second;
    if (auto it = by_map_to_.find(n); it != by_map_to_.end())
        if (auto jt = mats_.find(it->second); jt != mats_.end()) return &jt->second;
    // LOD materials ("<mat>_lod", "lod_<mat>", case variants): resolve through the base name so shared props
    // do not come out white just because their far LOD uses a different material name.
    const std::string base = material_base_name(n);
    if (!base.empty() && base != n) {
        if (auto it = by_base_.find(base); it != by_base_.end())
            if (auto jt = mats_.find(it->second); jt != mats_.end()) return &jt->second;
        if (auto it = mats_.find(base); it != mats_.end()) return &it->second;
    }
    return nullptr;
}

// ── alpha mode ───────────────────────────────────────────────────────────────────────────────────────────────
// 0 = opaque, 1 = alpha test (cutout), 2 = alpha blend (matches gfx::dx12::ProcMeshSubrange::alpha_mode).
// alphaTest without a blend op must win over the opacityMap heuristic: real foliage (trees_aspen
// Birch_Leaves_01 / m_birch_merged_foliage in east_coast_usa: alphaTest + alphaRef 180/100 + opacityMap +
// doubleSided) was classified as blend because of the opacityMap, so the nearest tree LOD was drawn
// semi-transparent, without depth write and without clipping the leaf cards. Materials that really blend
// (glass, tyre marks, road markings: translucent / translucentBlendOp) keep the blend pass.
std::uint8_t alpha_mode_for(const Material& m) {
    if (m.translucent) return 2;
    if (m.alpha_test) return 1;
    if (!m.stage0.opacity_map.empty()) return 2;
    return 0;
}

// ── mesh building ─────────────────────────────────────────────────────────────────────────────────────────────
std::shared_ptr<ReadyMesh> make_ready_mesh(const Vfs& vfs, const Level& level, const MaterialIndex* global,
                                           const std::string& shape_path, float detail_adjust) {
    auto out = std::make_shared<ReadyMesh>();
    auto txt = vfs.read_text(shape_path);
    if (!txt) {
        out->error = "no encontrado";
        return out;
    }
    auto shape = parse_dae(*txt);
    if (!shape || shape->lods.empty()) {
        out->error = "dae sin geometria";
        return out;
    }
    (void)detail_adjust; // LOD choice happens per instance at draw time (screen size x detailAdjust)
    out->lod_count = static_cast<int>(shape->lods.size());
    out->detail_size = shape->lods.front().detail_size;
    std::memcpy(out->bmin, shape->bounds_min, sizeof(out->bmin));
    std::memcpy(out->bmax, shape->bounds_max, sizeof(out->bmax));
    out->radius = shape->radius;
    auto vfs_tex = [](const std::string& p) { return p.empty() ? std::string() : "vfs:" + p; };
    // LOD0 subs, already resolved: the donor for a LOD whose own material name does not resolve (or resolves to
    // a variant without maps, e.g. "<mat>_lod"). A LOD must never end up unttextured/opaque just because its
    // material name differs from the LOD0 one.
    std::vector<ReadyMesh::Sub> lod0_subs;
    std::vector<std::string> lod0_base;
    auto sub_usable = [](const ReadyMesh::Sub& s) {
        return !s.texture.empty() || s.alpha_mode != 0 || s.double_sided;
    };
    // Donor by base material name, else by submesh index (LODs usually keep the submesh order).
    auto find_donor = [&](std::size_t idx, const std::string& mat) -> const ReadyMesh::Sub* {
        if (lod0_subs.empty()) return nullptr;
        const std::string base = material_base_name(mat);
        if (!base.empty())
            for (std::size_t i = 0; i < lod0_base.size() && i < lod0_subs.size(); ++i)
                if (lod0_base[i] == base && sub_usable(lod0_subs[i])) return &lod0_subs[i];
        if (idx < lod0_subs.size() && sub_usable(lod0_subs[idx])) return &lod0_subs[idx];
        return nullptr;
    };
    std::size_t lod_index = 0;
    for (const auto& lod : shape->lods) {
        const bool is_lod0 = lod_index++ == 0;
        ReadyMesh::Lod L;
        L.detail_size = lod.detail_size;
        for (const auto& sm : lod.submeshes) {
            if (sm.indices.empty()) continue;
            ReadyMesh::Sub sub;
            sub.material = sm.material;
            const Material* m = level.material_for(sm.material);
            if (!m && global) m = global->find(sm.material);
            float bc[4]{1.f, 1.f, 1.f, 1.f};
            const bool have_material = m != nullptr;
            if (m) {
                sub.texture = vfs_tex(m->stage0.color_map);
                sub.normal = vfs_tex(m->stage0.normal_map);
                sub.detail = vfs_tex(m->stage0.detail_map);
                sub.detail_normal = vfs_tex(m->stage0.detail_normal_map);
                sub.opacity = vfs_tex(m->stage0.opacity_map);
                sub.opacity_factor = m->stage0.opacity_factor;
                sub.detail_scale = m->stage0.detail_scale[0];
                sub.detail_scale_y = m->stage0.detail_scale[1];
                sub.alpha_ref = m->alpha_ref;
                sub.detail_distance = m->stage0.detail_far;
                // Cutout for foliage (alphaTest), blend only for genuinely translucent materials. An opacityMap
                // alone already means transparency (tyre marks / dirt decals).
                sub.alpha_mode = alpha_mode_for(*m);
                sub.double_sided = m->double_sided;
                std::memcpy(bc, m->stage0.base_color, sizeof(bc));
                std::memcpy(sub.base_color, m->stage0.base_color, sizeof(sub.base_color));
            } else if (const auto* d0 = find_donor(&sm - lod.submeshes.data(), sm.material)) {
                // No material for this LOD at all (missing "<mat>_lod" / no materials.json entry): inherit the
                // LOD0 sub (maps + cutout + doubleSided) instead of falling back to a flat/white surface.
                const std::string keep = sub.material;
                sub = *d0;
                sub.material = keep;
                sub.index_start = 0;
                sub.index_count = 0;
            }
            if (m && sub.texture.empty()) {
                // The LOD material resolved but carries no colorMap: when the base name matches LOD0, reuse its
                // maps and its cutout / doubleSided state so foliage does not turn into opaque white cards.
                const std::string base = material_base_name(sm.material);
                for (std::size_t i = 0; i < lod0_base.size() && i < lod0_subs.size() && !base.empty(); ++i)
                    if (lod0_base[i] == base && !lod0_subs[i].texture.empty()) {
                        const auto& d0 = lod0_subs[i];
                        sub.texture = d0.texture;
                        sub.normal = d0.normal;
                        sub.detail = d0.detail;
                        sub.detail_normal = d0.detail_normal;
                        sub.opacity = d0.opacity;
                        if (sub.alpha_mode == 0) sub.alpha_mode = d0.alpha_mode;
                        sub.double_sided = sub.double_sided || d0.double_sided;
                        if (sub.alpha_ref == 127) sub.alpha_ref = d0.alpha_ref;
                        break;
                    }
            }
            if (sub.texture.empty() && !sm.texture.empty()) {
                // fallback: diffuse declared inside the .dae (library_images + effect), relative to the shape
                const auto slash = shape_path.find_last_of('/');
                const std::string dir = slash == std::string::npos ? std::string() : shape_path.substr(0, slash);
                std::string rel = sm.texture;
                if (!rel.empty() && rel[0] != '/' && rel[0] != '\\' && !dir.empty()) rel = dir + "/" + rel; // ../textures/x.dds
                const auto tex = vfs.resolve_texture(rel, dir);
                if (!tex.empty()) sub.texture = vfs_tex(tex);
            }
            // Average vertex colour x base colour factor. Kept even when there IS a colorMap: it is the colour the
            // renderer falls back to when the texture file cannot be loaded (a failed load used to leave the white
            // 1x1 placeholder: "objetos con textura blanca" as soon as the camera got close and the near LOD was
            // uploaded).
            double acc[4]{0, 0, 0, 0};
            for (const auto& v : sm.vertices)
                for (int c = 0; c < 4; ++c) acc[c] += static_cast<double>((v.color >> (8 * c)) & 0xFFu) / 255.0;
            const double nv = (std::max)(std::size_t(1), sm.vertices.size());
            for (int c = 0; c < 4; ++c) sub.solid[c] = static_cast<float>(acc[c] / nv) * bc[c];
            sub.has_solid = true;
            // baseColor ~0: never draw absolute black (tyre marks / missing materials).
            const float lum = 0.299f * sub.solid[0] + 0.587f * sub.solid[1] + 0.114f * sub.solid[2];
            if (lum < 0.02f)
                for (int c = 0; c < 3; ++c) sub.solid[c] = have_material ? 0.12f : 0.45f;
            if (sub.solid[3] < 0.02f) sub.solid[3] = 1.f; // all-zero vertex alpha = missing data
            const auto base = static_cast<std::uint32_t>(L.vertices.size());
            sub.index_start = static_cast<std::uint32_t>(L.indices.size());
            for (const auto& v : sm.vertices) {
                omsi::o3d::Vertex o{};
                std::memcpy(o.position, v.pos, sizeof(o.position));
                std::memcpy(o.normal, v.normal, sizeof(o.normal));
                o.uv[0] = v.uv0[0];
                o.uv[1] = v.uv0[1];
                L.vertices.push_back(o);
            }
            for (const auto i : sm.indices) L.indices.push_back(base + i);
            sub.index_count = static_cast<std::uint32_t>(L.indices.size()) - sub.index_start;
            L.subs.push_back(std::move(sub));
        }
        if (is_lod0 && !L.subs.empty()) {
            lod0_subs = L.subs;
            lod0_base.clear();
            for (const auto& s : L.subs) lod0_base.push_back(material_base_name(s.material));
        }
        if (!L.indices.empty()) out->lods.push_back(std::move(L));
    }
    if (!out->lods.empty()) {
        out->vertices = out->lods.front().vertices;
        out->indices = out->lods.front().indices;
        out->subs = out->lods.front().subs;
    }
    out->triangles = out->indices.size() / 3;
    out->ok = !out->indices.empty();
    if (!out->ok) out->error = "sin triangulos";
    return out;
}

AssetPool::AssetPool(const Vfs& vfs, const Level& level, int threads) : vfs_(vfs), level_(level) {
    global_mats_ = std::make_shared<MaterialIndex>();
    auto gm = global_mats_;
    mat_thread_ = std::thread([gm, &vfs]() { gm->build(vfs); });
    if (threads <= 0) threads = (std::max)(1, static_cast<int>(std::thread::hardware_concurrency()) - 2);
    for (int i = 0; i < threads; ++i) threads_.emplace_back([this]() { worker_main(); });
}

AssetPool::~AssetPool() {
    stop_.store(true);
    cv_.notify_all();
    for (auto& t : threads_)
        if (t.joinable()) t.join();
    if (mat_thread_.joinable()) mat_thread_.join();
}

void AssetPool::request(const std::string& shape, float priority) {
    if (shape.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (auto it = queued_.find(shape); it != queued_.end()) {
        it->second = (std::min)(it->second, priority);
        return;
    }
    if (!known_.insert(shape).second) return; // already built or building
    queued_[shape] = priority;
    cv_.notify_one();
}

bool AssetPool::take_ready(std::string& shape, std::shared_ptr<ReadyMesh>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (ready_.empty()) return false;
    shape = std::move(ready_.front().first);
    out = std::move(ready_.front().second);
    ready_.pop_front();
    return true;
}

std::size_t AssetPool::pending() const {
    std::lock_guard<std::mutex> lk(mu_);
    return queued_.size();
}

void AssetPool::worker_main() {
    for (;;) {
        std::string shape;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_.load() || !queued_.empty(); });
            if (stop_.load()) return;
            // nearest first
            auto best = queued_.begin();
            for (auto it = queued_.begin(); it != queued_.end(); ++it)
                if (it->second < best->second) best = it;
            shape = best->first;
            queued_.erase(best);
        }
        // shared materials are needed to texture most props; wait briefly for the index on first use
        for (int i = 0; i < 200 && !global_mats_->ready() && !stop_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::shared_ptr<ReadyMesh> m;
        try {
            m = make_ready_mesh(vfs_, level_, global_mats_.get(), shape, detail_adjust_.load());
        } catch (...) {
            m = std::make_shared<ReadyMesh>();
            m->error = "excepcion";
        }
        done_.fetch_add(1);
        std::lock_guard<std::mutex> lk(mu_);
        ready_.emplace_back(std::move(shape), std::move(m));
    }
}

} // namespace bng
