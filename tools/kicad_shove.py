#!/usr/bin/env python3
"""Negotiated-congestion shove router: route a net by displacing other nets.

Why this exists (2026-07-29): headless routing of the I2C revision hit a wall
that avoid-only tooling cannot pass. U11's east pad field ends in an escape
mouth that fits exactly two 0.254 mm tracks; TT5 and TT7 held them and TT2
could not route -- 13k exact-math path candidates, six rip-and-reorder cycles
and two schematic pin-move attempts all failed, because every tool treated
existing copper as immovable. The fix (Kevin's observation, from one look at
the layout: "you could have just moved those two vias") is displacement:
treat designated nets' copper as MOVABLE, route through it at a cost, then
relocate what the new route displaced. That is a shove router.

This is the PathFinder (negotiated congestion) formulation, not an elastic
PNS-style shover: iterative rip-up-and-reroute with rising history costs on
contested cells, which converges where one-shot shoving oscillates.

The algorithm:
  1. Grid A* over F.Cu/In1.Cu/B.Cu with via transitions. Copper on FIXED
     nets blocks a cell outright; copper on MOVABLE nets adds a displacement
     penalty plus that cell's accumulated history cost.
  2. Lay the cheapest path, then compute -- with exact segment/segment and
     point/segment math from raw endpoints, never GetEffectiveShape/Collide,
     which under-covers midpoints and misreports via shapes on inner layers
     (bisected the hard way, see CLAUDE.md) -- which movable items now
     conflict.
  3. Cheapest displacement first: a conflicting VIA gets a ring-search
     relocation with its attached segments re-stitched. If no legal spot
     exists (or the conflict is a track), that net's tracks are ripped and
     the net joins the routing queue; its old cells take a history penalty
     so the next attempt explores elsewhere.
  4. Repeat until the queue is empty or the iteration budget dies. A final
     exact-math validation over every touched net decides success; the
     board is only saved on a clean result.

Hard-won pcbnew rules honored here: proxies of removed items stay referenced
until SaveBoard (SWIG session corruption otherwise); one board per process;
PCB_VIA.GetWidth needs a layer argument (avoided entirely -- geometry comes
from GetDrillValue and rule constants).

Usage (KiCad's python, never system python):
  "C:/Program Files/KiCad/10.0/bin/python.exe" tools/kicad_shove.py \
      board.kicad_pcb --net /TT2_LED_K --movable "/TT*_LED_K" \
      --out routed.kicad_pcb

Then run tools/kicad_verify.py on the result -- this tool's own validation
is necessary, not sufficient; the staged-rules DRC is the only judge.
"""

from __future__ import annotations

import argparse
import fnmatch
import heapq
import math
import sys

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_UNROUTED = 2

# geometry constants matched to tools/kicad_rules.json's default netclass
TRACK_W = 0.254
CLR = 0.1016
VIA_D = 0.6
VIA_DRILL = 0.3
GRID_MM = 0.2
SHOVE_COST = 40          # grid-steps equivalent for entering a movable cell
HISTORY_STEP = 25        # added to a cell each time its occupant is ripped
VIA_COST = 14
NODE_CAP = 600000
MAX_ROUTES = 24          # total routing attempts across the whole negotiation


def pt_seg(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    L2 = dx * dx + dy * dy
    if L2 == 0:
        return math.hypot(px - x1, py - y1)
    t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / L2))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def seg_seg(a, c):
    (x1, y1, x2, y2) = a
    (x3, y3, x4, y4) = c
    d1 = (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1)
    d2 = (x2 - x1) * (y4 - y1) - (y2 - y1) * (x4 - x1)
    d3 = (x4 - x3) * (y1 - y3) - (y4 - y3) * (x1 - x3)
    d4 = (x4 - x3) * (y2 - y3) - (y4 - y3) * (x2 - x3)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
        return 0.0
    return min(pt_seg(x1, y1, x3, y3, x4, y4), pt_seg(x2, y2, x3, y3, x4, y4),
               pt_seg(x3, y3, x1, y1, x2, y2), pt_seg(x4, y4, x1, y1, x2, y2))


