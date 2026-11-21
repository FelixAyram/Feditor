"""Hardware OpenGL preview (moderngl) — same mesh as the engine, Luna Z-up camera."""

from __future__ import annotations

import math
import struct
import sys

from mesh import SplineMesh


def dbg(*a) -> None:
    print("[sli3d]", *a, file=sys.stderr, flush=True)


VS = """
#version 330
in vec3 in_pos;
in vec2 in_uv;
uniform mat4 mvp;
out vec2 v_uv;
out vec3 v_pos;
void main() {
    v_uv = in_uv;
    v_pos = in_pos;
    gl_Position = mvp * vec4(in_pos, 1.0);
}
"""

FS = """
#version 330
uniform sampler2D u_tex;
uniform int u_has_tex;
uniform int u_alpha;
in vec2 v_uv;
in vec3 v_pos;
out vec4 f_color;
void main() {
    vec3 n = normalize(cross(dFdx(v_pos), dFdy(v_pos)));
    float lit = 0.55 + 0.55 * max(0.0, dot(n, normalize(vec3(0.25, 0.15, 0.95))));
    vec4 texc = vec4(0.95, 0.62, 0.22, 1.0);
    if (u_has_tex == 1) texc = texture(u_tex, v_uv);
    float a = texc.a;
    if (u_alpha == 0) a = 1.0;
    if (u_alpha == 1 && a < 0.5) discard;
    f_color = vec4(texc.rgb * lit, a);
}
"""


def _col_major(rows: list[list[float]]) -> list[float]:
    return [rows[r][c] for c in range(4) for r in range(4)]


def _mul_col(a: list[float], b: list[float]) -> list[float]:
    o = [0.0] * 16
    for c in range(4):
        for r in range(4):
            o[c * 4 + r] = (
                a[0 * 4 + r] * b[c * 4 + 0]
                + a[1 * 4 + r] * b[c * 4 + 1]
                + a[2 * 4 + r] * b[c * 4 + 2]
                + a[3 * 4 + r] * b[c * 4 + 3]
            )
    return o


def _look_at(ex, ey, ez, tx, ty, tz) -> list[float]:
    fx, fy, fz = tx - ex, ty - ey, tz - ez
    fl = math.sqrt(fx * fx + fy * fy + fz * fz) or 1.0
    fx, fy, fz = fx / fl, fy / fl, fz / fl
    upx, upy, upz = 0.0, 0.0, 1.0
    if abs(fz) > 0.995:
        upx, upy, upz = 0.0, 1.0, 0.0
    sx = fy * upz - fz * upy
    sy = fz * upx - fx * upz
    sz = fx * upy - fy * upx
    sl = math.sqrt(sx * sx + sy * sy + sz * sz) or 1.0
    sx, sy, sz = sx / sl, sy / sl, sz / sl
    ux = sy * fz - sz * fy
    uy = sz * fx - sx * fz
    uz = sx * fy - sy * fx
    return _col_major(
        [
            [sx, sy, sz, -(sx * ex + sy * ey + sz * ez)],
            [ux, uy, uz, -(ux * ex + uy * ey + uz * ez)],
            [-fx, -fy, -fz, fx * ex + fy * ey + fz * ez],
            [0.0, 0.0, 0.0, 1.0],
        ]
    )


def _perspective(fov, aspect, zn, zf, mirror_x) -> list[float]:
    f = 1.0 / math.tan(fov * 0.5)
    a = f / max(aspect, 0.05)
    if mirror_x:
        a = -a
    return _col_major(
        [
            [a, 0.0, 0.0, 0.0],
            [0.0, f, 0.0, 0.0],
            [0.0, 0.0, (zf + zn) / (zn - zf), (2.0 * zf * zn) / (zn - zf)],
            [0.0, 0.0, -1.0, 0.0],
        ]
    )


class LunaCam:
    def __init__(self) -> None:
        self.tx, self.ty, self.tz = 0.0, 6.0, 0.2
        self.radius = 16.0
        self.theta = 1.5 * math.pi + 0.45
        self.phi = 0.85
        self.mirror_x = True

    def eye(self):
        s, c = math.sin(self.phi), math.cos(self.phi)
        return (
            self.tx + self.radius * s * math.cos(self.theta),
            self.ty + self.radius * s * math.sin(self.theta),
            self.tz + self.radius * c,
        )

    def frame_bounds(self, minx, miny, minz, maxx, maxy, maxz) -> None:
        self.tx = (minx + maxx) * 0.5
        self.ty = (miny + maxy) * 0.5
        self.tz = (minz + maxz) * 0.5
        dx, dy, dz = max(maxx - minx, 0.2), max(maxy - miny, 0.2), max(maxz - minz, 0.2)
        # Vereda 1 m de ancho x 12 m: no alejarse con el diagonal (queda una cinta invisible).
        self.radius = max(dy * 0.55, dx * 6.0, dz * 8.0, 5.0)
        self.phi = 0.72
        dbg(
            f"cam frame target=({self.tx:.2f},{self.ty:.2f},{self.tz:.2f}) r={self.radius:.2f} "
            f"bbox x={minx:.2f}..{maxx:.2f} y={miny:.2f}..{maxy:.2f} z={minz:.2f}..{maxz:.2f}"
        )

    def mvp(self, aspect: float) -> bytes:
        ex, ey, ez = self.eye()
        view = _look_at(ex, ey, ez, self.tx, self.ty, self.tz)
        proj = _perspective(0.25 * math.pi, aspect, 0.05, 500.0, self.mirror_x)
        return struct.pack("16f", *_mul_col(proj, view))


