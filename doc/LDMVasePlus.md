# LDM Vase Plus — CeramicaSlicer Developer Build Manual

CeramicaSlicer is a fork of OrcaSlicer for **LDM printing** (liquid/paste
deposition: wet clay, porcelain, and similar — auger extruder, ram-fed
reservoir, large nozzles, spiral-vase-centric workflows). This page
documents every LDM-specific setting and every behavior this fork adds on
top of stock OrcaSlicer. Hovering any LDM setting in the slicer shows its
short tooltip; clicking the setting label opens the matching section here.

**Baseline guarantee:** with *LDM Modded Printer* off, this build slices
byte-identically to the upstream OrcaSlicer commit it is based on. Every
CI run enforces this with the full upstream test suite plus an LDM "trust
gate" that re-slices three reference models and checks the analysis
verdicts against measured fixtures.

**The machine model** (matching real LDM hardware): the printer's
**E axis always drives the auger** — that is what slicer extrusion
commands meter. Material reaches the auger from a **reservoir**
(tube/syringe/cartridge) pushed by either a **pneumatic ram** (air
pressure; no extra motor control) or a **mechanical ram** (a second
motor, on Marlin machines typically run as a mixing extruder via
M163/M164 — see *LDM reservoir feed*).

---

## Where to find the settings

- **Printer tab → Basic information → Advanced, right under
  "Pellet Modded Printer"** — the machine group: *LDM Modded Printer*
  (master switch), *LDM reservoir feed*, *LDM ram mix factor*,
  *LDM reservoir volume*, *LDM tip cone angle/length*.
- **Process tab → Others page → "LDM Vase Plus" group** — the analysis
  parameters. Make sure the **Advanced** view toggle is on.
- **Printer tab → Machine G-code page → below "Machine start G-code"** —
  *LDM start G-code mode*.

Turn on *LDM Modded Printer* once on your printer preset and every print
on that machine gets the LDM analysis — no per-project setup. The
reference projects in `tests/data/clay_corpus/*_clay.3mf` arrive
preconfigured.

---

## Machine settings (Printer tab)

### LDM Modded Printer

`ldm_modded_printer` — off | on (default off)

The master switch, and deliberately a *printer* property rather than a
per-print mode: a machine that prints paste never prints filament, so LDM
behavior is part of the machine's identity — exactly like the neighboring
*Pellet Modded Printer* flag (which is untouched by this fork and remains
available for pellet machines in the same profile library). When on,
every print on this printer gets, at slicing time:

1. **Config conflict warnings** — retraction, Z-hop, and broken-path
   workflows that are usually hostile to paste extrusion.
2. **Startup G-code inspection** — flags purge- and retract-like lines
   (see *LDM start G-code mode*).
3. **Body continuity analysis** — classifies the print's risk
   distribution from the generated wall structure: `clean_control`,
   `body_spread` (sustained wall-role fragmentation — a form that
   stresses vase mode), `base_concentrated` (narrow rescue structure in
   the base — the signature of filament-style overhang corrections that
   paste cannot print), or `mixed`.
4. **Support-margin analysis** — measures each wall loop's outward step
   past the loop below (exact point-to-segment distance) against the
   admissible step; classifies `safe / marginal / failing` with the
   first-violation height.
5. **Reservoir capacity check** — see *LDM reservoir volume*.
6. **Analysis sidecar** — exporting G-code also writes
   `<name>.gcode.ldm-analysis.json` with the full machine-readable
   verdict.

It does **not** modify toolpaths. Analysis earns trust before any
correction ships; correction (locally tilted spiral layers) is the next
phase of the roadmap.

### LDM reservoir feed

`ldm_feed_type` — Pneumatic ram | Mechanical ram (default Pneumatic ram)

How material reaches the auger.

- **Pneumatic ram** (e.g. a 2L syringe at ~120 psi): pressure does the
  feeding; only the auger is G-code-controlled. No mixing commands are
  needed or used.
