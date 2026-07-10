#!/usr/bin/env python3
"""Left-facing galloping pony silhouette, skeleton-based.

Body = smoothed blob; neck/head/legs/tail/mane = tapered strokes rendered by
stamping circles along interpolated paths (thin parts survive, joints smooth).
Outputs pony.png (white on transparent) + pony_preview.png (dark bg).
"""
from PIL import Image, ImageDraw

W, H = 480, 300
SS = 4
BOX = (1000, 620)

def catmull(points, samples=18):
    pts = list(points); n = len(pts); out = []
    for i in range(n):
        p0, p1, p2, p3 = pts[(i-1) % n], pts[i], pts[(i+1) % n], pts[(i+2) % n]
        for j in range(samples):
            t = j / samples; t2, t3 = t*t, t*t*t
            out.append((
                0.5*((2*p1[0]) + (-p0[0]+p2[0])*t + (2*p0[0]-5*p1[0]+4*p2[0]-p3[0])*t2 + (-p0[0]+3*p1[0]-3*p2[0]+p3[0])*t3),
                0.5*((2*p1[1]) + (-p0[1]+p2[1])*t + (2*p0[1]-5*p1[1]+4*p2[1]-p3[1])*t2 + (-p0[1]+3*p1[1]-3*p2[1]+p3[1])*t3)))
    return out

def stroke(d, joints, sx, sy):
    """joints: [(x, y, radius), ...] -- stamp lerped circles along the path."""
    for i in range(len(joints) - 1):
        x0, y0, r0 = joints[i]; x1, y1, r1 = joints[i+1]
        dist = ((x1-x0)**2 + (y1-y0)**2) ** 0.5
        steps = max(2, int(dist / 2))
        for s in range(steps + 1):
            t = s / steps
            x, y, r = x0+(x1-x0)*t, y0+(y1-y0)*t, r0+(r1-r0)*t
            d.ellipse([(x-r)*sx, (y-r)*sy, (x+r)*sx, (y+r)*sy], fill=255)

# ---- skeleton (design box 1000 x 620, y down, horse faces LEFT) ----
TORSO = [(258, 298), (300, 242), (420, 226), (560, 238), (660, 262),
         (700, 310), (660, 352), (520, 366), (360, 358), (280, 340)]

NECK  = [(295, 295, 52), (225, 250, 38), (172, 215, 26)]
HEAD  = [(172, 215, 24), (118, 240, 16), (70, 262, 11)]          # poll -> muzzle
EARS  = [[(168, 198), (150, 158), (182, 192)],
         [(190, 200), (182, 156), (205, 196)]]

MANE  = [(175, 207, 7), (225, 196, 13), (285, 204, 13), (345, 219, 10), (400, 230, 6)]
TAIL  = [(695, 285, 18), (775, 245, 24), (865, 225, 18), (950, 235, 7)]

LEGS = [
    # front extended (reaching)
    [(300, 322, 24), (245, 392, 13), (185, 418, 9), (105, 452, 6), (88, 459, 9)],
    # front trailing (less extended)
    [(330, 332, 22), (285, 418, 11), (246, 466, 7), (226, 484, 5), (216, 492, 8)],
    # hind extended (kicking back)
    [(655, 330, 26), (758, 380, 13), (850, 416, 8), (908, 436, 6), (924, 443, 9)],
    # hind trailing
    [(625, 340, 24), (698, 412, 11), (758, 455, 7), (788, 475, 5), (800, 483, 8)],
]

def render():
    img = Image.new("L", (W*SS, H*SS), 0)
    d = ImageDraw.Draw(img)
    sx, sy = W*SS/BOX[0], H*SS/BOX[1]

    d.polygon([(x*sx, y*sy) for x, y in catmull(TORSO)], fill=255)
    for e in EARS:
        d.polygon([(x*sx, y*sy) for x, y in e], fill=255)
    for s in (NECK, HEAD, MANE, TAIL, *LEGS):
        stroke(d, s, sx, sy)

    img = img.resize((W, H), Image.LANCZOS)
    out = Image.new("RGBA", (W, H), (255, 255, 255, 0))
    out.paste(Image.new("RGBA", (W, H), (255, 255, 255, 255)), (0, 0), img)
    out.save("pony.png", optimize=True)
    prev = Image.new("RGB", (W, H), (10, 14, 20))
    prev.paste((230, 232, 236), (0, 0), img)
    prev.save("pony_preview.png")
    print("wrote pony.png", out.size)

if __name__ == "__main__":
    render()
