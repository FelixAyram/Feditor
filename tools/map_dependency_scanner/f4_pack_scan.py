#!/usr/bin/env python3
"""
Editor bridge for map_dependency_scanner.py (Archivo > Empaquetar mapa in the OMSI F4 editor).

Runs the same scan as the scanner GUI / CLI and writes one line per file:
    <rel path below OMSI root>\t<categories>\t<exists 1|0>
categories (comma separated): map, objects, splines, ai, depot - where in the map the file is
used from (a texture shared by an object and a spline gets both). The editor filters by the
categories the user ticked, drops Steam content and zips the rest.

Progress lines on stdout: "PROGRESS <text>".
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.setrecursionlimit(10000)

import map_dependency_scanner as mds  # noqa: E402

# role of the edge from the map -> category of the whole subtree below it
ROLE_CAT = {
    "instance": "objects",
    "scenObjList": "objects",
    "spline": "splines",
    "splineObjTypes": "splines",
    "ailist_bus": "ai",
    "ailist_ovh": "ai",
    "human_model": "ai",
    "driver": "ai",
    "ailists": "ai",
    "humans": "ai",
    "drivers": "ai",
    "standarddepot": "depot",
    "depot_hof": "depot",
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--omsi-root", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--no-ai-buses", action="store_true")
    ap.add_argument("--no-catalog", action="store_true")
    a = ap.parse_args()

    root = Path(a.omsi_root).resolve()
    cfg = Path(a.map).resolve()
    print("PROGRESS escaneando dependencias...", flush=True)
    sc = mds.MapDependencyScanner(
        root,
        cfg,
        include_catalog=not a.no_catalog,
        include_ai_buses=not a.no_ai_buses,
    )
    g = sc.scan()
    print(f"PROGRESS clasificando {len(g.nodes)} archivos...", flush=True)

    memo: dict[str, frozenset] = {}

    def cats(key: str) -> frozenset:
        # iterative walk up the edges (role decides the category, the map root is "map")
        if key in memo:
            return memo[key]
        out: set[str] = set()
        seen = {key}
        stack = [key]
        while stack:
            k = stack.pop()
            if k in memo and k != key:
                out |= memo[k]
                continue
            n = g.nodes.get(k)
            if n is None or not n.edges_in:
                out.add("map")
                continue
            for e in n.edges_in:
                c = ROLE_CAT.get(e.role)
                if c is None and e.role.startswith("tree:"):
                    c = "objects"
                if c:
                    out.add(c)
                elif e.parent.startswith("global_cfg:") or e.parent.startswith("tile_map:"):
                    out.add("map")
                elif e.parent not in seen:
                    seen.add(e.parent)
                    stack.append(e.parent)
        r = frozenset(out or {"map"})
        memo[key] = r
        return r

    def rel(p: Path) -> str:
        try:
            return str(p.resolve().relative_to(root)).replace("/", "\\")
        except ValueError:
            return str(p.resolve())

    rows: dict[str, list] = {}
    for k, n in g.nodes.items():
        rp = rel(n.path)
        if any(ch in rp for ch in (chr(9), chr(10), chr(13))):
            continue  # junk read from a broken cfg line
        low = rp.lower()
        c = cats(k)
        if low in rows:
            rows[low][1] |= set(c)
            rows[low][2] = rows[low][2] or n.exists
        else:
            rows[low] = [rp, set(c), n.exists]
    out = Path(a.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="\n") as f:
        for rp, c, ex in rows.values():
            f.write(f"{rp}\t{','.join(sorted(c))}\t{1 if ex else 0}\n")
    print(f"PROGRESS listo: {len(rows)} archivos", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
