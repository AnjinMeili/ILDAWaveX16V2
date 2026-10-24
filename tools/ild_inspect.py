#!/usr/bin/env python3
"""Inspect .ild files for what matters to a galvo: steps between points.

For each file: frames, points, blanked share, and the largest X/Y step between
consecutive points as a percentage of full scale (65535 = the whole scan
angle), split into lit steps and blanked moves. Also counts steps above
--big (default 12.5 %), the size at which a single-sample jump is a hard
step input for the scanner rather than a drawn line.

  python3 tools/ild_inspect.py ~/ilda/idlawave/Alphalite/ILDATEST.ILD
  python3 tools/ild_inspect.py --big 10 --per-frame file.ild
  python3 tools/ild_inspect.py ~/ilda/idlawave/*.ild
"""

from __future__ import annotations

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ilw_sender import load_ilda  # noqa: E402

FULL_SCALE = 65535.0


def frame_stats(points, big_pct: float) -> dict:
    lit_max = blank_max = 0
    lit_big = blank_big = 0
    blanked = 0
    for a, b in zip(points, points[1:]):
        step = max(abs(b.x - a.x), abs(b.y - a.y))
        pct = step * 100.0 / FULL_SCALE
        if b.blanked:
            blank_max = max(blank_max, pct)
            if pct > big_pct:
                blank_big += 1
        else:
            lit_max = max(lit_max, pct)
            if pct > big_pct:
                lit_big += 1
    blanked = sum(1 for p in points if p.blanked)
    xs = [p.x for p in points]
    ys = [p.y for p in points]
    return {
        "points": len(points),
        "blanked_pct": round(100.0 * blanked / len(points), 1) if points else 0.0,
        "extent_pct": round(max(max(xs) - min(xs), max(ys) - min(ys)) * 100.0 / FULL_SCALE, 1) if points else 0.0,
        "lit_step_max_pct": round(lit_max, 2),
        "blank_step_max_pct": round(blank_max, 2),
        "lit_big_steps": lit_big,
        "blank_big_steps": blank_big,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--big", type=float, default=12.5, help="big-step threshold in %% of full scale")
    ap.add_argument("--per-frame", action="store_true")
    ap.add_argument("--pps", type=int, default=30000, help="rate used to express frame time")
    args = ap.parse_args()

    hdr = f"{'file':34s} {'frames':>6s} {'points':>7s} {'ms/frame':>8s} {'blank%':>6s} {'extent%':>7s} " \
          f"{'lit max%':>8s} {'blank max%':>10s} {'lit>big':>7s} {'blank>big':>9s}"
    print(hdr)
    for path in args.files:
        try:
            frames = load_ilda(path)
        except Exception as exc:  # unreadable file: report and continue
            print(f"{os.path.basename(path)[:34]:34s} error: {exc}")
            continue
        if not frames:
            print(f"{os.path.basename(path)[:34]:34s} no frames")
            continue
        totals = {"points": 0, "lit_big_steps": 0, "blank_big_steps": 0}
        lit_max = blank_max = extent = 0.0
        blanked_pts = 0
        for i, fr in enumerate(frames):
            st = frame_stats(fr.points, args.big)
            totals["points"] += st["points"]
            totals["lit_big_steps"] += st["lit_big_steps"]
            totals["blank_big_steps"] += st["blank_big_steps"]
            lit_max = max(lit_max, st["lit_step_max_pct"])
            blank_max = max(blank_max, st["blank_step_max_pct"])
            extent = max(extent, st["extent_pct"])
            blanked_pts += round(st["blanked_pct"] * st["points"] / 100.0)
            if args.per_frame:
                print(f"  frame {i:4d} {fr.name:8s} {st}")
        avg_points = totals["points"] / len(frames)
        print(f"{os.path.basename(path)[:34]:34s} {len(frames):6d} {totals['points']:7d} "
              f"{avg_points * 1000.0 / args.pps:8.1f} {100.0 * blanked_pts / max(1, totals['points']):6.1f} "
              f"{extent:7.1f} {lit_max:8.2f} {blank_max:10.2f} {totals['lit_big_steps']:7d} {totals['blank_big_steps']:9d}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
