"""OMSI F4 spline creator — same layout/tools as the C++ dialog, implemented in Python."""

from __future__ import annotations

import argparse
import copy
import math
import shutil
import sys
from pathlib import Path

import pygame

from mesh import build_meshes
from preview_gl import LunaCam, dbg, try_create
from sli_io import HeightProfile, ProfilePoint, SliPath, SplineDefinition, empty_def, format_sli, load_sli, write_sli

try:
    from PIL import Image
except ImportError:
    Image = None

try:
    import tkinter as tk
    from tkinter import filedialog, simpledialog, messagebox
except ImportError:
    tk = None

SLATE = (30, 41, 59)
SLATE2 = (51, 65, 85)
WHITE = (248, 250, 252)
TXT = (226, 232, 240)
ACCENT = (56, 189, 248)
BTN = (71, 85, 105)
GREEN = (22, 163, 74)
RED = (220, 38, 38)
YELLOW = (202, 138, 4)
SNAP_M = [0.01, 0.05, 0.1, 0.5, 0.0]
SIDE_FOOTER_H = 138
HIT_PT = 16
HIT_SEG = 14
# (frozen release: tools\spline_creator\spline_creator.exe, the assets sit next to the editor)
ASSETS = (Path(sys.executable) if getattr(sys, "frozen", False) else Path(__file__)).resolve().parents[2] / "assets"
ICON_FILES = {
    "espejar": "spline_icon_espejar.png",
    "flip": "spline_icon_flip.png",
    "delPt": "spline_icon_delete.png",
    "pencil": "spline_pencil_tool.png",
    "marquee": "spline_icon_select.png",
    "drawTex": "spline_icon_texture.png",
    "drawPath": "spline_icon_path.webp",
}


def tk_root():
    if tk is None:
        return None
    r = tk.Tk()
    r.withdraw()
    return r


def dist_sq_seg(px, py, ax, ay, bx, by) -> float:
    abx, aby = bx - ax, by - ay
    apx, apy = px - ax, py - ay
    ab2 = abx * abx + aby * aby
    if ab2 < 1e-6:
        return apx * apx + apy * apy
    t = max(0.0, min(1.0, (apx * abx + apy * aby) / ab2))
    dx, dy = px - (ax + t * abx), py - (ay + t * aby)
    return dx * dx + dy * dy


def guess_omsi_root(from_path: Path | None) -> Path | None:
    if not from_path:
        return None
    p = from_path if from_path.is_dir() else from_path.parent
    for _ in range(16):
        tex = (p / "texture").is_dir() or (p / "Texture").is_dir()
        if tex and (p / "maps").is_dir():
            return p
        if p.name.lower() == "splines":
            return p.parent
        if p.parent == p:
            break
        p = p.parent
    return None


def _try_tex_dir(owner: Path, rel: Path) -> Path | None:
    fname = rel.name
    stem = rel.stem
    alts = [stem + ".dds", fname, stem + ".bmp", stem + ".tga", stem + ".png", stem + ".jpg", stem + ".jpeg"]
    folders = [owner / "texture", owner / "Texture", owner]
    if rel.parent != Path("."):
        folders = [owner / rel.parent / "texture", owner / rel.parent / "Texture", owner / rel.parent] + folders
    seen: set[str] = set()
    for folder in folders:
        for a in alts:
            c = folder / a
            key = str(c).lower()
            if key in seen:
                continue
            seen.add(key)
            if c.is_file():
                return c
    return None


def resolve_tex(d: SplineDefinition, name: str, omsi: Path, sli_hint: Path | None = None) -> Path | None:
    if name in d.tex_sources and d.tex_sources[name].is_file():
        return d.tex_sources[name]
    rel = Path(name.replace("\\", "/"))
    abs_try = Path(name)
    if abs_try.is_absolute() and abs_try.is_file():
        return abs_try
    sli = sli_hint or d.origin_sli or d.sli_path
    owners: list[Path] = []
    if sli:
        owners.append(sli.parent)
    roots: list[Path] = []
    for r in (omsi, guess_omsi_root(sli), guess_omsi_root(omsi)):
        if r and r not in roots:
            roots.append(r)
    for owner in owners:
        hit = _try_tex_dir(owner, rel)
        if hit:
            return hit
    for root in roots:
        hit = _try_tex_dir(root, rel)
        if hit:
            return hit
        hit = _try_tex_dir(root / "Splines", rel)
        if hit:
            return hit
    return None


def copy_file_unlocked(src: Path, dst: Path) -> str | None:
    """Copy texture; skip if same path; never crash on WinError 32."""
    try:
        if dst.exists() and src.resolve() == dst.resolve():
            return None
    except OSError:
        pass
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copy2(src, dst)
        return None
    except OSError:
        pass
    try:
        dst.write_bytes(src.read_bytes())
        return None
    except OSError as e:
        return f"{src.name}: {e}"


def load_rgba(path: Path, max_edge: int = 1024) -> tuple[int, int, bytes] | None:
    if Image is None:
        return None
    try:
        im = Image.open(path).convert("RGBA")
        im.thumbnail((max_edge, max_edge))
        return im.size[0], im.size[1], im.tobytes()
    except OSError:
        return None


def load_icon_file(cmd: str, size: int) -> pygame.Surface | None:
    name = ICON_FILES.get(cmd)
    if not name:
        return None
    p = ASSETS / name
    if not p.is_file() or Image is None:
        return None
    try:
        im = Image.open(p).convert("RGBA")
        im = im.resize((size - 6, size - 6), Image.Resampling.LANCZOS)
        return pygame.image.frombuffer(im.tobytes(), im.size, "RGBA").convert_alpha()
    except OSError:
        return None


def paint_icon(surf: pygame.Surface, cmd: str) -> None:
    w, h = surf.get_size()
    cx, cy = w // 2, h // 2
    c = (248, 250, 252)
    if cmd == "espejar":
        pygame.draw.polygon(surf, c, [(cx - 10, cy), (cx - 2, cy - 7), (cx - 2, cy + 7)])
        pygame.draw.polygon(surf, c, [(cx + 10, cy), (cx + 2, cy - 7), (cx + 2, cy + 7)])
        pygame.draw.line(surf, c, (cx, cy - 8), (cx, cy + 8), 1)
    elif cmd == "flip":
        pygame.draw.arc(surf, c, pygame.Rect(cx - 8, cy - 8, 16, 16), 0.4, 4.8, 2)
        pygame.draw.polygon(surf, c, [(cx + 8, cy - 8), (cx + 2, cy - 2), (cx + 10, cy)])
    elif cmd == "delPt":
        pygame.draw.line(surf, (248, 113, 113), (cx - 6, cy - 6), (cx + 6, cy + 6), 2)
        pygame.draw.line(surf, (248, 113, 113), (cx + 6, cy - 6), (cx - 6, cy + 6), 2)
    elif cmd == "pencil":
        pygame.draw.polygon(surf, c, [(cx - 7, cy + 7), (cx - 4, cy + 8), (cx + 8, cy - 4), (cx + 5, cy - 7)])
        pygame.draw.rect(surf, (251, 191, 36), (cx - 8, cy + 5, 4, 4))
    elif cmd == "marquee":
        pygame.draw.rect(surf, c, (cx - 8, cy - 8, 16, 16), 1)
        pygame.draw.line(surf, c, (cx - 3, cy + 2), (cx - 3, cy - 4), 2)
        pygame.draw.line(surf, c, (cx - 3, cy + 2), (cx + 3, cy + 2), 2)
    elif cmd == "drawTex":
        pygame.draw.rect(surf, (148, 163, 184), (cx - 8, cy - 8, 8, 8))
        pygame.draw.rect(surf, (71, 85, 105), (cx, cy - 8, 8, 8))
        pygame.draw.rect(surf, (71, 85, 105), (cx - 8, cy, 8, 8))
        pygame.draw.rect(surf, (148, 163, 184), (cx, cy, 8, 8))
    elif cmd == "drawPath":
        pygame.draw.line(surf, (34, 197, 94), (cx - 9, cy + 4), (cx + 9, cy - 4), 3)
        pygame.draw.circle(surf, (34, 197, 94), (cx - 9, cy + 4), 3)
        pygame.draw.circle(surf, (34, 197, 94), (cx + 9, cy - 4), 3)


def default_street() -> SplineDefinition:
    d = SplineDefinition()
    d.height_profiles = [HeightProfile(0.0, 6.0, 0.1, 0.1)]
    d.textures = ["str_asphdrk.bmp"]
    d.texture_alpha = [0]
    d.profiles = [[ProfilePoint(0.0, 0.1, 0.0, 0.167, 0), ProfilePoint(6.0, 0.1, 1.0, 0.167, 0)]]
    d.profile_comments = [""]
    d.paths = [SliPath(1, 3.0, 0.1, 3.0, 2)]
    return d


class View2D:
    def __init__(self) -> None:
        self.pan_x = 3.0
        self.pan_z = 0.3
        self.scale = 80.0

    def w2s(self, rect, x, z):
        cx = rect.centerx
        cy = rect.centery
        return int(cx + (x - self.pan_x) * self.scale), int(cy - (z - self.pan_z) * self.scale)

    def s2w(self, rect, sx, sy):
        cx = rect.centerx
        cy = rect.centery
        return (sx - cx) / self.scale + self.pan_x, (cy - sy) / self.scale + self.pan_z


