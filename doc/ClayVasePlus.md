# Clay Vase Plus — CeramicaSlicer Developer Build Manual

CeramicaSlicer is a fork of OrcaSlicer for **wet-clay / LDM printing**
(auger extruder, pneumatic paste feed, large nozzles, spiral-vase-centric
workflows). This page documents every clay-specific setting and every
behavior this fork adds on top of stock OrcaSlicer. Hovering any clay
setting in the slicer shows its short tooltip; clicking the setting label
opens the matching section on this page.

**Baseline guarantee:** with *Clay mode* set to Off, this build slices
byte-identically to the upstream OrcaSlicer commit it is based on. Every
CI run enforces this with the full upstream test suite plus a clay "trust
gate" that re-slices three reference models and checks the analysis
verdicts against measured fixtures.

---

## Where to find the settings

- **Process tab → Others page → "Clay Vase Plus" group** — all analysis
  settings (listed below). Make sure the **Advanced** view toggle is on;
  the clay options are advanced-mode settings.
- **Printer tab → Machine G-code page → below "Machine start G-code"** —
  *Clay start G-code mode*.

Nothing needs a special profile: open any project, switch Clay mode to
"Vase Plus", and slice. The reference clay projects in
`tests/data/clay_corpus/*_clay.3mf` arrive preconfigured.

---

## Settings reference

### Clay mode

`clay_mode` — Off | Vase Plus (default Off)

Master switch. Off means the fork behaves exactly like stock OrcaSlicer.
Vase Plus enables, at slicing time:

1. **Config conflict warnings** — retraction, Z-hop, and non-spiral
   workflows that are usually hostile to wet clay.
2. **Startup G-code inspection** — flags purge- and retract-like lines in
   the machine start G-code (see *Clay start G-code mode*).
3. **Body continuity analysis (B1)** — reads the generated wall structure
   and classifies the print's risk distribution:
   `clean_control` (nothing suspicious), `body_spread` (sustained
   wall-role fragmentation through the body — the signature of a form
   that stresses vase mode), `base_concentrated` (narrow rescue
   structure crammed into the base layers — the signature of stock
   "Make overhang printable" style corrections that clay cannot print),
   or `mixed`.
4. **Support-margin analysis (B2)** — measures, for every wall loop, how
   far it steps outward past the loop below it (exact point-to-segment
   distance), compares against the admissible step, and classifies
   `safe / marginal / failing` with the height of the first violation.
5. **Analysis sidecar** — exporting G-code also writes
   `<name>.gcode.clay-analysis.json` next to the file with the full
   machine-readable verdict (see below).

It does **not** modify toolpaths. Analysis earns trust before any
correction ships; correction (locally tilted spiral layers) is the next
phase of the roadmap.

### Clay nominal bead width

`clay_nominal_bead_width_mm` (default 0 = unset)

The reference extruded bead width of your clay setup (for the Ender5+ #4
machine with the 3.3 mm nozzle at 140% outer wall width this is 4.62).
Used by the analysis in two places:

- **Narrow-rescue detection**: any printed path narrower than **35%** of
  this bead is treated as physically unprintable rescue structure. With
  the bead unset (0), narrowness cannot be judged and that metric stays
  off.
- **Support-margin sampling**: wall loops are sampled every half bead for
  the outward-step measurement.

This setting never changes the slicer's actual line width — it is the
analysis yardstick only.

### Clay nominal layer height

`clay_nominal_layer_height_mm` (default 0 = unset)

Reference layer height for the analysis snapshot (1.32 for the current
Clay Vase Mode V4 process). Informational in this build; the
support-margin math uses each layer's real height.

### Clay max unsupported step

`clay_max_unsupported_step_mm` (default 0 = derive)

The admissible horizontal step of one wall loop past the loop below it.

- **0 (default):** derived as `layer_height × tan(40°)` — the 40°
  wet-clay envelope assumption (≈1.11 mm at 1.32 mm layers). The angle
  will become calibratable after the physical slump-ladder session.
- **> 0:** your explicit value wins. Set this after calibrating your
  clay body: print the slump ladder, note the last clean angle θ, set
  `layer_height × tan(θ)`.

Loops stepping beyond this are classified `failing` and produce the
`CVP_SUPPORT_MARGIN_FAILING` warning with the Z height of the worst
violation.

### Clay continuous path required

`clay_continuous_path_required` (default on)

Declares that your workflow expects one continuous deposition path
(spiral vase). Informational in this build; feeds the analysis result
model.

### Prefer no retracts

`clay_disable_retracts` (default on)

When on, active retraction settings produce a high-severity warning
(`CVP_RETRACT_BURDEN_HIGH`). Wet-clay flow through an auger is far more
stable without retract/restart events; even a nominally tiny retract
forces a pressure disturbance.