- **Mechanical ram**: a second motor (often a spare stepper driver)
  pushes the ram. On Marlin-family machines (e.g. Eazao) this is
  typically configured as a **mixing extruder**: one E axis drives both
  motors, split by mix factors set once in the start G-code:

  ```
  M163 S0 P0.9 ; ram share
  M163 S1 P0.1 ; auger share
  M164 S0      ; activate the mix
  M302         ; allow cold extrusion
  ```

  (Klipper machines usually handle ram sync with firmware macros
  instead; this scheme is Marlin-proven, untested on Klipper.)

The setting itself changes no G-code — it documents the machine and is
available to your start G-code template as `{ldm_feed_type}`, alongside
the mix factor below.

### LDM ram mix factor

`ldm_ram_mix_factor` — 0…1 (default 0.9), **mechanical ram only**

The ram's share of the Marlin mixing extruder (auger gets `1 − factor`).
Ignored for pneumatic feeds. Reference it in your start G-code as
`{ldm_ram_mix_factor}` so tuning the profile updates the emitted `M163`
lines. Change it only in small steps — the ratio directly loads the
drive hardware.

### LDM reservoir volume

`ldm_reservoir_volume_ml` — mL (default 0 = check off)

Usable material volume of the reservoir feeding the auger. When set, the
slicer compares the print's cumulative extruded volume against it and, if
the print needs more than one load, emits **`LVP_RESERVOIR_REFILL`**: the
total needed volume, the capacity, and **the Z height at which the
reservoir runs dry** — so you can plan the refill (or place a pause
there). The comparison is volume-to-volume; no density guesswork.
Skirt/brim volume is not counted, so treat the reported height as
slightly optimistic.

Common values: **100** (100cc syringe), **500** (500cc syringe),
**2000** (2L tube/syringe), and **0 for continuous feed** — an endless
supply (pump/hopper-fed) has no run-dry height, so 0 turns the check
off by design.

### LDM tip cone angle

`ldm_tip_cone_angle` — degrees (default 0 = undeclared)

Full cone angle of the deposition tip (e.g. your PME Supatube family).
Edit when you swap tips. Consumed by the upcoming tip-collision
validation for non-planar printing; declared now so tip presets can
carry it.

### LDM tip cone length

`ldm_tip_cone_length` — mm (default 0 = undeclared)

Length of the tip cone from orifice to body. Same consumer as the angle.

### LDM tip top diameter

`ldm_tip_top_diameter` — mm (default 0 = unused)

Alternative way to describe the cone, usually easier to measure than an
angle: caliper the outer diameter at the **top** of the tip cone. When
both this and the cone length are set, the effective cone angle is
derived —

```
half-angle = atan( (top diameter − nozzle diameter) / (2 × cone length) )
```

— and **overrides** the angle field. Enter whichever pair you actually
know: (angle + length) or (top diameter + length).

