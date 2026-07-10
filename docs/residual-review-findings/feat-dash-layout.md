# Residual Review Findings — feat/dash-layout

Source: multi-agent code review run `20260709-234607-72aedb8c` (8 reviewers +
1 validator), 2026-07-09/10. Twelve findings total; eleven were applied and
re-verified on hardware in `fix(review)` (2ca7c2b). The items below are the
durable record of what was **not** applied.

## Decision gate (needs Kevin's call)

- ~~**P1 — `MustangDash/MustangDash.ino:1` — file crossed 1000 lines (565 → ~1480), five concerns in one file** (maintainability, anchor 100).
  Suggested split: splash rendering → `splash_render.h`, gauge/mode/alarm
  renderers → `dash_render.h`, leaving the `.ino` as setup/loop/glue. Deferred
  because it is a structural design call that interacts with the offline
  build's explicit-prototype convention, and churning ~800 lines during the
  same PR as the feature would obscure the review trail. Good candidate for a
  small follow-up PR.~~ **Resolved 2026-07-10** by the `refactor/ino-split`
  follow-up (plan `docs/plans/2026-07-10-001-refactor-ino-split-plan.md`):
  pure code motion into `MustangDash/dash_render.h` and
  `MustangDash/splash_render.h`, `.ino` down to ~540 lines of setup/loop/glue.
  Verified: 11/11 host suite, both build paths clean with exactly agreeing
  section sizes (753984/16064/18464 — the invariant's operational form; raw
  binaries differ by pre-existing toolchain branch-offset drift, measured on
  the pre-refactor baseline too), bench smoke on the panel (boot banner sane,
  60 fps, both modes ack).

## Report-only residual risks (no action required now)

- **Simulator miles persist to the real odometer** — intentional per plan R16
  (`odo set` corrects when CAN lands), but note `sim off` holds a frozen
  nonzero speed valid, so a paused bench still accrues. Revisit at CAN
  integration when a data-provenance flag exists.
- **Crossfade frame (splash + dash in one display list) is the largest DL of
  the product's life and is never measured** against the 2048-entry budget
  (boot diagnostic measures the two modes separately).
- **`pump_serial` has no per-frame byte budget** — a deliberate sustained
  flood starves rendering; bench-only (no USB host in the car).
- **First-boot provisioning holds the panel dark for several seconds** with no
  visual heartbeat; cranking-voltage SPI integrity during `CMD_FLASHUPDATE`
  is unverified.
- **EVE fault auto-recovery does not reinitialize flash**
  (`EVE_commands.c:432`) — a trap if runtime flash rendering is ever added
  beyond the splash.
- **Testing gaps**: font zlib streams verified by magic byte only (host suite
  lacks zlib); no torn-provisioning scenario test; `dash_flash_phase(ms, 0)`
  guard untested.
- **Stale solution docs** (compound-refresh candidates):
  `docs/solutions/design-patterns/eve-ram-g-budgeting-multi-theme-splash-assets.md`
  (splash-resident-in-RAM_G worked example) and
  `docs/solutions/design-patterns/eve-logo-onchip-png-decode-skeleton-silhouette.md` (since deleted in the 2026-07-10 refresh — subject removed by this branch)
  (pony screen) describe superseded architecture.

## Verification still owed a human eyeball

- ~~The P0 font-format fix (L2→L4) and the ASTC splash migration are
  serial-verified only; confirm rendered glyph quality and splash visual
  fidelity on the panel.~~ **Verified 2026-07-10 by Kevin on the panel:
  "looks good — the font fix fixed the biggest issue"** — confirming both
  that the pre-fix L2/L4 corruption was visually real and that the fix
  resolved it, along with the flash-resident splash's fidelity. A formal
  AE8 side-by-side against `assets/dash-design/renderings/` remains
  optional polish.