### Prefer no Z hop

`clay_disable_z_hop` (default on)

When on, active Z-hop produces a warning (`CVP_ZHOP_WARNING`). Lift
events break bead continuity and leave witness marks in soft clay.

### Clay start G-code mode

`clay_start_gcode_mode` — Stock | Clay native (default Stock)

- **Stock:** the machine start G-code is emitted untouched (warnings
  only).
- **Clay native:** obvious filament-style **purge/prime lines and
  startup retracts are stripped** from the emitted start G-code and
  replaced with an explanatory comment
  (`; Clay Vase Plus removed startup line: …`), preserving everything
  else (homing, bed mesh, macros). Use this when your start G-code came
  from a filament profile and you don't want priming moves executed with
  clay in the barrel.

---

## Analysis outputs

### Warnings (shown in the slicer's warning panel)

| Code | Meaning |
|---|---|
| `CVP_SPIRAL_MODE_REQUIRED` | Clay mode is on but spiral vase is off |
| `CVP_RETRACT_BURDEN_HIGH` | Retraction active while *Prefer no retracts* is on |
| `CVP_ZHOP_WARNING` | Z-hop active while *Prefer no Z hop* is on |
| `CVP_STARTUP_RETRACT_WARNING` / `CVP_STARTUP_PURGE_WARNING` | Retract-/purge-like lines found in start G-code |
| `CVP_WALL_FRAGMENTATION_HIGH` | Sustained body region with fragmented wall roles (body_spread evidence), with the zone's Z range |
| `CVP_NARROW_GAP_MEDIUM` | Sub-bead rescue structure in the base region |
| `CVP_SUPPORT_MARGIN_MARGINAL` / `CVP_SUPPORT_MARGIN_FAILING` | Outward step near / beyond the admissible envelope, with the worst Z |

### The analysis sidecar

Exporting G-code with clay mode active writes
`<gcode>.clay-analysis.json` containing: overall risk level, risk
distribution mode, the body fragmentation zone (Z range + peak section
counts), base rescue complexity, startup compatibility, the support
margin summary (status, first warning Z, worst margin in mm), the full
warning list, and the per-loop support-margin field
(`layer_idx, z_mm, a_max_mm, dz_budget_mm, worst_advance_mm,
violating_fraction` per wall loop). This is the machine-readable
contract consumed by CI and by the upcoming correction engine.

---

## Everything changed vs stock OrcaSlicer (developer changelog)

**Slicing engine (`src/libslic3r/`)**
- `PrintConfig.{hpp,cpp}`: the eight `clay_*` settings above.
- `Print.{hpp,cpp}`: the Clay Vase Plus analysis pass — config conflict
  scan at validate time; body-continuity extraction (B1) and
  support-margin measurement (B2) at the end of slicing, reading the
  generated perimeters only (no behavior change); result structs
  including the queryable per-loop support-margin field.
- `GCode.cpp`: clay-native startup sanitizer; analysis sidecar JSON
  export.

**GUI (`src/slic3r/GUI/`)**
- `Tab.cpp`: "Clay Vase Plus" settings group (Process → Others) and
  *Clay start G-code mode* (Printer → Machine G-code), each with hover
  tooltips and click-through links to this page.
- `OptionsGroup.cpp`: setting-label links may now be absolute URLs
  (fork-hosted docs) in addition to upstream wiki paths.

**Tests & CI**
- 13 clay unit tests in `tests/fff_print/` (config warnings, startup
  sanitizing, continuity classification incl. false-positive guards,
  support margin on known geometry: a plain cube must be `safe`, a 45°
  chamfer must be `failing` under the 40° envelope, an explicit step
  override must win, sidecar existence/contents).
- `.github/workflows/clay-ci.yml`: Linux build + full upstream test
  suite; **Windows x64 portable build** on every push; compiler caching;
  the **clay trust gate** — re-slices the three reference models
  (tumbler control, Julia Heatwave, Julia+Make-Overhang-Printable) plus
  their clay-enabled variants and asserts both the G-code-level
  classifications and the in-slicer verdicts against measured fixtures
  (tumbler: safe/clean_control at 0.50 mm worst step; Julia:
  failing/body_spread at 5.48 mm; Julia+MOP: failing/base_concentrated
  at 10.31 mm — stock overhang correction makes clay printability
  *worse*).
- `tests/data/clay_corpus/`: the reference models and expectations.
- `scripts/clay_trust_gate.py`: the gate runner (also usable locally).

**Not in this build (by design, next phases):** no toolpath correction,
no in-viewport risk overlay (warnings + sidecar only), thresholds not
yet calibrated to a specific clay body (defaults assume the Ender5+ #4 /
Clay Vase Mode V4 regime).
