"""U11 board half: fit the 3528 telltales, moving/rotating them to clear foreign copper.

Replaces the eight LED footprints, clears the stale local stubs that were routed
to the old pad positions, searches rotation+offset per part for a placement that
collides with nothing on a foreign net, then re-routes each pad to its net.
"""
import sys, math, pcbnew

BOARD = r'kicad/board3/ProPrj_New Project_2026-07-15_23-14-34_2026-07-27.kicad_pcb'
LIB = r'kicad/board3/JLCImport.pretty'
OUT = sys.argv[1]

ASSIGN = {'LED1': 'HL-A-3528U51GC-S1-13HL', 'LED2': 'HL-A-3528U51GC-S1-13HL',
          'LED3': 'HL-A-3528H343W-S1-13HL-HR3_SDCM_amp_lt_6_6000K-7000K',
          'LED4': 'HL-A-3528H203BC-S1-13HL', 'LED5': 'HVO-3528CPXA',
          'LED6': 'HL-A-3528S35FC-S1-13HL', 'LED7': 'HL-A-3528S35FC-S1-13HL',
          'LED8': 'HL-A-3528S31YC-S1-13HL'}
MM = pcbnew.FromMM
CLEAR = MM(0.15)          # 0.25 and 0.40 both scored worse; this is calibrated
OVERRIDE = {}   # a per-LED override was tried and scored worse; keep it uniform
LOCAL = MM(6.0)           # radius treated as "telltale-local" copper
# Avoiding neighbour courtyards too sounds strictly better and is not: it leaves
# LED8 unplaceable against R38, and the resulting NEW is 6 (2 shorts, 2
# clearance, 2 mask bridges) versus 3 courtyard-overlap warnings with it off.
# A courtyard overlap is an assembly-clearance warning; a short is a dead board.
AVOID_COURTYARDS = False

b = pcbnew.LoadBoard(BOARD)
olds = {f.GetReference(): f for f in b.GetFootprints() if f.GetReference() in ASSIGN}
home = {r: (olds[r].GetPosition(), olds[r].GetOrientationDegrees()) for r in olds}
nets = {}
for r, o in olds.items():
    d = {p.GetNumber(): p.GetNetname() for p in o.Pads()}
    cath = [n for n in d.values() if n.startswith('/TT')][0]
    anod = [n for n in d.values() if not n.startswith('/TT')][0]
    nets[r] = (cath, anod)

# --- 1. drop the stale local stubs -----------------------------------------
# Only copper that actually lands on an old telltale pad is stale. An earlier
# radius-based sweep also deleted /+5V feeding C30, which is why this is exact.
oldpads = []
for r, o in olds.items():
    for p in o.Pads():
        oldpads.append((p.GetPosition(), p.GetNetname()))
kill = []
for t in b.GetTracks():
    if isinstance(t, pcbnew.PCB_VIA) or not t.IsOnLayer(pcbnew.F_Cu):
        continue
    n = t.GetNetname()
    for pp, pn in oldpads:
        if pn != n:
            continue
        if min(math.hypot(t.GetStart().x - pp.x, t.GetStart().y - pp.y),
               math.hypot(t.GetEnd().x - pp.x, t.GetEnd().y - pp.y)) < MM(1.0):
            kill.append(t); break
for t in kill:
    b.Remove(t)
print('removed %d stale telltale stubs' % len(kill))

# --- 2. replace footprints --------------------------------------------------
news = {}
for r in sorted(ASSIGN):
    o = olds[r]
    vf, rf = o.Value(), o.Reference()
    keep = (vf.GetLayer(), vf.GetPosition(), vf.IsVisible(),
            rf.GetLayer(), rf.GetPosition(), rf.IsVisible())
    n = pcbnew.FootprintLoad(LIB, ASSIGN[r])
    n.SetReference(r); n.SetValue(ASSIGN[r])
    n.Value().SetLayer(keep[0]); n.Value().SetPosition(keep[1]); n.Value().SetVisible(keep[2])
    n.Reference().SetLayer(keep[3]); n.Reference().SetPosition(keep[4]); n.Reference().SetVisible(keep[5])
    cath, anod = nets[r]
    for p in n.Pads():
        p.SetNet(b.FindNet(cath if p.GetNumber() == '1' else anod))
    b.Remove(o); b.Add(n); news[r] = n

# --- 3. foreign copper to avoid --------------------------------------------
def shapes_near(pos):
    """Every F.Cu shape within reach, tagged with its net.

    Tagged rather than pre-filtered because "own net" is a property of the PAD,
    not the footprint: /+5V is LED6 pad 2's net, but a /+5V via touching pad 1
    is a real short. Filtering per-footprint hid exactly that.
    """
    out = []
    for t in b.GetTracks():
        if not t.IsOnLayer(pcbnew.F_Cu):
            continue
        if min(math.hypot(t.GetStart().x - pos.x, t.GetStart().y - pos.y),
               math.hypot(t.GetEnd().x - pos.x, t.GetEnd().y - pos.y)) > MM(12):
            continue
        out.append((t.GetEffectiveShape(pcbnew.F_Cu), t.GetNetname()))
    for f in b.GetFootprints():
        if f.GetReference() in ASSIGN:
            continue
        for p in f.Pads():
            if not p.IsOnLayer(pcbnew.F_Cu):
                continue
            pp = p.GetPosition()
            if math.hypot(pp.x - pos.x, pp.y - pos.y) > MM(12):
                continue
            out.append((p.GetEffectiveShape(pcbnew.F_Cu), p.GetNetname()))
    return out


