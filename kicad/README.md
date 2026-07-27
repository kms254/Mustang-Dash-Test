# kicad/

Converted KiCad copies of boards whose authoritative source lives elsewhere.

Today that is **Board3**, imported from EasyEDA Pro for the tooling evaluation in
[docs/plans/2026-07-27-001-chore-kicad-evaluation-plan.md](../docs/plans/2026-07-27-001-chore-kicad-evaluation-plan.md).

Three rules govern everything in this directory:

- **EasyEDA stays authoritative.** Conversion is one-way. Nothing here is ever
  converted back — round-trips accumulate error, and the EasyEDA project is the
  source of truth until a verdict says otherwise.
- **The design files are tracked on purpose.** `.kicad_pcb` and `.kicad_sch` are
  S-expression text, and `git diff` is the evaluation's verification mechanism for
  agent-driven writes. Keeping the board out of the repo would forfeit that.
- **The first commit of a converted board is the baseline.** It must contain
  converter output and nothing else. Every later measurement diffs against it.

Toolchain resolution for anything operating on these files is
[tools/kicad_env.py](../tools/kicad_env.py) — do not hardcode `kicad-cli` paths.
