#!/usr/bin/env python3
"""Clay Vase Plus trust gate: slice the corpus, classify, assert.

The unified roadmap's trust gate requires the analysis vocabulary to match
the three case studies before any path-changing code ships:

    tumbler control      -> clean_control
    Julia baseline       -> body_spread
    Julia + MakeOverhangsPrintable -> base_concentrated

This script slices the corpus 3MFs with a freshly built slicer (or accepts
pre-sliced G-code via --pre-sliced), extracts the G-code-level continuity
signals, classifies each case, and exits nonzero on any mismatch. Run by
the trust_gate job in .github/workflows/clay-ci.yml on every push.

Classification rules are distilled from the case evidence
(CeramicaSlicer docs: gcode-case-metrics-report, julia case studies):
  1. gap infill present with min width < 1.0 mm  -> base_concentrated
     (very narrow rescue structure; the Julia+MOP signature, 0.66 mm)
  2. else outer-wall sections >= 40 or overhang-wall sections >= 20
     -> body_spread (sustained wall-role fragmentation; Julia baseline
     shows 80/78)
  3. else -> clean_control

Only stdlib; runs anywhere Python 3.8+ exists.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

TYPE_RE = re.compile(r"^;TYPE:(.+)$")
WIDTH_RE = re.compile(r"^;WIDTH:([0-9.]+)$")


def extract_signals(gcode_path: Path) -> dict:
    type_counts: dict[str, int] = {}
    gap_min_width = None
    current_type = "Unknown"
    with gcode_path.open("r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            line = line.rstrip()
            m = TYPE_RE.match(line)
            if m:
                current_type = m.group(1).strip()
                type_counts[current_type] = type_counts.get(current_type, 0) + 1
                continue
            m = WIDTH_RE.match(line)
            if m and current_type == "Gap infill":
                w = float(m.group(1))
                if gap_min_width is None or w < gap_min_width:
                    gap_min_width = w
    return {
        "outer_wall_sections": type_counts.get("Outer wall", 0),
        "overhang_wall_sections": type_counts.get("Overhang wall", 0),
        "gap_infill_sections": type_counts.get("Gap infill", 0),
        "gap_min_width": gap_min_width,
    }


def classify(signals: dict) -> str:
    if signals["gap_infill_sections"] > 0 and (
        signals["gap_min_width"] is not None and signals["gap_min_width"] < 1.0
    ):
        return "base_concentrated"
    if signals["outer_wall_sections"] >= 40 or signals["overhang_wall_sections"] >= 20:
        return "body_spread"
    return "clean_control"


def slice_3mf(slicer: Path, model: Path, out_dir: Path) -> Path:
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(slicer), "--slice", "0", "--outputdir", str(out_dir), str(model)]
    print("::", " ".join(cmd), flush=True)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
    if proc.returncode != 0:
        # Some CLI paths want an explicit plate index; retry with plate 1.
        cmd[2] = "1"
        print("::retry:", " ".join(cmd), flush=True)
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout[-4000:] + "\n" + proc.stderr[-4000:] + "\n")
        raise RuntimeError(f"slicing failed for {model.name} (exit {proc.returncode})")
    gcodes = sorted(out_dir.glob("**/*.gcode"), key=lambda p: p.stat().st_mtime)
    if not gcodes:
        sys.stderr.write(proc.stdout[-4000:] + "\n")
        raise RuntimeError(f"no G-code produced for {model.name}")
    return gcodes[-1]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--corpus", type=Path, required=True,
                    help="directory containing expected.json and the 3MFs")
    ap.add_argument("--slicer", type=Path, default=None,
                    help="slicer binary/AppRun for fresh slicing")
    ap.add_argument("--pre-sliced", type=Path, default=None,
                    help="directory of existing G-codes (filename per manifest "
                         "'gcode' key); skips slicing")
    ap.add_argument("--out", type=Path, default=Path("trust-gate-out"))
    args = ap.parse_args()

    manifest = json.loads((args.corpus / "expected.json").read_text(encoding="utf-8"))
    failures = []
    print(f"{'case':<28} {'expected':<20} {'actual':<20} signals")
    for case in manifest:
        name = case["name"]
        if args.pre_sliced is not None:
            gcode = args.pre_sliced / case["gcode"]
        else:
            if args.slicer is None:
                ap.error("--slicer required unless --pre-sliced is given")
            gcode = slice_3mf(args.slicer, args.corpus / case["model"], args.out / name)
        signals = extract_signals(gcode)
        actual = classify(signals)
        ok = actual == case["expected"]
        if not ok:
            failures.append(name)
        print(f"{name:<28} {case['expected']:<20} {actual:<20} {signals}"
              + ("" if ok else "   <-- MISMATCH"))
    if failures:
        print(f"\nTRUST GATE FAILED: {failures}")
        return 1
    print("\nTRUST GATE PASSED: all classifications match the case studies.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
