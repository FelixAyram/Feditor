#pragma once

#include "omsi/model_cfg_types.hpp"

#include "f4f23ma/math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace omsi::model_cfg {

// Mesh/GPU space for model.cfg origin_trans (wiki X,Y-fwd,Z-up) before O3D draw matrix.
enum class OriginAxisMap : std::uint8_t {
    identity = 0,     // (x,y,z)
    wiki_yz_swap = 1, // (x,y,z)->(x,z,y) for DirectX Y-up file packs (OA101)
};

// Per-mesh local mats from [newanim].
// GPU: DirectXMath World = T(-P)*R*T(P) (row-vector), uploaded as Transpose(World).
class MeshAnimator {
public:
    void init(const ModelCfgDocument& cfg);
    void update(const ModelCfgDocument& cfg, float dt,
                const std::function<float(const std::string&)>& get_var);

    void debug_log(const ModelCfgDocument& cfg, const std::function<float(const std::string&)>& get_var,
                   const std::function<bool(const std::string&)>& var_exists) const;

    void xform_log(const ModelCfgDocument& cfg, float sim_time_sec,
                   const std::function<float(const std::string&)>& get_var);

    void door_vert_log(const ModelCfgDocument& cfg, float sim_time_sec);

    [[nodiscard]] const std::vector<f4f23ma::ma4>& matrices() const { return matrices_; }
    [[nodiscard]] f4f23ma::ma4 matrix_for_mesh(std::size_t mesh_index) const;
    [[nodiscard]] const std::vector<std::array<float, 16>>& matrices_dx_transposed() const {
        return matrices_dx_t_;
    }

private:
    std::vector<f4f23ma::ma4> matrices_;
    std::vector<std::array<float, 16>> matrices_dx_t_;
    std::vector<int> parent_index_;
    std::vector<std::optional<f4f23ma::f4>> mesh_file_pivot_;
    std::vector<std::optional<f4f23ma::ma4>> mesh_file_origin_;
    std::vector<OriginAxisMap> origin_axis_map_;
    std::vector<f4f23ma::ma4> mesh_draw_matrix_;
    std::vector<std::vector<f4f23ma::f4>> layer_pivot_gpu_;
    std::vector<std::vector<f4f23ma::f4>> layer_axis_gpu_;
    std::vector<std::optional<f4f23ma::f4>> mesh_bbox_center_gpu_;
    std::vector<std::optional<f4f23ma::f4>> mesh_hinge_gpu_;
    std::vector<std::vector<std::vector<float>>> last_values_;

    std::vector<std::vector<f4f23ma::f4>> door_rest_verts_;
    std::vector<float> door_vert_prev_smooth_;
    std::vector<bool> door_vert_rest_dumped_;
    bool door_vert_announced_ = false;

    std::vector<f4f23ma::ma4> xform_prev_mats_;
    std::vector<float> xform_prev_smooth_;
    std::vector<float> xform_last_emit_t_;
    bool xform_announced_ = false;
};

[[nodiscard]] f4f23ma::ma4 rotate_around_pivot(const f4f23ma::f4& pivot, const f4f23ma::f4& axis_unit,
                                               float angle_radians);

[[nodiscard]] f4f23ma::ma4 build_anim_layer_matrix(const MeshAnimLayer& layer,
                                                    const f4f23ma::f4& mesh_file_pivot,
                                                    const std::vector<float>& binding_values,
                                                    OriginAxisMap axis_map = OriginAxisMap::identity,
                                                    const f4f23ma::ma4* mesh_object_file = nullptr);

} // namespace omsi::model_cfg
