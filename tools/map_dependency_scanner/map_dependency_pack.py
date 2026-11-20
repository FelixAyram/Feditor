#!/usr/bin/env python3
"""
Pack OMSI map dependencies into a store-only ZIP (no compression).

Reads manifest JSON from map_dependency_scanner.py (version 1):
  { "omsi_root": "...", "files": [ {"rel": "texture\\foo.dds", "exists": true}, ... ] }

Usage:
  python tools/map_dependency_pack.py path\\to\\map_deps.json
  python tools/map_dependency_pack.py   # diálogo: elegir .json y .zip
"""
from __future__ import annotations

import argparse
import json
import sys
import zipfile
from pathlib import Path
from typing import Optional


def load_manifest(json_path: Path) -> tuple[Path, list[str]]:
    data = json.loads(json_path.read_text(encoding="utf-8"))
    omsi_root = Path(data.get("omsi_root", "")).resolve()
    if not omsi_root.is_dir():
        raise SystemExit(f"omsi_root inválido en JSON: {omsi_root}")

    rels: list[str] = []
    if "files" in data:
        for entry in data["files"]:
            if isinstance(entry, dict) and entry.get("exists", True):
                rel = entry.get("rel", "")
                if rel:
                    rels.append(rel.replace("/", "\\"))
    elif "nodes" in data:
        # legacy scanner output
        for node in data["nodes"]:
            if not node.get("exists", False):
                continue
            p = Path(node["path"])
            try:
                rels.append(str(p.resolve().relative_to(omsi_root)).replace("/", "\\"))
            except ValueError:
                rels.append(str(p))
    else:
        raise SystemExit("JSON sin 'files' ni 'nodes'")

    # unique, stable order
    seen: set[str] = set()
    unique: list[str] = []
    for r in rels:
        k = r.lower()
        if k not in seen:
            seen.add(k)
            unique.append(r)
    return omsi_root, unique


def pack_zip(json_path: Path, zip_path: Path) -> tuple[int, int, int]:
    omsi_root, rels = load_manifest(json_path)
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    added = 0
    skipped = 0
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_STORED) as zf:
        for rel in rels:
            src = omsi_root / rel
            if not src.is_file():
                skipped += 1
                continue
            zf.write(src, rel)
            added += 1
        # manifest inside zip for traceability
        zf.write(json_path, json_path.name)
    return added, skipped, len(rels)


def interactive_pick() -> Optional[tuple[Path, Path]]:
    try:
        import tkinter as tk
        from tkinter import filedialog
    except ImportError:
        return None
    root = tk.Tk()
    root.withdraw()
    try:
        root.attributes("-topmost", True)
    except Exception:
        pass
    jp = filedialog.askopenfilename(
        title="Elegí el manifest .json del escáner",
        filetypes=[("JSON", "*.json"), ("Todos", "*.*")],
    )
    if not jp:
        root.destroy()
        return None
    json_path = Path(jp)
    zp = filedialog.asksaveasfilename(
        title="Guardar ZIP (sin compresión)",
        defaultextension=".zip",
        initialfile=json_path.with_suffix(".zip").name,
        initialdir=str(json_path.parent),
        filetypes=[("ZIP", "*.zip")],
    )
    root.destroy()
    if not zp:
        return None
    return json_path, Path(zp)


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Empaquetar dependencias OMSI en ZIP store-only")
    ap.add_argument("json", nargs="?", type=Path, help="Manifest .json del escáner")
    ap.add_argument("-o", "--output", type=Path, help="Salida .zip (default: mismo nombre que .json)")
    args = ap.parse_args(argv)

    if args.json is None:
        picked = interactive_pick()
        if picked is None:
            sys.stderr.write("error: cancelado o sin tkinter\n")
            return 1
        json_path, zip_path = picked
    else:
        json_path = args.json.resolve()
        zip_path = (args.output or json_path.with_suffix(".zip")).resolve()

    if not json_path.is_file():
        sys.stderr.write(f"error: no existe {json_path}\n")
        return 2

    added, skipped, total = pack_zip(json_path, zip_path)
    print(f"ZIP: {zip_path}")
    print(f"Archivos: {added} empaquetados, {skipped} faltantes (de {total} en manifest)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