def courtyards_near(pos):
    """Neighbouring courtyards. Copper clearance alone is not enough -- the 3528
    body is wider than the 0805 it replaces, so a placement can be electrically
    clean and still overlap the telltale's own series resistor."""
    out = []
    for f in b.GetFootprints():
        if f.GetReference() in ASSIGN:
            continue
        fp_pos = f.GetPosition()
        if math.hypot(fp_pos.x - pos.x, fp_pos.y - pos.y) > MM(12):
            continue
        cy = f.GetCourtyard(pcbnew.F_CrtYd)
        if cy.OutlineCount():
            out.append(cy)
    return out


def hits(fp, obstacles, clear=None, courtyards=()):
    n = 0
    for p in fp.Pads():
        s = p.GetEffectiveShape(pcbnew.F_Cu)
        mynet = p.GetNetname()
        for o, onet in obstacles:
            if onet == mynet:
                continue
            if s.Collide(o, clear or CLEAR):
                n += 1
    if courtyards:
        cy = fp.GetCourtyard(pcbnew.F_CrtYd)
        if cy.OutlineCount():
            for ncy in courtyards:
                if cy.Collide(ncy, 0):
                    n += 1
    return n


# --- 4. search rotation + small offset --------------------------------------
STEP = MM(0.25)
placed = {}
for r in sorted(ASSIGN):
    fp = news[r]
    pos, rot0 = home[r]
    obs = shapes_near(pos)
    cys = courtyards_near(pos) if AVOID_COURTYARDS else ()
    best = None
    for k in range(0, 9):                        # rings of increasing offset
        for rot in (rot0, 0.0, 90.0, 180.0, 270.0):
            for dx in range(-k, k + 1):
                for dy in range(-k, k + 1):
                    if max(abs(dx), abs(dy)) != k:
                        continue
                    np_ = pcbnew.VECTOR2I(pos.x + dx * STEP, pos.y + dy * STEP)
                    fp.SetOrientationDegrees(rot); fp.SetPosition(np_)
                    if hits(fp, obs, OVERRIDE.get(r), cys) == 0:
                        cost = (k, 0 if rot == rot0 else 1)
                        if best is None or cost < best[0]:
                            best = (cost, rot, np_)
        if best is not None:
            break
    if best is None:
        fp.SetOrientationDegrees(rot0); fp.SetPosition(pos)
        print('  %-5s NO CLEAR PLACEMENT FOUND -- left at home' % r)
        placed[r] = None
        continue
    _, rot, np_ = best
    fp.SetOrientationDegrees(rot); fp.SetPosition(np_)
    dxmm = pcbnew.ToMM(np_.x - pos.x); dymm = pcbnew.ToMM(np_.y - pos.y)
    placed[r] = (rot, dxmm, dymm)
    print('  %-5s rot %7.1f -> %7.1f   offset (%+.2f, %+.2f) mm' % (r, rot0, rot, dxmm, dymm))

# --- 5. re-route each pad to its net ---------------------------------------
# Pass "noroute" to leave the telltale pads unrouted for a real autorouter.
# The naive nearest-endpoint connector below cannot re-establish the /+5V
# daisy-chain that runs through these pads, so it orphans C30.
if len(sys.argv) > 2 and sys.argv[2] == 'noroute':
    pcbnew.ZONE_FILLER(b).Fill(b.Zones())
    pcbnew.SaveBoard(OUT, b)
    print('left telltale pads unrouted; saved', OUT)
    raise SystemExit(0)

added = 0
for r in sorted(ASSIGN):
    fp = news[r]
    for p in fp.Pads():
        net = p.GetNetname()
        pp = p.GetPosition()
        tgt, bestd = None, None
        for t in b.GetTracks():
            if t.GetNetname() != net or isinstance(t, pcbnew.PCB_VIA):
                continue
            for e in (t.GetStart(), t.GetEnd()):
                d = math.hypot(e.x - pp.x, e.y - pp.y)
                if bestd is None or d < bestd:
                    bestd, tgt = d, e
        for f2 in b.GetFootprints():
            if f2.GetReference() == r:
                continue
            for p2 in f2.Pads():
                if p2.GetNetname() != net or not p2.IsOnLayer(pcbnew.F_Cu):
                    continue
                e = p2.GetPosition()
                d = math.hypot(e.x - pp.x, e.y - pp.y)
                if bestd is None or d < bestd:
                    bestd, tgt = d, e
        if tgt is None or bestd > MM(15):
            print('  %-5s pad%s [%s] no reachable target' % (r, p.GetNumber(), net))
            continue
        tr = pcbnew.PCB_TRACK(b)
        tr.SetStart(pp); tr.SetEnd(tgt)
        tr.SetWidth(MM(0.254)); tr.SetLayer(pcbnew.F_Cu)
        tr.SetNet(b.FindNet(net))
        b.Add(tr); added += 1
print('added %d connecting tracks' % added)

pcbnew.ZONE_FILLER(b).Fill(b.Zones())
pcbnew.SaveBoard(OUT, b)
print('saved', OUT)