class App:
    def __init__(self, omsi: Path, sli: Path | None, status: Path | None) -> None:
        pygame.init()
        pygame.display.set_caption("Crear spline (.sli)")
        self.screen = pygame.display.set_mode((1440, 860))
        self.clock = pygame.time.Clock()
        self.font = pygame.font.SysFont("Segoe UI", 14)
        self.font_s = pygame.font.SysFont("Segoe UI", 12)
        self.omsi = omsi
        self.status = status
        self.copy_tex = True
        self.d = load_sli(sli) if sli else None
        if self.d is None:
            self.d = default_street()
            if sli:
                self.d.sli_path = sli
        self.undo: list[SplineDefinition] = []
        self.redo: list[SplineDefinition] = []
        self.side_scroll = 0
        self.side_max_scroll = 0
        self.nudge_active: str | None = None
        self.nudge_next_ms = 0
        self.view = View2D()
        self.cam = LunaCam()
        self.gl = try_create()
        self.texels: dict[str, tuple[int, int, bytes]] = {}
        self.thumbs: dict[str, pygame.Surface] = {}
        self.icons: dict[str, pygame.Surface] = {}
        self.sel_profile = 0
        self.sel_tex = 0
        self.sel_path = 0
        self.sel_pts: list[int] = []
        self.sel_multi: list[tuple[int, int]] = []
        self.sel_profiles: set[int] = set()  # multi-select whole profiles (for shared UV texture)
        self.clipboard_profiles: list = []
        self.paste_place = False  # pasted profiles follow mouse until LMB
        self.paste_anchor_x = 0.0
        self.paste_anchor_z = 0.0
        self.paste_ghost: list = []  # list of strips (ProfilePoint lists) being placed
        self.snap_i = 1
        self.pencil = False
        self.marquee = False
        self.draw_tex = True
        self.draw_path = True
        self.grab = False
        self.grab_origin = False
        self.grab_ax = self.grab_az = 0.0
        self.drag_group: list[tuple[int, int, float, float]] = []
        self.path_place = False
        self.path_drag = False
        self.path_ox = self.path_oz = self.path_wx = self.path_wz = 0.0
        self.panning = False
        self.orbit = False
        self.pencil_drag = False
        self.marquee_drag = False
        self.px0 = self.pz0 = self.px1 = self.pz1 = 0.0
        self.mx0 = self.my0 = self.mx1 = self.my1 = 0
        self.last_mouse = (0, 0)
        self.hover_tip = ""
        self.dirty3d = True
        self._framed = False
        self._mesh_sig = None
        self.preview_surf: pygame.Surface | None = None
        self.tk = tk_root()
        self.buttons: list[tuple[pygame.Rect, str, str, str]] = []
        self._load_icons()
        self.center_view()
        self.reload_tex()
        dbg("open sli", self.d.sli_path)
        dbg("profiles", len(self.d.profiles), "textures", self.d.textures, "gl", bool(self.gl))

    def _load_icons(self) -> None:
        for cmd in ICON_FILES:
            surf = pygame.Surface((26, 26), pygame.SRCALPHA)
            file_ic = load_icon_file(cmd, 26)
            if file_ic:
                surf.blit(file_ic, file_ic.get_rect(center=(13, 13)))
            else:
                paint_icon(surf, cmd)
            self.icons[cmd] = surf

    def push_undo(self) -> None:
        self.undo.append(copy.deepcopy(self.d))
        if len(self.undo) > 40:
            self.undo.pop(0)
        self.redo.clear()

    def do_undo(self) -> None:
        if self.undo:
            self.redo.append(copy.deepcopy(self.d))
            self.d = self.undo.pop()
            self.dirty3d = True

    def do_redo(self) -> None:
        if self.redo:
            self.undo.append(copy.deepcopy(self.d))
            self.d = self.redo.pop()
            self.dirty3d = True

    def snap(self, v: float) -> float:
        step = SNAP_M[self.snap_i]
        if step <= 1e-6:
            return v
        return round(v / step) * step

    def grid_m(self) -> float:
        return SNAP_M[self.snap_i] if SNAP_M[self.snap_i] > 0 else 0.05

    def nudge_step(self) -> float:
        step = SNAP_M[self.snap_i]
        return step if step > 1e-6 else 0.05

    def snap_pencil(self, v: float) -> float:
        step = SNAP_M[self.snap_i]
        if step <= 1e-6:
            step = 0.05
        return round(v / step) * step

    def mirror_targets(self) -> list[int]:
        # Prefer explicit multi-profile selection (side panel Ctrl+click)
        if self.sel_profiles:
            return sorted(pi for pi in self.sel_profiles if 0 <= pi < len(self.d.profiles))
        seen: set[int] = set()
        out: list[int] = []
        for pi, _ in self.sel_multi:
            if pi not in seen:
                seen.add(pi)
                out.append(pi)
        if out:
            return out
        if 0 <= self.sel_profile < len(self.d.profiles):
            return [self.sel_profile]
        return []

    def nudge_selection(self, sx: int, sz: int, new_stroke: bool) -> None:
        step = self.nudge_step()
        if step <= 1e-6:
            return
        if not self.sel_multi and self.d.profiles:
            self.select_whole_profile(self.sel_profile)
        seeds = list(self.sel_multi)
        if not seeds:
            return
        if new_stroke:
            self.push_undo()
        moved: set[tuple[int, int]] = set()
        for pi, i in seeds:
            if (pi, i) in moved:
                continue
            moved.add((pi, i))
            if 0 <= pi < len(self.d.profiles) and 0 <= i < len(self.d.profiles[pi]):
                pt = self.d.profiles[pi][i]
                pt.x = self.snap(pt.x + sx * step)
                pt.z = self.snap(pt.z + sz * step)
        self.dirty3d = True

    def nudge_once(self, cmd: str, new_stroke: bool = False) -> None:
        deltas = {"nudgeL": (-1, 0), "nudgeR": (1, 0), "nudgeU": (0, 1), "nudgeD": (0, -1)}
        d = deltas.get(cmd)
        if d:
            self.nudge_selection(d[0], d[1], new_stroke)

    def tick_nudge_hold(self) -> None:
        if not self.nudge_active:
            return
        if not pygame.mouse.get_pressed()[0]:
            self.nudge_active = None
            return
        now = pygame.time.get_ticks()
        if now < self.nudge_next_ms:
            return
        self.nudge_next_ms = now + 70
        self.nudge_once(self.nudge_active, new_stroke=False)

    def reload_tex(self) -> None:
        self.texels.clear()
        self.thumbs.clear()
        for name in self.d.textures:
            p = resolve_tex(self.d, name, self.omsi)
            if not p:
                continue
            t = load_rgba(p)
            if t:
                self.texels[name] = t
                w, h, px = t
                im = pygame.image.frombuffer(px, (w, h), "RGBA")
                self.thumbs[name] = pygame.transform.smoothscale(im, (56, 56))
                dbg("tex ok", name, "->", p, f"{w}x{h}")
            else:
                dbg("tex decode FAIL", name, p)
        for name in self.d.textures:
            if name not in self.texels:
                p = resolve_tex(self.d, name, self.omsi)
                dbg("tex missing", name, "resolved", p)
        self._mesh_sig = None
        self.dirty3d = True

    def layout(self):
        w, h = self.screen.get_size()
        chrome_h = 210
        side = 300
        canv = pygame.Rect(8, 8, 420, h - chrome_h - 16)
        prev = pygame.Rect(436, 8, max(280, w - 436 - side - 16), h - chrome_h - 16)
        side_r = pygame.Rect(w - side - 8, 8, side, h - 16)
        chrome = pygame.Rect(8, h - chrome_h, w - side - 24, chrome_h - 8)
        return canv, prev, side_r, chrome

    def center_view(self) -> None:
        xs, zs = [], []
        for s in self.d.profiles:
            for p in s:
                xs.append(p.x)
                zs.append(p.z)
        if not xs:
            xs, zs = [0, 6], [0, 0.3]
        self.view.pan_x = (min(xs) + max(xs)) * 0.5
        self.view.pan_z = (min(zs) + max(zs)) * 0.5
        canv, *_ = self.layout()
        ww = max(max(xs) - min(xs), 0.5)
        hh = max(max(zs) - min(zs), 0.5)
        self.view.scale = min((canv.w - 48) / ww, (canv.h - 48) / hh)

    def rebuild_3d(self, prev: pygame.Rect) -> None:
        if not self.gl:
            self.preview_surf = None
            dbg("rebuild_3d skipped: no GL")
            return
        self.gl.resize(prev.w, prev.h)
        meshes = build_meshes(self.d)
        # Vertex counts stay the same while dragging; always upload so 2D edits show in 3D.
        self.gl.set_meshes(meshes, self.texels)
        xs, ys, zs = [], [], []
        for m in meshes:
            xs += [v.x for v in m.vertices]
            ys += [v.y for v in m.vertices]
            zs += [v.z for v in m.vertices]
        if xs and not self._framed:
            self.cam.frame_bounds(min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))
            self._framed = True
        rgb = self.gl.render(self.cam)
        surf = pygame.image.frombuffer(rgb, self.gl.size, "RGB").convert()
        self.preview_surf = pygame.transform.flip(surf, False, True)
        self.dirty3d = False

    def add_profile_strip(self, x0, z0, x1, z1) -> None:
        x0, z0 = self.snap_pencil(x0), self.snap_pencil(z0)
        x1, z1 = self.snap_pencil(x1), self.snap_pencil(z1)
        mat = min(self.sel_tex, max(0, len(self.d.textures) - 1)) if self.d.textures else 0
        if x0 > x1:
            x0, x1, z0, z1 = x1, x0, z1, z0
        self.d.profiles.append(
            [ProfilePoint(x0, z0, 0.0, 0.167, mat), ProfilePoint(x1, z1, 1.0, 0.167, mat)]
        )
        self.d.profile_comments.append("")
        self.sel_profile = len(self.d.profiles) - 1
        self.select_whole_profile(self.sel_profile)

    def select_whole_profile(self, pi: int, *, additive: bool = False) -> None:
        if not (0 <= pi < len(self.d.profiles)):
            return
        self.sel_profile = pi
        self.sel_pts = list(range(len(self.d.profiles[pi])))
        if additive:
            # Toggle this profile in the multi-set; rebuild sel_multi from all selected profiles
            if pi in self.sel_profiles:
                self.sel_profiles.discard(pi)
            else:
                self.sel_profiles.add(pi)
            if not self.sel_profiles:
                self.sel_profiles.add(pi)
            self.sel_multi = []
            for p in sorted(self.sel_profiles):
                if 0 <= p < len(self.d.profiles):
                    for i in range(len(self.d.profiles[p])):
                        self.sel_multi.append((p, i))
            self.sel_pts = [i for p, i in self.sel_multi if p == self.sel_profile]
        else:
            self.sel_profiles = {pi}
            self.sel_multi = [(pi, i) for i in self.sel_pts]

    def _paste_ghost_at(self, wx: float, wz: float) -> list:
        """Return deep-copied strips translated so centroid sits at (wx, wz)."""
        dx = self.snap(wx) - self.paste_anchor_x
        dz = self.snap(wz) - self.paste_anchor_z
        out = []
        for strip in self.paste_ghost:
            ns = copy.deepcopy(strip)
            for p in ns:
                p.x = self.snap(p.x + dx)
                p.z = self.snap(p.z + dz)
            out.append(ns)
        return out

    def _commit_paste_at(self, wx: float, wz: float) -> None:
        if not self.paste_ghost:
            self.paste_place = False
            return
        self.push_undo()
        placed = self._paste_ghost_at(wx, wz)
        new_idxs: list[int] = []
        for strip in placed:
            self.d.profiles.append(strip)
            self.d.profile_comments.append("")
            new_idxs.append(len(self.d.profiles) - 1)
        self.paste_place = False
        self.paste_ghost = []
        # Keep pasted profiles selected and ready to move with mouse (grab)
        self.sel_profiles = set(new_idxs)
        self.sel_multi = []
        for pi in new_idxs:
            for i in range(len(self.d.profiles[pi])):
                self.sel_multi.append((pi, i))
        self.sel_profile = new_idxs[0]
        self.sel_pts = [i for p, i in self.sel_multi if p == self.sel_profile]
        self.dirty3d = True
        # Auto-start grab so they follow the mouse until click
        self.start_grab()

    def profile_has_tex(self, strip: list) -> bool:
        if not strip:
            return False
        m = strip[0].material_id
        return 0 <= m < len(self.d.textures)

    def profile_geo_label(self, strip: list) -> str:
        if not strip:
            return "vacío"
        xs = [p.x for p in strip]
        zs = [p.z for p in strip]
        w = max(xs) - min(xs)
        return f"X {min(xs):+.2f}..{max(xs):+.2f}  Δ{w:.2f} m  Z {min(zs):+.2f}..{max(zs):+.2f}"

    def espejar(self) -> None:
        targets = self.mirror_targets()
        if not targets:
            return
        self.push_undo()
        created: list[int] = []
        for pi in targets:
            if not (0 <= pi < len(self.d.profiles)):
                continue
            strip = self.d.profiles[pi]
            cmt = self.d.profile_comments[pi] if pi < len(self.d.profile_comments) else ""
            if "(espejo)" in cmt or not strip:
                continue
            avg = sum(p.x for p in strip) / len(strip)
            if avg < -1e-4:
                continue
            mir = [ProfilePoint(-p.x, p.z, p.uv_x, p.y_tiling, p.material_id) for p in strip if abs(p.x) > 1e-4]
            if len(mir) < 2:
                continue
            mir.reverse()
            self.d.profiles.append(mir)
            self.d.profile_comments.append((cmt or f"Perfil {pi+1}") + " (espejo)")
            created.append(len(self.d.profiles) - 1)
        if created:
            orig = list(self.d.paths)
            for p in orig:
                if abs(p.lateral) > 1e-4:
                    self.d.paths.append(SliPath(p.typ, -p.lateral, p.height, p.width, p.direction, p.secondary))
            self.select_whole_profile(created[-1])
            self.dirty3d = True

    def flip(self) -> None:
        self.push_undo()
        s = self.d.profiles[self.sel_profile]
        s.reverse()
        self.dirty3d = True

    def apply_tex(self) -> None:
        if not self.d.textures or not self.d.profiles or self.sel_tex < 0:
            return
        self.push_undo()
        mat = min(self.sel_tex, len(self.d.textures) - 1)
        # Profiles to affect: multi-selection (unique profile indices) or active one.
        targets = self.mirror_targets()
        if not targets:
            targets = [self.sel_profile] if 0 <= self.sel_profile < len(self.d.profiles) else []
        n = len(targets)
        for k, pi in enumerate(targets):
            if not (0 <= pi < len(self.d.profiles)):
                continue
            strip = self.d.profiles[pi]
            if not strip:
                continue
            # Even UV split across selected profiles: [0..1/n], [1/n..2/n], ...
            u_lo = k / n
            u_hi = (k + 1) / n
            for p in strip:
                p.material_id = mat
            # Assign U only at the strip endpoints (OMSI transversal UV)
            strip[0].uv_x = u_lo
            strip[-1].uv_x = u_hi
            # Linear intermediate U if more than 2 points
            if len(strip) > 2:
                for i, p in enumerate(strip):
                    t = i / (len(strip) - 1)
                    p.uv_x = u_lo + (u_hi - u_lo) * t
        self.dirty3d = True

    def del_pt(self) -> None:
        if not self.d.profiles:
            return
        strip = self.d.profiles[self.sel_profile]
        if strip and len(strip) - len(self.sel_pts) >= 2 and self.sel_pts and len(self.sel_pts) < len(strip):
            self.push_undo()
            for i in sorted(self.sel_pts, reverse=True):
                if 0 <= i < len(strip):
                    strip.pop(i)
            self.sel_pts.clear()
            self.dirty3d = True
        elif len(self.d.profiles) > 1:
            self.push_undo()
            self.d.profiles.pop(self.sel_profile)
            if self.sel_profile < len(self.d.profile_comments):
                self.d.profile_comments.pop(self.sel_profile)
            self.sel_profile = max(0, self.sel_profile - 1)
            self.select_whole_profile(self.sel_profile)
            self.dirty3d = True

    def tex_add(self) -> None:
        if self.tk is None:
            return
        path = filedialog.askopenfilename(
            filetypes=[("Texturas", "*.bmp;*.tga;*.png;*.jpg;*.dds"), ("Todos", "*.*")]
        )
        if not path:
            return
        p = Path(path)
        name = p.name
        self.push_undo()
        if name not in self.d.textures:
            self.d.textures.append(name)
            self.d.texture_alpha.append(0)
        self.d.tex_sources[name] = p
        self.sel_tex = self.d.textures.index(name)
        self.reload_tex()
        self.apply_tex()

    def tex_del(self) -> None:
        if not self.d.textures or self.sel_tex < 0:
            return
        self.push_undo()
        i = min(max(self.sel_tex, 0), len(self.d.textures) - 1)
        for strip in self.d.profiles:
            for p in strip:
                if p.material_id < 0:
                    continue
                if p.material_id > i:
                    p.material_id -= 1
                elif p.material_id == i:
                    p.material_id = -1
        self.d.textures.pop(i)
        if i < len(self.d.texture_alpha):
            self.d.texture_alpha.pop(i)
        self.sel_tex = -1
        self.reload_tex()
        self.dirty3d = True

    def path_add(self) -> None:
        self.push_undo()
        self.d.paths.append(SliPath())
        self.sel_path = len(self.d.paths) - 1

    def path_del(self) -> None:
        if not self.d.paths:
            return
        self.push_undo()
        self.d.paths.pop(min(self.sel_path, len(self.d.paths) - 1))
        self.sel_path = max(0, self.sel_path - 1)

    def wrap_add(self) -> None:
        self.push_undo()
        x0, z0, x1 = 0.0, 0.1, 3.0
        if self.d.profiles and self.d.profiles[-1]:
            x0 = self.d.profiles[-1][-1].x
            z0 = self.d.profiles[-1][-1].z
            x1 = x0 + 3.0
        self.add_profile_strip(x0, z0, x1, z0)
        self.dirty3d = True

    def open_sli(self) -> None:
        if self.tk is None:
            return
        path = filedialog.askopenfilename(filetypes=[("Spline OMSI", "*.sli")])
        if not path:
            return
        loaded = load_sli(Path(path))
        if loaded:
            self.push_undo()
            self.d = loaded
            self._framed = False
            self._mesh_sig = None
            self.reload_tex()
            self.center_view()

    def save_sli(self) -> None:
        if self.tk is None:
            return
        initial = self.d.sli_path
        init_dir = str(initial.parent) if initial else str(self.omsi / "Splines")
        init_name = initial.name if initial else "nueva.sli"
        picked = filedialog.asksaveasfilename(
            title="Guardar spline (.sli) y texturas",
            defaultextension=".sli",
            filetypes=[("Spline OMSI", "*.sli"), ("Todos", "*.*")],
            initialdir=init_dir,
            initialfile=init_name,
        )
        if not picked:
            return
        path = Path(picked)
        if path.suffix.lower() != ".sli":
            path = path.with_suffix(".sli")
        warns: list[str] = []
        tex_from = self.d.origin_sli or self.d.sli_path
        try:
            write_sli(self.d, path)
            if self.d.origin_sli is None:
                self.d.origin_sli = tex_from
            if self.copy_tex:
                dests = [path.parent / "texture", path.parent]
                for name in self.d.textures:
                    src = resolve_tex(self.d, name, self.omsi, tex_from)
                    if not src:
                        warns.append(f"no se encontró {name}")
                        continue
                    err = copy_file_unlocked(src, dests[0] / src.name)
                    if err:
                        err2 = copy_file_unlocked(src, dests[1] / src.name)
                        if err2:
                            warns.append(err)
            if self.status:
                self.status.write_text(str(path.resolve()), encoding="utf-8")
        except OSError as e:
            messagebox.showerror("guardar .sli", str(e))
            return
        msg = str(path)
        if warns:
            msg += "\n\nTexturas (archivo en uso o no copiado):\n" + "\n".join(warns[:12])
        messagebox.showinfo("guardar .sli", msg)

    def wrap_dialog(self) -> None:
        if self.tk is None or not self.d.profiles:
            return
        strip = self.d.profiles[self.sel_profile]
        if len(strip) < 2:
            return
        win = tk.Toplevel()
        win.title("Textur konfigurieren — wrapping del perfil")
        win.geometry("1280x820")
        win.minsize(960, 560)
        left_wrap = tk.Frame(win, width=460)
        left_wrap.pack(side="left", fill="y")
        left_wrap.pack_propagate(False)
        btns = tk.Frame(left_wrap, padx=8, pady=8)
        btns.pack(side="bottom", fill="x")
        sb = tk.Scrollbar(left_wrap, orient="vertical")
        canvas_l = tk.Canvas(left_wrap, highlightthickness=0, yscrollcommand=sb.set)
        sb.config(command=canvas_l.yview)
        sb.pack(side="right", fill="y")
        canvas_l.pack(side="left", fill="both", expand=True)
        left = tk.Frame(canvas_l, padx=10, pady=6)
        left_id = canvas_l.create_window((0, 0), window=left, anchor="nw")

        def _sync_scroll(_=None):
            canvas_l.configure(scrollregion=canvas_l.bbox("all"))
            canvas_l.itemconfigure(left_id, width=max(canvas_l.winfo_width(), 400))

        left.bind("<Configure>", _sync_scroll)
        canvas_l.bind("<Configure>", lambda e: canvas_l.itemconfigure(left_id, width=e.width))

        def _wheel(e):
            canvas_l.yview_scroll(int(-e.delta / 120), "units")

        canvas_l.bind("<Enter>", lambda _e: canvas_l.bind_all("<MouseWheel>", _wheel))
        canvas_l.bind("<Leave>", lambda _e: canvas_l.unbind_all("<MouseWheel>"))
        # left form continues below
        name = tk.StringVar(value=(self.d.profile_comments[self.sel_profile] if self.sel_profile < len(self.d.profile_comments) else f"Perfil {self.sel_profile+1}"))
        tex_i = tk.IntVar(value=strip[0].material_id)
        u0 = tk.DoubleVar(value=strip[0].uv_x)
        u1 = tk.DoubleVar(value=strip[-1].uv_x)
        x0 = tk.DoubleVar(value=strip[0].x)
        z0 = tk.DoubleVar(value=strip[0].z)
        x1 = tk.DoubleVar(value=strip[-1].x)
        z1 = tk.DoubleVar(value=strip[-1].z)
        til = tk.DoubleVar(value=strip[0].y_tiling)
        alpha = tk.IntVar(value=self.d.texture_alpha[strip[0].material_id] if strip[0].material_id < len(self.d.texture_alpha) else 0)
        tk.Label(left, text="Textura del perfil (clic en la lista)", font=("Segoe UI", 9, "bold")).pack(anchor="w")
        tk.Label(
            left,
            text="Archivo [texture] del .sli. Recorte U abajo; el visor de la derecha muestra el BMP.",
            wraplength=400,
            fg="#475569",
            justify="left",
        ).pack(anchor="w")
        lb = tk.Listbox(left, height=6, width=48)
        for t in self.d.textures:
            lb.insert("end", t)
        if self.d.textures:
            lb.selection_set(min(max(tex_i.get(), 0), len(self.d.textures) - 1))
        lb.pack(anchor="w", pady=(2, 8))

        def field(title, var, hint, width=14):
            tk.Label(left, text=title, font=("Segoe UI", 9, "bold")).pack(anchor="w")
            row = tk.Frame(left)
            row.pack(fill="x", pady=(0, 2))
            tk.Entry(row, textvariable=var, width=width).pack(side="left")
            tk.Label(row, text=hint, fg="#475569", wraplength=280, justify="left").pack(side="left", padx=8)

        field("Bezeichnung (nombre)", name, "Solo etiqueta en el editor; no va al .sli.", 28)
        tk.Label(left, text="--- Texturbereich (UV transversal) ---", font=("Segoe UI", 9, "bold")).pack(anchor="w", pady=(6, 0))
        field("Von U", u0, "UV horizontal al inicio del perfil. 0 = borde izq. de la textura, 1 = borde der.")
        field("bis U", u1, "UV al final del perfil. Calle entera suele ser 0→1; un cordón puede ser 0.8→1.")
        pick = [0]
        img_meta = {"ox": 0, "oy": 0, "dw": 1, "dh": 1}
        pick_hint = tk.StringVar(value="Clic Start/Ende wählen y después en la imagen (U).")
        pick_row = tk.Frame(left)
        pick_row.pack(fill="x", pady=6)

        def set_pick(n: int):
            pick[0] = n
            if n == 1:
                pick_hint.set("Clic en la imagen: START (Von U). Esc cancela el modo.")
            elif n == 2:
                pick_hint.set("Clic en la imagen: ENDE (bis U). Esc cancela el modo.")
            else:
                pick_hint.set("Clic Start/Ende wählen y después en la imagen (U).")
            redraw_uv()

        tk.Button(pick_row, text="Start wählen", command=lambda: set_pick(1)).pack(
            side="left", fill="x", expand=True, padx=(0, 4)
        )
        tk.Button(pick_row, text="Ende wählen", command=lambda: set_pick(2)).pack(side="left", fill="x", expand=True)
        tk.Label(left, textvariable=pick_hint, fg="#0369a1", wraplength=400, justify="left").pack(anchor="w")
        tk.Label(left, text="--- Position (metros, corte XZ) ---", font=("Segoe UI", 9, "bold")).pack(anchor="w", pady=(6, 0))
        field("Startpunkt X (m)", x0, "Lateral del primer nodo. 0 = eje de la spline; +X a la derecha.")
        field("Starthöhe Z (m)", z0, "Altura del primer nodo sobre el terreno / eje.")
        field("Endpunkt X (m)", x1, "Lateral del último nodo (ancho del strip = |X1−X0|).")
        field("Endhöhe Z (m)", z1, "Altura del último nodo. Distinto de Z0 = rampa/cordón.")
        field(
            "V-Steigung (1/m)",
            til,
            "Repetición de la textura a lo largo (eje Y de OMSI). 0.167 ≈ 6 m por tile; 1 = 1 m por repetición.",
        )
        tk.Label(left, text="[matl_alpha]", font=("Segoe UI", 9, "bold")).pack(anchor="w")
        tk.Label(
            left,
            text="0 opaco · 1 alpha-test (rejas) · 2 blend (vidrio). Tag del material en el .sli.",
            fg="#475569",
            wraplength=400,
            justify="left",
        ).pack(anchor="w")
        tk.Entry(left, textvariable=alpha, width=8).pack(anchor="w")
        lb.configure(height=4)
        tex_cv = tk.Canvas(win, bg="#1e293b", highlightthickness=0, cursor="crosshair")
        tex_cv.pack(side="right", fill="both", expand=True)

        def redraw_uv():
            tex_cv.delete("uv")
            dw = max(img_meta["dw"], 1)
            ox, oy, dh = img_meta["ox"], img_meta["oy"], img_meta["dh"]
            for u, which in ((u0.get(), 1), (u1.get(), 2)):
                try:
                    uu = float(u)
                except (tk.TclError, TypeError, ValueError):
                    uu = 0.0
                x = ox + max(0.0, min(1.0, uu)) * dw
                col = "#38bdf8" if pick[0] == which else "#f8fafc"
                tex_cv.create_line(x, oy, x, oy + dh, fill=col, width=3, tags="uv")
                tex_cv.create_text(
                    x + 4,
                    oy + 10,
                    text=("Start" if which == 1 else "Ende"),
                    fill=col,
                    anchor="nw",
                    tags="uv",
                    font=("Segoe UI", 9, "bold"),
                )

        def show_tex(*_):
            sel = lb.curselection()
            tex_cv.delete("all")
            if not sel or Image is None:
                tex_cv.create_text(16, 16, text="(sin textura)", fill="#94a3b8", anchor="nw")
                return
            nm = self.d.textures[int(sel[0])]
            p = resolve_tex(self.d, nm, self.omsi)
            if not p:
                tex_cv.create_text(16, 16, text=f"no: {nm}", fill="#f87171", anchor="nw")
                return
            im = Image.open(p).convert("RGBA")
            cw = max(tex_cv.winfo_width(), 200)
            ch = max(tex_cv.winfo_height(), 200)
            im.thumbnail((max(cw - 24, 64), max(ch - 24, 64)))
            from PIL import ImageTk

            ph = ImageTk.PhotoImage(im)
            tex_cv.image = ph
            ox = (cw - ph.width()) // 2
            oy = (ch - ph.height()) // 2
            img_meta.update(ox=ox, oy=oy, dw=ph.width(), dh=ph.height())
            tex_cv.create_image(ox, oy, image=ph, anchor="nw")
            redraw_uv()

        def on_tex_click(e):
            if pick[0] == 0:
                return
            dw = max(img_meta["dw"], 1)
            u = max(0.0, min(1.0, (e.x - img_meta["ox"]) / dw))
            if pick[0] == 1:
                u0.set(round(u, 4))
            else:
                u1.set(round(u, 4))
            redraw_uv()

        tex_cv.bind("<Button-1>", on_tex_click)
        tex_cv.bind("<Configure>", lambda _e: show_tex())
        lb.bind("<<ListboxSelect>>", show_tex)
        for v in (u0, u1):
            v.trace_add("write", lambda *_: redraw_uv())
        win.after(80, show_tex)

        def ok():
            self.push_undo()
            sel = lb.curselection()
            mat = int(sel[0]) if sel else 0
            a, b = strip[0], strip[-1]
            a.x, a.z, a.uv_x, a.y_tiling, a.material_id = x0.get(), z0.get(), u0.get(), til.get(), mat
            b.x, b.z, b.uv_x, b.y_tiling, b.material_id = x1.get(), z1.get(), u1.get(), til.get(), mat
            for p in strip:
                p.material_id = mat
                p.y_tiling = til.get()
            if self.sel_profile < len(self.d.profile_comments):
                self.d.profile_comments[self.sel_profile] = name.get()
            while len(self.d.texture_alpha) < len(self.d.textures):
                self.d.texture_alpha.append(0)
            if 0 <= mat < len(self.d.texture_alpha):
                self.d.texture_alpha[mat] = max(0, min(2, int(alpha.get())))
            self.dirty3d = True
            canvas_l.unbind_all("<MouseWheel>")
            win.destroy()

        def flip_uv():
            uu, vv = u0.get(), u1.get()
            u0.set(vv)
            u1.set(uu)
            xx, yy = x0.get(), x1.get()
            x0.set(-yy)
            x1.set(-xx)

        def cancel():
            canvas_l.unbind_all("<MouseWheel>")
            win.destroy()

        def on_esc(_e):
            if pick[0]:
                set_pick(0)
                return "break"
            cancel()

        win.bind("<Escape>", on_esc)
        win.protocol("WM_DELETE_WINDOW", cancel)
        tk.Button(btns, text="Flip (espeja U y X)", command=flip_uv).pack(fill="x", pady=2)
        tk.Button(btns, text="Ok", command=ok).pack(fill="x", pady=2)
        tk.Button(btns, text="Abbrechen", command=cancel).pack(fill="x")
        win.wait_window()

    def path_dialog(self) -> None:
        if self.tk is None:
            return
        if not self.d.paths:
            self.path_add()
        p = self.d.paths[min(self.sel_path, len(self.d.paths) - 1)]
        win = tk.Toplevel()
        win.title("Path konfigurieren")
        win.geometry("520x420")
        frm = tk.Frame(win, padx=12, pady=10)
        frm.pack(fill="both", expand=True)
        tk.Label(
            frm,
            text="El [path] es el carril que usa el tráfico AI / el jugador sobre esta spline.\n"
            "En el visor 2D es la línea verde: arrastrala para cambiar lateral y altura.",
            justify="left",
            fg="#334155",
        ).pack(anchor="w", pady=(0, 8))
        typ = tk.IntVar(value=p.typ)
        lat = tk.DoubleVar(value=p.lateral)
        ht = tk.DoubleVar(value=p.height)
        w = tk.DoubleVar(value=p.width)
        direc = tk.IntVar(value=p.direction)
        sec = tk.BooleanVar(value=p.secondary)

        def field(title, var, hint):
            tk.Label(frm, text=title, font=("Segoe UI", 9, "bold")).pack(anchor="w")
            row = tk.Frame(frm)
            row.pack(fill="x", pady=(0, 6))
            tk.Entry(row, textvariable=var, width=12).pack(side="left")
            tk.Label(row, text=hint, fg="#475569", wraplength=360, justify="left").pack(side="left", padx=8)

        field("typ", typ, "0 = carril (coches/buses) · 1 = peatón · 2 = riel (tranvía).")
        field("lateral (m)", lat, "Desplazamiento X del centro del carril. 0 = eje de la spline. También se arrastra en 2D.")
        field("height (m)", ht, "Altura Z del path sobre el perfil. Suele coincidir con la calzada (p. ej. 0.10).")
        field("width (m)", w, "Ancho del carril. En 2D es el largo de la línea verde (de −width/2 a +width/2).")
        field("direction", direc, "Sentido OMSI: 0 / 1 un sentido, 2 ambos (típico en calles).")
        tk.Checkbutton(
            frm,
            text="[path_2]  — segundo path del mismo .sli (p. ej. carril contrario o vereda)",
            variable=sec,
        ).pack(anchor="w", pady=4)

        def ok():
            self.push_undo()
            p.typ, p.lateral, p.height, p.width, p.direction = (
                typ.get(),
                lat.get(),
                ht.get(),
                w.get(),
                direc.get(),
            )
            p.secondary = sec.get()
            win.destroy()

        tk.Button(frm, text="Ok", command=ok).pack(side="left", padx=(0, 8), pady=12)
        tk.Button(frm, text="Abbrechen", command=win.destroy).pack(side="left")
        win.wait_window()

    def rename_profile(self) -> None:
        if self.tk is None or not self.d.profiles:
            return
        cur = self.d.profile_comments[self.sel_profile] if self.sel_profile < len(self.d.profile_comments) else ""
        n = simpledialog.askstring("Renombrar", "Nombre del perfil", initialvalue=cur)
        if n is not None:
            while len(self.d.profile_comments) <= self.sel_profile:
                self.d.profile_comments.append("")
            self.d.profile_comments[self.sel_profile] = n

    def start_grab(self) -> None:
        if not self.sel_pts and self.d.profiles:
            self.select_whole_profile(self.sel_profile)
        if not self.sel_pts and not self.sel_multi:
            return
        self.drag_group.clear()
        seeds = list(self.sel_multi) if self.sel_multi else [(self.sel_profile, i) for i in self.sel_pts]
        seen = set()
        for pi, i in seeds:
            if not (0 <= pi < len(self.d.profiles)):
                continue
            strip = self.d.profiles[pi]
            if not (0 <= i < len(strip)):
                continue
            px, pz = strip[i].x, strip[i].z
            for pj, st in enumerate(self.d.profiles):
                for k, pt in enumerate(st):
                    if abs(pt.x - px) < 1e-3 and abs(pt.z - pz) < 1e-3:
                        key = (pj, k)
                        if key not in seen:
                            seen.add(key)
                            self.drag_group.append((pj, k, pt.x, pt.z))
        if not self.drag_group:
            return
        self.grab = True
        self.grab_origin = False
        self.grab_ax = self.grab_az = 0.0

    def cmd(self, c: str) -> None:
        if c == "espejar":
            self.espejar()
        elif c == "flip":
            self.flip()
        elif c == "delPt":
            self.del_pt()
        elif c == "pencil":
            self.pencil = not self.pencil
            if self.pencil:
                self.marquee = False
                self.path_place = False
        elif c == "marquee":
            self.marquee = not self.marquee
            if self.marquee:
                self.pencil = False
                self.path_place = False
        elif c == "drawTex":
            self.draw_tex = not self.draw_tex
        elif c == "drawPath":
            self.draw_path = not self.draw_path
        elif c == "centerView":
            self.center_view()
        elif c == "snap":
            self.snap_i = (self.snap_i + 1) % len(SNAP_M)
        elif c == "wrapDel":
            self.del_pt()
        elif c == "wrapEdit":
            self.rename_profile()
        elif c == "wrapClone":
            self.espejar()
        elif c in ("nudgeL", "nudgeR", "nudgeU", "nudgeD"):
            self.nudge_once(c, new_stroke=True)
        elif c == "wrapAdd":
            self.wrap_add()
        elif c == "wrapOpen":
            self.wrap_dialog()
        elif c == "selProfAll":
            if len(self.sel_profiles) == len(self.d.profiles) and self.d.profiles:
                # Toggle off → keep only active
                self.select_whole_profile(self.sel_profile, additive=False)
            else:
                self.sel_profiles = set(range(len(self.d.profiles)))
                self.sel_multi = []
                for p in sorted(self.sel_profiles):
                    for i in range(len(self.d.profiles[p])):
                        self.sel_multi.append((p, i))
                if self.d.profiles:
                    self.sel_profile = 0
                    self.sel_pts = list(range(len(self.d.profiles[0])))
        elif c == "applyTex":
            self.apply_tex()
        elif c == "texDel":
            self.tex_del()
        elif c == "texAdd":
            self.tex_add()
        elif c == "pathDel":
            self.path_del()
        elif c == "pathAdd":
            self.path_add()
        elif c == "pathCreate":
            self.pencil = False
            self.marquee = False
            self.path_place = True
        elif c == "pathConfig":
            self.path_dialog()
        elif c == "openSli":
            self.open_sli()
        elif c == "save":
            self.save_sli()
        elif c == "cancel":
            pygame.event.post(pygame.event.Event(pygame.QUIT))
        elif c == "copyTex":
            self.copy_tex = not self.copy_tex

    def hit_pt(self, canv, mx, my):
        best, who = float(HIT_PT), None
        for pi, strip in enumerate(self.d.profiles):
            for i, p in enumerate(strip):
                sx, sy = self.view.w2s(canv, p.x, p.z)
                d = math.hypot(sx - mx, sy - my)
                if d < best:
                    best, who = d, (pi, i)
        return who

    def hit_profile(self, canv, mx, my):
        thr2 = float(HIT_SEG * HIT_SEG)
        best_pi, best_d = None, thr2
        for pi, strip in enumerate(self.d.profiles):
            pts = [self.view.w2s(canv, p.x, p.z) for p in strip]
            for i in range(1, len(pts)):
                d = dist_sq_seg(mx, my, pts[i - 1][0], pts[i - 1][1], pts[i][0], pts[i][1])
                if d <= best_d:
                    best_d, best_pi = d, pi
        return best_pi

    def hit_path(self, canv, mx, my):
        if not self.draw_path:
            return None
        thr2 = 20.0 * 20.0
        best, best_d = None, thr2
        for i, p in enumerate(self.d.paths):
            a = self.view.w2s(canv, p.lateral - p.width * 0.5, p.height)
            b = self.view.w2s(canv, p.lateral + p.width * 0.5, p.height)
            d = dist_sq_seg(mx, my, a[0], a[1], b[0], b[1])
            if d <= best_d:
                best_d, best = d, i
        return best

    def draw_paths_overlay(self, canv) -> None:
        if not self.draw_path:
            return
        for i, p in enumerate(self.d.paths):
            a = self.view.w2s(canv, p.lateral - p.width * 0.5, p.height)
            b = self.view.w2s(canv, p.lateral + p.width * 0.5, p.height)
            sel = i == self.sel_path
            pygame.draw.line(self.screen, (10, 40, 20), a, b, 8 if sel else 6)
            pygame.draw.line(self.screen, (52, 211, 153) if sel else (16, 185, 129), a, b, 5 if sel else 3)
            pygame.draw.circle(self.screen, (6, 95, 70), a, 6)
            pygame.draw.circle(self.screen, (6, 95, 70), b, 6)
            pygame.draw.circle(self.screen, (167, 243, 208) if sel else (52, 211, 153), a, 4)
            pygame.draw.circle(self.screen, (167, 243, 208) if sel else (52, 211, 153), b, 4)
            mid = ((a[0] + b[0]) // 2, (a[1] + b[1]) // 2 - 12)
            lab = self.font_s.render(f"path {i+1}", True, (6, 78, 59))
            self.screen.blit(lab, lab.get_rect(center=mid))

    def finish_marquee(self, canv) -> None:
        minx, maxx = min(self.mx0, self.mx1), max(self.mx0, self.mx1)
        miny, maxy = min(self.my0, self.my1), max(self.my0, self.my1)
        self.sel_multi.clear()
        self.sel_pts.clear()
        if maxx - minx < 6 and maxy - miny < 6:
            cx, cy = (minx + maxx) // 2, (miny + maxy) // 2
            ph = self.hit_path(canv, cx, cy)
            if ph is not None:
                self.sel_path = ph
                return
            hit = self.hit_pt(canv, cx, cy)
            if hit:
                self.sel_profile, i = hit
                self.sel_pts = [i]
                self.sel_multi = [hit]
            else:
                pi = self.hit_profile(canv, cx, cy)
                if pi is not None:
                    self.select_whole_profile(pi)
            return
        for pi, strip in enumerate(self.d.profiles):
            for i, p in enumerate(strip):
                sx, sy = self.view.w2s(canv, p.x, p.z)
                if minx <= sx <= maxx and miny <= sy <= maxy:
                    self.sel_multi.append((pi, i))
                    if pi == self.sel_profile:
                        self.sel_pts.append(i)
        if self.sel_multi:
            self.sel_profile = self.sel_multi[0][0]
            self.sel_pts = [i for pi, i in self.sel_multi if pi == self.sel_profile]

    def add_btn(self, r, cmd, extra="", tip=""):
        self.buttons.append((r, cmd, extra, tip))

    def draw_btn(self, r: pygame.Rect, label: str, on=False, color=BTN, icon=None):
        pygame.draw.rect(self.screen, ACCENT if on else color, r, border_radius=4)
        pygame.draw.rect(self.screen, (148, 163, 184), r, 1, border_radius=4)
        if icon and icon in self.icons:
            ic = self.icons[icon]
            self.screen.blit(ic, ic.get_rect(center=r.center))
        elif label:
            t = self.font_s.render(label, True, WHITE)
            self.screen.blit(t, t.get_rect(center=r.center))


    def draw_side_panel(self, side: pygame.Rect) -> None:
        pygame.draw.rect(self.screen, SLATE2, side)
        footer_top = side.bottom - SIDE_FOOTER_H
        area_top = side.y + 8
        area_h = max(0, footer_top - area_top)
        old_clip = self.screen.get_clip()
        self.screen.set_clip(pygame.Rect(side.x, area_top, side.w, area_h))

        def sy(vy: int) -> int:
            return area_top + vy - self.side_scroll

        vy = 0
        self.screen.blit(self.font.render("Perfiles (texture wrapping)", True, TXT), (side.x + 8, sy(vy)))
        vy += 22
        for i, strip in enumerate(self.d.profiles):
            geo = self.profile_geo_label(strip)
            miss = "" if self.profile_has_tex(strip) else "  · sin textura"
            multi = i in self.sel_profiles
            active = i == self.sel_profile
            bg = (14, 116, 144) if active else ((30, 64, 175) if multi else BTN)
            r = pygame.Rect(side.x + 8, sy(vy), side.w - 16, 34)
            pygame.draw.rect(self.screen, bg, r)
            if multi:
                pygame.draw.rect(self.screen, (96, 165, 250), r, 2)
            mark = "✓ " if multi else ""
            self.screen.blit(self.font_s.render(f"{mark}P{i+1}{miss}", True, WHITE), (r.x + 4, r.y + 2))
            self.screen.blit(self.font_s.render(geo, True, WHITE), (r.x + 4, r.y + 16))
            self.add_btn(
                r,
                "selProf",
                str(i),
                tip="Clic: seleccionar | Ctrl+clic: multi-perfil | Doble clic: Texture-Wrapper",
            )
            vy += 36
        vy += 4
        n_multi = len(self.sel_profiles)
        r_multi = pygame.Rect(side.x + 8, sy(vy), side.w - 16, 24)
        self.draw_btn(
            r_multi,
            f"Multi perfiles ({n_multi})" if n_multi else "Multi perfiles: off",
            on=n_multi > 1,
        )
        self.add_btn(
            r_multi,
            "selProfAll",
            tip="Seleccionar TODOS los perfiles (luego Apply tex divide U 0→1 entre ellos)",
        )
        vy += 28
        for i, (lab, cmd, tip) in enumerate([
            ("Borrar", "wrapDel", "Borrar perfil o nodos"),
            ("Renombrar", "wrapEdit", "Nombre del perfil"),
            ("Agregar", "wrapAdd", "Agregar un perfil de 3 m"),
        ]):
            r = pygame.Rect(side.x + 8 + i * 94, sy(vy), 90, 24)
            self.draw_btn(r, lab)
            self.add_btn(r, cmd, tip=tip)
        vy += 30
        r_clone = pygame.Rect(side.x + 8, sy(vy), side.w - 16, 24)
        self.draw_btn(r_clone, "Clonar al otro lado")
        self.add_btn(r_clone, "wrapClone", tip="Espejar el perfil seleccionado a X negativo (como OMSI)")
        vy += 30
        r1 = pygame.Rect(side.x + 8, sy(vy), 140, 26)
        r2 = pygame.Rect(side.x + 152, sy(vy), 140, 26)
        self.draw_btn(r1, "Textur-Wrapper")
        self.draw_btn(r2, "Aplicar textura", color=(37, 99, 235))
        self.add_btn(r1, "wrapOpen", tip="UV, nombre y recorte de textura del perfil")
        self.add_btn(r2, "applyTex", tip="Poner la textura seleccionada en el perfil activo")
        vy += 36
        self.screen.blit(self.font.render("Texturas", True, TXT), (side.x + 8, sy(vy)))
        vy += 20
        for i, name in enumerate(self.d.textures):
            r = pygame.Rect(side.x + 8, sy(vy), side.w - 16, 60)
            pygame.draw.rect(self.screen, (14, 116, 144) if i == self.sel_tex and self.sel_tex >= 0 else (30, 41, 59), r, 1)
            if name in self.thumbs:
                self.screen.blit(self.thumbs[name], (r.x + 4, r.y + 2))
            else:
                pygame.draw.rect(self.screen, (51, 65, 85), (r.x + 4, r.y + 2, 56, 56))
            self.screen.blit(self.font_s.render(name[:28], True, WHITE), (r.x + 66, sy(vy) + 20))
            self.add_btn(r, "selTex", str(i), tip="Seleccionar textura (clic aplica al perfil activo)")
            vy += 62
        vy += 4
        for i, (lab, cmd, tip) in enumerate([
            ("Borrar", "texDel", "Quitar textura de la lista"),
            ("Pegar", "texAdd", "Elegir archivo de textura"),
            ("Agregar", "texAdd", "Elegir archivo de textura"),
        ]):
            r = pygame.Rect(side.x + 8 + i * 94, sy(vy), 90, 24)
            self.draw_btn(r, lab)
            self.add_btn(r, cmd, tip=tip)
        vy += 36
        self.screen.blit(self.font.render("Path", True, TXT), (side.x + 8, sy(vy)))
        vy += 20
        for i, pth in enumerate(self.d.paths):
            r = pygame.Rect(side.x + 8, sy(vy), side.w - 16, 20)
            pygame.draw.rect(self.screen, (14, 116, 144) if i == self.sel_path else BTN, r)
            self.screen.blit(
                self.font_s.render(
                    f"typ={pth.typ} lat={pth.lateral:.2f} h={pth.height:.2f} w={pth.width:.2f}", True, WHITE
                ),
                (r.x + 4, r.y + 2),
            )
            self.add_btn(r, "selPath", str(i), tip="Seleccionar path (doble clic: configurar)")
            vy += 22
        vy += 6
        for i, (lab, cmd, tip) in enumerate([
            ("Borrar", "pathDel", "Borrar path"),
            ("Crear en 2D", "pathCreate", "Clic en el visor 2D para colocar un path"),
            ("Agregar", "pathAdd", "Agregar path con valores por defecto"),
        ]):
            r = pygame.Rect(side.x + 8 + i * 94, sy(vy), 90, 24)
            self.draw_btn(r, lab)
            self.add_btn(r, cmd, tip=tip)
        vy += 30
        r = pygame.Rect(side.x + 8, sy(vy), side.w - 16, 24)
        self.draw_btn(r, "Configurar path…")
        self.add_btn(r, "pathConfig", tip="Editar typ / lateral / alto / ancho")
        vy += 36

        self.side_max_scroll = max(0, vy - area_h)
        self.side_scroll = max(0, min(self.side_scroll, self.side_max_scroll))
        self.screen.set_clip(old_clip)

        fy = footer_top + 4
        for lab, cmd, col, tip in [
            ("abrir .sli", "openSli", YELLOW, "Abrir otro .sli"),
            ("guardar .sli", "save", GREEN, "Guardar (Ctrl+S)"),
            ("cancelar", "cancel", RED, "Cerrar sin guardar"),
        ]:
            r = pygame.Rect(side.x + 8, fy, side.w - 16, 32)
            self.draw_btn(r, lab, color=col)
            self.add_btn(r, cmd, tip=tip)
            fy += 38
        r = pygame.Rect(side.x + 8, fy, 22, 22)
        pygame.draw.rect(self.screen, GREEN if self.copy_tex else BTN, r)
        self.add_btn(r, "copyTex", tip="Copiar BMP/PNG junto al .sli al guardar")
        self.screen.blit(self.font_s.render("Copiar texturas al exportar", True, TXT), (r.x + 28, r.y + 2))

    def draw_tex_on_strip(self, canv, strip) -> None:
        if len(strip) < 2:
            return
        a = self.view.w2s(canv, strip[0].x, strip[0].z)
        b = self.view.w2s(canv, strip[-1].x, strip[-1].z)
        mx, my = (a[0] + b[0]) // 2, (a[1] + b[1]) // 2
        if self.draw_tex and self.profile_has_tex(strip):
            name = self.d.textures[strip[0].material_id]
            if name in self.thumbs:
                th = pygame.transform.smoothscale(self.thumbs[name], (28, 28))
                self.screen.blit(th, (mx - 14, my - 36))
        elif not self.profile_has_tex(strip):
            t = self.font_s.render("sin textura", True, (180, 40, 40))
            self.screen.blit(t, (mx - t.get_width() // 2, my - 28))

    def draw(self) -> None:
        self.screen.fill(SLATE)
        canv, prev, side, chrome = self.layout()
        self.buttons.clear()
        pygame.draw.rect(self.screen, WHITE, canv)
        pygame.draw.rect(self.screen, (100, 116, 139), canv, 1)
        self.screen.set_clip(canv)
        gm = self.grid_m()
        if gm > 0 and gm * self.view.scale > 8:
            wx0, wz0 = self.view.s2w(canv, canv.left, canv.bottom)
            wx1, wz1 = self.view.s2w(canv, canv.right, canv.top)
            x0, x1 = min(wx0, wx1), max(wx0, wx1)
            z0, z1 = min(wz0, wz1), max(wz0, wz1)
            ix = math.floor(x0 / gm)
            while ix * gm <= x1:
                sx, _ = self.view.w2s(canv, ix * gm, 0)
                pygame.draw.line(self.screen, (230, 230, 230), (sx, canv.top), (sx, canv.bottom))
                ix += 1
            iz = math.floor(z0 / gm)
            while iz * gm <= z1:
                _, sy = self.view.w2s(canv, 0, iz * gm)
                pygame.draw.line(self.screen, (230, 230, 230), (canv.left, sy), (canv.right, sy))
                iz += 1
        ax, ay = self.view.w2s(canv, 0, 0)
        pygame.draw.line(self.screen, (140, 140, 140), (canv.left, ay), (canv.right, ay))
        pygame.draw.line(self.screen, (140, 140, 140), (ax, canv.top), (ax, canv.bottom))
        for pi, strip in enumerate(self.d.profiles):
            multi = pi in self.sel_profiles
            col = (30, 90, 180) if pi == self.sel_profile else ((37, 99, 235) if multi else (160, 180, 210))
            pts = [self.view.w2s(canv, p.x, p.z) for p in strip]
            if len(pts) >= 2:
                pygame.draw.lines(self.screen, col, False, pts, 5 if (pi == self.sel_profile or multi) else 3)
            self.draw_tex_on_strip(canv, strip)
            if pi == self.sel_profile and len(strip) >= 2:
                geo = self.profile_geo_label(strip)
                gs = self.font_s.render(geo, True, (15, 23, 42))
                gx = (pts[0][0] + pts[-1][0]) // 2 - gs.get_width() // 2
                gy = min(p[1] for p in pts) - 18
                bg = pygame.Rect(gx - 4, gy - 2, gs.get_width() + 8, gs.get_height() + 4)
                pygame.draw.rect(self.screen, (255, 251, 235), bg)
                self.screen.blit(gs, (gx, gy))
            for i, pt in enumerate(pts):
                c = (40, 160, 60) if (pi, i) in self.sel_multi or (pi == self.sel_profile and i in self.sel_pts) else (
                    (200, 60, 40) if pi == self.sel_profile else (190, 190, 200)
                )
                pygame.draw.circle(self.screen, c, pt, 7)
        # Ghost of profiles being placed (Ctrl+V → follow mouse)
        if self.paste_place and self.paste_ghost:
            mx, my = pygame.mouse.get_pos()
            if canv.collidepoint((mx, my)):
                wx, wz = self.view.s2w(canv, mx, my)
                for strip in self._paste_ghost_at(wx, wz):
                    pts = [self.view.w2s(canv, p.x, p.z) for p in strip]
                    if len(pts) >= 2:
                        pygame.draw.lines(self.screen, (250, 204, 21), False, pts, 3)
                    for pt in pts:
                        pygame.draw.circle(self.screen, (250, 204, 21), pt, 6)
        if self.pencil_drag:
            pygame.draw.line(
                self.screen,
                (220, 80, 40),
                self.view.w2s(canv, self.px0, self.pz0),
                self.view.w2s(canv, self.px1, self.pz1),
                2,
            )
        if self.marquee_drag:
            r = pygame.Rect(min(self.mx0, self.mx1), min(self.my0, self.my1), abs(self.mx1 - self.mx0), abs(self.my1 - self.my0))
            pygame.draw.rect(self.screen, (60, 120, 200), r, 1)
        self.draw_paths_overlay(canv)
        if canv.collidepoint(self.last_mouse):
            wx, wz = self.view.s2w(canv, self.last_mouse[0], self.last_mouse[1])
            lines = [f"mouse  X {wx:+.3f} m   Z {wz:+.3f} m"]
            if self.d.profiles and 0 <= self.sel_profile < len(self.d.profiles):
                strip = self.d.profiles[self.sel_profile]
                extra = "" if self.profile_has_tex(strip) else "  sin textura"
                lines.append(f"sel P{self.sel_profile+1}  {self.profile_geo_label(strip)}{extra}")
            hover = self.hit_pt(canv, *self.last_mouse)
            if hover:
                pi, i = hover
                pt = self.d.profiles[pi][i]
                lines.append(f"nodo bajo cursor  perfil {pi+1}  #{i+1}  X {pt.x:+.3f}  Z {pt.z:+.3f}")
            elif self.d.profiles and 0 <= self.sel_profile < len(self.d.profiles):
                strip = self.d.profiles[self.sel_profile]
                if self.sel_pts and 0 <= self.sel_pts[0] < len(strip):
                    pt = strip[self.sel_pts[0]]
                    lines.append(f"nodo sel  #{self.sel_pts[0]+1}  X {pt.x:+.3f}  Z {pt.z:+.3f}")
            ph = self.hit_path(canv, *self.last_mouse)
            if ph is not None:
                p = self.d.paths[ph]
                lines.append(f"path {ph+1}  lat {p.lateral:+.3f}  h {p.height:+.3f}  w {p.width:.3f}")
            yhud = canv.y + 8
            for t in lines:
                s = self.font_s.render(t, True, (15, 23, 42))
                bg = pygame.Rect(canv.x + 8, yhud, s.get_width() + 10, s.get_height() + 4)
                pygame.draw.rect(self.screen, (254, 240, 138), bg, border_radius=3)
                self.screen.blit(s, (bg.x + 5, bg.y + 2))
                yhud += bg.h + 2
        self.screen.set_clip(None)
        if self.path_place:
            hint = "COLOCAR PATH: clic en el visor 2D  |  Esc"
        elif self.path_drag:
            hint = "PATH: arrastra para mover lateral/altura  |  suelta para fijar"
        elif self.grab:
            hint = "G: mover seleccion  |  clic confirma  |  Esc cancela"
        elif self.pencil:
            hint = "LAPIZ: arrastra para crear perfil  |  Esc"
        elif self.marquee:
            hint = "SELECCION CAJA (A)  |  G mover  |  Esc"
        else:
            hint = "Path verde encima de todo  |  arrastralo para mover  |  clic perfil  |  G"
        self.screen.blit(self.font_s.render(hint, True, TXT), (canv.x + 6, canv.bottom + 4))

        pygame.draw.rect(self.screen, (41, 43, 51), prev)
        if self.dirty3d or self.preview_surf is None or self.preview_surf.get_size() != (prev.w, prev.h):
            self.rebuild_3d(prev)
        if self.preview_surf:
            self.screen.blit(self.preview_surf, prev)
        else:
            msg = self.font.render("Visor 3D: py -3 -m pip install moderngl pygame Pillow", True, (248, 113, 113))
            self.screen.blit(msg, msg.get_rect(center=prev.center))
        pygame.draw.rect(self.screen, (100, 116, 139), prev, 1)
        self.screen.blit(self.font_s.render("Visor 3D (Luna Z-up, orbitar / rueda)", True, TXT), (prev.x + 8, prev.y + 6))

        pygame.draw.rect(self.screen, SLATE2, chrome)
        y = chrome.y + 8
        x = chrome.x + 8
        snap_lab = ["0.01 m", "0.05 m", "0.1 m", "0.5 m", "Libre"][self.snap_i]
        r = pygame.Rect(x, y, 130, 32)
        self.draw_btn(r, f"Snap: {snap_lab}")
        self.add_btn(r, "snap", tip="Paso de snap al mover nodos (ciclo 0.01 / 0.05 / 0.1 / 0.5 / libre)")
        x += 136
        r = pygame.Rect(x, y, 110, 32)
        self.draw_btn(r, "Centrar vista")
        self.add_btn(r, "centerView", tip="Encuadrar todos los perfiles en el visor 2D")
        x += 118
        tools = [
            ("espejar", False, "Espejar perfiles a X negativo (como OMSI)"),
            ("flip", False, "Invertir orden de nodos del perfil seleccionado"),
            ("delPt", False, "Borrar nodos seleccionados, o el perfil si estan todos"),
            ("pencil", self.pencil, "Lápiz: arrastrar en 2D para dibujar un perfil nuevo"),
            ("marquee", self.marquee, "Selección caja (A): arrastrar para elegir nodos"),
            ("drawTex", self.draw_tex, "Mostrar miniatura de textura sobre cada perfil"),
            ("drawPath", self.draw_path, "Mostrar paths (línea verde) en el visor 2D"),
        ]
        for cmd, on, tip in tools:
            r = pygame.Rect(x, y, 32, 32)
            self.draw_btn(r, "", on=on, icon=cmd)
            self.add_btn(r, cmd, tip=tip)
            x += 36
        ny = y + 36
        for i, (cmd, tip) in enumerate([
            ("nudgeL", "Mover selección a la izquierda (snap; mantener = repetir)"),
            ("nudgeR", "Mover selección a la derecha"),
            ("nudgeU", "Mover selección arriba (+Z)"),
            ("nudgeD", "Mover selección abajo (-Z)"),
        ]):
            r = pygame.Rect(chrome.x + 8 + i * 36, ny, 32, 28)
            self.draw_btn(r, "")
            cx, cy = r.centerx, r.centery
            col = WHITE
            if cmd == "nudgeL":
                pygame.draw.polygon(self.screen, col, [(cx + 7, cy - 8), (cx - 8, cy), (cx + 7, cy + 8)])
            elif cmd == "nudgeR":
                pygame.draw.polygon(self.screen, col, [(cx - 7, cy - 8), (cx + 8, cy), (cx - 7, cy + 8)])
            elif cmd == "nudgeU":
                pygame.draw.polygon(self.screen, col, [(cx - 8, cy + 7), (cx, cy - 8), (cx + 8, cy + 7)])
            else:
                pygame.draw.polygon(self.screen, col, [(cx - 8, cy - 7), (cx, cy + 8), (cx + 8, cy - 7)])
            self.add_btn(r, cmd, tip=tip)

        sli = pygame.Rect(chrome.x + 8, ny + 34, chrome.w - 16, chrome.h - ny - 42)
        pygame.draw.rect(self.screen, (15, 23, 42), sli)
        lines = format_sli(self.d).splitlines()[:16]
        for i, line in enumerate(lines):
            self.screen.blit(self.font_s.render(line[:90], True, (148, 163, 184)), (sli.x + 6, sli.y + 4 + i * 14))

        self.draw_side_panel(side)

        if self.hover_tip:
            mx, my = self.last_mouse
            tip_s = self.font_s.render(self.hover_tip, True, (15, 23, 42))
            pad = pygame.Rect(mx + 14, my + 16, tip_s.get_width() + 10, tip_s.get_height() + 8)
            if pad.right > self.screen.get_width():
                pad.x = mx - pad.w - 8
            if pad.bottom > self.screen.get_height():
                pad.y = my - pad.h - 8
            pygame.draw.rect(self.screen, (253, 224, 71), pad, border_radius=4)
            self.screen.blit(tip_s, (pad.x + 5, pad.y + 4))

    def update_hover(self, pos) -> None:
        self.hover_tip = ""
        for r, _cmd, _extra, tip in self.buttons:
            if tip and r.collidepoint(pos):
                self.hover_tip = tip
                return

    def handle_click(self, pos, canv, prev) -> None:
        mx, my = pos
        if self.grab and canv.collidepoint(pos):
            self.grab = False
            return
        # Confirm paste placement at mouse
        if self.paste_place and canv.collidepoint(pos):
            wx, wz = self.view.s2w(canv, mx, my)
            self._commit_paste_at(wx, wz)
            return
        for r, cmd, extra, _tip in self.buttons:
            if r.collidepoint(pos):
                if cmd == "selProf":
                    mods = pygame.key.get_mods()
                    additive = bool(mods & pygame.KMOD_CTRL)
                    self.select_whole_profile(int(extra), additive=additive)
                    return
                if cmd == "selTex":
                    self.sel_tex = int(extra)
                    self.apply_tex()
                    return
                if cmd == "selPath":
                    self.sel_path = int(extra)
                    return
                if cmd.startswith("nudge"):
                    self.nudge_once(cmd, new_stroke=True)
                    self.nudge_active = cmd
                    self.nudge_next_ms = pygame.time.get_ticks() + 350
                else:
                    self.cmd(cmd)
                return
        if canv.collidepoint(pos):
            if self.path_place:
                wx, wz = self.view.s2w(canv, mx, my)
                self.push_undo()
                w = self.d.paths[self.sel_path].width if self.d.paths else 3.0
                self.d.paths.append(SliPath(1, self.snap(wx), self.snap(wz), w, 2))
                self.sel_path = len(self.d.paths) - 1
                self.path_place = False
                return
            if self.marquee:
                self.marquee_drag = True
                self.mx0 = self.mx1 = mx
                self.my0 = self.my1 = my
                return
            if self.pencil:
                hit = self.hit_pt(canv, mx, my)
                if hit:
                    self.sel_profile, i = hit
                    p = self.d.profiles[self.sel_profile][i]
                    self.px0, self.pz0 = p.x, p.z
                else:
                    wx, wz = self.view.s2w(canv, mx, my)
                    self.px0, self.pz0 = self.snap_pencil(wx), self.snap_pencil(wz)
                self.px1, self.pz1 = self.px0, self.pz0
                self.pencil_drag = True
                return
            ph = self.hit_path(canv, mx, my)
            if ph is not None:
                self.sel_path = ph
                p = self.d.paths[ph]
                self.path_drag = True
                self.push_undo()
                self.path_ox, self.path_oz = p.lateral, p.height
                self.path_wx, self.path_wz = self.view.s2w(canv, mx, my)
                return
            hit = self.hit_pt(canv, mx, my)
            mods = pygame.key.get_mods()
            if hit:
                pi, i = hit
                self.sel_profile = pi
                if mods & pygame.KMOD_CTRL:
                    if (pi, i) not in self.sel_multi:
                        self.sel_multi.append((pi, i))
                    if i not in self.sel_pts:
                        self.sel_pts.append(i)
                else:
                    self.sel_pts = [i]
                    self.sel_multi = [(pi, i)]
                return
            pi = self.hit_profile(canv, mx, my)
            if pi is not None:
                self.select_whole_profile(pi)
            return
        if prev.collidepoint(pos):
            self.orbit = True

    def handle_dblclick(self, pos) -> None:
        for r, cmd, extra, _tip in self.buttons:
            if r.collidepoint(pos):
                if cmd == "selProf":
                    self.select_whole_profile(int(extra))
                    self.wrap_dialog()
                elif cmd == "selPath":
                    self.sel_path = int(extra)
                    self.path_dialog()
                return

    def run(self) -> int:
        canv, prev, *_ = self.layout()
        running = True
        while running:
            canv, prev, *_ = self.layout()
            for e in pygame.event.get():
                if e.type == pygame.QUIT:
                    running = False
                elif e.type == pygame.KEYDOWN:
                    if e.key == pygame.K_ESCAPE:
                        if self.grab or self.path_drag:
                            self.do_undo()
                        self.pencil = self.marquee = self.grab = self.pencil_drag = self.marquee_drag = self.path_place = self.path_drag = False
                    elif e.key == pygame.K_a:
                        self.cmd("marquee")
                    elif e.key == pygame.K_g:
                        self.start_grab()
                    elif e.key == pygame.K_z and (e.mod & pygame.KMOD_CTRL):
                        self.do_undo()
                    elif e.key == pygame.K_y and (e.mod & pygame.KMOD_CTRL):
                        self.do_redo()
                    elif e.key == pygame.K_s and (e.mod & pygame.KMOD_CTRL):
                        self.save_sli()
                    elif e.key == pygame.K_c and (e.mod & pygame.KMOD_CTRL):
                        self.clipboard_profiles = []
                        for pi in self.mirror_targets():
                            if 0 <= pi < len(self.d.profiles):
                                self.clipboard_profiles.append(copy.deepcopy(self.d.profiles[pi]))
                    elif e.key == pygame.K_v and (e.mod & pygame.KMOD_CTRL):
                        if self.clipboard_profiles:
                            # Enter placement mode: ghost follows mouse until LMB
                            self.paste_ghost = [copy.deepcopy(s) for s in self.clipboard_profiles]
                            # Anchor = centroid of first strip
                            xs = [p.x for s in self.paste_ghost for p in s]
                            self.paste_anchor_x = sum(xs) / len(xs) if xs else 0.0
                            zs = [p.z for s in self.paste_ghost for p in s]
                            self.paste_anchor_z = sum(zs) / len(zs) if zs else 0.0
                            self.paste_place = True
                            self.pencil = self.marquee = self.path_place = False
                    elif e.key == pygame.K_ESCAPE and self.paste_place:
                        self.paste_place = False
                        self.paste_ghost = []
                elif e.type == pygame.MOUSEWHEEL:
                    _, _, side_r, _ = self.layout()
                    if side_r.collidepoint(pygame.mouse.get_pos()):
                        self.side_scroll = max(0, min(self.side_max_scroll, self.side_scroll - e.y * 28))
                elif e.type == pygame.MOUSEBUTTONDOWN:
                    if e.button == 1:
                        if e.dict.get("clicks", 1) >= 2:
                            self.handle_dblclick(e.pos)
                        else:
                            self.handle_click(e.pos, canv, prev)
                    elif e.button == 2:
                        self.panning = canv.collidepoint(e.pos)
                    elif e.button == 4:
                        if canv.collidepoint(e.pos):
                            self.view.scale *= 1.12
                        if prev.collidepoint(e.pos):
                            self.cam.radius = max(4.0, self.cam.radius * 0.9)
                            self.dirty3d = True
                    elif e.button == 5:
                        if canv.collidepoint(e.pos):
                            self.view.scale /= 1.12
                        if prev.collidepoint(e.pos):
                            self.cam.radius = min(80.0, self.cam.radius * 1.1)
                            self.dirty3d = True
                elif e.type == pygame.MOUSEBUTTONUP:
                    if e.button == 1:
                        if self.pencil_drag:
                            self.pencil_drag = False
                            self.push_undo()
                            self.add_profile_strip(self.px0, self.pz0, self.px1, self.pz1)
                            self.dirty3d = True
                        if self.marquee_drag:
                            self.marquee_drag = False
                            self.finish_marquee(canv)
                        self.path_drag = False
                        self.orbit = False
                        self.nudge_active = None
                    if e.button == 2:
                        self.panning = False
                elif e.type == pygame.MOUSEMOTION:
                    self.last_mouse = e.pos
                    self.update_hover(e.pos)
                    dx, dy = e.rel
                    if self.panning:
                        self.view.pan_x -= dx / self.view.scale
                        self.view.pan_z += dy / self.view.scale
                    if self.orbit:
                        # Luna mirror_x: drag right should rotate the mesh to the right (no extra theta flip).
                        self.cam.theta += dx * 0.01
                        self.cam.phi = max(0.05, min(math.pi - 0.05, self.cam.phi - dy * 0.01))
                        self.dirty3d = True
                    if self.pencil_drag:
                        wx, wz = self.view.s2w(canv, e.pos[0], e.pos[1])
                        self.px1 = self.snap_pencil(wx)
                        self.pz1 = self.snap_pencil(wz)
                    if self.marquee_drag:
                        self.mx1, self.my1 = e.pos
                    if self.path_drag and canv.collidepoint(e.pos) and self.d.paths:
                        wx, wz = self.view.s2w(canv, e.pos[0], e.pos[1])
                        p = self.d.paths[min(self.sel_path, len(self.d.paths) - 1)]
                        p.lateral = self.snap(self.path_ox + (wx - self.path_wx))
                        p.height = self.snap(self.path_oz + (wz - self.path_wz))
                    if self.grab and canv.collidepoint(e.pos):
                        wx, wz = self.view.s2w(canv, e.pos[0], e.pos[1])
                        if not self.grab_origin:
                            self.push_undo()
                            self.grab_ax, self.grab_az = wx, wz
                            self.grab_origin = True
                        dxw, dzw = wx - self.grab_ax, wz - self.grab_az
                        for pi, i, x0, z0 in self.drag_group:
                            if 0 <= pi < len(self.d.profiles) and 0 <= i < len(self.d.profiles[pi]):
                                self.d.profiles[pi][i].x = self.snap(x0 + dxw)
                                self.d.profiles[pi][i].z = self.snap(z0 + dzw)
                        self.dirty3d = True
            self.tick_nudge_hold()
            self.draw()
            pygame.display.flip()
            self.clock.tick(60)
        pygame.quit()
        return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--omsi", type=Path, default=Path(r"F:\SteamLibrary\steamapps\common\OMSI 2"))
    ap.add_argument("--sli", "--open", dest="sli", type=Path, default=None)
    ap.add_argument("sli_pos", nargs="?", type=Path)
    ap.add_argument("--status-file", type=Path, default=None)
    args = ap.parse_args(argv)
    print("Spline creator Python. Deps: py -3 -m pip install pygame moderngl Pillow")
    return App(args.omsi, args.sli or args.sli_pos, args.status_file).run()


if __name__ == "__main__":
    raise SystemExit(main())
