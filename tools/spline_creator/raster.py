"""Luna-style Z-up LH camera + software raster (same axes as the DX12 preview)."""

from __future__ import annotations

import math
from array import array

from mesh import SplineMesh, Vertex

XM_PI = 3.141592653589793


def luna_eye(tx: float, ty: float, tz: float, radius: float, theta: float, phi: float) -> tuple[float, float, float]:
    sphi, cphi = math.sin(phi), math.cos(phi)
    return (
        tx + radius * sphi * math.cos(theta),
        ty + radius * sphi * math.sin(theta),
        tz + radius * cphi,
    )


def look_at_lh(ex: float, ey: float, ez: float, tx: float, ty: float, tz: float) -> list[float]:
    zx, zy, zz = tx - ex, ty - ey, tz - ez
    zlen = math.sqrt(zx * zx + zy * zy + zz * zz) or 1.0
    zx, zy, zz = zx / zlen, zy / zlen, zz / zlen
    ux, uy, uz = 0.0, 0.0, 1.0
    xx = uy * zz - uz * zy
    xy = uz * zx - ux * zz
    xz = ux * zy - uy * zx
    xlen = math.sqrt(xx * xx + xy * xy + xz * xz) or 1.0
    xx, xy, xz = xx / xlen, xy / xlen, xz / xlen
    yx = zy * xz - zz * xy
    yy = zz * xx - zx * xz
    yz = zx * xy - zy * xx
    return [
        xx, yx, zx, 0.0,
        xy, yy, zy, 0.0,
        xz, yz, zz, 0.0,
        -(xx * ex + xy * ey + xz * ez),
        -(yx * ex + yy * ey + yz * ez),
        -(zx * ex + zy * ey + zz * ez),
        1.0,
    ]


def perspective_lh(fov: float, aspect: float, zn: float, zf: float, mirror_x: bool) -> list[float]:
    f = 1.0 / math.tan(fov * 0.5)
    m = [
        f / aspect, 0.0, 0.0, 0.0,
        0.0, f, 0.0, 0.0,
        0.0, 0.0, zf / (zf - zn), 1.0,
        0.0, 0.0, -zn * zf / (zf - zn), 0.0,
    ]
    if mirror_x:
        m[0] = -m[0]
    return m


def mul4(a: list[float], b: list[float]) -> list[float]:
    o = [0.0] * 16
    for r in range(4):
        for c in range(4):
            o[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] + a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3]
    return o


def xform(m: list[float], x: float, y: float, z: float) -> tuple[float, float, float, float]:
    xp = m[0] * x + m[4] * y + m[8] * z + m[12]
    yp = m[1] * x + m[5] * y + m[9] * z + m[13]
    zp = m[2] * x + m[6] * y + m[10] * z + m[14]
    wp = m[3] * x + m[7] * y + m[11] * z + m[15]
    return xp, yp, zp, wp


class LunaCam:
    def __init__(self) -> None:
        self.tx = 1.5
        self.ty = 6.0
        self.tz = 0.15
        self.radius = 22.0
        self.theta = 1.5 * XM_PI + 0.45
        self.phi = 0.55
        self.mirror_x = True

    def wvp(self, aspect: float) -> list[float]:
        ex, ey, ez = luna_eye(self.tx, self.ty, self.tz, self.radius, self.theta, self.phi)
        view = look_at_lh(ex, ey, ez, self.tx, self.ty, self.tz)
        proj = perspective_lh(0.25 * XM_PI, max(aspect, 0.1), 0.1, 10000.0, self.mirror_x)
        return mul4(view, proj)


def raster_meshes(
    meshes: list[SplineMesh],
    texels: dict[str, tuple[int, int, bytes]],
    cam: LunaCam,
    width: int,
    height: int,
) -> bytes:
    """Return RGB bytes (row-major). texels[name] = (w, h, RGBA)."""
    rgb = array("B", [40, 42, 48] * (width * height))
    zbuf = array("f", [1e30] * (width * height))
    wvp = cam.wvp(width / max(height, 1))

    def sample(name: str, u: float, v: float) -> tuple[int, int, int]:
        img = texels.get(name)
        if not img:
            return 210, 210, 214
        tw, th, px = img
        uu = u - math.floor(u)
        vv = v - math.floor(v)
        ix = min(tw - 1, max(0, int(uu * tw)))
        iy = min(th - 1, max(0, int(vv * th)))
        o = (iy * tw + ix) * 4
        return px[o], px[o + 1], px[o + 2]

    def project(v: Vertex) -> tuple[float, float, float, float, float] | None:
        x, y, z, w = xform(wvp, v.x, v.y, v.z)
        if w <= 1e-5:
            return None
        ndc_x = x / w
        ndc_y = y / w
        ndc_z = z / w
        sx = (ndc_x * 0.5 + 0.5) * (width - 1)
        sy = (1.0 - (ndc_y * 0.5 + 0.5)) * (height - 1)
        return sx, sy, ndc_z, v.u, v.v

    for mesh in meshes:
        verts = mesh.vertices
        idx = mesh.indices
        name = mesh.texture
        for t in range(0, len(idx) - 2, 3):
            pa = project(verts[idx[t]])
            pb = project(verts[idx[t + 1]])
            pc = project(verts[idx[t + 2]])
            if pa is None or pb is None or pc is None:
                continue
            (ax, ay, az, au, av) = pa
            (bx, by, bz, bu, bv) = pb
            (cx, cy, cz, cu, cv) = pc
            minx = max(0, int(math.floor(min(ax, bx, cx))))
            maxx = min(width - 1, int(math.ceil(max(ax, bx, cx))))
            miny = max(0, int(math.floor(min(ay, by, cy))))
            maxy = min(height - 1, int(math.ceil(max(ay, by, cy))))
            area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
            if abs(area) < 1e-8:
                continue
            inv = 1.0 / area
            for py in range(miny, maxy + 1):
                for px in range(minx, maxx + 1):
                    w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) * inv
                    w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) * inv
                    w2 = 1.0 - w0 - w1
                    if w0 < 0 or w1 < 0 or w2 < 0:
                        continue
                    z = w0 * az + w1 * bz + w2 * cz
                    pi = py * width + px
                    if z >= zbuf[pi]:
                        continue
                    zbuf[pi] = z
                    u = w0 * au + w1 * bu + w2 * cu
                    v = w0 * av + w1 * bv + w2 * cv
                    r, g, b = sample(name, u, v)
                    o = pi * 3
                    rgb[o] = r
                    rgb[o + 1] = g
                    rgb[o + 2] = b
    return bytes(rgb)
