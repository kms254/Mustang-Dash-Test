# Hand-route log

Every copper edit applied to Board3 by `tools/kicad_handroute.py`, in the order
it went on. Each file carries a `$comment` explaining the intent and the
constraints that shaped it.

This is a **log, not a replay script**. Some steps were superseded by later ones,
and one whole unit was walked back — re-applying these in order against the
imported board will not reproduce the current board. Read them for the reasoning;
trust `git log` and the board itself for the state.

| Order | File | Unit | Status |
|---|---|---|---|
| 1 | `u6-flagged-nets.json` | U6 | Applied. The CAN and USB steps still stand; the crystal and QSPI steps were later reverted by step 6. |
| 2 | `u7-can1-term.json` | U6 follow-up | Applied. CAN1_TERM's +5V plane slot 9.70 → 1.51 mm. |
| 3 | `u7-thermal-fiducials.json` | U7 | Applied. U4's 5 thermal vias and the 3 fiducials still stand; U2's 9 were re-placed by step 7. |
| 4 | `u7-depad-r28.json` | U7 follow-up | Applied. R28.2's via moved out of its pad. |
| 5 | `u7-btn1-annular.json` | U7 follow-up | Applied. U1.93's via to 0.5/0.2 for a 0.15 mm ring. |
| 6 | `u7-walkback-crystal-qspi.json` | U7 follow-up | Applied. Reverts U6's crystal move and QSPI reroute to the imported geometry — see the plan's U6 amendment for why. |
| 7 | `u7-thermal-u2-replaced.json` | U7 follow-up | Applied. U2's 9 thermal vias re-placed against the restored routing. |

A step that was tried and reverted is not kept here — `u7-depad-qspi.json`
(moving the QSPI escape vias out of U2's pads by hand) produced 8 shorts and 1
clearance violation and was rolled back in favour of step 6. Its failure is
recorded in the plan, which is the right place for it.
