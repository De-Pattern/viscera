# Generates the Viscera mark: a tape reel made of tendons. The rim, hub and
# three spokes are bundles of thin fibres, like tendon strands: they pinch
# in the middle and splay where they attach, and a single strand of "tape"
# peels off the rim. Deterministic (seeded), so the logo never changes.
#
#   python3 tools/make_logo.py            -> site/logo.svg, site/mark.js
import math, random, os
random.seed(23)
S = 120.0
C = S / 2
R_RIM, R_HUB, R_PACK = 50.0, 9.5, 23.0
paths = []      # (points, width, alpha)

def noise_fn(seed, k=3):
    rnd = random.Random(seed)
    terms = [(rnd.uniform(1, 4.5), rnd.uniform(0, 6.283), rnd.uniform(0.3, 1.0)) for _ in range(k)]
    return lambda t: sum(a * math.sin(f * t + p) for f, p, a in terms) / k

def strand_arc(r, a0, a1, wobble, seed, steps=90):
    n = noise_fn(seed)
    pts = []
    for i in range(steps + 1):
        a = a0 + (a1 - a0) * i / steps
        rr = r + n(a * 2.0) * wobble
        pts.append((C + rr * math.cos(a), C + rr * math.sin(a)))
    return pts

def bundle_arc(r, count, spread, a0, a1, base_w):
    for k in range(count):
        off = (k - (count - 1) / 2) * spread + random.uniform(-0.35, 0.35)
        # each fibre starts and stops a little differently: a sketched, not closed, loop
        s = a0 + random.uniform(-0.25, 0.35)
        e = a1 + random.uniform(-0.35, 0.25)
        paths.append((strand_arc(r + off, s, e, 0.9 + random.random() * 0.8, random.random() * 1e6),
                      base_w * random.uniform(0.6, 1.25), random.uniform(0.55, 1.0)))

# rim: a thick bundle, two overlapping sweeps so the joins never line up
bundle_arc(R_RIM, 6, 0.85, -0.2, 2 * math.pi - 0.05, 0.9)
bundle_arc(R_RIM + 0.4, 3, 1.1, 2.2, 2 * math.pi + 1.4, 0.7)
# hub
bundle_arc(R_HUB, 4, 0.8, 0.3, 2 * math.pi + 0.2, 0.8)
# the wound tape: a full pack of fibres around the hub, loosely wound
for k in range(5):
    bundle_arc(R_PACK - 2.4 + k * 1.2 + random.uniform(-0.3, 0.3), 1, 0, 0.0 + k * 0.9, 2 * math.pi + k * 0.9 - 0.2, 0.55)
# drive slots in the hub
for k in range(3):
    a = math.radians(-90 + k * 120)
    x, y = C + 5.2 * math.cos(a), C + 5.2 * math.sin(a)
    paths.append(([(x + 1.3 * math.cos(t), y + 1.3 * math.sin(t)) for t in [i * 0.42 for i in range(16)]], 0.7, 0.9))

# spokes: tendon bundles from hub to rim, pinched in the middle, splayed at both ends
for k in range(3):
    ang = math.radians(-90 + 60 + k * 120)
    nx, ny = math.cos(ang), math.sin(ang)
    tx, ty = -ny, nx
    fibres = 9
    for f in range(fibres):
        u = (f - (fibres - 1) / 2) / ((fibres - 1) / 2)       # -1..1 across the bundle
        n = noise_fn(random.random() * 1e6)
        pts = []
        for i in range(41):
            s = i / 40
            r = R_HUB + 1.0 + s * (R_RIM - R_HUB - 1.8)
            # width profile: splays at the hub (s=0) and wider at the rim (s=1), pinched near 0.45
            width = 1.8 + 3.0 * (abs(s - 0.4) / 0.6) ** 1.6 + 10.5 * s ** 3.2
            # fibres twist slightly around each other
            lateral = u * width + n(s * 5) * 0.55 + 0.5 * math.sin(s * 7 + f) * (1 - abs(u))
            pts.append((C + nx * r + tx * lateral, C + ny * r + ty * lateral))
        # trim some fibres short: frayed ends
        cut0 = random.randint(0, 3); cut1 = 41 - random.randint(0, 4)
        paths.append((pts[cut0:cut1], random.uniform(0.45, 1.0), random.uniform(0.6, 1.0)))

# the leader: tape peels off the pack, runs out through a window, over the rim, and hangs
lead = []
a0 = math.radians(22)
px, py = C + R_PACK * math.cos(a0), C + R_PACK * math.sin(a0)
for i in range(60):
    s_ = i / 59
    x = px + 33 * (1 - (1 - s_) ** 2.2)              # runs out, then stops travelling sideways
    y = py + 6 * s_ + 52 * s_ ** 2.4                 # and falls
    lead.append((x + math.sin(s_ * 11) * 0.6 * s_, y))
paths.append((lead, 0.95, 0.95))
paths.append(([(x + 0.8, y + 0.5) for x, y in lead[4:52]], 0.4, 0.55))
paths.append(([(x - 0.6, y + 0.9) for x, y in lead[20:58]], 0.35, 0.5))

def to_path(pts):
    # Catmull-Rom through the points, as cubic Beziers
    d = 'M%.2f %.2f' % pts[0]
    for i in range(len(pts) - 1):
        p0 = pts[i - 1] if i else pts[i]
        p1, p2 = pts[i], pts[i + 1]
        p3 = pts[i + 2] if i + 2 < len(pts) else p2
        c1 = (p1[0] + (p2[0] - p0[0]) / 6, p1[1] + (p2[1] - p0[1]) / 6)
        c2 = (p2[0] - (p3[0] - p1[0]) / 6, p2[1] - (p3[1] - p1[1]) / 6)
        d += 'C%.2f %.2f %.2f %.2f %.2f %.2f' % (c1 + c2 + p2)
    return d

H = S + 24   # room below for the hanging leader
svg = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" fill="none" stroke="#1a1917" stroke-linecap="round" role="img" aria-label="Viscera">' % (S, H)]
for pts, w, a in paths:
    svg.append('<path d="%s" stroke-width="%.2f" stroke-opacity="%.2f"/>' % (to_path(pts), w, a))
svg.append('</svg>')

# points along every fibre, for the hero animation's "settle" state
pts_out = []
for pts, w, a in paths:
    for i in range(0, len(pts), 2):
        pts_out.append('[%.1f,%.1f]' % pts[i])
here = os.path.dirname(os.path.abspath(__file__))
site = os.path.join(here, '..', 'site')
open(os.path.join(site, 'logo.svg'), 'w').write(''.join(svg))
open(os.path.join(site, 'mark.js'), 'w').write(
    '/* Generated by tools/make_logo.py: points along the logo\'s fibres (box %dx%d). */\n'
    'window.VISCERA_MARK={w:%d,h:%d,p:[%s]};\n' % (S, H, S, H, ','.join(pts_out)))
print(len(paths), 'fibres,', len(pts_out), 'points')
