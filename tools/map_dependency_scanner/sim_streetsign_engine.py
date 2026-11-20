#!/usr/bin/env python3
"""Simulate omsi-f4-engine load/spawn/raster for a .sco (StreetSign_1.sco).

Mirrors:
  sco_loader.cpp, x_mesh_loader.cpp, oft_font.cpp,
  map_editor_run_setup_01.inc.cpp (append_sco_meshes_at + apply_sco_script_visibility)
"""
from __future__ import annotations

import os
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


def ieq(a: str, b: str) -> bool:
    return a.lower() == b.lower()


def trim(s: str) -> str:
    return s.strip(" \t\r\n")


def texture_key(name: str) -> str:
    return name.lower()


def read_cfg_lines(path: Path) -> list[str]:
    raw = path.read_bytes()
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        text = raw.decode("utf-16")
    else:
        text = raw.decode("latin-1")
    return text.replace("\r\n", "\n").replace("\r", "\n").split("\n")


@dataclass
class Material:
    diffuse_texture: str = ""
    texture_key: str = ""
    mat_id: int = -1
    mesh_index: int = -1
    alpha_mode: int = 0
    use_text_texture: Optional[int] = None
    is_matl_change: bool = False


@dataclass
class TextTex:
    source_index: int = 0
    string_var: str = ""
    font_name: str = ""
    width: int = 128
    height: int = 128
    color_mode: int = 0
    r: int = 255
    g: int = 0
    b: int = 0


@dataclass
class MeshRef:
    mesh_file: Path = Path()
    is_o3d: bool = False


@dataclass
class Sco:
    sco_path: Path = Path()
    friendly_name: str = ""
    meshes: list[MeshRef] = field(default_factory=list)
    materials: list[Material] = field(default_factory=list)
    material_textures: list[str] = field(default_factory=list)
    text_textures: list[TextTex] = field(default_factory=list)
    only_editor_mesh: bool = False


def load_sco(sco_path: Path) -> Sco:
    lines = read_cfg_lines(sco_path)
    obj = Sco(sco_path=sco_path)
    base = sco_path.parent

    tag = ""
    for i, raw in enumerate(lines):
        if not raw:
            continue
        t = trim(raw)
        if ieq(t, "[friendlyname]") and i + 1 < len(lines):
            nxt = trim(lines[i + 1])
            if nxt:
                obj.friendly_name = nxt
        if ieq(t, "[onlyeditor]"):
            obj.only_editor_mesh = True

    # [mesh] files — sco_loader.cfg_section style: first non-empty after tag
    i = 0
    while i < len(lines):
        if trim(lines[i]).lower() == "[mesh]":
            j = i + 1
            while j < len(lines) and not trim(lines[j]):
                j += 1
            if j < len(lines) and not trim(lines[j]).startswith("["):
                rel = trim(lines[j])
                p = base / rel
                if not p.exists():
                    alt = base / "model" / rel
                    if alt.exists():
                        p = alt
                ext = p.suffix.lower()
                obj.meshes.append(MeshRef(mesh_file=p, is_o3d=ext == ".o3d"))
        i += 1

    tag = ""
    cur: Optional[Material] = None
    mesh_i = -1
    i = 0
    while i < len(lines):
        raw = lines[i]
        if not raw:
            i += 1
            continue
        if raw[0] == "[":
            line = trim(raw)
            if line.endswith("]"):
                tag = line
                if ieq(tag, "[mesh]"):
                    mesh_i += 1
            i += 1
            continue
        line = trim(raw)
        if not line:
            i += 1
            continue
        if ieq(tag, "[matl]") or ieq(tag, "[matl_change]"):
            mat = Material()
            mat.is_matl_change = ieq(tag, "[matl_change]")
            mat.diffuse_texture = line
            mat.texture_key = texture_key(line)
            mat.mesh_index = mesh_i
            if i + 1 < len(lines):
                id_line = trim(lines[i + 1])
                if id_line and not id_line.startswith("["):
                    try:
                        mat.mat_id = int(id_line)
                    except ValueError:
                        mat.mat_id = -1
                    i += 1
            obj.materials.append(mat)
            cur = mat
            obj.material_textures.append(mat.diffuse_texture)
            tag = ""
        elif cur and ieq(tag, "[matl_alpha]"):
            try:
                cur.alpha_mode = max(0, min(2, int(line)))
            except ValueError:
                cur.alpha_mode = 0
            tag = ""
        elif cur and ieq(tag, "[usetexttexture]"):
            try:
                cur.use_text_texture = int(line)
            except ValueError:
                cur.use_text_texture = 0
            tag = ""
        i += 1

    i = 0
    while i < len(lines):
        raw = lines[i]
        if not raw or raw[0] != "[":
            i += 1
            continue
        tok = trim(raw)
        if ieq(tok, "[texttexture]"):

            def read_next() -> str:
                nonlocal i
                i += 1
                while i < len(lines):
                    r = lines[i]
                    if r and r[0] == "[":
                        i -= 1
                        return ""
                    t = trim(r)
                    if t:
                        return t
                    i += 1
                return ""

            src = read_next()
            tt = TextTex()
            as_index = bool(src) and re.fullmatch(r"[+-]?\d+", src) is not None
            if as_index:
                tt.source_index = int(src)
            else:
                tt.string_var = src
                tt.source_index = -1
            tt.font_name = read_next()
            tt.width = int(read_next() or "128")
            tt.height = int(read_next() or "128")
            tt.color_mode = int(read_next() or "0")
            tt.r = int(read_next() or "255")
            tt.g = int(read_next() or "0")
            tt.b = int(read_next() or "0")
            obj.text_textures.append(tt)
        i += 1
    return obj


