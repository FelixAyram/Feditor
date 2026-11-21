"""Load / save OMSI .sli (same tags as src/spline/sli_loader.cpp)."""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class HeightProfile:
    left: float = 0.0
    right: float = 0.0
    height: float = 0.0
    skew: float = 0.0


@dataclass
class ProfilePoint:
    x: float = 0.0
    z: float = 0.0
    uv_x: float = 0.0
    y_tiling: float = 1.0
    material_id: int = 0


@dataclass
class SliPath:
    typ: int = 1
    lateral: float = 0.0
    height: float = 0.1
    width: float = 3.0
    direction: int = 2
    secondary: bool = False


@dataclass
class SplineDefinition:
    sli_path: Path | None = None
    origin_sli: Path | None = None  # carpeta de donde se abrió (texturas); no cambia al "guardar como"
    height_profiles: list[HeightProfile] = field(default_factory=list)
    profiles: list[list[ProfilePoint]] = field(default_factory=list)
    profile_comments: list[str] = field(default_factory=list)
    textures: list[str] = field(default_factory=list)
    texture_alpha: list[int] = field(default_factory=list)
    paths: list[SliPath] = field(default_factory=list)
    only_editor: bool = False
    tex_sources: dict[str, Path] = field(default_factory=dict)


def _trim(s: str) -> str:
    return s.strip(" \t\r\n")


def _ieq(a: str, b: str) -> bool:
    return a.lower() == b.lower()


def empty_def() -> SplineDefinition:
    d = SplineDefinition()
    d.height_profiles = [HeightProfile(0.0, 6.0, 0.1, 0.1)]
    d.profiles = [
        [
            ProfilePoint(0.0, 0.1, 0.0, 0.2, 0),
            ProfilePoint(3.0, 0.1, 1.0, 0.2, 0),
        ]
    ]
    d.profile_comments = [""]
    d.paths = [SliPath()]
    return d


def load_sli(path: Path) -> SplineDefinition | None:
    try:
        raw = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    lines = raw.splitlines()
    d = SplineDefinition(sli_path=path, origin_sli=path)
    tag = ""
    pending_comment: str | None = None
    cur_mat = 0
    i = 0
    while i < len(lines):
        line = _trim(lines[i])
        if not line:
            i += 1
            continue
        if line.startswith("[") and line.endswith("]"):
            tag = line
            if _ieq(tag, "[onlyeditor]"):
                d.only_editor = True
            if _ieq(tag, "[matl]"):
                tag = ""
            i += 1
            continue
        if not tag and line.startswith("###"):
            end = line.rfind("###")
            if end > 3:
                pending_comment = _trim(line[3:end])
            i += 1
            continue
        if _ieq(tag, "[heightprofile]"):
            try:
                hp = HeightProfile(
                    float(line),
                    float(_trim(lines[i + 1])),
                    float(_trim(lines[i + 2])),
                    float(_trim(lines[i + 3])),
                )
                d.height_profiles.append(hp)
                i += 4
            except (ValueError, IndexError):
                i += 1
            tag = ""
            continue
        if _ieq(tag, "[profile]"):
            try:
                cur_mat = int(float(line))
            except ValueError:
                cur_mat = 0
            d.profiles.append([])
            d.profile_comments.append(pending_comment or "")
            pending_comment = None
            tag = ""
            i += 1
            continue
        if _ieq(tag, "[profilepnt]"):
            if not d.profiles:
                d.profiles.append([])
                d.profile_comments.append(pending_comment or "")
                pending_comment = None
            try:
                pt = ProfilePoint(
                    float(line),
                    float(_trim(lines[i + 1])),
                    float(_trim(lines[i + 2])),
                    float(_trim(lines[i + 3])),
                    cur_mat,
                )
                d.profiles[-1].append(pt)
                i += 4
            except (ValueError, IndexError):
                i += 1
            tag = ""
            continue
        if _ieq(tag, "[texture]"):
            d.textures.append(line)
            d.texture_alpha.append(0)
            tag = ""
            i += 1
            continue
        if _ieq(tag, "[matl_alpha]"):
            try:
                mode = max(0, min(2, int(float(line))))
            except ValueError:
                mode = 0
            if d.texture_alpha:
                d.texture_alpha[-1] = mode
            tag = ""
            i += 1
            continue
        if _ieq(tag, "[path]") or _ieq(tag, "[path_2]"):
            p = SliPath(secondary=_ieq(tag, "[path_2]"))
            try:
                p.typ = int(float(line))
                p.lateral = float(_trim(lines[i + 1]))
                p.height = float(_trim(lines[i + 2]))
                p.width = float(_trim(lines[i + 3]))
                p.direction = int(float(_trim(lines[i + 4])))
                i += 5
            except (ValueError, IndexError):
                i += 1
            d.paths.append(p)
            tag = ""
            continue
        i += 1
    return d


def format_sli(d: SplineDefinition) -> str:
    out: list[str] = []
    if d.height_profiles:
        out += ["---------------------------", "      Height Profiles", "---------------------------", ""]
        for hp in d.height_profiles:
            out += [
                "[heightprofile]",
                f"{hp.left:.3f}",
                f"{hp.right:.3f}",
                f"{hp.height:.3f}",
                f"{hp.skew:.3f}",
                "",
            ]
    if d.textures:
        out += ["---------------------------", "         Textures", "---------------------------", ""]
        for ti, name in enumerate(d.textures):
            out += ["[texture]", name, ""]
            if ti < len(d.texture_alpha) and d.texture_alpha[ti]:
                out += ["[matl_alpha]", str(d.texture_alpha[ti]), ""]
    if d.profiles:
        out += ["---------------------------", "     Graphical Lanes", "---------------------------", ""]
        for pi, strip in enumerate(d.profiles):
            if not strip:
                continue
            if pi < len(d.profile_comments) and d.profile_comments[pi]:
                out += [f"### {d.profile_comments[pi]} ###", ""]
            out += ["[profile]", str(strip[0].material_id), ""]
            for pt in strip:
                out += [
                    "[profilepnt]",
                    f"{pt.x:.3f}",
                    f"{pt.z:.3f}",
                    f"{pt.uv_x:.3f}",
                    f"{pt.y_tiling:.3f}",
                    "",
                ]
    if d.paths:
        out += ["---------------------------", "          Paths", "---------------------------", ""]
        for p in d.paths:
            out += [
                "[path_2]" if p.secondary else "[path]",
                str(p.typ),
                f"{p.lateral:.3f}",
                f"{p.height:.3f}",
                f"{p.width:.3f}",
                str(p.direction),
                "",
            ]
    if d.only_editor:
        out += ["[onlyeditor]", ""]
    return "\n".join(out) + "\n"


def write_sli(d: SplineDefinition, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(format_sli(d), encoding="utf-8")
    d.sli_path = path
