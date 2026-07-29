---
title: Verifying EasyEDA design state by reading the .eprj2 project file
date: 2026-07-25
category: developer-experience
module: easyeda-workflow
problem_type: developer_experience
component: tooling
severity: medium
applies_when:
  - "An agent needs to verify EasyEDA schematic/PCB edits and the MCP bridge is not connected in the session"
  - "Confirming whether a user's in-app edit was actually saved"
  - "Auditing part placement, deletions, or board copies against a spec"
tags: [easyeda, eprj2, sqlite, verification, thumbnails, agent-workflow]
---

# Verifying EasyEDA design state by reading the .eprj2 project file

## Context

During Board3 bring-up (2026-07-25), the agent needed to verify part placements and deletions on an EasyEDA Pro schematic while the MCP bridge was unavailable to the session. The local `.eprj2` project file turned out to support a full read-only verification loop.

## Guidance

The `.eprj2` file is a **SQLite database**. Two tables carry everything needed for verification:

- **`project_structures`** — rows of JSON in the `structure` column; the latest row (highest `id`) holds the current tree: `boards`, `schematics`, `sheets`, `pcbs` keyed by UUID, each with `title`/`name`, cross-link fields (`board`, `schematic_uuid`), and a `version`/`updateTime` stamp (epoch ms).
- **`project_images`** — one row per document UUID; `image_data` is a **base64-encoded WebP** thumbnail (TEXT, not BLOB) of the full sheet/board, high-resolution enough to audit part placement, designators, and values.

Working verification loop (Windows: use `py`; the `python3` alias is a Store stub):

```python
import sqlite3, base64, json
c = sqlite3.connect('New Project_....eprj2')
s = json.loads(c.execute(
    'select structure from project_structures order by cast(id as integer) desc limit 1'
).fetchone()[0])
print(s['schematics']['<uuid>']['version'])          # save stamp
row = c.execute('select image_data from project_images where uuid=?', ('<sheet-uuid>',)).fetchone()
open('sheet.webp','wb').write(base64.b64decode(row[0]))  # view with an image-capable Read
```

**Save semantics — the critical part:**

- The `version` stamp and thumbnail regenerate **only when the document is saved** (Ctrl+S with that document's tab active). The `.eprj2` file's mtime can advance without them (project-level autosave) — an unchanged version stamp + identical thumbnail byte count means the user's edits have NOT been saved yet, not that they didn't happen.
- Thumbnail byte-size deltas are a fast tell before viewing: growth ≈ parts added, shrinkage ≈ deletions.

**Limits:** document *content* (netlists, primitives) is not locally readable — the `documents` table rows are empty locally (cloud-synced). Net-level verification needs the bridge or exports; the thumbnail supports placement/designator/value audits and gross wiring review only. Treat the file as **read-only** — schema is undocumented and writes risk corruption.

## Why This Matters

This makes agent-verifies-human-CAD possible with zero tooling: the user draws in the app, saves, and the agent independently confirms what landed — catching unsaved work (happened twice in one session), missed deletions, and malformed designators from the thumbnail alone.

## When to Apply

- Bridge down or session started before the MCP server (tools can't hot-attach)
- Any "double-check me" request after in-app edits
- Confirming a board copy (compare `project_images` base64 prefixes — identical content = identical copy)

## Examples

From the Board3 session: three successive pulls of sheet P3 showed (1) version unchanged → "you forgot to save"; (2) version bumped, bytes 274,166 → placement audit from thumbnail found two designator issues and three undeleted symbols; (3) bytes down to 251,730 → deletions confirmed visually.

## Related

- docs/solutions/integration-issues/easyeda-jlc-ghost-listing-zero-stock.md — same-session EasyEDA sourcing trap
- docs/plans/2026-07-24-001-feat-board3-h755-carrier-plan.md — the execution posture this workflow supports (Kevin drives the app, agent verifies via .eprj2)
