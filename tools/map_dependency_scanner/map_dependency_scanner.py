#!/usr/bin/env python3
"""
OMSI 2 map dependency scanner.

Walks global.cfg → tiles → scenery/splines/vehicles/AI and records every file
the game loads or references (textures, meshes, scripts, fonts, ticket packs, …).

Mirrors F4 engine + omsi_rebuilt + Omsi.exe decompilation (F:\\...\\decompiled\\units_c):
  _Unit126.c  TMap.loadGlobalFile, TMapKachel.loadMapFile, GetGroundTexPath, GetAerialFileName
  _Unit123.c  TWeather.loadCloudType (weather\\clouds.cfg, rain/snow textures)
  _Unit129.pas TRoadVehicle *.hof beside .bus
  _Unit121.c  Timetable TTData\\*.ttl / *.ttp
  _Unit130.c  [matl_*] texture modifiers (nightmap, lightmap, bumpmap, envmap…)
  src/map/omsi_map_loader.cpp, sco_loader.cpp, model_cfg_loader.cpp, cti_loader.cpp

Usage:
  python tools/map_dependency_scanner.py
      → diálogos: elegir global.cfg y dónde guardar el .json
  python tools/map_dependency_scanner.py --omsi-root "F:\\OMSI 2" --map maps\\Grundorf\\global.cfg
  python tools/map_dependency_scanner.py --omsi-root ... --map ... --format json -o deps.json
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from collections import defaultdict, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Iterable, Iterator, Optional

_TOOLS_DIR = Path(__file__).resolve().parent
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))
from sim_streetsign_engine import (  # noqa: E402
    resolve_texture_path,
    texture_owner_dir,
    try_texture_folder,
)

# _Unit130 / TTextureMan — extra maps live IN texture\<season>\ (same filename / stem.dds)
SEASONAL_TEXTURE_DIRS = (
    "night",
    "WinterSnow",
    "Fall",
    "Spring",
    "Winter",
    "SummerDry",
    "WinterSnowfall",
)

# ---------------------------------------------------------------------------
# Text / path helpers
# ---------------------------------------------------------------------------

TEXTURE_EXTS = {".dds", ".bmp", ".tga", ".png", ".jpg", ".jpeg", ".gif"}
MESH_EXTS = {".o3d", ".x"}
CFG_EXTS = {".sco", ".sli", ".bus", ".ovh", ".osc", ".cfg", ".cti", ".txt", ".otp", ".oft"}


def ieq(a: str, b: str) -> bool:
    return a.lower() == b.lower()


def trim(s: str) -> str:
    return s.strip(" \t\r\n\ufeff")


def is_section(line: str) -> bool:
    t = trim(line)
    return len(t) >= 2 and t[0] == "[" and t[-1] == "]"


def read_cfg_lines(path: Path) -> list[str]:
    raw = path.read_bytes()
    if raw.startswith(b"\xff\xfe"):
        text = raw.decode("utf-16-le")
    elif raw.startswith(b"\xfe\xff"):
        text = raw.decode("utf-16-be")
    elif raw.startswith(b"\xef\xbb\xbf"):
        text = raw.decode("utf-8-sig")
    else:
        for enc in ("utf-8", "latin-1", "cp1252"):
            try:
                text = raw.decode(enc)
                break
            except UnicodeDecodeError:
                continue
        else:
            text = raw.decode("latin-1", errors="replace")
    return text.replace("\r\n", "\n").replace("\r", "\n").split("\n")


def norm_key(path: Path) -> str:
    try:
        return str(path.resolve()).lower()
    except OSError:
        return str(path).lower().replace("/", "\\")


def join_omsi(omsi_root: Path, rel: str, base: Optional[Path] = None) -> Path:
    """Resolve OMSI relative paths: base dir first, then omsi_root for \\-prefixed paths."""
    rel = rel.strip().replace("/", "\\")
    if not rel:
        return Path()
    if len(rel) > 1 and rel[1] == ":":
        return Path(rel).resolve()
    if rel.startswith("\\"):
        return (omsi_root / rel.lstrip("\\")).resolve()
    p = Path(rel)
    if p.is_absolute():
        return p.resolve()
    if base is not None:
        return (base / rel).resolve()
    return (omsi_root / rel).resolve()


def resolve_mesh_path(base_file: Path, rel: str, omsi_root: Path) -> Path:
    """Resolve .o3d/.x like sco_loader.cpp + model.cfg (model_dir / rel)."""
    rel = rel.strip().replace("/", "\\")
    if not rel:
        return Path()
    if len(rel) > 1 and rel[1] == ":":
        p = Path(rel)
        return p.resolve() if p.is_file() else p
    if rel.startswith("\\"):
        p = (omsi_root / rel.lstrip("\\")).resolve()
        if p.is_file():
            return p

    owner = base_file.parent
    is_model_cfg = base_file.suffix.lower() == ".cfg" and owner.name.lower() == "model"
    vehicle_root = owner.parent if is_model_cfg else owner

    candidates: list[Path] = []
    if is_model_cfg:
        # mesh_animator.cpp: o3d_path = model_dir / mesh_file
        candidates.extend([owner / rel, vehicle_root / rel, vehicle_root / "model" / rel])
    else:
        # sco_loader.cpp: base/rel then base/model/rel
        candidates.extend([owner / rel, owner / "model" / rel])
        if rel.lower().startswith("model\\"):
            candidates.insert(0, owner / rel)

    candidates.extend([join_omsi(omsi_root, rel, owner), omsi_root / rel, vehicle_root / rel])
    seen: set[str] = set()
    for cand in candidates:
        key = str(cand).lower()
        if key in seen:
            continue
        seen.add(key)
        if cand.is_file():
            return cand.resolve()
    return (owner / rel).resolve()


def resolve_tree_texture_path(sco_rel: str, tex_name: str, omsi_root: Path) -> Path:
    """Tile tree override: resolve from instanced .sco (mod_streaming.inc.cpp)."""
    sco_path = join_omsi(omsi_root, sco_rel)
    hit = resolve_texture_omsi(sco_path, tex_name, omsi_root)
    return hit if hit else join_omsi(omsi_root, tex_name, sco_path.parent)


def ctc_pack_root(cfg_or_sco: Path) -> Path:
    """[CTC] folder is relative to pack root (parent of model\\), not model.cfg dir (Unit87)."""
    d = cfg_or_sco.parent
    if d.name.lower() == "model":
        return d.parent
    return d


def iter_ctc_cti(cfg_or_sco: Path, folder: str, omsi_root: Path) -> Iterator[tuple[str, Path, str]]:
    folder = folder.strip().replace("/", "\\")
    if not folder:
        return
    root = ctc_pack_root(cfg_or_sco)
    seen_dir: set[str] = set()
    for cand in (root / folder, cfg_or_sco.parent / folder, join_omsi(omsi_root, folder, root)):
        key = str(cand).lower()
        if key in seen_dir:
            continue
        seen_dir.add(key)
        if not cand.is_dir():
            continue
        for cti in sorted(cand.glob("*.cti")):
            yield ("cti", cti.resolve(), "ctc")
        return


def resolve_texture_omsi(base_file: Path, tex_name: str, omsi_root: Path) -> Optional[Path]:
    """Owner/texture, then next to the referring file (CTI folder), then OMSI\\texture."""
    hit = resolve_texture_path(base_file, tex_name, omsi_root)
    if hit:
        return hit
    rel = tex_name.replace("/", "\\")
    name = Path(rel).name
    stem = Path(name).stem
    owner = texture_owner_dir(base_file)
    folders = [base_file.parent, owner]
    if owner.name.lower() == "model":
        folders.append(owner.parent)
    seen: set[str] = set()
    for folder in folders:
        for cand in (folder / name, folder / f"{stem}.dds", folder / rel):
            key = str(cand).lower()
            if key in seen:
                continue
            seen.add(key)
            if cand.is_file():
                return cand.resolve()
        extra = try_texture_folder(folder, name) or try_texture_folder(folder, rel)
        if extra:
            return extra
    return None


def iter_seasonal_textures(hit: Path, owner: Path, role: str) -> Iterator[tuple[str, Path, str]]:
    """TTextureMan extra maps: ExtractFilePath(base) + season + filename (_Unit130)."""
    try:
        hit_r = hit.resolve()
    except OSError:
        hit_r = hit
    seen: set[str] = {str(hit_r).lower()}
    stem = hit.stem
    name = hit.name
    bases = [hit.parent]
    for folder_name in ("texture", "Texture"):
        td = owner / folder_name
        if td.is_dir() and td not in bases:
            bases.append(td)
    for season in SEASONAL_TEXTURE_DIRS:
        for base in bases:
            for cand in (base / season / name, base / season / f"{stem}.dds"):
                if not cand.is_file():
                    continue
                key = str(cand.resolve()).lower()
                if key not in seen:
                    seen.add(key)
                    yield ("texture", cand.resolve(), f"{role}_{season}")


def resolve_sound_wav(sound_dir: Path, rel: str) -> Path:
    """OMSI sound.cfg wav lookup (sound_runtime.cpp)."""
    rel = rel.strip().replace("/", "\\")
    name = Path(rel).name
    if not name:
        return sound_dir / rel
    for cand in (sound_dir / rel, sound_dir / name):
        if cand.is_file():
            return cand.resolve()
    if sound_dir.is_dir():
        for sub in sound_dir.iterdir():
            if sub.is_dir() and (sub / name).is_file():
                return (sub / name).resolve()
    return (sound_dir / name).resolve()


def yield_resolved_texture(
    base_file: Path, tex_name: str, omsi_root: Path, role: str
) -> Iterator[tuple[str, Path, str]]:
    if not tex_name or not looks_like_texture(tex_name):
        return
    hit = resolve_texture_omsi(base_file, tex_name, omsi_root)
    if hit:
        yield ("texture", hit, role)
        yield from iter_seasonal_textures(hit, texture_owner_dir(base_file), role)
    else:
        yield ("texture", join_omsi(omsi_root, tex_name, base_file.parent), f"{role}_unresolved")


MATL_TEXTURE_TAGS = frozenset(
    {
        "[matl_transmap]",
        "[matl_envmap]",
        "[matl_envmaprealtime]",
        "[matl_bumpmap]",
        "[matl_envmap_mask]",
        "[matl_nightmap]",
        "[matl_lightmap]",
    }
)


def looks_like_texture(s: str) -> bool:
    if not s or "." not in s:
        return False
    ext = Path(s).suffix.lower()
    if ext not in TEXTURE_EXTS:
        return False
    stem = Path(s).stem
    if not stem or stem.replace(".", "").replace("-", "").replace("_", "").isdigit():
        return False
    return True


def parse_counted_paths(lines: list[str]) -> list[str]:
    """[scenObjList] / [splineObjTypes]: optional count line then paths."""
    out: list[str] = []
    i = 0
    while i < len(lines) and not trim(lines[i]):
        i += 1
    if i >= len(lines):
        return out
    rest = [trim(x) for x in lines[i:] if trim(x)]
    if not rest:
        return out
    if rest[0].isdigit():
        n = int(rest[0])
        out.extend(rest[1 : 1 + n])
        if len(out) < n:
            out.extend(rest[1 + len(out) :])
    else:
        out.extend(rest)
    return out


# ---------------------------------------------------------------------------
# Dependency graph
# ---------------------------------------------------------------------------

@dataclass
class DepEdge:
    parent: str
    role: str


@dataclass
class DepNode:
    kind: str
    path: Path
    display: str
    exists: bool
    edges_in: list[DepEdge] = field(default_factory=list)

    @property
    def key(self) -> str:
        return f"{self.kind}:{norm_key(self.path)}"


class DepGraph:
    def __init__(self) -> None:
        self.nodes: dict[str, DepNode] = {}
        self.by_kind: dict[str, set[str]] = defaultdict(set)
        self.missing: set[str] = set()

    def add(self, kind: str, path: Path, parent: Optional[tuple[str, Path]], role: str) -> str:
        if not path or str(path) in (".", ""):
            return ""
        node = DepNode(kind=kind, path=path, display=str(path), exists=path.exists())
        k = node.key
        if k not in self.nodes:
            self.nodes[k] = node
            self.by_kind[kind].add(k)
            if not node.exists:
                self.missing.add(k)
        if parent:
            pk = f"{parent[0]}:{norm_key(parent[1])}"
            self.nodes[k].edges_in.append(DepEdge(parent=pk, role=role))
        return k

    def stats(self) -> dict[str, int]:
        return {k: len(v) for k, v in sorted(self.by_kind.items())}


# ---------------------------------------------------------------------------
# Generic cfg document (section → list of line groups)
# ---------------------------------------------------------------------------

class CfgDoc:
    def __init__(self, lines: list[str]) -> None:
        self.sections: dict[str, list[list[str]]] = defaultdict(list)
        section = ""
        buf: list[str] = []
        for raw in lines:
            line = trim(raw)
            if is_section(line):
                if section and buf:
                    self.sections[section.lower()].append(buf)
                section = line
                buf = []
                continue
            if not section:
                continue
            if line or section.lower() in ("[backgroundimage]", "[description]"):
                buf.append(line)
        if section and buf:
            self.sections[section.lower()].append(buf)

    def blocks(self, tag: str) -> list[list[str]]:
        return self.sections.get(tag.lower(), [])

    def first_line(self, tag: str) -> str:
        for block in self.blocks(tag):
            for ln in block:
                if trim(ln):
                    return trim(ln)
        return ""


# ---------------------------------------------------------------------------
# Expanders (return list of (kind, path, role))
# ---------------------------------------------------------------------------

ExpandFn = Callable[[Path, Path, Path], Iterator[tuple[str, Path, str]]]


def is_asset_path(s: str) -> bool:
    if not s or s.startswith("#") or s.startswith(";") or s.startswith("*"):
        return False
    if s.startswith("[") or s.startswith("'"):
        return False
    if len(s) > 1 and s[0] == "-" and s[1].isdigit():
        return False
    ext = Path(s).suffix.lower()
    if ext in MESH_EXTS | TEXTURE_EXTS | {".osc", ".cfg", ".wav", ".ogg", ".oft"}:
        return True
    if "\\" in s or "/" in s:
        return True
    return False


def parse_sco_deps(sco_path: Path, omsi_root: Path) -> Iterator[tuple[str, Path, str]]:
    """Sequential parse mirroring sco_loader.cpp + sim_streetsign_engine.load_sco."""
    if not sco_path.exists():
        return
    lines = read_cfg_lines(sco_path)
    sco_dir = sco_path.parent
    doc = CfgDoc(lines)

    # [mesh] — sco_loader.cpp: first line of each [mesh] block; fallback model/
    for block in doc.blocks("[mesh]"):
        if not block:
            continue
        rel = trim(block[0])
        if is_asset_path(rel):
            yield ("mesh", resolve_mesh_path(sco_path, rel, omsi_root), "mesh")

    # [matl] sequential
    tag = ""
    cur_matl = False
    mesh_i = -1
    i = 0
    while i < len(lines):
        raw = lines[i]
        if not raw:
            i += 1
            continue
        if raw[0] == "[":
            line = trim(raw)
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
            if looks_like_texture(line):
                yield from yield_resolved_texture(sco_path, line, omsi_root, "matl")
            elif Path(line).suffix.lower() in MESH_EXTS:
                yield ("mesh", resolve_mesh_path(sco_path, line, omsi_root), "matl_mesh")
            cur_matl = True
            if i + 1 < len(lines):
                nxt = trim(lines[i + 1])
                if nxt and not nxt.startswith("[") and nxt.isdigit():
                    i += 1
            tag = ""
        elif cur_matl and tag.lower() in MATL_TEXTURE_TAGS:
            if looks_like_texture(line):
                yield from yield_resolved_texture(sco_path, line, omsi_root, tag.strip("[]"))
            tag = ""
        i += 1

    # [texttexture] — line must start with '[' (not tab-indented SDK comments)
    mesh_i = -1
    i = 0
    while i < len(lines):
        raw = lines[i]
        if not raw or raw[0] != "[":
            i += 1
            continue
        tok = trim(raw)
        if ieq(tok, "[mesh]"):
            mesh_i += 1
            i += 1
            continue
        if ieq(tok, "[texttexture]") or ieq(tok, "[texttexture_enh]"):

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

            _src = read_next()
            font = read_next()
            if font and not font.isdigit():
                font_path = join_omsi(omsi_root, font if font.lower().endswith(".oft") else f"Fonts\\{font}.oft")
                yield ("font", font_path, "texttexture")
            for _ in range(6):
                read_next()
            if ieq(tok, "[texttexture_enh]"):
                read_next()
                read_next()
        i += 1

    for tag, role in (
        ("[collision_mesh]", "collision_mesh"),
        ("[collisionmesh]", "collision_mesh"),
        ("[crossing_heightdeformation]", "crossing_height"),
    ):
        p = doc.first_line(tag)
        if p and is_asset_path(p):
            yield ("mesh", resolve_mesh_path(sco_path, p, omsi_root), role)

    for block in doc.blocks("[splinehelper]"):
        if block and block[0].strip().lower().endswith(".sli"):
            yield ("sli", join_omsi(omsi_root, trim(block[0]), sco_dir), "splinehelper")

    for block in doc.blocks("[tree]"):
        if len(block) >= 3 and looks_like_texture(block[2]):
            yield from yield_resolved_texture(sco_path, block[2], omsi_root, "tree")

    for tag in ("[script]",):
        for block in doc.blocks(tag):
            items = block[1:] if block and block[0].isdigit() else block
            for ln in items:
                if trim(ln).lower().endswith(".osc"):
                    yield ("osc", join_omsi(omsi_root, trim(ln), sco_dir), "script")

    for tag in ("[varnamelist]", "[stringvarnamelist]", "[constfile]"):
        for block in doc.blocks(tag):
            items = block[1:] if block and block[0].isdigit() else block
            for ln in items:
                if trim(ln):
                    yield ("txt", join_omsi(omsi_root, trim(ln), sco_dir), tag.strip("[]"))

    for block in doc.blocks("[ctc]"):
        if len(block) >= 2:
            yield from iter_ctc_cti(sco_path, block[1], omsi_root)
    for block in doc.blocks("[ctctexture]"):
        if len(block) >= 2 and looks_like_texture(block[1]):
            yield from yield_resolved_texture(sco_path, block[1], omsi_root, "ctctexture")


def expand_sco(sco_path: Path, omsi_root: Path, base: Path) -> Iterator[tuple[str, Path, str]]:
    yield from parse_sco_deps(sco_path, omsi_root)


def expand_sli(sli_path: Path, omsi_root: Path, base: Path) -> Iterator[tuple[str, Path, str]]:
    if not sli_path.exists():
        return
    lines = read_cfg_lines(sli_path)
    doc = CfgDoc(lines)
    sli_dir = sli_path.parent
    for block in doc.blocks("[texture]"):
        for ln in block:
            t = trim(ln)
            if t and looks_like_texture(t):
                yield from yield_resolved_texture(sli_path, t, omsi_root, "sli_texture")


def expand_bus(bus_path: Path, omsi_root: Path, base: Path) -> Iterator[tuple[str, Path, str]]:
    if not bus_path.exists():
        return
    lines = read_cfg_lines(bus_path)
    doc = CfgDoc(lines)
    bus_dir = bus_path.parent

    for tag, role in (
        ("[model]", "model_cfg"),
        ("[sound]", "sound_cfg"),
        ("[sound_ai]", "sound_ai_cfg"),
        ("[paths]", "paths_cfg"),
        ("[passengercabin]", "passengercabin_cfg"),
    ):
        p = doc.first_line(tag)
        if p:
            kind = "sound_cfg" if "sound" in role else "cfg"
            yield (kind, join_omsi(omsi_root, p, bus_dir), role)

    for tag in ("[script]", "[varnamelist]", "[stringvarnamelist]", "[constfile]"):
        for block in doc.blocks(tag):
            if block and block[0].isdigit():
                for ln in block[1:]:
                    if trim(ln):
                        ext = Path(trim(ln)).suffix.lower()
                        kind = "osc" if ext == ".osc" else "txt"
                        yield (kind, join_omsi(omsi_root, trim(ln), bus_dir), tag.strip("[]"))
            else:
                for ln in block:
                    if not trim(ln):
                        continue
                    ext = Path(trim(ln)).suffix.lower()
                    kind = "osc" if ext == ".osc" else "txt"
                    yield (kind, join_omsi(omsi_root, trim(ln), bus_dir), tag.strip("[]"))
    yield from iter_bus_hof_deps(bus_path)


def expand_sound_cfg(sound_cfg_path: Path, omsi_root: Path, base: Path) -> Iterator[tuple[str, Path, str]]:
    """Parse [sound]/[loopsound] wav paths (sound_runtime.cpp)."""
    if not sound_cfg_path.exists():
        return
    sound_dir = sound_cfg_path.parent
    mode = ""
    skip_lines = 0
    for raw in read_cfg_lines(sound_cfg_path):
        line = trim(raw)
        if not line or line.startswith("'"):
            continue
        if line.startswith("[") and line.endswith("]"):
            low = line.lower()
            if low in ("[sound]", "[loopsound]"):
                mode = low
                skip_lines = 0
            else:
                mode = ""
            continue
        if skip_lines > 0:
            skip_lines -= 1
            continue
        if mode == "[sound]":
            yield ("sound", resolve_sound_wav(sound_dir, line), "sound")
            skip_lines = 1  # volume line
            mode = ""
        elif mode == "[loopsound]":
            yield ("sound", resolve_sound_wav(sound_dir, line), "loopsound")
            skip_lines = 4  # rate, var, ref, vol
            mode = ""
        elif line.lower().endswith((".wav", ".ogg", ".mp3")):
            yield ("sound", resolve_sound_wav(sound_dir, line), "sound_inline")


def expand_model_cfg(cfg_path: Path, omsi_root: Path, base: Path) -> Iterator[tuple[str, Path, str]]:
    if not cfg_path.exists():
        return
    lines = read_cfg_lines(cfg_path)
    cfg_dir = cfg_path.parent

    i = 0
    while i < len(lines):
        raw = lines[i]
        if not raw or raw[0] != "[":
            i += 1
            continue
        sec = trim(raw).strip("[]").lower()
        if sec == "mesh":
            j = i + 1
            while j < len(lines) and not trim(lines[j]):
                j += 1
            if j < len(lines) and not trim(lines[j]).startswith("["):
                rel = trim(lines[j])
                if is_asset_path(rel):
                    yield ("mesh", resolve_mesh_path(cfg_path, rel, omsi_root), "model_mesh")
        elif sec == "ctc":
            vals: list[str] = []
            j = i + 1
            while j < len(lines) and not (lines[j] and lines[j][0] == "["):
                if trim(lines[j]):
                    vals.append(trim(lines[j]))
                j += 1
            if len(vals) >= 2:
                yield from iter_ctc_cti(cfg_path, vals[1], omsi_root)
        elif sec == "ctctexture":
            vals = []
            j = i + 1
            while j < len(lines) and not (lines[j] and lines[j][0] == "["):
                if trim(lines[j]):
                    vals.append(trim(lines[j]))
                j += 1
            if len(vals) >= 2 and looks_like_texture(vals[1]):
                yield from yield_resolved_texture(cfg_path, vals[1], omsi_root, "ctctexture")
        elif sec in ("matl", "matl_change"):
            j = i + 1
            while j < len(lines) and not trim(lines[j]):
                j += 1
            if j < len(lines) and not trim(lines[j]).startswith("["):
                tex = trim(lines[j])
                if looks_like_texture(tex):
                    yield from yield_resolved_texture(cfg_path, tex, omsi_root, "model_matl")
        elif sec == "texttexture":
            vals = []
            j = i + 1
            while j < len(lines) and not (lines[j] and lines[j][0] == "["):
                if trim(lines[j]):
                    vals.append(trim(lines[j]))
                j += 1
            if len(vals) >= 2 and not vals[1].isdigit():
                font = vals[1]
                fp = join_omsi(omsi_root, font if font.lower().endswith(".oft") else f"Fonts\\{font}.oft")
                yield ("font", fp, "texttexture")
        i += 1

    # matl modifier sections (model.cfg / sco share grammar — Unit130)
    tag = ""
    i = 0
    while i < len(lines):
        raw = lines[i]
        if raw and raw[0] == "[":
            tag = trim(raw).lower()
            i += 1
            continue
        line = trim(raw) if raw else ""
        if line and tag in MATL_TEXTURE_TAGS:
            if looks_like_texture(line):
                yield from yield_resolved_texture(cfg_path, line, omsi_root, tag.strip("[]"))
            tag = ""
        i += 1


def expand_cfg_loose(cfg_path: Path, omsi_root: Path, base: Path) -> Iterator[tuple[str, Path, str]]:
    """Shallow cfg scan for ticketpack, moneysystem, hof, weather\\clouds.cfg, etc."""
    if not cfg_path.exists():
        return
    cfg_dir = cfg_path.parent
    for raw in read_cfg_lines(cfg_path):
        t = trim(raw)
        if not t or is_section(t):
            continue
        low = t.lower()
        if looks_like_texture(t):
            yield from yield_resolved_texture(cfg_path, t, omsi_root, "cfg_texture")
        elif low.endswith((".bus", ".ovh", ".sco", ".sli", ".o3d", ".x", ".osc", ".cti", ".cfg", ".hof")):
            ext = Path(t).suffix.lower()
            kind_map = {
                ".bus": "bus",
                ".ovh": "ovh",
                ".sco": "sco",
                ".sli": "sli",
                ".o3d": "mesh",
                ".x": "mesh",
                ".osc": "osc",
                ".cti": "cti",
                ".cfg": "cfg",
                ".hof": "hof",
            }
            yield (kind_map.get(ext, "cfg"), join_omsi(omsi_root, t, cfg_dir), "cfg_ref")
        elif low.endswith((".wav", ".ogg", ".mp3")):
            yield ("sound", join_omsi(omsi_root, t, cfg_dir), "cfg_sound")


def iter_weather_engine_deps(omsi_root: Path) -> Iterator[tuple[str, Path, str]]:
    """TWeather.Create / loadCloudType (_Unit123.c) — global, not per-map."""
    clouds = omsi_root / "weather" / "clouds.cfg"
    yield ("weather", clouds, "clouds_cfg")
    if clouds.exists():
        lines = read_cfg_lines(clouds)
        i = 0
        while i < len(lines):
            if ieq(trim(lines[i]), "[cloudtype]"):
                vals: list[str] = []
                j = i + 1
                while j < len(lines) and not is_section(trim(lines[j])) and len(vals) < 2:
                    if trim(lines[j]):
                        vals.append(trim(lines[j]))
                    j += 1
                if len(vals) >= 2 and looks_like_texture(vals[1]):
                    yield ("texture", join_omsi(omsi_root, vals[1]), "cloudtype")
                i = j
                continue
            i += 1
    for rel, role in (
        ("texture/raindrop.tga", "rain"),
        ("texture/snowflake.tga", "snow"),
        ("helper/clouds.o3d", "clouds_mesh"),
        ("helper/snowfall.x", "snow_mesh"),
    ):
        yield ("texture" if rel.endswith((".tga", ".dds", ".bmp")) else "mesh", omsi_root / rel, role)


def iter_timetable_deps(map_root: Path, omsi_root: Path) -> Iterator[tuple[str, Path, str]]:
    """map\\TTData\\*.ttl / *.ttp (_Unit121 timetable load)."""
    ttdata = map_root / "TTData"
    if not ttdata.is_dir():
        return
    path_re = re.compile(
        r'[\w\\./:-]+\.(?:bus|ovh|sco|hof|cfg|bmp|dds|tga|png|jpg|osc)\b',
        re.I,
    )
    for p in sorted(ttdata.rglob("*")):
        if not p.is_file():
            continue
        ext = p.suffix.lower()
        if ext in (".ttl", ".ttp", ".hof", ".cfg", ".txt"):
            yield ("timetable", p, "ttdata")
            try:
                text = p.read_text(encoding="latin-1", errors="replace")
            except OSError:
                continue
            for m in path_re.finditer(text):
                ref = m.group(0).strip('"\'')
                if not ref or ref.startswith("["):
                    continue
                e = Path(ref).suffix.lower()
                kind = {
                    ".bus": "bus",
                    ".ovh": "ovh",
                    ".sco": "sco",
                    ".hof": "hof",
                    ".osc": "osc",
                    ".cfg": "cfg",
                }.get(e, "texture" if looks_like_texture(ref) else "cfg")
                yield (kind, join_omsi(omsi_root, ref, p.parent), "tt_ref")


def iter_bus_hof_deps(bus_path: Path) -> Iterator[tuple[str, Path, str]]:
    """TRoadVehicle loads *.hof beside .bus (_Unit129.pas @ 0x007CE1DB)."""
    if not bus_path.exists():
        return
    for hof in sorted(bus_path.parent.glob("*.hof")):
        yield ("hof", hof, "bus_hof")


def tile_aerial_path(omsi_root: Path, map_root: Path, tile_path: Path) -> Path:
    """TMapKachel.GetAerialFileName → maps\\aerials\\<map>\\<tile>.aerial.jpg"""
    return omsi_root / "maps" / "aerials" / map_root.name / f"{tile_path.stem}.aerial.jpg"


def expand_cti(cti_path: Path, omsi_root: Path, base: Path) -> Iterator[tuple[str, Path, str]]:
    if not cti_path.exists():
        return
    doc = CfgDoc(read_cfg_lines(cti_path))
    for block in doc.blocks("[item]"):
        if len(block) >= 3 and looks_like_texture(block[2]):
            yield from yield_resolved_texture(cti_path, block[2], omsi_root, "cti_item")


def expand_oft(oft_path: Path, omsi_root: Path, base: Path) -> Iterator[tuple[str, Path, str]]:
    """[newfont] color_file + alpha_file beside the .oft (oft_font.cpp)."""
    if not oft_path.exists():
        return
    oft_dir = oft_path.parent
    lines = read_cfg_lines(oft_path)
    i = 0
    while i < len(lines):
        if ieq(trim(lines[i]), "[newfont]"):
            vals: list[str] = []
            j = i + 1
            while j < len(lines) and len(vals) < 3:
                t = trim(lines[j])
                if t.startswith("["):
                    break
                if t:
                    vals.append(t)
                j += 1
            for rel in vals[1:3]:
                if not rel or (rel.startswith("{") and rel.endswith("}")):
                    continue
                p = oft_dir / rel
                if not p.is_file():
                    p = oft_dir / Path(rel).name
                yield ("texture", p.resolve() if p.is_file() else (oft_dir / Path(rel).name), "oft_image")
            i = j
            continue
        i += 1


def expand_osc(osc_path: Path, omsi_root: Path, base: Path, depth: int = 0) -> Iterator[tuple[str, Path, str]]:
    if depth > 6 or not osc_path.exists():
        return
    try:
        text = osc_path.read_text(encoding="latin-1", errors="replace")
    except OSError:
        return
    osc_dir = osc_path.parent
    for m in re.finditer(r'^\s*#include\s+["\']?([^"\'\s]+)["\']?', text, re.I | re.M):
        inc = m.group(1).strip()
        inc_path = join_omsi(omsi_root, inc, osc_dir)
        yield ("osc", inc_path, "include")
        yield from expand_osc(inc_path, omsi_root, base, depth + 1)
    for m in re.finditer(r'["\']([^"\']+\.(?:cfg|wav|ogg|mp3))["\']', text, re.I):
        ref = m.group(1)
        kind = "cfg" if ref.lower().endswith(".cfg") else "sound"
        yield (kind, join_omsi(omsi_root, ref, osc_dir), "osc_string")


def expand_ovh(ovh_path: Path, omsi_root: Path, base: Path) -> Iterator[tuple[str, Path, str]]:
    """AI vehicle .ovh — similar numbered lists to .bus."""
    yield from expand_bus(ovh_path, omsi_root, base)  # same cfg grammar for many fields


def expand_hum(hum_path: Path, omsi_root: Path, base: Path) -> Iterator[tuple[str, Path, str]]:
    """[model] in .hum points at model.cfg (THumanBeing.LoadFromFile)."""
    if not hum_path.exists():
        return
    doc = CfgDoc(read_cfg_lines(hum_path))
    rel = doc.first_line("[model]")
    if not rel:
        return
    cfg_path = join_omsi(omsi_root, rel, hum_path.parent)
    ext = cfg_path.suffix.lower()
    kind = "cfg" if ext == ".cfg" else "mesh" if ext in MESH_EXTS else kind_for_ref(rel)
    yield (kind, cfg_path, "human_model")


# O3D binary — chunk 0x26 materials (Pascal string texture names, o3d_loader.cpp)
_O3D_MAGIC = 0x1984
_O3D_CHUNK_VERT = 0x17
_O3D_CHUNK_TRI = 0x49
_O3D_CHUNK_MAT = 0x26
_O3D_CHUNK_BONE = 0x54
_O3D_CHUNK_XFORM = 0x79
_O3D_MAT_FIXED = 44


def _o3d_read_count(data: bytes, off: int, long_header: bool) -> tuple[int, int]:
    if long_header:
        if off + 4 > len(data):
            return 0, off
        return struct.unpack_from("<I", data, off)[0], off + 4
    if off + 2 > len(data):
        return 0, off
    return struct.unpack_from("<H", data, off)[0], off + 2


def parse_o3d_material_textures(o3d_path: Path) -> list[str]:
    """Return diffuse texture names from [matl] chunk 0x26 (not regex on whole file)."""
    try:
        data = o3d_path.read_bytes()
    except OSError:
        return []
    if len(data) < 3:
        return []
    magic, ver = struct.unpack_from("<HB", data, 0)
    if magic != _O3D_MAGIC or ver > 7:
        return []
    off = 3
    long_header = ver > 3
    long_tris = False
    if ver >= 3:
        options = data[off]
        off += 1
        long_tris = (options & 1) != 0
        if ver >= 4:
            enc = struct.unpack_from("<I", data, off)[0]
            off += 4
            if enc != 0xFFFFFFFF:
                return []  # encrypted — cannot read materials
    names: list[str] = []
    while off < len(data):
        tag = data[off]
        off += 1
        if tag == _O3D_CHUNK_VERT:
            cnt, off = _o3d_read_count(data, off, long_header)
            off += cnt * 32
        elif tag == _O3D_CHUNK_TRI:
            cnt, off = _o3d_read_count(data, off, long_header)
            stride = 14 if long_tris else 8
            off += cnt * stride
        elif tag == _O3D_CHUNK_MAT:
            if off + 2 > len(data):
                break
            mat_n = struct.unpack_from("<H", data, off)[0]
            off += 2
            for _ in range(mat_n):
                off += _O3D_MAT_FIXED
                if off >= len(data):
                    break
                slen = data[off]
                off += 1
                if off + slen > len(data):
                    break
                tex = data[off : off + slen].decode("latin-1", errors="replace").strip()
                off += slen
                if tex:
                    names.append(tex)
        elif tag == _O3D_CHUNK_BONE:
            if off + 2 > len(data):
                break
            bone_n = struct.unpack_from("<H", data, off)[0]
            off += 2
            pair = 8 if long_tris else 6
            for _ in range(bone_n):
                if off >= len(data):
                    break
                nlen = data[off]
                off += 1 + nlen
                if off + 2 > len(data):
                    break
                wn = struct.unpack_from("<H", data, off)[0]
                off += 2 + wn * pair
        elif tag == _O3D_CHUNK_XFORM:
            off += 64
        else:
            break
    return names


def expand_o3d(o3d_path: Path, omsi_root: Path, base: Path) -> Iterator[tuple[str, Path, str]]:
    """Parse chunk 0x26 materials (.o3d) or TextureFilename blocks (.x)."""
    if not o3d_path.exists():
        return
    if o3d_path.suffix.lower() == ".x":
        from sim_streetsign_engine import load_all_x_meshes  # noqa: WPS433

        seen_tex: set[str] = set()
        for xm in load_all_x_meshes(o3d_path):
            tex = xm.texture_filename.strip()
            if not tex or tex.lower() in seen_tex:
                continue
            seen_tex.add(tex.lower())
            yield from yield_resolved_texture(o3d_path, tex, omsi_root, "x_matl")
        return
    for tex_name in parse_o3d_material_textures(o3d_path):
        yield from yield_resolved_texture(o3d_path, tex_name, omsi_root, "o3d_matl")


# ---------------------------------------------------------------------------
# Map / global parsers
# ---------------------------------------------------------------------------

@dataclass
class GlobalMapInfo:
    root: Path
    global_cfg: Path
    ground_textures: list[tuple[str, str]]  # diffuse, detail
    tiles: list[tuple[int, int, Path]]
    ticket_pack: str = ""
    money_system: str = ""
    scen_obj_list: list[str] = field(default_factory=list)
    spline_obj_types: list[str] = field(default_factory=list)
    background_image: str = ""
    standard_depot: str = ""


def parse_global_cfg(global_cfg: Path) -> GlobalMapInfo:
    lines = read_cfg_lines(global_cfg)
    root = global_cfg.parent
    info = GlobalMapInfo(root=root, global_cfg=global_cfg, ground_textures=[], tiles=[])

    section = ""
    buf: list[str] = []
    ep_mode = False
    ep_lines: list[str] = []

    def flush_section() -> None:
        nonlocal buf, section
        if not section:
            return
        tag = section.lower()
        if tag == "[map]" and len(buf) >= 3:
            gx, gy, rel = int(buf[0]), int(buf[1]), buf[2]
            info.tiles.append((gx, gy, (root / rel).resolve()))
        elif tag == "[groundtex]" and buf:
            diffuse = buf[0]
            detail = buf[1] if len(buf) > 1 else ""
            info.ground_textures.append((diffuse, detail))
        elif tag == "[ticketpack]" and buf:
            info.ticket_pack = buf[0]
        elif tag == "[moneysystem]" and buf:
            info.money_system = buf[0]
        elif tag == "[standarddepot]" and buf:
            info.standard_depot = buf[0]
        elif tag == "[scenobjlist]":
            info.scen_obj_list.extend(parse_counted_paths(buf))
        elif tag == "[splineobjtypes]":
            info.spline_obj_types.extend(parse_counted_paths(buf))
        elif tag == "[backgroundimage]" and buf:
            for ln in buf:
                if ln and not ln.isdigit() and "." in ln:
                    info.background_image = ln
                    break
        buf = []

    for raw in lines:
        line = trim(raw)
        if ieq(line, "[entrypoints]"):
            ep_mode = True
            continue
        if ep_mode:
            if is_section(line):
                ep_mode = False
            else:
                ep_lines.append(raw)
                continue
        if is_section(line):
            flush_section()
            section = line
            if tag_flat := line.lower():
                if tag_flat in ("[worldcoordinates]", "[dynhelperactive]", "[realrail]", "[lht]"):
                    section = ""
            continue
        if not section:
            continue
        if section.lower() == "[map]":
            if line:
                buf.append(line)
                if len(buf) >= 3:
                    flush_section()
                    section = ""
            continue
        if line or section.lower() == "[backgroundimage]":
            buf.append(line)
    flush_section()

    if not info.tiles:
        for p in sorted(root.glob("tile_*.map")):
            stem = p.stem
            if stem.startswith("tile_"):
                parts = stem[5:].split("_", 1)
                if len(parts) == 2:
                    info.tiles.append((int(parts[0]), int(parts[1]), p.resolve()))

    return info


def extract_tree_texture(body: list[str]) -> Optional[str]:
    """Tree texture from tile [object] body (omsi_map_loader.cpp push_object)."""
    if len(body) < 10:
        return None

    def as_int(s: str) -> Optional[int]:
        try:
            return int(s)
        except ValueError:
            return None

    v9 = as_int(body[9])
    if v9 == 4 and len(body) >= 13 and looks_like_texture(body[10]):
        return body[10]
    if v9 is not None and 0 < v9 <= 32 and len(body) >= 10 + v9:
        if looks_like_texture(body[10]):
            return body[10]
    if len(body) >= 13 and looks_like_texture(body[10]):
        return body[10]
    return None


def parse_tile_map(tile_path: Path, omsi_root: Path) -> dict[str, list]:
    """File-order entity parse (omsi_map_loader.cpp)."""
    lines = read_cfg_lines(tile_path)
    out: dict[str, list] = {
        "objects": [],
        "attachments": [],
        "splines": [],
        "spline_attachments": [],
        "tree_textures": [],
    }
    i = 0
    spline_index = -1

    def read_block(start: int) -> tuple[list[str], int]:
        body: list[str] = []
        j = start
        while j < len(lines):
            t = trim(lines[j])
            if is_section(t):
                break
            if t:
                body.append(t)
            j += 1
        return body, j

    while i < len(lines):
        tag = trim(lines[i])
        if not is_section(tag):
            i += 1
            continue
        low = tag.lower()
        body, ni = read_block(i + 1)
        i = ni

        if low in ("[object]", "[scenobj]") and len(body) >= 2:
            sco = body[1]
            out["objects"].append(sco)
            tree_tex = extract_tree_texture(body)
            if tree_tex:
                out["tree_textures"].append((sco, tree_tex))
        elif low == "[attachobj]" and len(body) >= 2:
            out["attachments"].append(body[1])
        elif low in ("[spline]", "[spline_h]") and len(body) >= 2:
            spline_index += 1
            out["splines"].append(body[1])
        elif low in ("[splineattachement]", "[splineattachement_repeater]") and len(body) >= 2:
            out["spline_attachments"].append(body[1])
    return out


_HUMAN_EXTS = {".sco", ".cfg", ".ovh", ".bus", ".o3d", ".x", ".hum"}


def kind_for_ref(ref: str) -> str:
    ext = Path(ref).suffix.lower()
    return {
        ".sco": "sco",
        ".sli": "sli",
        ".cfg": "cfg",
        ".ovh": "ovh",
        ".bus": "bus",
        ".o3d": "mesh",
        ".x": "mesh",
        ".cti": "cti",
        ".oft": "font",
        ".hum": "hum",
    }.get(ext, "sco")


def parse_humans_txt(path: Path, omsi_root: Path) -> list[str]:
    """humans.txt: .sco / model.cfg / .ovh / meshes (THumanBeing.LoadFromFile)."""
    lines = read_cfg_lines(path)
    paths: list[str] = []
    for raw in lines:
        t = trim(raw)
        if not t or is_section(t):
            continue
        low = t.lower()
        ext = Path(t).suffix.lower()
        if (
            ext in _HUMAN_EXTS
            or "\\" in t
            or low.startswith(("humans", "sceneryobjects", "vehicles"))
        ):
            paths.append(t)
    return paths


def parse_drivers_txt(path: Path, omsi_root: Path) -> list[str]:
    lines = read_cfg_lines(path)
    paths: list[str] = []
    for raw in lines:
        t = trim(raw)
        if not t or is_section(t):
            continue
        low = t.lower()
        if low.endswith((".bus", ".ovh", ".sco", ".o3d", ".x", ".cfg")) or "vehicles" in low.replace("/", "\\"):
            paths.append(t)
    return paths


def parse_ailists_cfg(path: Path, omsi_root: Path) -> list[str]:
    """ailists.cfg: vehicle pack folders / .bus paths."""
    lines = read_cfg_lines(path)
    paths: list[str] = []
    for raw in lines:
        t = trim(raw)
        if not t or is_section(t):
            continue
        if t.endswith("\\") or "vehicles" in t.lower().replace("/", "\\") or t.lower().endswith(".bus"):
            paths.append(t)
    return paths


HELPER_FILES = [
    "helper/path.sli",
    "helper/skybox.X",
    "helper/sonne.x",
    "helper/mond.x",
    "helper/stars.dat",
    "helper/ekliptik.x",
    "helper/clouds.o3d",
    "helper/snowfall.x",
    "helper/splinepnt.X",
    "helper/snappoint.x",
    "helper/hbusstop.o3d",
    "helper/trafficlight.o3d",
    "texture/path_aicars.bmp",
    "texture/path_aihumans.bmp",
    "texture/path_aicars_bi.bmp",
]


# ---------------------------------------------------------------------------
# Scanner
# ---------------------------------------------------------------------------

class MapDependencyScanner:
    EXPANDERS: dict[str, ExpandFn] = {}

    def __init__(
        self,
        omsi_root: Path,
        global_cfg: Path,
        *,
        include_catalog: bool = True,
        include_helper: bool = False,
        include_engine: bool = False,
        include_timetable: bool = True,
        include_ai_buses: bool = True,
        max_depth: int = 12,
    ) -> None:
        self.omsi_root = omsi_root.resolve()
        self.global_cfg = global_cfg.resolve()
        self.include_catalog = include_catalog
        self.include_helper = include_helper
        self.include_engine = include_engine
        self.include_timetable = include_timetable
        self.include_ai_buses = include_ai_buses
        self.max_depth = max_depth
        self.g = DepGraph()
        self._visited: set[str] = set()
        self._queue: deque[tuple[str, Path, tuple[str, Path], str, int]] = deque()

        self.EXPANDERS = {
            "sco": expand_sco,
            "sli": expand_sli,
            "bus": expand_bus,
            "ovh": expand_ovh,
            "cfg": expand_model_cfg,
            "sound_cfg": expand_sound_cfg,
            "cti": expand_cti,
            "osc": expand_osc,
            "hof": expand_cfg_loose,
            "ticketpack": expand_cfg_loose,
            "money": expand_cfg_loose,
            "weather": expand_cfg_loose,
            "timetable": expand_cfg_loose,
            "mesh": expand_o3d,
            "o3d": expand_o3d,
            "x": expand_o3d,
            "font": expand_oft,
            "hum": expand_hum,
        }

    def _enqueue(self, kind: str, path: Path, parent: tuple[str, Path], role: str, depth: int) -> None:
        if depth > self.max_depth:
            return
        if not path or str(path) == ".":
            return
        key = f"{kind}:{norm_key(path)}"
        if key in self._visited:
            self.g.add(kind, path, parent, role)
            return
        self._visited.add(key)
        self.g.add(kind, path, parent, role)
        self._queue.append((kind, path, parent, role, depth))

    def _walk_file(self, kind: str, path: Path, parent: tuple[str, Path], role: str, depth: int) -> None:
        self._enqueue(kind, path, parent, role, depth)
        fn = self.EXPANDERS.get(kind)
        if not fn:
            # extension-based routing
            ext = path.suffix.lower()
            if ext == ".sco":
                fn = expand_sco
            elif ext == ".sli":
                fn = expand_sli
            elif ext == ".bus":
                fn = expand_bus
            elif ext == ".ovh":
                fn = expand_ovh
            elif ext in (".o3d", ".x"):
                fn = expand_o3d
            elif ext == ".osc":
                fn = expand_osc
            elif ext == ".cti":
                fn = expand_cti
            elif ext == ".oft":
                fn = expand_oft
            elif ext == ".hum":
                fn = expand_hum
            elif ext.endswith("cfg") or ext == ".cfg":
                if "sound" in path.name.lower() or "sound" in role.lower():
                    fn = expand_sound_cfg
                else:
                    fn = expand_model_cfg
            else:
                return
        try:
            for ck, cp, cr in fn(path, self.omsi_root, parent[1]):
                self._enqueue(ck, cp, (kind, path), cr, depth + 1)
        except Exception as exc:
            sys.stderr.write(f"warn: expand {path}: {exc}\n")

    def scan(self) -> DepGraph:
        info = parse_global_cfg(self.global_cfg)
        parent_gc = ("global_cfg", self.global_cfg)
        self._enqueue("global_cfg", self.global_cfg, parent_gc, "root", 0)

        # Side files (Unit126)
        for side, role in (
            ("Holidays.txt", "holidays"),
            ("timezone.txt", "timezone"),
            ("humans.txt", "humans"),
            ("drivers.txt", "drivers"),
        ):
            p = info.root / side
            if p.exists():
                self._enqueue("txt", p, parent_gc, role, 1)

        for ailist in sorted(info.root.glob("ailists*.cfg")):
            self._enqueue("cfg", ailist, parent_gc, "ailists", 1)

        if info.ticket_pack:
            self._enqueue("ticketpack", join_omsi(self.omsi_root, info.ticket_pack), parent_gc, "ticketpack", 1)
        if info.money_system:
            self._enqueue("money", join_omsi(self.omsi_root, info.money_system), parent_gc, "moneysystem", 1)
        if info.standard_depot:
            dep = join_omsi(self.omsi_root, info.standard_depot, info.root)
            self._enqueue("cfg", dep, parent_gc, "standarddepot", 1)
            if dep.is_dir():
                for hof in dep.rglob("*.hof"):
                    self._enqueue("hof", hof, parent_gc, "depot_hof", 2)

        for opl in sorted(info.root.glob("*.opl")):
            self._enqueue("scenario", opl, parent_gc, "opl", 1)

        if self.include_timetable:
            for kind, path, role in iter_timetable_deps(info.root, self.omsi_root):
                self._enqueue(kind, path, parent_gc, role, 1)

        if self.include_engine:
            for kind, path, role in iter_weather_engine_deps(self.omsi_root):
                self._enqueue(kind, path, parent_gc, role, 1)

        if info.background_image:
            for cand in (info.root / info.background_image, info.root / "texture" / info.background_image):
                self._enqueue("texture", cand, parent_gc, "background", 1)
            self._enqueue("texture", join_omsi(self.omsi_root, info.background_image), parent_gc, "background", 1)

        for diffuse, detail in info.ground_textures:
            for tex, role in ((diffuse, "groundtex_diffuse"), (detail, "groundtex_detail")):
                if not tex:
                    continue
                for kind, p, r in yield_resolved_texture(self.global_cfg, tex, self.omsi_root, role):
                    self._enqueue(kind, p, parent_gc, r, 1)

        if self.include_catalog:
            for sco in info.scen_obj_list:
                self._enqueue("sco", join_omsi(self.omsi_root, sco), parent_gc, "scenObjList", 1)
            for sli in info.spline_obj_types:
                self._enqueue("sli", join_omsi(self.omsi_root, sli), parent_gc, "splineObjTypes", 1)

        if self.include_helper or self.include_engine:
            for rel in HELPER_FILES:
                self._enqueue("helper", self.omsi_root / rel, parent_gc, "helper", 1)

        # Tiles
        for gx, gy, tile_path in info.tiles:
            parent_tile = ("tile_map", tile_path)
            self._enqueue("tile_map", tile_path, parent_gc, f"tile[{gx},{gy}]", 1)
            terrain = Path(str(tile_path) + ".terrain")
            if terrain.exists():
                self._enqueue("terrain", terrain, parent_tile, "terrain", 2)
            lm = Path(str(tile_path) + ".LM.bmp")
            if lm.exists():
                self._enqueue("texture", lm, parent_tile, "lightmap", 2)
            aerial = tile_aerial_path(self.omsi_root, info.root, tile_path)
            if aerial.exists():
                self._enqueue("texture", aerial, parent_tile, "aerial", 2)

            parsed = parse_tile_map(tile_path, self.omsi_root)
            for sco in parsed["objects"] + parsed["attachments"] + parsed["spline_attachments"]:
                self._enqueue("sco", join_omsi(self.omsi_root, sco), parent_tile, "instance", 2)
            for sco, tex in parsed["tree_textures"]:
                sco_path = join_omsi(self.omsi_root, sco)
                for kind, p, r in yield_resolved_texture(sco_path, tex, self.omsi_root, f"tree:{sco}"):
                    self._enqueue(kind, p, parent_tile, r, 2)
            for sli in parsed["splines"]:
                self._enqueue("sli", join_omsi(self.omsi_root, sli), parent_tile, "spline", 2)

            # Ground paint masks texture/map/tile_x_y.map.N.dds
            mask_dir = info.root / "texture" / "map"
            if mask_dir.is_dir():
                for n in range(max(1, len(info.ground_textures))):
                    mask = mask_dir / f"{tile_path.name}.{n}.dds"
                    if mask.exists():
                        self._enqueue("texture", mask, parent_tile, f"ground_mask[{n}]", 2)

        # AI sidecars content
        humans_p = info.root / "humans.txt"
        if humans_p.exists():
            for ref in parse_humans_txt(humans_p, self.omsi_root):
                self._enqueue(
                    kind_for_ref(ref),
                    join_omsi(self.omsi_root, ref),
                    ("txt", humans_p),
                    "human_model",
                    2,
                )

        drivers_p = info.root / "drivers.txt"
        if drivers_p.exists():
            for ref in parse_drivers_txt(drivers_p, self.omsi_root):
                kind = kind_for_ref(ref)
                self._enqueue(kind, join_omsi(self.omsi_root, ref), ("txt", drivers_p), "driver", 2)

        if self.include_ai_buses:
            for ailist in sorted(info.root.glob("ailists*.cfg")):
                for ref in parse_ailists_cfg(ailist, self.omsi_root):
                    p = join_omsi(self.omsi_root, ref)
                    if p.is_dir():
                        for bus in p.rglob("*.bus"):
                            self._enqueue("bus", bus, ("cfg", ailist), "ailist_bus", 3)
                        for ovh in p.rglob("*.ovh"):
                            self._enqueue("ovh", ovh, ("cfg", ailist), "ailist_ovh", 3)
                    elif ref.lower().endswith(".bus"):
                        self._enqueue("bus", p, ("cfg", ailist), "ailist_bus", 3)
                    elif ref.lower().endswith(".ovh"):
                        self._enqueue("ovh", p, ("cfg", ailist), "ailist_ovh", 3)

        # BFS expand
        while self._queue:
            kind, path, parent, role, depth = self._queue.popleft()
            self._walk_file(kind, path, parent, role, depth)

        return self.g


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def to_json(g: DepGraph, info: GlobalMapInfo, omsi_root: Path) -> dict:
    """Pack-friendly manifest: unique paths relative to omsi_root (for map_dependency_pack.py)."""
    omsi_root = omsi_root.resolve()
    files: list[dict] = []
    missing: list[dict] = []
    seen: set[str] = set()

    def rel_path(p: Path) -> str:
        try:
            return str(p.resolve().relative_to(omsi_root)).replace("/", "\\")
        except ValueError:
            return str(p.resolve())

    for n in sorted(g.nodes.values(), key=lambda x: (x.kind, str(x.path))):
        rp = rel_path(n.path)
        key = rp.lower()
        if key in seen:
            continue
        seen.add(key)
        entry = {"rel": rp, "kind": n.kind, "exists": n.exists}
        if n.exists:
            files.append(entry)
        else:
            missing.append(entry)

    o3d_nodes = [n for n in g.nodes.values() if n.path.suffix.lower() == ".o3d" and n.exists]
    o3d_tex_edges = sum(1 for n in g.nodes.values() if any(e.role.startswith("o3d_matl") for e in n.edges_in))

    return {
        "version": 1,
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "omsi_root": str(omsi_root),
        "global_cfg": str(info.global_cfg),
        "map_root": str(info.root),
        "stats": g.stats(),
        "missing_count": len(missing),
        "file_count": len(files),
        "scan": {
            "o3d_meshes": len(o3d_nodes),
            "textures_from_o3d": o3d_tex_edges,
        },
        "files": files,
        "missing": missing,
    }


def to_text(g: DepGraph, info: GlobalMapInfo, missing_only: bool) -> str:
    lines = [
        f"Map: {info.global_cfg}",
        f"Root: {info.root}",
        f"Tiles: {len(info.tiles)}",
        f"Nodes: {len(g.nodes)}  Missing: {len(g.missing)}",
        "",
        "Stats by kind:",
    ]
    for k, v in g.stats().items():
        lines.append(f"  {k}: {v}")
    lines.append("")
    lines.append("Files:" if not missing_only else "Missing files:")
    for n in sorted(g.nodes.values(), key=lambda x: (x.kind, str(x.path))):
        if missing_only and n.exists:
            continue
        flag = "" if n.exists else " [MISSING]"
        lines.append(f"  [{n.kind}] {n.path}{flag}")
    return "\n".join(lines)


def find_omsi_root(start: Path) -> Optional[Path]:
    """Walk parents until Omsi.exe is found."""
    p = start.resolve()
    for _ in range(16):
        if (p / "Omsi.exe").is_file():
            return p
        parent = p.parent
        if parent == p:
            break
        p = parent
    return None


def _default_maps_dir() -> Path:
    for cand in (
        Path(r"C:\Program Files (x86)\Steam\steamapps\common\OMSI 2\maps"),
        Path(r"C:\Program Files (x86)\OMSI 2\maps"),
        Path(r"C:\Program Files\OMSI 2\maps"),
        Path.home() / "Documents",
    ):
        if cand.is_dir():
            return cand
    return Path.home()


def interactive_pick_paths() -> Optional[tuple[Path, Path, Path]]:
    """File dialogs: global.cfg → JSON output (omsi root auto-detected)."""
    try:
        import tkinter as tk
        from tkinter import filedialog, messagebox
    except ImportError:
        sys.stderr.write("error: tkinter no disponible; usá --map y -o por consola\n")
        return None

    root = tk.Tk()
    root.withdraw()
    try:
        root.attributes("-topmost", True)
    except tk.TclError:
        pass

    global_cfg_s = filedialog.askopenfilename(
        title="Elegí el global.cfg del mapa",
        filetypes=[
            ("global.cfg", "global.cfg"),
            ("Config OMSI", "*.cfg"),
            ("Todos", "*.*"),
        ],
        initialdir=str(_default_maps_dir()),
    )
    if not global_cfg_s:
        root.destroy()
        return None

    global_cfg = Path(global_cfg_s).resolve()
    omsi_root = find_omsi_root(global_cfg)
    if omsi_root is None:
        omsi_pick = filedialog.askdirectory(
            title="Carpeta raíz de OMSI 2 (donde está Omsi.exe)",
            initialdir=str(global_cfg.parent),
        )
        if not omsi_pick:
            root.destroy()
            return None
        omsi_root = Path(omsi_pick).resolve()
        if not (omsi_root / "Omsi.exe").is_file():
            messagebox.showerror(
                "OMSI 2",
                f"No se encontró Omsi.exe en:\n{omsi_root}",
            )
            root.destroy()
            return None

    default_json = global_cfg.parent / f"{global_cfg.parent.name}_deps.json"
    output_s = filedialog.asksaveasfilename(
        title="Guardar lista de dependencias (JSON)",
        defaultextension=".json",
        filetypes=[("JSON", "*.json"), ("Todos", "*.*")],
        initialdir=str(global_cfg.parent),
        initialfile=default_json.name,
    )
    if not output_s:
        root.destroy()
        return None

    root.destroy()
    return global_cfg, omsi_root, Path(output_s).resolve()


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="OMSI 2 map dependency scanner")
    ap.add_argument("--omsi-root", type=Path, help="OMSI 2 installation root (Omsi.exe)")
    ap.add_argument("--map", type=Path, help="Path to map global.cfg")
    ap.add_argument("-o", "--output", type=Path, help="Write report to file")
    ap.add_argument("--format", choices=("text", "json"), default=None)
    ap.add_argument("--missing-only", action="store_true")
    ap.add_argument("--no-catalog", action="store_true", help="Skip scenObjList / splineObjTypes")
    ap.add_argument("--include-helper", action="store_true", help="Include helper/ + path AI textures")
    ap.add_argument("--include-engine", action="store_true", help="Weather clouds.cfg, rain/snow FX, helper meshes")
    ap.add_argument("--no-timetable", action="store_true", help="Skip map/TTData/*.ttl *.ttp")
    ap.add_argument("--no-ai-buses", action="store_true", help="Skip ailists → Vehicles scan")
    ap.add_argument("--max-depth", type=int, default=12)
    ap.add_argument("--no-gui", action="store_true", help="No abrir diálogos aunque falte --map")
    args = ap.parse_args(argv)

    interactive = args.map is None and not args.no_gui
    if interactive:
        picked = interactive_pick_paths()
        if picked is None:
            return 1
        global_cfg, omsi_root, output_path = picked
        if args.format is None:
            args.format = "json"
        if args.output is None:
            args.output = output_path
    else:
        if args.omsi_root is None:
            sys.stderr.write("error: falta --omsi-root (o ejecutá sin --map para elegir por diálogo)\n")
            return 2
        if args.map is None:
            sys.stderr.write("error: falta --map\n")
            return 2
        omsi_root = args.omsi_root.resolve()
        global_cfg = args.map if args.map.is_absolute() else (omsi_root / args.map)
        global_cfg = global_cfg.resolve()
        if args.format is None:
            args.format = "text"

    if not omsi_root.is_dir():
        sys.stderr.write(f"error: omsi-root not found: {omsi_root}\n")
        return 2
    if not global_cfg.is_file():
        sys.stderr.write(f"error: global.cfg not found: {global_cfg}\n")
        return 2

    info = parse_global_cfg(global_cfg)
    scanner = MapDependencyScanner(
        omsi_root,
        global_cfg,
        include_catalog=not args.no_catalog,
        include_helper=args.include_helper,
        include_engine=args.include_engine,
        include_timetable=not args.no_timetable,
        include_ai_buses=not args.no_ai_buses,
        max_depth=args.max_depth,
    )
    g = scanner.scan()

    report: Optional[dict] = None
    if args.format == "json":
        report = to_json(g, info, omsi_root)
        out = json.dumps(report, indent=2, ensure_ascii=False)
    else:
        out = to_text(g, info, args.missing_only)

    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(out + "\n", encoding="utf-8")
        if report:
            msg = (
                f"Guardado: {args.output}\n"
                f"{report['file_count']} archivos, {report['missing_count']} faltantes\n"
                f"O3D: {report['scan']['o3d_meshes']} meshes -> "
                f"{report['scan']['textures_from_o3d']} texturas"
            )
        else:
            msg = f"Guardado: {args.output}\n{len(g.nodes)} nodos, {len(g.missing)} faltantes"
        print(msg)
        if interactive:
            try:
                import tkinter as tk
                from tkinter import messagebox

                r = tk.Tk()
                r.withdraw()
                messagebox.showinfo("Map dependency scanner", msg)
                r.destroy()
            except ImportError:
                pass
    else:
        print(out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
