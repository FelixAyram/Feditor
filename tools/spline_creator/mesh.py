"""Spline tessellation matching src/spline/spline_geometry.cpp (preview length, no patchwork)."""

from __future__ import annotations

import math
from dataclasses import dataclass, field

from sli_io import SplineDefinition

PI = math.pi
DEG2RAD = PI / 180.0
PREVIEW_LEN = 12.0


@dataclass
class Vertex:
    x: float
    y: float
    z: float
    u: float
    v: float


@dataclass
class SplineMesh:
    vertices: list[Vertex] = field(default_factory=list)
    indices: list[int] = field(default_factory=list)
    texture: str = ""
    alpha: int = 0  # [matl_alpha] 0 opaque, 1 test, 2 blend


@dataclass
class Inst:
    length: float = PREVIEW_LEN
    radius: float = 0.0
    heading_deg: float = 0.0
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    start_grad: float = 0.0
    end_grad: float = 0.0
    delta_height: float = 0.0
    use_delta_height: bool = False
    is_spline_h: bool = False
    cant_start: float = 0.0
    cant_end: float = 0.0
    skew_start: float = 0.0
    skew_end: float = 0.0
    mirror: bool = False


def _grad(sp: Inst) -> tuple[float, float, float]:
    length = max(sp.length, 1e-4)
    b = sp.start_grad * 0.01
    delta = sp.delta_height
    cubic = sp.use_delta_height or sp.is_spline_h
    if cubic:
        c = ((sp.end_grad - sp.start_grad) * 0.01 * length - 2.0 * (delta - sp.start_grad * 0.01 * length)) / (
            length * length * length
        )
        a = (-(sp.end_grad - sp.start_grad) * 0.01 * length + 3.0 * (delta - sp.start_grad * 0.01 * length)) / (
            length * length
        )
        return c, a, b
    a = (sp.end_grad - sp.start_grad) * 0.01 / (2.0 * length)
    return 0.0, a, b


def _centerline(sp: Inst, along: float) -> tuple[float, float, float]:
    rot = sp.heading_deg * DEG2RAD
    if abs(sp.radius) < 1e-6:
        return (
            sp.x + along * math.sin(rot),
            sp.y + along * math.cos(rot),
            rot,
        )
    theta = along / sp.radius
    x_temp = sp.radius * (1.0 - math.cos(theta))
    y_temp = sp.radius * math.sin(theta)
    hx = sp.x + x_temp * math.cos(rot) + y_temp * math.sin(rot)
    hy = sp.y - x_temp * math.sin(rot) + y_temp * math.cos(rot)
    return hx, hy, rot + theta


def evaluate(sp: Inst, along: float, lateral: float, height_off: float) -> tuple[float, float, float]:
    ox = -lateral if (sp.mirror) else lateral
    c, a, b = _grad(sp)
    length = max(sp.length, 1e-4)
    cant_delta = (sp.cant_end - sp.cant_start) * 0.01 / length
    cant0 = sp.cant_start * 0.01
    hx, hy, heading = _centerline(sp, along)
    dir_x, dir_y = math.sin(heading), math.cos(heading)
    nx, ny = dir_y, -dir_x
    z = sp.z + ((c * along + a) * along + b) * along + height_off - ox * (along * cant_delta + cant0)
    return hx + nx * ox, hy + ny * ox, z


def omis_teile(sp: Inst, max_teilung: int = 200) -> int:
    length = max(sp.length, 0.0)
    parts = max(1, int(math.trunc(length / 10.0)))
    c, a, _b = _grad(sp)
    if abs(c) < 1e-12 and abs(a) < 1e-12:
        steig_a = (sp.end_grad - sp.start_grad) * 0.01 / (2.0 * max(length, 1e-4))
        if abs(steig_a) >= 1e-8:
            parts = max(parts, max(1, int(math.trunc(1.0 / max(abs(2.0 * steig_a), 1e-8))) + 1))
    else:
        denom = abs(2.0 * a) + abs(3.0 * c * length + 2.0 * a)
        parts = max(parts, max(1, int(math.trunc(1.0 / max(denom, 1e-8))) + 1))
    if abs(sp.radius) > 1e-6:
        inv_s = 1.0 / sp.radius
        denom = abs(2.0 * inv_s) + abs(3.0 * inv_s * length + 2.0 * inv_s)
        from_inv = max(1, int(math.trunc(length * denom)) + 1)
        ang = abs(length * inv_s)
        from_ang = max(1, int(math.trunc(ang / (PI / 36.0))) + 1)
        parts = max(parts, from_inv, from_ang)
        if length < 5.0:
            parts = max(parts, from_ang, 8)
    if max_teilung > 0:
        parts = min(parts, max_teilung)
    return max(1, parts)


def build_meshes(d: SplineDefinition, length: float = PREVIEW_LEN) -> list[SplineMesh]:
    sp = Inst(length=length)
    out: list[SplineMesh] = []
    if not d.profiles or length <= 1e-4:
        return out
    segs = max(omis_teile(sp), 8)
    for strip in d.profiles:
        if len(strip) < 2:
            continue
        mesh = SplineMesh()
        mat = strip[0].material_id
        if 0 <= mat < len(d.textures):
            mesh.texture = d.textures[mat]
        else:
            mesh.texture = ""
        if 0 <= mat < len(d.texture_alpha):
            mesh.alpha = max(0, min(2, int(d.texture_alpha[mat])))
        n_pt = len(strip)
        flip = sp.mirror
        for s in range(segs + 1):
            along = (s / segs) * sp.length
            skew = sp.skew_start * (1.0 - along / sp.length) + sp.skew_end * (along / sp.length)
            for pp in strip:
                along_pt = along + skew * pp.x
                x, y, z = evaluate(sp, along_pt, pp.x, pp.z)
                tiling = pp.y_tiling if pp.y_tiling != 0.0 else 1.0
                vv = along_pt * tiling
                if sp.mirror:
                    vv = -vv
                mesh.vertices.append(Vertex(x, y, z, pp.uv_x, vv))
        for s in range(segs):
            for p in range(n_pt - 1):
                i0 = s * n_pt + p
                i1 = s * n_pt + p + 1
                i2 = (s + 1) * n_pt + p
                i3 = (s + 1) * n_pt + p + 1
                if flip:
                    mesh.indices += [i0, i2, i3, i1, i0, i3]
                else:
                    mesh.indices += [i0, i3, i2, i1, i3, i0]
        if mesh.vertices and mesh.indices:
            out.append(mesh)
    return out