class GlPreview:
    def __init__(self) -> None:
        import moderngl

        self.mgl = moderngl
        self.ctx = moderngl.create_standalone_context()
        self.ctx.enable(moderngl.DEPTH_TEST)
        # Streets were inside-out with back-face cull; OpenGL front matches the .sli winding.
        self.ctx.enable(moderngl.CULL_FACE)
        self.ctx.cull_face = "front"
        self.prog = self.ctx.program(vertex_shader=VS, fragment_shader=FS)
        self.fbo = None
        self.size = (640, 400)
        self.vaos: list = []
        self.tex_objs: dict = {}
        self.white = self._solid_tex(220, 220, 224)
        self.err = ""
        self._dbg_n = 0
        dbg("GL", self.ctx.info.get("GL_RENDERER"), "v", self.ctx.version_code)

    def _solid_tex(self, r, g, b):
        return self.ctx.texture((1, 1), 4, bytes([r, g, b, 255]))

    def _tex(self, rgba: bytes, w: int, h: int):
        t = self.ctx.texture((w, h), 4, rgba)
        t.filter = (self.mgl.LINEAR, self.mgl.LINEAR)
        t.repeat_x = True
        t.repeat_y = True
        return t

    def resize(self, w: int, h: int) -> None:
        w, h = max(64, w), max(64, h)
        if self.fbo is not None and self.size == (w, h):
            return
        self.size = (w, h)
        if self.fbo:
            self.fbo.release()
        self.fbo = self.ctx.framebuffer(
            color_attachments=[self.ctx.texture((w, h), 4)],
            depth_attachment=self.ctx.depth_texture((w, h)),
        )
        dbg(f"fbo resize {w}x{h}")

    def set_meshes(self, meshes: list[SplineMesh], texels: dict[str, tuple[int, int, bytes]]) -> None:
        for vao, vbo, ibo, *_rest in self.vaos:
            vao.release()
            vbo.release()
            ibo.release()
        self.vaos.clear()
        for mesh in meshes:
            if not mesh.vertices or not mesh.indices:
                continue
            raw = b"".join(struct.pack("5f", v.x, v.y, v.z, v.u, v.v) for v in mesh.vertices)
            idx = struct.pack(f"{len(mesh.indices)}I", *mesh.indices)
            vbo = self.ctx.buffer(raw)
            ibo = self.ctx.buffer(idx)
            vao = self.ctx.vertex_array(self.prog, [(vbo, "3f 2f", "in_pos", "in_uv")], ibo)
            tex = self.white
            if mesh.texture and mesh.texture in texels:
                key = mesh.texture
                if key not in self.tex_objs:
                    tw, th, px = texels[key]
                    self.tex_objs[key] = self._tex(px, tw, th)
                    dbg(f"tex GPU {key} {tw}x{th}")
                tex = self.tex_objs[key]
            has = 1 if tex is not self.white else 0
            alpha = int(getattr(mesh, "alpha", 0))
            self.vaos.append((vao, vbo, ibo, tex, has, len(mesh.indices), alpha))
            xs = [v.x for v in mesh.vertices]
            ys = [v.y for v in mesh.vertices]
            zs = [v.z for v in mesh.vertices]
            dbg(
                f"mesh tex={mesh.texture!r} has_tex={has} alpha={alpha} v={len(mesh.vertices)} i={len(mesh.indices)} "
                f"x={min(xs):.2f}..{max(xs):.2f} y={min(ys):.2f}..{max(ys):.2f} z={min(zs):.2f}..{max(zs):.2f}"
            )

    def render(self, cam: LunaCam) -> bytes:
        w, h = self.size
        self.resize(w, h)
        self.fbo.use()
        self.ctx.viewport = (0, 0, w, h)
        self.ctx.front_face = "ccw" if cam.mirror_x else "cw"
        self.ctx.enable(self.mgl.CULL_FACE)
        self.ctx.cull_face = "front"
        self.ctx.clear(0.08, 0.09, 0.12, 1.0)
        self.prog["mvp"].write(cam.mvp(w / max(h, 1)))
        opaque = [it for it in self.vaos if it[-1] != 2]
        blend = [it for it in self.vaos if it[-1] == 2]
        self.ctx.disable(self.mgl.BLEND)
        self.ctx.depth_mask = True
        for item in opaque:
            vao, _v, _i, tex, has, _n, alpha = item
            tex.use(0)
            self.prog["u_tex"] = 0
            self.prog["u_has_tex"] = has
            self.prog["u_alpha"] = alpha
            vao.render()
        if blend:
            self.ctx.enable(self.mgl.BLEND)
            self.ctx.blend_func = (self.mgl.SRC_ALPHA, self.mgl.ONE_MINUS_SRC_ALPHA)
            self.ctx.depth_mask = False
            for item in blend:
                vao, _v, _i, tex, has, _n, alpha = item
                tex.use(0)
                self.prog["u_tex"] = 0
                self.prog["u_has_tex"] = has
                self.prog["u_alpha"] = alpha
                vao.render()
            self.ctx.depth_mask = True
            self.ctx.disable(self.mgl.BLEND)
        rgb = self.fbo.read(components=3, alignment=1)
        if self._dbg_n < 4 and rgb:
            n = max(len(rgb) // 3, 1)
            mean = sum(rgb[::3]) // n
            ex, ey, ez = cam.eye()
            dbg(
                f"draw#{self._dbg_n} fbo {w}x{h} vaos={len(self.vaos)} meanR={mean} "
                f"eye=({ex:.1f},{ey:.1f},{ez:.1f})"
            )
            self._dbg_n += 1
        return rgb


def try_create() -> GlPreview | None:
    try:
        return GlPreview()
    except Exception as e:
        dbg("moderngl FAILED", type(e).__name__, e)
        return None