**Tip orifice diameter (the bottom of the tip):** deliberately *not* a
separate setting — the orifice **is** the nozzle, so keep using
`nozzle_diameter` (3.3 mm for the Supatube #4 setup). One source of
truth keeps all flow math correct, including the line widths your
process defines as percentages of the nozzle. This does **not** assume
the tip never changes — it assumes a tip swap updates `nozzle_diameter`,
the same way FFF users handle nozzle swaps.

**Tips are not Supatube-specific.** Any conical tip — Supatube, generic
cake nozzle, machined tip, cut syringe — is described by the same three
numbers: bottom orifice (`nozzle_diameter`), cone length, and cone angle
*or* top diameter. All are freely editable.

**Recommended tip-swap workflow:** a physical tip change alters these
values together, so keep **one printer preset per tip** — e.g.
"Ender5+ Supatube #3", "Ender5+ 6mm generic" — each bundling orifice +
cone geometry. Save any combination under your own name with the normal
preset save button. Swap the tip, pick the matching preset, and flow
math, analysis sampling, and the future collision checks all follow
automatically. (This is the same preset mechanism stock Orca uses for
0.2/0.4/0.6 nozzle variants; built-in tip presets for common tip
families are planned once real measured dimensions are collected.)

---

## Material settings (Filament tab → Basic information, under Density)

### LDM wet yield strength

`ldm_wet_yield_strength` — kPa (default 0 = screening off)

Yield strength of the wet paste **as printed** (printable clay bodies are
typically 4–20 kPa). Together with the material **Density** (the standard
field above it — set your real wet-clay density, ≈1.8–2.0 g/cm³, not a
filament placeholder) and the nominal bead width, this enables the
**self-weight stability screening**: for every layer, the accumulated
weight of everything above it is converted to wall stress and compared
against what the wet material can carry (squash), and the overturning
moment of off-center mass is checked against the wall's resisting moment
(cantilever). The screening is deliberately **conservative — it gives no
credit for drying/stiffening during the print**. Calibrate with the
squash-cylinder print from the Track D session plan.

### LDM wet elastic modulus

`ldm_e_modulus` — kPa (default 0 = buckling screen off)

Elastic modulus of the wet paste (typically 300–1000 kPa for printable
clay). Adds the third failure mode to the screening: **shell buckling** —
a slender wall bowing sideways well below the squash limit, the classic
"it should have worked" collapse. Curved walls are stiffer (folds act as
corrugation); the screen uses the flattest spans of each loop. Calibrate
with a thin-wall tube printed to failure.

---

## Process settings (Process tab → Others → LDM Vase Plus)

### LDM nominal bead width

`ldm_nominal_bead_width_mm` (default 0 = unset)

The reference extruded bead width of your setup (4.62 for the Ender5+ #4
at 140% outer wall width). The analysis yardstick — never changes the
slicer's actual line width. Used for:

- **Narrow-rescue detection**: any printed path narrower than **35%** of
  this bead is physically unprintable rescue structure. Unset (0) turns
  that metric off (narrowness cannot be judged).
- **Support-margin sampling**: wall loops are sampled every half bead.

### LDM nominal layer height

`ldm_nominal_layer_height_mm` (default 0 = unset)

Reference layer height for the analysis snapshot (1.32 for Clay Vase
Mode V4). Informational; the support-margin math uses each layer's real
height.

### LDM max unsupported step

`ldm_max_unsupported_step_mm` (default 0 = derive)

The admissible horizontal step of one wall loop past the loop below.

- **0 (default):** derived as `layer_height × tan(40°)` (≈1.11 mm at
  1.32 mm layers) — the wet-clay envelope assumption, calibratable after
  the physical slump-ladder session.
- **> 0:** your explicit value wins (`layer_height × tan(θ)` from your
  measured last-clean angle θ).

Loops stepping beyond this are `failing` and produce
`LVP_SUPPORT_MARGIN_FAILING` with the worst Z.

### Require continuous LDM path

`ldm_continuous_path_required` (default on)

Declares that this process expects one continuous deposition path
(spiral vase). When on and spiral vase is off, you get the
`LVP_SPIRAL_MODE_REQUIRED` reminder. **Clear it for legitimate non-vase
LDM processes** (solid parts, tiles) — you keep all other analysis
without spiral nagging.

### Prefer no retracts

`ldm_disable_retracts` (default on)

Warns (`LVP_RETRACT_BURDEN_HIGH`) when retraction is active: paste flow
through an auger is far more stable without retract/restart pressure
disturbances.

### Prefer no Z hop

`ldm_disable_z_hop` (default on)

Warns (`LVP_ZHOP_WARNING`) when Z-hop is active: lift events break bead
continuity and leave witness marks in soft material.

### LDM start G-code mode

`ldm_start_gcode_mode` — Stock | Clay native (default Stock),
**printer setting** (Machine G-code page)

- **Stock:** start G-code emitted untouched (warnings only).
- **Clay native:** obvious filament-style purge/prime lines and startup
  retracts are **stripped** from the emitted start G-code, each replaced
  with an explanatory comment (`; LDM Vase Plus removed startup line: …`),
  preserving everything else (homing, bed mesh, macros, M163/M164).

---

## Analysis outputs

### Warnings (shown in the slicer's warning panel)

| Code | Meaning |
|---|---|
| `LVP_SPIRAL_MODE_REQUIRED` | Continuous path required but spiral vase is off |
| `LVP_RETRACT_BURDEN_HIGH` | Retraction active while *Prefer no retracts* is on |
| `LVP_ZHOP_WARNING` | Z-hop active while *Prefer no Z hop* is on |
| `LVP_STARTUP_RETRACT_WARNING` / `LVP_STARTUP_PURGE_WARNING` | Retract-/purge-like lines in start G-code |
| `LVP_WALL_FRAGMENTATION_HIGH` | Sustained body region with fragmented wall roles, with the zone's Z range |
| `LVP_NARROW_GAP_MEDIUM` | Sub-bead rescue structure in the base region |
| `LVP_SUPPORT_MARGIN_MARGINAL` / `LVP_SUPPORT_MARGIN_FAILING` | Outward step near / beyond the admissible envelope, with the worst Z |
| `LVP_RESERVOIR_REFILL` | Print volume exceeds the reservoir; includes the run-dry Z height |
| `LVP_STABILITY_SQUASH` / `LVP_STABILITY_BUCKLE` / `LVP_STABILITY_CANTILEVER` | Self-weight stability screening near (medium) or beyond (high) the limit, with mode, utilization, and the failing height |

### The analysis sidecar

Exporting G-code with the LDM printer flag on writes
`<gcode>.ldm-analysis.json`: `ldm_active`, overall risk level, risk
distribution mode, the body fragmentation zone (Z range + peak section
counts), base rescue complexity, startup compatibility, the support
margin summary (status, first warning Z, worst margin in mm), the
stability screen (evaluated flag, squash/buckle/cantilever utilization
ratios, predicted mode, failing height), the full warning list, and the
per-loop support-margin field (`layer_idx, z_mm, a_max_mm, dz_budget_mm,
worst_advance_mm, violating_fraction`). This is the machine-readable
contract consumed by CI and by the upcoming correction engine.

---

## Everything changed vs stock OrcaSlicer (developer changelog)

**Slicing engine (`src/libslic3r/`)**
- `PrintConfig.{hpp,cpp}`: the LDM settings above — the
  `ldm_modded_printer` machine flag, the machine group (feed type, ram
  mix factor, reservoir volume, tip cone angle/length), seven
  process/machine analysis parameters; preset registration so they
  persist in printer and process presets.
- `Print.{hpp,cpp}`: the LDM Vase Plus analysis pass — config conflict
  scan at validate time; body-continuity and support-margin measurement
  at the end of slicing, reading generated perimeters only (no behavior
  change); the reservoir capacity check; the conservative self-weight
  stability screening (squash / shell-buckle / cantilever, Suiker/Wolfs
  lineage, no drying credit); result structs including the queryable
  per-loop support-margin field.
- `GCode.cpp`: clay-native startup sanitizer; analysis sidecar JSON
  export.

**GUI (`src/slic3r/GUI/`)**
- `Tab.cpp`: LDM machine group on Printer → Basic information (under
  Pellet Modded Printer); "LDM Vase Plus" group on Process → Others;
  *LDM start G-code mode* on Printer → Machine G-code — each setting
  with hover tooltip and click-through link to this page.
- `OptionsGroup.cpp`: setting-label links may be absolute URLs
  (fork-hosted docs) in addition to upstream wiki paths.

**Tests & CI**
- 14 LDM unit tests in `tests/fff_print/` (config warnings, startup
  sanitizing, continuity classification incl. false-positive guards,
  support margin on known geometry — a plain cube must be `safe`, a 45°
  chamfer must be `failing` under the 40° envelope, an explicit step
  override must win — sidecar existence/contents, reservoir refill
  warning).
- `.github/workflows/clay-ci.yml`: Linux build + full upstream suite;
  **Windows x64 portable build** every push; compiler caching; the
  **trust gate** — re-slices the three reference models (tumbler
  control, Julia Heatwave, Julia+Make-Overhang-Printable) plus their
  LDM-enabled variants and asserts both G-code-level classifications and
  in-slicer verdicts against measured fixtures (tumbler: safe /
  clean_control at 0.50 mm worst step; Julia: failing / body_spread at
  5.48 mm; Julia+MOP: failing / base_concentrated at 10.31 mm — stock
  overhang correction makes paste printability *worse*).
- `tests/data/clay_corpus/`: reference models and expectations.
- `scripts/clay_trust_gate.py`: the gate runner (also usable locally).

**Not in this build (by design, next phases):** no toolpath correction,
no in-viewport risk overlay (warnings + sidecar only), thresholds not
yet calibrated to a specific clay body (defaults assume the Ender5+ #4 /
Clay Vase Mode V4 regime).
