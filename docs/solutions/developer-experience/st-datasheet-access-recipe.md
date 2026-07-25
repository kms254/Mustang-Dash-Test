---
title: ST datasheet access recipe — r.jina.ai proxy beats timeouts and mirror rot
date: 2026-07-25
category: developer-experience
module: hardware-reference
problem_type: developer_experience
component: tooling
severity: medium
applies_when:
  - "Verifying pin/electrical facts against an ST datasheet, app note, or errata (DS/AN/ES documents)"
  - "Any st.com PDF fetch from this workstation times out"
  - "Tempted to cite a third-party datasheet mirror in a doc or plan"
tags: [st-datasheets, network-workaround, jina-proxy, pypdf, documentation-sources]
---

# ST datasheet access recipe — r.jina.ai proxy beats timeouts and mirror rot

## Context

Board3 planning (2026-07-25) required verifying DS12923 (STM32H755 datasheet),
AN2606 (bootloader), and ES0445 (errata) claims before they became KTDs.
Direct `st.com` PDF fetches time out from this workstation, and the obvious
fallback — a third-party mirror — proved unreliable: the Avnet mirror URL used
on 2026-07-25 returned 404 within hours of first use.

## Guidance

Working method, in order of preference:

1. **Markdown via the Jina reader proxy** (fastest, no local tooling):

   ```bash
   curl -L "https://r.jina.ai/<full st.com PDF url>"
   ```

   Returns the PDF converted to markdown text. Large ST datasheets
   (DS12923 is several hundred pages) came through without hitting a size cap.

2. **Raw PDF + local extraction** when the markdown conversion mangles tables
   (pin tables, AF maps): download the PDF by whatever route succeeds, then
   extract text with `pypdf` inside WSL (no host Python assumptions on
   Windows boxes without one).

3. **Avoid third-party mirrors as cited sources.** They rot — the one used
   2026-07-25 404'd the same day. If a mirror is the only way to get the file,
   fine for reading, but cite the canonical st.com URL + document
   number/revision in any doc or plan.

## Why This Matters

Pin maps and power-tree decisions (VCAP counts, SMPS strapping, QUADSPI AF
assignments) are copper-level commitments — a wrong claim costs a board spin.
That makes datasheet verification non-optional, and an unreliable access path
quietly encourages "trust the forum post" shortcuts. A known-good fetch recipe
keeps verification cheap enough to actually do.

## When to Apply

- Any ST document verification (DS/AN/ES/RM) from this workstation
- Other vendors whose PDF hosting blocks or times out on direct fetch —
  the r.jina.ai prefix trick is vendor-agnostic
- Whenever a doc cites a datasheet: cite document number + revision +
  canonical URL, never a mirror link

## Examples

```bash
# DS12923 (STM32H755ZI datasheet) as markdown text:
curl -L "https://r.jina.ai/https://www.st.com/resource/en/datasheet/stm32h755zi.pdf"

# Raw PDF text extraction in WSL when tables matter:
wsl -- bash -lc "python3 -c \"from pypdf import PdfReader; r=PdfReader('ds12923.pdf'); print(r.pages[100].extract_text())\""
```

Used to verify DS12923 / AN2606 / ES0445 for
`docs/plans/2026-07-24-001-feat-board3-h755-carrier-plan.md` (KTD2-KTD4) and
`docs/hardware/board3-h755-pin-map.md`.

## Related

- [Board3 H755 carrier plan](../../plans/2026-07-24-001-feat-board3-h755-carrier-plan.md)
- [Board3 pin map](../../hardware/board3-h755-pin-map.md)