def bind_x_part(s: Sco, mesh_idx: int, parts: list[XMesh], part_i: int) -> tuple[int, int]:
    if part_i >= len(parts):
        return -1, 0
    key = parts[part_i].texture_filename.lower()
    occ = 0
    if key:
        for i in range(part_i):
            if parts[i].texture_filename.lower() == key:
                occ += 1
    exact = None
    for m in s.materials:
        if getattr(m, "is_matl_change", False):
            continue
        if mesh_idx >= 0 and m.mesh_index >= 0 and m.mesh_index != mesh_idx:
            continue
        if not key or m.texture_key != key:
            continue
        if m.mat_id == occ:
            exact = m
            break
    if not exact:
        return -1, (1 if s.only_editor_mesh else 0)
    tt = exact.use_text_texture if exact.use_text_texture is not None else -1
    return tt, exact.alpha_mode


def sco_texttex(s: Sco, mesh_idx: int) -> tuple[int, int]:
    for m in s.materials:
        if mesh_idx >= 0 and m.mesh_index >= 0 and m.mesh_index != mesh_idx:
            continue
        if m.use_text_texture is not None:
            return m.use_text_texture, m.alpha_mode
    return -1, 0


def sco_x_alpha(s: Sco, mesh_idx: int) -> int:
    a = 0
    for m in s.materials:
        if mesh_idx >= 0 and m.mesh_index >= 0 and m.mesh_index != mesh_idx:
            continue
        a = max(a, m.alpha_mode)
    if s.only_editor_mesh and a == 0:
        a = 1
    return a


# --- .x text loader (x_mesh_loader.cpp) ---

def skip_ws(s: str, i: int) -> int:
    n = len(s)
    while i < n:
        if s[i] == "/" and i + 1 < n and s[i + 1] == "/":
            while i < n and s[i] != "\n":
                i += 1
            continue
        if s[i].isspace():
            i += 1
            continue
        break
    return i


def skip_separators(s: str, i: int) -> int:
    while True:
        i = skip_ws(s, i)
        if i < len(s) and s[i] in ",;":
            i += 1
            continue
        return i


