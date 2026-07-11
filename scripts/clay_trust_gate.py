#!/usr/bin/env python3
"""LDM Vase Plus trust gate: slice the corpus, classify, assert.

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


MOVE_RE = re.compile(
    r"^G1(?=[ ;])"
    r"(?:\s+X(?P<x>-?[\d.]+))?"
    r"(?:\s+Y(?P<y>-?[\d.]+))?"
    r"(?:\s+Z(?P<z>-?[\d.]+))?"
    r"(?:\s+E(?P<e>-?[\d.]+))?"
)
Z_MARK_RE = re.compile(r"^;Z:([\d.]+)$")

BODY_WALL_TYPES = {"Outer wall", "Overhang wall"}


def verify_spiral_invariants(gcode_path: Path, expected: dict) -> list[str]:
    """G-code-level intent verification for spiral/vase LDM output.

    Proves the emitted toolpath is a valid continuous spiral, not just
    that the analysis classified it. Invariants:
      1. a wall-only spiral body exists after the base (in true spiral
         vase, ;TYPE:Outer wall is declared once and carries across many
         ;Z: markers — so the body is long runs of wall-typed moves with
         no infill/support type ever becoming active);
      2. Z rises monotonically through the body;
      3. zero retractions in the body (the end-of-print retract/lift is
         excluded);
      4. zero real travel moves in the body — a travel is a no-extrusion
         XY move over a meaningful distance; sub-bead zero-extrusion
         smoothing segments do not count;
      5. Z-marker pitch equals the layer height within 2%;
      6. median extrusion per XY mm sits in the declared flow band
         (mid-body only; spiral start/finish flow ramps excluded).
    """
    problems: list[str] = []
    travel_min_mm = expected.get("travel_min_mm", 1.0)

    # Parse into a flat move stream, carrying the active TYPE forward across
    # ;Z: markers (the defining trait of spiral vase). The body span below
    # runs to the last wall extrusion, so the end-of-print retract/lift that
    # follows it is naturally excluded — no end-marker handling needed.
    moves: list[dict] = []          # {lineno, z, e, dist, has_xy, type, layer_idx}
    layer_z: list[float] = []
    cur_type = "Unknown"
    layer_idx = -1
    x = y = z = 0.0
    with gcode_path.open("r", encoding="utf-8", errors="replace") as fh:
        for lineno, raw in enumerate(fh, 1):
            line = raw.rstrip()
            m = Z_MARK_RE.match(line)
            if m:
                layer_idx += 1
                layer_z.append(float(m.group(1)))
                continue
            m = TYPE_RE.match(line)
            if m:
                cur_type = m.group(1).strip()
                continue
            m = MOVE_RE.match(line)
            if not m:
                continue
            nx = float(m.group("x")) if m.group("x") else x
            ny = float(m.group("y")) if m.group("y") else y
            nz = float(m.group("z")) if m.group("z") else z
            e = float(m.group("e")) if m.group("e") else None
            dist = ((nx - x) ** 2 + (ny - y) ** 2) ** 0.5
            moves.append({"lineno": lineno, "z": nz, "e": e, "dist": dist,
                          "has_xy": bool(m.group("x") or m.group("y")),
                          "type": cur_type, "layer": layer_idx})
            x, y, z = nx, ny, nz
    if len(layer_z) < 8:
        return [f"too few layer markers ({len(layer_z)})"]
    total_z = max(layer_z) - min(layer_z)

    # (1): the spiral body is the longest contiguous run of moves that is
    # "clean" — wall-only extrusion, no retraction, no real travel. The solid
    # base (infill + retracts) and any top cap fall outside it by construction;
    # that is correct spiral-vase behavior (PrusaSlicer/Cura force a solid
    # bottom). The body must then cover most of the print's height, which is
    # what proves spiral mode actually engaged.
    def is_break(mv: dict) -> bool:
        if mv["e"] is not None and mv["e"] < 0:            # retraction
            return True
        if mv["e"] and mv["e"] > 0 and mv["type"] not in BODY_WALL_TYPES:  # non-wall extrusion
            return True
        if mv["has_xy"] and (mv["e"] is None or mv["e"] <= 0) and mv["dist"] >= travel_min_mm:  # travel
            return True
        return False

    best = (0, 0)  # (start, end) of the longest clean run, half-open
    run_start = 0
    for i, mv in enumerate(moves):
        if is_break(mv):
            if i - run_start > best[1] - best[0]:
                best = (run_start, i)
            run_start = i + 1
    if len(moves) - run_start > best[1] - best[0]:
        best = (run_start, len(moves))
    body = [mv for mv in moves[best[0]:best[1]] if mv["e"] and mv["e"] > 0 and mv["has_xy"]]
    if len(body) < 50:
        return [f"no continuous spiral body found (longest clean run {len(body)} extrusions)"]

    body_z_span = body[-1]["z"] - body[0]["z"]
    min_frac = expected.get("min_body_z_fraction", 0.5)
    if total_z > 0 and body_z_span / total_z < min_frac:
        problems.append(f"spiral body covers only {body_z_span/total_z:.0%} of print height "
                        f"(< {min_frac:.0%}); spiral mode may not be engaged over the body")

    # (2): monotonic Z through the body (near-guaranteed by construction; a
    # residual check catches within-run anomalies)
    z_drops = sum(1 for a, b in zip(body, body[1:]) if b["z"] < a["z"] - 1e-3)
    if z_drops:
        problems.append(f"Z not monotonic in spiral body: {z_drops} drop(s)")

    # (5): median layer pitch == layer height over the body's layer span
    # (median, not per-layer: a body-boundary layer can share a Z with its
    # neighbor, and that 0.0 artifact must not fail an otherwise clean spiral).
    layer_h = expected.get("layer_height", 1.32)
    body_layers = sorted({mv["layer"] for mv in body if mv["layer"] >= 0})
    zs = [layer_z[i] for i in body_layers if 0 <= i < len(layer_z)]
    pitches = sorted(b - a for a, b in zip(zs, zs[1:]))
    if pitches:
        median_pitch = pitches[len(pitches) // 2]
        if abs(median_pitch - layer_h) > 0.02 * layer_h:
            problems.append(f"median layer pitch {median_pitch:.4f} deviates >2% from {layer_h}")
        # a pitch of ~2x layer height means a revolution was skipped
        if pitches[-1] > 1.6 * layer_h:
            problems.append(f"max layer pitch {pitches[-1]:.4f} exceeds 1.6x layer height "
                            "(possible skipped revolution)")

    # (6): flow intent on the mid-body (exclude the spiral start/finish ramps)
    band = expected.get("flow_band_e_per_mm")
    if band:
        trim = max(len(body) // 10, 1)
        mid = body[trim:-trim]
        ratios = sorted(mv["e"] / mv["dist"] for mv in mid if mv["dist"] > 0.5)
        if not ratios:
            problems.append("no extrusion moves found for flow check")
        else:
            median = ratios[len(ratios) // 2]
            lo, hi = band
            if not (lo <= median <= hi):
                problems.append(f"median flow {median:.3f} E/mm outside intent band [{lo}, {hi}]")
    return problems


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
            if "gcode" not in case:
                continue  # clay variants exist only as fresh slices
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

        # Spiral-vase intent verification: prove the emitted toolpath is a
        # valid continuous spiral, not just that the analysis classified it.
        spiral = case.get("spiral")
        if spiral is not None:
            spiral_problems = verify_spiral_invariants(gcode, spiral)
            if spiral_problems:
                for p in spiral_problems:
                    failures.append(f"{name}:spiral")
                    print(f"    {name}: SPIRAL INVARIANT FAILED - {p}")
            else:
                print(f"    {name}: spiral invariants OK "
                      "(wall-only body, monotonic Z, no retracts, no travels, pitch, flow)")

        # Phase 2: in-slicer analysis assertions via the sidecar JSON written
        # by the fork when clay_mode is active (audit gaps 2+3: fixture
        # acceptance for docs/b2-support-margin-contract.md on the real
        # corpus geometry).
        expected_analysis = case.get("analysis")
        if expected_analysis is None or args.pre_sliced is not None:
            continue
        sidecar = gcode.with_name(gcode.name + ".ldm-analysis.json")
        if not sidecar.exists():
            failures.append(name + ":sidecar-missing")
            print(f"    {name}: analysis sidecar MISSING at {sidecar}")
            continue
        analysis = json.loads(sidecar.read_text(encoding="utf-8"))
        worst_advance = max(
            (loop["worst_advance_mm"] for loop in analysis.get("support_margin_field", [])),
            default=None,
        )
        checks = {
            "risk_distribution_mode": (
                expected_analysis.get("risk_distribution_mode"),
                analysis.get("risk_distribution_mode"),
            ),
            "support_margin_status": (
                expected_analysis.get("support_margin_status"),
                analysis.get("support_margin_summary", {}).get("status"),
            ),
        }
        for key, (want, got) in checks.items():
            if want is not None and got != want:
                failures.append(f"{name}:{key}")
                print(f"    {name}: {key} expected {want!r}, got {got!r}   <-- MISMATCH")
        lo = expected_analysis.get("worst_advance_min_mm")
        hi = expected_analysis.get("worst_advance_max_mm")
        if lo is not None and (worst_advance is None or not (lo <= worst_advance <= hi)):
            failures.append(f"{name}:worst_advance")
            print(f"    {name}: worst_advance {worst_advance} outside [{lo}, {hi}]   <-- MISMATCH")
        print(f"    {name}: in-slicer worst_advance_mm={worst_advance} "
              f"status={analysis.get('support_margin_summary', {}).get('status')} "
              f"mode={analysis.get('risk_distribution_mode')}")
    if failures:
        print(f"\nTRUST GATE FAILED: {failures}")
        return 1
    print("\nTRUST GATE PASSED: all classifications match the case studies.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
