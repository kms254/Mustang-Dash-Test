# kicad/

The project's board designs.

Today that is **Board3**. It arrived here as an import from EasyEDA Pro for the
tooling evaluation in
[docs/plans/2026-07-27-001-chore-kicad-evaluation-plan.md](../docs/plans/2026-07-27-001-chore-kicad-evaluation-plan.md),
but **that evaluation is settled**: as of 2026-07-31 Kevin has moved off EasyEDA
entirely, so this directory is the design, not a copy of one.

![Board3, angled](board3/renders/angled.png)

| Top | Bottom |
| --- | --- |
| ![Top](board3/renders/top.png) | ![Bottom](board3/renders/bottom.png) |

These are generated, not drawn — `.kicad_pcb` is the truth. See
[Renders](#renders) for how they stay current.

Three rules govern everything in this directory:

- **This is the source of truth.** ~~EasyEDA stays authoritative; conversion is
  one-way.~~ Superseded 2026-07-31 — EasyEDA is no longer maintained, so there is
  no upstream to defer to and nothing to reconcile against. Edit the schematic and
  board here directly. The `EasyEDA/` directory stays in the repo as history; do
  not mirror changes into it, and do not treat a KiCad-only change as partial.
  The one-way rule still describes the *import*: nothing here was ever converted
  back, and `docs/solutions/integration-issues/easyeda-pro-to-kicad-migration-silent-data-loss.md`
  records six silent losses that make a re-import a bad idea now that this copy
  carries all the routing.
- **The design files are tracked on purpose.** `.kicad_pcb` and `.kicad_sch` are
  S-expression text, and `git diff` is the evaluation's verification mechanism for
  agent-driven writes. Keeping the board out of the repo would forfeit that.
- **The first commit of a converted board is the baseline.** It must contain
  converter output and nothing else. Every later measurement diffs against it.

Toolchain resolution for anything operating on these files is
[tools/kicad_env.py](../tools/kicad_env.py) — do not hardcode `kicad-cli` paths.

**Never pick a board by globbing for the newest `.kicad_pcb`.** Routing and
cascade experiments leave variant boards (`…-c0.kicad_pcb`, `…-it.kicad_pcb`)
sitting beside the real one, and being scratch they are *always* newer — so
newest-wins reliably selects the wrong file. This bit twice on 2026-07-28:
`kicad_render.py` rendered a leftover temp board, and `kicad_live_view.py` spent
a session watching one that was then deleted underneath it. Filter to files git
tracks (`git ls-files -- '*.kicad_pcb'`) and take the newest of those; untracked
neighbours are working scratch, not the design.

## Renders

`board3/renders/*.png` are committed so the board is visible in a diff, a PR, or
a browser without installing KiCad. They are **build output, not source** — the
board is the truth and these are regenerated from it:

```sh
python tools/kicad_render.py            # refresh all three views
python tools/kicad_render.py --check    # exit 2 if older than the board
```

A tracked pre-commit hook keeps them honest. Install it once per clone:

```sh
git config core.hooksPath .githooks
```

The hook is convenience, not enforcement — it is opt-in per clone, so it is
silently absent on a fresh clone or under `--no-verify`. CI is what actually
holds the line: `.github/workflows/kicad-renders.yml` fails any PR that changes
the board without refreshing the renders, and `kicad-parts.yml` runs the part
audit on every PR.

The hook only fires when a `.kicad_pcb` is actually staged — each refresh is
~360 KB of binary that lands in history permanently, so regenerating on every
commit would bloat the repo to show a picture that did not change. If rendering
fails (no KiCad on this machine) it warns and lets the commit through: refusing
to commit firmware because a PCB picture could not be drawn is the wrong trade.

Renders are 1200×750 on purpose. Enough to read placement and routing at a
glance; open the board if you need detail.

## Watching the board change live

To see the board *while* an agent edits it, without pcbnew in the way:

```sh
python tools/kicad_live_view.py                     # newest board under kicad/
python tools/kicad_live_view.py path/to/x.kicad_pcb --port 8010
```

It serves a local page that renders the file with
[KiCanvas](https://kicanvas.org/) and reloads on every save, keeping your zoom.
Pan/zoom, layer toggles and net highlight all work.

**3D is on the second tab**, built by `kicad-cli` in the background — but only
while that tab is open, since a build costs real CPU and nobody would see it.
Two modes, because the cost difference is an order of magnitude:

| mode | what it is | board3 |
|---|---|---|
| **snapshot** (default) | `pcb render` PNG, six camera presets | **~3 s**, 0.1 MB |
| **model** | `export glb`, orbit in the browser | **~29 s**, 13.5 MB |
| **model + copper** | as above with tracks/zones/silk as geometry | **~47 s**, 31 MB |

So 3D follows an agent at roughly one frame per edit, not live — the header
always says how far behind it is (`3D showing rev 4, board at rev 7`) rather
than quietly showing you a stale board. Snapshot is the default because 3 s is
fast enough to actually watch work happen; switch to model when you want to
orbit something. There is no cheap middle: a components-free GLB builds in 1.2 s
but is a bare board, which is no use for checking a footprint.

Model mode opens **top-down on the top face** (model-viewer's own default is
nearly edge-on, which on a board this thin is a sliver). That framing is applied
once, so an orbit you set up afterwards survives every rebuild.

3D is built from the *same validated bytes* the 2D tab is showing, written to a
temp copy — so the two tabs can never disagree and a build can never catch a
half-written save. That relocation would normally drop every 3D model, since
footprints reference `${KIPRJMOD}/EASYEDA_MODELS/…` and KIPRJMOD follows the
file; the builder passes `-D KIPRJMOD=<board dir>` to put them back. Worth
knowing because the failure is silent — without it you get a bare green board
and no error.

**Why not just open pcbnew.** The editor takes a `.lck` on the board and holds
its own in-memory copy, so an agent's write and your session become two
authorities on one file — whoever saves last wins, silently. This viewer only
ever reads. That is also why it is a *viewer*: it is safe to leave open all day
precisely because it cannot write, so it does not replace the GUI for editing,
and KTD12 still holds (Update PCB from Schematic is Kevin, in the GUI, after a
restart).

Two things worth knowing:

- **A save is not atomic from the reader's side.** Every re-read is validated
  (starts with `(kicad_pcb`, parens balance outside strings) before it is
  published, so a file caught mid-write never reaches the screen — the viewer
  holds the last good revision and turns its status dot amber. Verified by
  truncating a board under a running viewer: revision held, bytes served
  unchanged.
- **KiCanvas predates KiCad 10 and skips tokens it does not know**, warning to
  the browser console. On board3 that is 344 warnings and zero errors, all of
  them non-geometry policy fields (`tenting`, `covering`, `plugging`, `capping`,
  `filling`, `duplicate_pad_numbers_are_jumpers`, dimension `units`). Copper,
  zones, footprints and silk all render. It is a preview, not an authority —
  DRC and the fab package remain the check that matters.

The bundle is cached to `tools/vendor/kicanvas.js` on first run (gitignored);
`--refresh-viewer` re-fetches it.

## Adding a part

**Every part placed on a board here must be an LCSC part, carrying supplier
metadata and a 3D model.** Not a convention — a build dependency. `kicad_fab.py`
builds the JLCPCB BOM by reading the schematic's supplier fields, so a part added
without them does not produce a warning, it produces a BOM that is quietly
missing a component.

Add parts with [tools/kicad_lcsc.py](../tools/kicad_lcsc.py), never by dragging a
generic symbol out of the stock KiCad libraries:

```sh
python tools/kicad_lcsc.py add C116592   # symbol + footprint + STEP/WRL + metadata
python tools/kicad_lcsc.py check         # audit; exits non-zero on any gap
python tools/kicad_lcsc.py models        # fetch any 3D model the board references but lacks
```

`add` drives the JLCImport plugin and then rewrites the imported symbol's fields
into Board3's vocabulary. That rewrite is the whole point: JLCImport writes the
LCSC code to a property named `LCSC`, and this project reads it from
`Supplier Part`. Import without remapping and the part looks sourced in the
schematic editor while being invisible to the BOM.

`LCSC Part Name` is **not** the part number despite its name — it holds
descriptive text. The C-number goes in `Supplier Part`.

Run `check` before generating fab output. It audits the **board**, not the
library — the board is what gets fabricated, and each placed footprint carries
its own copy of the model reference, so an instance placed before a library fix
can be wrong while the library looks perfect. It fails on any of:

- a placed part **not mapped to LCSC** (missing `Supplier Part`, or absent from
  the schematic entirely)
- a placed footprint with **no 3D model reference at all**
- a model reference whose **file is missing from disk**
- a schematic symbol with no LCSC code, placed or not

Footprints that legitimately have no body — fiducials, free pads, mounting
holes, test points — are exempt via the `BODILESS` table in the tool, and every
exemption is **printed on every run**. An exemption you cannot see is one you
cannot audit, and that table is the only path by which a real part could pass
unnoticed. Board3 currently exempts 8: three fiducials, four free pads (MP1-MP4) and H2, the Tag-Connect land pattern -- a cable presses pogo pins onto bare copper, so nothing is fitted and there is no part to source.

3D models live in `board3/EASYEDA_MODELS/` and **are tracked** (~53 MB, ~9.5 MB
in history). They are regenerable from the schematic's LCSC codes via
`kicad_lcsc.py models`, but they are committed anyway so a fresh clone renders
and exports 3D offline, and so the geometry stays pinned to this board revision
rather than to whatever EasyEDA serves later.