def seg_rect(a, rx1, ry1, rx2, ry2):
    x1, y1, x2, y2 = a
    if (rx1 <= x1 <= rx2 and ry1 <= y1 <= ry2) or (rx1 <= x2 <= rx2 and ry1 <= y2 <= ry2):
        return 0.0
    edges = [(rx1, ry1, rx2, ry1), (rx2, ry1, rx2, ry2),
             (rx2, ry2, rx1, ry2), (rx1, ry2, rx1, ry1)]
    return min(seg_seg(a, e) for e in edges)


class Shover:
    def __init__(self, board_path, movable_patterns):
        import pcbnew
        self.pcbnew = pcbnew
        self.b = pcbnew.LoadBoard(board_path)
        self.mm = pcbnew.ToMM
        self.FromMM = pcbnew.FromMM
        self.layers = [pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.B_Cu]
        self.all_layers = [pcbnew.F_Cu, pcbnew.In1_Cu, pcbnew.In2_Cu, pcbnew.B_Cu]
        self.movable_patterns = movable_patterns
        self.keepalive = []          # SWIG lifetime rule
        self.history = {}            # cell -> accumulated rip penalty
        self.touched = set()
        bb = self.b.GetBoardEdgesBoundingBox()
        self.edge = (self.mm(bb.GetLeft()) + 0.45, self.mm(bb.GetTop()) + 0.45,
                     self.mm(bb.GetRight()) - 0.45, self.mm(bb.GetBottom()) - 0.45)
        self.fps = {fp.GetReference(): fp for fp in self.b.GetFootprints()}
        self._tracks = None
        self._vias = None
        self._pads = None

    # ---------- classification ----------
    def is_movable(self, net):
        return any(fnmatch.fnmatch(net, p) for p in self.movable_patterns)

    # ---------- exact-geometry inventories (cached; invalidate on mutation) ----------
    # A 5 mm spatial hash makes cell probing O(nearby) instead of O(board):
    # without it the first field run burned 3 CPU-hours before being killed.
    BUCKET = 5.0

    def invalidate(self):
        self._tracks = None
        self._vias = None
        self._index = None

    def _buckets_for(self, x1, y1, x2, y2, pad=0.7):
        bx1 = int((min(x1, x2) - pad) // self.BUCKET)
        bx2 = int((max(x1, x2) + pad) // self.BUCKET)
        by1 = int((min(y1, y2) - pad) // self.BUCKET)
        by2 = int((max(y1, y2) + pad) // self.BUCKET)
        for bx in range(bx1, bx2 + 1):
            for by in range(by1, by2 + 1):
                yield (bx, by)

    def index(self):
        """spatial hash: bucket -> (tracks, vias, pads) lists"""
        if getattr(self, "_index", None) is None:
            idx = {}
            for o in self.tracks():
                a = self.track_mm(o)
                for k in self._buckets_for(a[0], a[1], a[2], a[3]):
                    idx.setdefault(k, ([], [], []))[0].append(o)
            for o in self.vias():
                p = o.GetPosition()
                x, y = self.mm(p.x), self.mm(p.y)
                for k in self._buckets_for(x, y, x, y):
                    idx.setdefault(k, ([], [], []))[1].append(o)
            for pr in self.pad_rects():
                for k in self._buckets_for(pr[1], pr[2], pr[3], pr[4]):
                    idx.setdefault(k, ([], [], []))[2].append(pr)
            self._index = idx
        return self._index

    def near(self, x, y):
        """(tracks, vias, pads) in the point's bucket"""
        return self.index().get((int(x // self.BUCKET), int(y // self.BUCKET)),
                                ([], [], []))

    def tracks(self):
        if self._tracks is None:
            self._tracks = [t for t in self.b.GetTracks() if t.GetClass() != "PCB_VIA"]
        return self._tracks

    def vias(self):
        if self._vias is None:
            self._vias = [t for t in self.b.GetTracks() if t.GetClass() == "PCB_VIA"]
        return self._vias

    def track_mm(self, t):
        s, e = t.GetStart(), t.GetEnd()
        return (self.mm(s.x), self.mm(s.y), self.mm(e.x), self.mm(e.y))

    def pad_rects(self):
        if self._pads is None:
            out = []
            for fp in self.b.GetFootprints():
                for pad in fp.Pads():
                    bb = pad.GetBoundingBox()
                    out.append((pad.GetNetname(), self.mm(bb.GetLeft()), self.mm(bb.GetTop()),
                                self.mm(bb.GetRight()), self.mm(bb.GetBottom()),
                                fp.GetReference(), pad.GetNumber()))
            self._pads = out
        return self._pads

    # ---------- exact conflict tests ----------
    def seg_conflicts(self, a, net, layer, ignore=()):
        """items conflicting with a candidate segment (mm tuple) on layer"""
        out = []
        for o in self.tracks():
            if o in ignore or o.GetNetname() == net or o.GetLayer() != layer:
                continue
            need = TRACK_W / 2 + self.mm(o.GetWidth()) / 2 + CLR
            if seg_seg(a, self.track_mm(o)) < need - 0.001:
                out.append(o)
        for o in self.vias():
            if o in ignore or o.GetNetname() == net:
                continue
            p = o.GetPosition()
            if pt_seg(self.mm(p.x), self.mm(p.y), *a) < VIA_D / 2 + TRACK_W / 2 + CLR - 0.001:
                out.append(o)
        for (pn, rx1, ry1, rx2, ry2, ref, num) in self.pad_rects():
            if pn == net:
                continue
            if seg_rect(a, rx1, ry1, rx2, ry2) < TRACK_W / 2 + CLR - 0.001:
                out.append(("pad", ref, num))
        return out

    def via_conflicts(self, vx, vy, net, ignore=()):
        out = []
        for o in self.tracks():
            if o in ignore or o.GetNetname() == net:
                continue
            if pt_seg(vx, vy, *self.track_mm(o)) < VIA_D / 2 + self.mm(o.GetWidth()) / 2 + CLR - 0.001:
                out.append(o)
        for o in self.vias():
            if o in ignore or o.GetNetname() == net:
                continue
            p = o.GetPosition()
            if math.hypot(vx - self.mm(p.x), vy - self.mm(p.y)) < VIA_D + CLR - 0.001:
                out.append(o)
        for (pn, rx1, ry1, rx2, ry2, ref, num) in self.pad_rects():
            if pn == net:
                continue
            cx = min(max(vx, rx1), rx2)
            cy = min(max(vy, ry1), ry2)
            if math.hypot(vx - cx, vy - cy) < VIA_D / 2 + CLR - 0.001:
                out.append(("pad", ref, num))
        return out

    # ---------- A* with soft (movable) obstacles ----------
    def snap(self, v):
        return round(v / GRID_MM) * GRID_MM

    def cell_state(self, x, y, layer, net):
        """None=free, 'fixed'=hard block, or the movable blocking net name.
        Probe radius 0.27 covers midpoint gaps at this grid (CLAUDE.md)."""
        r = 0.27
        if not (self.edge[0] <= x <= self.edge[2] and self.edge[1] <= y <= self.edge[3]):
            return "fixed"
        hit_movable = None
        ntracks, nvias, npads = self.near(x, y)
        for o in ntracks:
            if o.GetNetname() == net or o.GetLayer() != layer:
                continue
            if pt_seg(x, y, *self.track_mm(o)) < r + self.mm(o.GetWidth()) / 2:
                if self.is_movable(o.GetNetname()):
                    hit_movable = o.GetNetname()
                else:
                    return "fixed"
        for o in nvias:
            if o.GetNetname() == net:
                continue
            p = o.GetPosition()
            if math.hypot(x - self.mm(p.x), y - self.mm(p.y)) < r + VIA_D / 2:
                if self.is_movable(o.GetNetname()):
                    hit_movable = o.GetNetname()
                else:
                    return "fixed"
        for (pn, rx1, ry1, rx2, ry2, ref, num) in npads:
            if pn == net:
                continue
            cx = min(max(x, rx1), rx2)
            cy = min(max(y, ry1), ry2)
            if math.hypot(x - cx, y - cy) < r:
                return "fixed"   # pads are never shoved
        return hit_movable

    def goals_for(self, net, exclude_ref):
        pts = []
        for t in self.b.GetTracks():
            if t.GetNetname() != net:
                continue
            if t.GetClass() == "PCB_VIA":
                p = t.GetPosition()
                for L in self.layers:
                    pts.append((self.mm(p.x), self.mm(p.y), L))
                continue
            a = self.track_mm(t)
            n = max(1, int(math.hypot(a[2] - a[0], a[3] - a[1]) / 0.5))
            for i in range(n + 1):
                pts.append((a[0] + (a[2] - a[0]) * i / n,
                            a[1] + (a[3] - a[1]) * i / n, t.GetLayer()))
        for fp in self.b.GetFootprints():
            if fp.GetReference() == exclude_ref:
                continue
            for pad in fp.Pads():
                if pad.GetNetname() == net and pad.IsOnLayer(self.pcbnew.F_Cu):
                    p = pad.GetPosition()
                    pts.append((self.mm(p.x), self.mm(p.y), self.pcbnew.F_Cu))
        return pts

    def route(self, net, ref, padnum):
        """A* from the pad; returns (pad point, path cells, goal map) or None."""
        pcbnew = self.pcbnew
        pad = next(p for p in self.fps[ref].Pads() if p.GetNumber() == padnum)
        pp = pad.GetPosition()
        px, py = self.mm(pp.x), self.mm(pp.y)
        fpc = self.fps[ref].GetPosition()
        dx, dy = pp.x - fpc.x, pp.y - fpc.y
        ex, ey = ((1 if dx > 0 else -1), 0) if abs(dx) >= abs(dy) else (0, (1 if dy > 0 else -1))
        goals = self.goals_for(net, ref)
        goals = [g for g in goals if abs(g[0] - px) > 3.0 or abs(g[1] - py) > 3.0]
        if not goals:
            print(f"  {net}: no goals")
            return None
        gset = {}
        for gx, gy, gL in goals:
            gset.setdefault((self.snap(gx), self.snap(gy), gL), []).append((gx, gy))

        # clip exploration to a corridor around pad+goals: without this the
        # search floods the whole board's open field (600k-node failures)
        rx1 = min([px] + [g[0] for g in goals]) - 12.0
        rx2 = max([px] + [g[0] for g in goals]) + 12.0
        ry1 = min([py] + [g[1] for g in goals]) - 12.0
        ry2 = max([py] + [g[1] for g in goals]) + 12.0

        HW = 2.0  # weighted A*: goal-directedness must survive shove tolls

        def h(x, y):
            return HW * min(abs(x - gx) + abs(y - gy) for gx, gy, _ in goals) / GRID_MM

        state_memo = {}

        def state(x, y, L):
            k = (round(x, 3), round(y, 3), L)
            if k not in state_memo:
                state_memo[k] = self.cell_state(x, y, L, net)
            return state_memo[k]

        openq, gcost, came = [], {}, {}
        # comb seeding along the legal-by-construction pad exit lane
        lane_open = True
        for k in range(0, 16):
            sx, sy = px + ex * k * GRID_MM, py + ey * k * GRID_MM
            if k > 4:
                if state(self.snap(sx), self.snap(sy), pcbnew.F_Cu) == "fixed":
                    lane_open = False
            if not lane_open:
                break
            c = (self.snap(sx), self.snap(sy), pcbnew.F_Cu)
            if c not in gcost:
                gcost[c] = k
                heapq.heappush(openq, (k + h(c[0], c[1]), k, c, None))
        found, nodes = None, 0
        while openq and nodes < NODE_CAP:
            f, g, cur, parent = heapq.heappop(openq)
            if cur in came:
                continue
            came[cur] = parent
            nodes += 1
            x, y, L = cur
            if (x, y, L) in gset:
                found = cur
                break
            for ddx, ddy in ((GRID_MM, 0), (-GRID_MM, 0), (0, GRID_MM), (0, -GRID_MM)):
                nx, ny = round(x + ddx, 3), round(y + ddy, 3)
                if not (rx1 <= nx <= rx2 and ry1 <= ny <= ry2):
                    continue
                nxt = (nx, ny, L)
                if nxt in came:
                    continue
                st = state(nx, ny, L)
                if st == "fixed":
                    continue
                step = 1
                if st is not None:
                    step += SHOVE_COST + min(self.history.get(nxt, 0), 100)
                ng = g + step
                if ng < gcost.get(nxt, 1e18):
                    gcost[nxt] = ng
                    heapq.heappush(openq, (ng + h(nx, ny), ng, nxt, cur))
            for L2 in self.layers:
                if L2 == L:
                    continue
                nxt = (x, y, L2)
                if nxt in came:
                    continue
                bad = False
                soft = 0
                for LA in self.all_layers:
                    st = self.cell_state(x, y, LA, net)
                    if st == "fixed":
                        bad = True
                        break
                    if st is not None:
                        soft = SHOVE_COST
                if bad:
                    continue
                ng = g + VIA_COST + soft + self.history.get(nxt, 0)
                if ng < gcost.get(nxt, 1e18):
                    gcost[nxt] = ng
                    heapq.heappush(openq, (ng + h(x, y), ng, nxt, cur))
        if not found:
            print(f"  {net}: no path ({nodes} nodes)")
            return None
        path = []
        cur = found
        while cur is not None:
            path.append(cur)
            cur = came[cur]
        path.reverse()
        print(f"  {net}: path {len(path)} cells, {nodes} nodes")
        return (px, py), path, gset

    # ---------- laying + displacement ----------
    def lay_path(self, net, pad_pt, path, gset):
        pcbnew = self.pcbnew
        FromMM = self.FromMM
        laid = []

        def seg(p1, p2, layer):
            s = pcbnew.PCB_TRACK(self.b)
            s.SetStart(pcbnew.VECTOR2I(FromMM(p1[0]), FromMM(p1[1])))
            s.SetEnd(pcbnew.VECTOR2I(FromMM(p2[0]), FromMM(p2[1])))
            s.SetLayer(layer)
            s.SetWidth(FromMM(TRACK_W))
            s.SetNet(self.b.FindNet(net))
            self.b.Add(s)
            self.keepalive.append(s)
            laid.append(s)

        def via(p):
            v = pcbnew.PCB_VIA(self.b)
            v.SetPosition(pcbnew.VECTOR2I(FromMM(p[0]), FromMM(p[1])))
            v.SetWidth(FromMM(VIA_D))
            v.SetDrill(FromMM(VIA_DRILL))
            v.SetViaType(pcbnew.VIATYPE_THROUGH)
            v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
            v.SetNet(self.b.FindNet(net))
            self.b.Add(v)
            self.keepalive.append(v)
            laid.append(v)

        first = path[0]
        if (pad_pt[0], pad_pt[1]) != (first[0], first[1]):
            seg(pad_pt, (first[0], first[1]), pcbnew.F_Cu)
        i = 0
        while i < len(path) - 1:
            x, y, L = path[i]
            nx, ny, nL = path[i + 1]
            if nL != L:
                via((x, y))
                i += 1
                continue
            j = i + 1
            sdx, sdy = round(nx - x, 3), round(ny - y, 3)
            while (j + 1 < len(path) and path[j + 1][2] == L
                   and round(path[j + 1][0] - path[j][0], 3) == sdx
                   and round(path[j + 1][1] - path[j][1], 3) == sdy):
                j += 1
            seg((x, y), (path[j][0], path[j][1]), L)
            i = j
        last = path[-1]
        gpts = gset[(last[0], last[1], last[2])]
        gx, gy = min(gpts, key=lambda q: abs(q[0] - last[0]) + abs(q[1] - last[1]))
        if (gx, gy) != (last[0], last[1]):
            seg((last[0], last[1]), (gx, gy), last[2])
        self.touched.add(net)
        self.invalidate()
        return laid

    def displaced_by(self, laid, net):
        """movable items in exact conflict with freshly laid geometry"""
        out = []
        for item in laid:
            if item.GetClass() == "PCB_VIA":
                p = item.GetPosition()
                cs = self.via_conflicts(self.mm(p.x), self.mm(p.y), net, ignore=set(laid))
            else:
                cs = self.seg_conflicts(self.track_mm(item), net, item.GetLayer(), ignore=set(laid))
            for c in cs:
                if isinstance(c, tuple):
                    continue  # pad conflict: A* treats pads fixed; validation surfaces it
                if self.is_movable(c.GetNetname()) and c not in out:
                    out.append(c)
        return out

    def relocate_via(self, v):
        """ring-search a conflicting via to a legal spot, re-stitching."""
        pcbnew = self.pcbnew
        FromMM = self.FromMM
        p = v.GetPosition()
        vx, vy = self.mm(p.x), self.mm(p.y)
        net = v.GetNetname()
        attached = []
        for t in self.tracks():
            if t.GetNetname() != net:
                continue
            if (t.GetStart() - p).EuclideanNorm() < FromMM(0.05):
                attached.append((t, "s"))
            elif (t.GetEnd() - p).EuclideanNorm() < FromMM(0.05):
                attached.append((t, "e"))
        for dr in (0.3, 0.45, 0.6, 0.8, 1.0, 1.3, 1.7, 2.2):
            for ang in range(0, 360, 20):
                nx = vx + dr * math.cos(math.radians(ang))
                ny = vy + dr * math.sin(math.radians(ang))
                if not (self.edge[0] <= nx <= self.edge[2] and self.edge[1] <= ny <= self.edge[3]):
                    continue
                if self.via_conflicts(nx, ny, net, ignore={v}):
                    continue
                ok = True
                for t, endc in attached:
                    a = self.track_mm(t)
                    cand = (nx, ny, a[2], a[3]) if endc == "s" else (a[0], a[1], nx, ny)
                    if self.seg_conflicts(cand, net, t.GetLayer(), ignore={t, v}):
                        ok = False
                        break
                if not ok:
                    continue
                v.SetPosition(pcbnew.VECTOR2I(FromMM(nx), FromMM(ny)))
                for t, endc in attached:
                    if endc == "s":
                        t.SetStart(pcbnew.VECTOR2I(FromMM(nx), FromMM(ny)))
                    else:
                        t.SetEnd(pcbnew.VECTOR2I(FromMM(nx), FromMM(ny)))
                self.touched.add(net)
                print(f"    shoved via [{net}] ({vx:.2f},{vy:.2f}) -> ({nx:.2f},{ny:.2f})")
                return True
        return False

    def rip_net_routes(self, net):
        """rip a net's tracks/vias, penalize their cells, return count"""
        n = 0
        for t in list(self.b.GetTracks()):
            if t.GetNetname() != net:
                continue
            if t.GetClass() == "PCB_VIA":
                p = t.GetPosition()
                cells = [(self.snap(self.mm(p.x)), self.snap(self.mm(p.y)), L)
                         for L in self.layers]
            else:
                a = self.track_mm(t)
                steps = max(1, int(math.hypot(a[2] - a[0], a[3] - a[1]) / GRID_MM))
                cells = [(self.snap(a[0] + (a[2] - a[0]) * i / steps),
                          self.snap(a[1] + (a[3] - a[1]) * i / steps), t.GetLayer())
                         for i in range(steps + 1)]
            for c in cells:
                self.history[c] = self.history.get(c, 0) + HISTORY_STEP
            self.b.Remove(t)
            self.keepalive.append(t)
            n += 1
        self.invalidate()
        return n

    def net_pad(self, net):
        """(ref, padnum) of the net's routing origin: the pad on the
        footprint with the most pads (the IC end, not the LED end)."""
        best = None
        for fp in self.b.GetFootprints():
            for pad in fp.Pads():
                if pad.GetNetname() == net:
                    n = len(list(fp.Pads()))
                    if best is None or n > best[0]:
                        best = (n, fp.GetReference(), pad.GetNumber())
        return (best[1], best[2]) if best else None

    def validate_all(self):
        errs = []
        for t in self.tracks():
            if t.GetNetname() not in self.touched:
                continue
            a = self.track_mm(t)
            for c in self.seg_conflicts(a, t.GetNetname(), t.GetLayer(), ignore={t}):
                desc = c if isinstance(c, tuple) else f"{c.GetClass()}[{c.GetNetname()}]"
                errs.append(f"{t.GetNetname()} seg ({a[0]:.1f},{a[1]:.1f}) vs {desc}")
        for v in self.vias():
            if v.GetNetname() not in self.touched:
                continue
            p = v.GetPosition()
            for c in self.via_conflicts(self.mm(p.x), self.mm(p.y), v.GetNetname(), ignore={v}):
                desc = c if isinstance(c, tuple) else f"{c.GetClass()}[{c.GetNetname()}]"
                errs.append(f"{v.GetNetname()} via ({self.mm(p.x):.1f},{self.mm(p.y):.1f}) vs {desc}")
        return errs

    def run(self, target_nets):
        queue = [(n, self.net_pad(n)) for n in target_nets]
        routes = 0
        while queue and routes < MAX_ROUTES:
            net, origin = queue.pop(0)
            if origin is None:
                print(f"  {net}: no origin pad found")
                return False
            ref, num = origin
            print(f"[{routes + 1}] routing {net} from {ref}:{num}")
            self.rip_net_routes(net)
            r = self.route(net, ref, num)
            routes += 1
            if r is None:
                queue.append((net, origin))
                continue
            pad_pt, path, gset = r
            laid = self.lay_path(net, pad_pt, path, gset)
            for victim in self.displaced_by(laid, net):
                vnet = victim.GetNetname()
                if victim.GetClass() == "PCB_VIA" and self.relocate_via(victim):
                    continue
                print(f"    displacing net [{vnet}] (rip + requeue)")
                self.rip_net_routes(vnet)
                if all(q[0] != vnet for q in queue):
                    queue.append((vnet, self.net_pad(vnet)))
        if queue:
            print(f"budget exhausted with {len(queue)} net(s) unrouted")
            return False
        errs = self.validate_all()
        if errs:
            print("exact validation FAILED:")
            for e in errs[:12]:
                print("  " + e)
            return False
        return True

    def finish(self, out_path):
        pcbnew = self.pcbnew
        filler = pcbnew.ZONE_FILLER(self.b)
        filler.Fill(self.b.Zones())
        pcbnew.SaveBoard(out_path, self.b)
        conn = self.b.GetConnectivity()
        conn.RecalculateRatsnest()
        try:
            return conn.GetUnconnectedCount(True)
        except TypeError:
            return conn.GetUnconnectedCount()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("board")
    ap.add_argument("--net", action="append", required=True,
                    help="target net to route (repeatable)")
    ap.add_argument("--movable", action="append", required=True,
                    help="fnmatch pattern for nets whose copper may be shoved")
    ap.add_argument("--out", required=True, help="output board path")
    args = ap.parse_args()

    try:
        import pcbnew  # noqa: F401
    except ImportError:
        print("run under KiCad's python.exe (pcbnew required)", file=sys.stderr)
        return EXIT_ERROR

    s = Shover(args.board, args.movable)
    ok = s.run(args.net)
    if not ok:
        print("not saved -- negotiation failed")
        return EXIT_UNROUTED
    air = s.finish(args.out)
    print(f"saved {args.out}; airwires now {air}")
    print("run tools/kicad_verify.py -- this tool's validation is not the gate")
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