def parse_number(s: str, i: int) -> tuple[Optional[float], int]:
    i = skip_ws(s, i)
    m = re.match(r"[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?", s[i:])
    if not m:
        return None, i
    return float(m.group(0)), i + m.end()


def parse_dword(s: str, i: int) -> tuple[Optional[int], int]:
    f, i = parse_number(s, i)
    if f is None:
        return None, i
    return int(f), i


def find_mesh_block(s: str, start: int = 0) -> int:
    pos = start
    n = len(s)
    while pos < n:
        p = s.find("Mesh", pos)
        if p < 0:
            return -1
        if p > 0 and s[p - 1].isalnum():
            pos = p + 4
            continue
        j = p + 4
        j = skip_ws(s, j)
        if j < n and (s[j].isalpha() or s[j] == "_"):
            while j < n and (s[j].isalnum() or s[j] == "_"):
                j += 1
            j = skip_ws(s, j)
        if j < n and s[j] == "{":
            kw = s[p:j]
            if any(x in kw for x in ("MeshNormal", "MeshTexture", "MeshMaterial", "MeshVertex", "MeshFace")):
                pos = j + 1
                continue
            return j
        pos = p + 4
    return -1


def find_named_block(s: str, name: str, from_: int = 0) -> int:
    pos = from_
    n = len(s)
    while pos < n:
        p = s.find(name, pos)
        if p < 0:
            return -1
        if p > 0 and s[p - 1].isalnum():
            pos = p + len(name)
            continue
        j = p + len(name)
        j = skip_ws(s, j)
        if j < n and (s[j].isalpha() or s[j] == "_"):
            while j < n and (s[j].isalnum() or s[j] == "_"):
                j += 1
            j = skip_ws(s, j)
        if j < n and s[j] == "{":
            return j
        pos = p + len(name)
    return -1


@dataclass
class XMesh:
    vertices: list = field(default_factory=list)
    indices: list = field(default_factory=list)
    texture_filename: str = ""
    n_verts: int = 0
    n_faces: int = 0
    brace: int = -1


def load_x_mesh_engine(path: Path) -> Optional[XMesh]:
    """First Mesh only + first TextureFilename in the whole file (our C++)."""
    s = path.read_text(encoding="latin-1", errors="replace")
    if "txt" not in s.lower():
        return None
    mesh_brace = find_mesh_block(s)
    if mesh_brace < 0:
        return None
    return parse_one_mesh(s, mesh_brace, whole_file_tex=True)


def parse_one_mesh(s: str, mesh_brace: int, whole_file_tex: bool) -> Optional[XMesh]:
    i = mesh_brace + 1
    n_verts, i = parse_dword(s, i)
    i = skip_ws(s, i)
    if n_verts is None or n_verts == 0:
        return None
    if i < len(s) and s[i] == ";":
        i += 1
    out = XMesh(n_verts=n_verts, brace=mesh_brace)
    verts = []
    for _ in range(n_verts):
        x, i = parse_number(s, i)
        i = skip_ws(s, i)
        if i < len(s) and s[i] == ";":
            i += 1
        y, i = parse_number(s, i)
        i = skip_ws(s, i)
        if i < len(s) and s[i] == ";":
            i += 1
        z, i = parse_number(s, i)
        if x is None or y is None or z is None:
            return None
        i = skip_separators(s, i)
        verts.append((x, y, z))
    out.vertices = verts
    n_faces, i = parse_dword(s, i)
    i = skip_ws(s, i)
    if i < len(s) and s[i] == ";":
        i += 1
    if n_faces is None:
        return None
    out.n_faces = n_faces
    indices = []
    for _ in range(n_faces):
        n_idx, i = parse_dword(s, i)
        i = skip_ws(s, i)
        if i < len(s) and s[i] == ";":
            i += 1
        if n_idx is None or n_idx < 3:
            return None
        face = []
        for k in range(n_idx):
            idx, i = parse_dword(s, i)
            if idx is None:
                return None
            face.append(idx)
            i = skip_ws(s, i)
            if k + 1 < n_idx:
                if i < len(s) and s[i] == ",":
                    i += 1
            else:
                i = skip_separators(s, i)
        i = skip_separators(s, i)
        for k in range(1, n_idx - 1):
            indices.extend([face[0], face[k + 1], face[k]])
    out.indices = indices
    if whole_file_tex:
        tf = s.find("TextureFilename")
        if tf >= 0:
            q0 = s.find('"', tf)
            q1 = s.find('"', q0 + 1) if q0 >= 0 else -1
            if q0 >= 0 and q1 >= 0:
                out.texture_filename = s[q0 + 1 : q1]
    else:
        end = find_mesh_block(s, mesh_brace + 1)
        chunk = s[mesh_brace : end if end >= 0 else len(s)]
        tf = chunk.find("TextureFilename")
        if tf >= 0:
            q0 = chunk.find('"', tf)
            q1 = chunk.find('"', q0 + 1) if q0 >= 0 else -1
            if q0 >= 0 and q1 >= 0:
                out.texture_filename = chunk[q0 + 1 : q1]
    return out


def load_all_x_meshes(path: Path) -> list[XMesh]:
    s = path.read_text(encoding="latin-1", errors="replace")
    out = []
    start = 0
    while True:
        b = find_mesh_block(s, start)
        if b < 0:
            break
        m = parse_one_mesh(s, b, whole_file_tex=False)
        if m:
            out.append(m)
        start = b + 1
    return out


def try_texture_folder(owner: Path, filename: str) -> Optional[Path]:
    stem = Path(filename).stem
    for folder in ("texture", "Texture"):
        tex_dir = owner / folder
        dds = (tex_dir / (stem + ".dds")).resolve()
        if dds.is_file():
            return dds
        orig = (tex_dir / Path(filename).name).resolve()
        if orig.is_file():
            return orig
    return None


def texture_owner_dir(base_file: Path) -> Path:
    d = base_file.parent
    ext = base_file.suffix.lower()
    if ext in (".sco", ".sli"):
        return d
    p = d
    for _ in range(16):
        if (p / "texture").is_dir() or (p / "Texture").is_dir():
            return p
        if p.name.lower() == "model":
            return p.parent
        if p.parent == p:
            break
        p = p.parent
    return d


def resolve_texture_path(base_file: Path, texture_name: str, omsi_root: Path) -> Optional[Path]:
    if not texture_name:
        return None
    hit = try_texture_folder(texture_owner_dir(base_file), texture_name)
    if hit:
        return hit
    if omsi_root:
        hit = try_texture_folder(omsi_root, texture_name)
        if hit:
            return hit
    return None


# --- OFT + BMP ---

def load_bmp_rgba(path: Path) -> Optional[tuple[int, int, bytes]]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        return None
    off = struct.unpack_from("<I", data, 10)[0]
    hdr = struct.unpack_from("<I", data, 14)[0]
    w, h_signed = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    h = abs(h_signed)
    top_down = h_signed < 0
    if compression != 0:
        return None
    row_bytes = ((w * bpp + 31) // 32) * 4
    palette = []
    if bpp <= 8:
        ncolors = struct.unpack_from("<I", data, 46)[0] or (1 << bpp)
        pal_off = 14 + hdr
        for i in range(ncolors):
            b, g, r, _ = data[pal_off + i * 4 : pal_off + i * 4 + 4]
            palette.append((r, g, b, 255))
    out = bytearray(w * h * 4)
    for y in range(h):
        src_y = y if top_down else (h - 1 - y)
        row = off + src_y * row_bytes
        for x in range(w):
            o = (y * w + x) * 4
            if bpp == 24:
                b, g, r = data[row + x * 3 : row + x * 3 + 3]
                out[o : o + 4] = bytes((r, g, b, 255))
            elif bpp == 32:
                b, g, r, a = data[row + x * 4 : row + x * 4 + 4]
                out[o : o + 4] = bytes((r, g, b, a if a else 255))
            elif bpp == 8:
                idx = data[row + x]
                pal = palette[idx] if idx < len(palette) else (0, 0, 0, 255)
                out[o : o + 4] = bytes(pal)
            else:
                return None
    return w, h, bytes(out)


@dataclass
class Glyph:
    x0: int = 0
    x1: int = 0
    y0: int = 0


@dataclass
class OftFont:
    name: str = ""
    height: int = 16
    gap: int = 0
    color: Optional[tuple[int, int, bytes]] = None
    alpha: Optional[tuple[int, int, bytes]] = None
    glyphs: dict[int, Glyph] = field(default_factory=dict)


def utf8_next(s: str, i: int) -> tuple[int, int]:
    if i >= len(s):
        return 0, i
    return ord(s[i]), i + 1


def load_oft_file(path: Path) -> Optional[OftFont]:
    lines = read_cfg_lines(path)
    font = OftFont()
    color_file = ""
    alpha_file = ""
    i = 0
    while i < len(lines):
        tok = lines[i]
        if not tok or tok[0] != "[":
            i += 1
            continue
        tag = trim(tok)
        if ieq(tag, "[newfont]"):

            def nxt() -> str:
                nonlocal i
                i += 1
                while i < len(lines):
                    t = trim(lines[i])
                    if t:
                        return t
                    i += 1
                return ""

            font.name = nxt()
            color_file = nxt()
            alpha_file = nxt()
            try:
                font.height = int(nxt())
            except ValueError:
                font.height = 16
            try:
                font.gap = int(nxt())
            except ValueError:
                font.gap = 0
            i += 1
            continue
        if ieq(tag, "[char]"):
            i += 1
            if i >= len(lines):
                break
            ch = lines[i]
            if ch.endswith("\r"):
                ch = ch[:-1]
            ch = " " if not trim(ch) else trim(ch)

            def next_num() -> int:
                nonlocal i
                i += 1
                while i < len(lines):
                    t = trim(lines[i])
                    if t:
                        try:
                            return int(t)
                        except ValueError:
                            return 0
                    i += 1
                return 0

            g = Glyph(x0=next_num(), x1=next_num(), y0=next_num())
            cp = ord(ch[0]) if ch else 0
            if cp:
                font.glyphs[cp] = g
        i += 1
    d = path.parent
    if color_file:
        p = d / color_file
        if not p.is_file():
            p = d / Path(color_file).name
        font.color = load_bmp_rgba(p) if p.is_file() else None
    if alpha_file:
        p = d / alpha_file
        if not p.is_file():
            p = d / Path(alpha_file).name
        font.alpha = load_bmp_rgba(p) if p.is_file() else None
    if font.alpha is None:
        font.alpha = font.color
    return font


def sample_lum(img: tuple[int, int, bytes], x: int, y: int) -> int:
    w, h, rgba = img
    if x < 0 or y < 0 or x >= w or y >= h:
        return 0
    o = (y * w + x) * 4
    return (rgba[o] + rgba[o + 1] + rgba[o + 2]) // 3


def rasterize(tt: TextTex, text: str, font: OftFont) -> tuple[int, int, bytearray, int, int]:
    tw = tt.width if tt.width > 8 else 128
    th = tt.height if tt.height > 8 else 128
    out = bytearray(tw * th * 4)
    lines = []
    cur = ""
    for ch in text.replace("\r", ""):
        if ch in "@\n":
            lines.append(cur)
            cur = ""
        else:
            cur += ch
    lines.append(cur)
    line_h = font.height if font.height > 0 else 16
    y = (th - len(lines) * line_h) // 2
    painted = 0
    missing = 0

    def line_w(line: str) -> int:
        w = 0
        for ch in line:
            g = font.glyphs.get(ord(ch))
            if not g:
                w += max(4, font.height // 4)
            else:
                w += max(1, g.x1 - g.x0)
        return w

    def blit(g: Glyph, dx: int, dy: int) -> int:
        nonlocal painted
        gw = g.x1 - g.x0
        gh = line_h
        for gy in range(gh):
            py = dy + gy
            if py < 0 or py >= th:
                continue
            for gx in range(gw):
                px = dx + gx
                if px < 0 or px >= tw:
                    continue
                a = sample_lum(font.alpha, g.x0 + gx, g.y0 + gy) if font.alpha else 0
                if a < 8:
                    continue
                o = (py * tw + px) * 4
                out[o] = tt.r & 255
                out[o + 1] = tt.g & 255
                out[o + 2] = tt.b & 255
                out[o + 3] = a
                painted += 1
        return gw

    for line in lines:
        x = (tw - line_w(line)) // 2
        for ch in line:
            cp = ord(ch)
            g = font.glyphs.get(cp)
            if g is None and cp > 127:
                g = font.glyphs.get(cp & 0xFF)
            if g is None:
                missing += 1
                x += max(4, line_h // 4)
                continue
            x += blit(g, x, y)
        y += line_h
    return tw, th, out, painted, missing


def write_tga(path: Path, w: int, h: int, rgba: bytes) -> None:
    hdr = bytearray(18)
    hdr[2] = 2
    hdr[12:14] = struct.pack("<H", w)
    hdr[14:16] = struct.pack("<H", h)
    hdr[16] = 32
    hdr[17] = 40  # origin top-left, 8 alpha bits
    body = bytearray(w * h * 4)
    for i in range(w * h):
        r, g, b, a = rgba[i * 4 : i * 4 + 4]
        body[i * 4 : i * 4 + 4] = bytes((b, g, r, a))
    path.write_bytes(bytes(hdr) + body)


def sim_one(sco_path: Path, omsi_root: Path, sample: str) -> None:
    print("=" * 60)
    print(f"sco={sco_path}")
    sco = load_sco(sco_path)
    print(f"  friendly={sco.friendly_name!r} meshes={len(sco.meshes)} "
          f"texttex={len(sco.text_textures)} matl={len(sco.materials)}")
    for i, m in enumerate(sco.meshes):
        print(f"  [mesh] {i} {m.mesh_file.name} exists={m.mesh_file.is_file()} o3d={m.is_o3d}")
    for i, tt in enumerate(sco.text_textures):
        print(f"  [texttexture] {i} src={tt.string_var or tt.source_index} font={tt.font_name!r} "
              f"{tt.width}x{tt.height} mode={tt.color_mode} rgb=({tt.r},{tt.g},{tt.b})")
    for i, mat in enumerate(sco.materials):
        print(f"  [matl] {i} tex={mat.diffuse_texture!r} occ={mat.mat_id} mesh={mat.mesh_index} "
              f"alpha={mat.alpha_mode} useTextTexture={mat.use_text_texture}")

    print("  -- O3D: apply_sco_script_visibility (is_x_chunk=false -> all [useTextTexture] on mesh) --")
    for mesh_idx, mesh_ref in enumerate(sco.meshes):
        if not mesh_ref.is_o3d:
            continue
        mats = [
            m
            for m in sco.materials
            if m.use_text_texture is not None
            and (m.mesh_index < 0 or m.mesh_index == mesh_idx)
        ]
        print(f"  sco-mesh {mesh_idx} {mesh_ref.mesh_file.name}: O3D chunks get text_texture_index=-1 is_x_chunk=0")
        if not mats:
            print("    no [useTextTexture] on this mesh")
            continue
        for m in mats:
            ti = m.use_text_texture
            print(
                f"    APPLY tt={ti} mat={m.diffuse_texture!r} occ={m.mat_id} alpha={m.alpha_mode} "
                f"(GPU batch already has useTextTexture on matching O3D slot)"
            )

    print("  -- spawn per .x Mesh (bind_x_part, is_x_chunk=true) --")
    for mesh_idx, mesh_ref in enumerate(sco.meshes):
        if mesh_ref.is_o3d:
            print(f"  sco-mesh {mesh_idx}: O3D skip")
            continue
        if not mesh_ref.mesh_file.is_file():
            print(f"  sco-mesh {mesh_idx}: MISSING {mesh_ref.mesh_file}")
            continue
        parts = load_all_x_meshes(mesh_ref.mesh_file)
        print(f"  sco-mesh {mesh_idx} {mesh_ref.mesh_file.name}: {len(parts)} parts")
        for pi, xm in enumerate(parts):
            tt_idx, amode = bind_x_part(sco, mesh_idx, parts, pi)
            tex = resolve_texture_path(mesh_ref.mesh_file, xm.texture_filename, omsi_root)
            if tex is None:
                tex = resolve_texture_path(sco_path, xm.texture_filename, omsi_root)
            blend = {0: "opaque", 1: "alpha_test", 2: "alpha_blend"}.get(amode, str(amode))
            print(
                f"    part {pi}: verts={xm.n_verts} tris={len(xm.indices)//3} tex={xm.texture_filename!r} "
                f"file={'OK' if tex and tex.is_file() else 'MISS'} alpha={amode}/{blend} "
                f"useTextTexture={tt_idx}"
            )
            if tt_idx >= 0:
                print(f"      is_x_chunk=1 -> raster only if text_texture_index=={tt_idx}")
            else:
                print("      is_x_chunk=1 text_texture_index=-1 -> raster SKIPPED (fixed for .x non-text parts)")

    if not sco.text_textures:
        print("  no [texttexture]")
        return
    fonts_dir = omsi_root / "Fonts"
    font = None
    want = sco.text_textures[0].font_name.lower()
    for p in fonts_dir.glob("*.oft"):
        loaded = load_oft_file(p)
        if loaded and (loaded.name.lower() == want or p.stem.lower() == want):
            font = loaded
            print(f"  oft {loaded.name!r} glyphs={len(loaded.glyphs)} h={loaded.height}")
            break
    if not font:
        print(f"  oft MISSING {want!r}")
        return
    tt = sco.text_textures[0]
    text = sample
    tw, th, rgba, painted, missing = rasterize(tt, text, font)
    print(f"  raster text={text!r} {tw}x{th} painted={painted} miss={missing}")


def main() -> int:
    omsi_root = Path(r"E:\OMSI 22\OMSI 2")
    defaults = [
        Path(r"E:\OMSI 22\OMSI 2\Sceneryobjects\Verkehrszeichen_MC\StreetSign_1.sco"),
        Path(r"E:\OMSI 22\OMSI 2\Sceneryobjects\Ruede\hst_wartehaus_alt01.sco"),
        Path(r"E:\OMSI 22\OMSI 2\Sceneryobjects\Streetobjects_RUE\Busstop_Name_Timetable_1.sco"),
        Path(r"E:\OMSI 22\OMSI 2\Sceneryobjects\Streetobjects_RUE\Busstop_Lineplate_yellow_2.sco"),
        Path(r"E:\OMSI 22\OMSI 2\Sceneryobjects\Streetobjects_RUE\Busstop_Lineplate_green_2.sco"),
        Path(r"E:\OMSI 22\OMSI 2\Sceneryobjects\Generic\routearrow_L.sco"),
    ]
    args = [Path(a) for a in sys.argv[1:] if a.lower().endswith(".sco")]
    scos = args if args else defaults
    sample = next((a for a in sys.argv[1:] if not a.lower().endswith(".sco")), "")
    print("=== SIM omsi-f4-engine (all .x Mesh + bind_x_part) ===")
    for sco_path in scos:
        sim_one(sco_path, omsi_root, sample)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
