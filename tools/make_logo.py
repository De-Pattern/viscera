# Generates the Viscera mark: a tape reel whose three spokes are swept
# membranes of fibre. Each blade is string art, a ruled surface between a
# spiral leading edge and the rim, so the fibres crowd into a dark core around
# the hub and fan out into thin crescents. Toward the rim the fibres break up
# into grains, part of the rim dissolves, and the bottom sags into drips.
# Deterministic (seeded), so the logo never changes.
#
#   python3 tools/make_logo.py
#     -> site/logo.svg        full mark (hairlines; large sizes and the README)
#        site/logo-small.svg  fewer, heavier fibres, for the header and favicon
#        site/mark.js         points along the fibres, for the hero animation
import math, random, os
S = 200.0
C = S / 2
R = 78.0       # rim
HUB = 10.0     # hub hole
INK = '#1a1917'

def P(r, a):
    return (C + r * math.cos(a), C + r * math.sin(a))

def blades(lines_per_blade, rnd):
    """Fibres of the three membranes: [(p0, p1, width, alpha, u)].
    A blade is a twisted ribbon between two spiral edges that meet at the hub
    and again at a point on the rim. Fibres join one edge at u to the other at
    u + skew, so they cross and their envelope draws the crescent."""
    out = []
    sweep = 2.15
    def edge_a(a0, u):
        return P(HUB + 1 + (R - HUB - 1) * u, a0 + sweep * u)
    def edge_b(a0, u):
        return P(HUB + 1 + (R - HUB - 1) * u ** 0.62, a0 + sweep * u - 1.05 * math.sin(math.pi * u) ** 0.85)
    for b in range(3):
        a0 = math.radians(-150 + b * 120)
        for k in range(lines_per_blade):
            u = k / (lines_per_blade - 1)
            skew = 0.16 + 0.08 * math.sin(u * 3)
            p0 = edge_a(a0, u)
            p1 = edge_b(a0, min(1.0, u + skew))
            p0 = (p0[0] + rnd.gauss(0, 0.25), p0[1] + rnd.gauss(0, 0.25))
            p1 = (p1[0] + rnd.gauss(0, 0.25), p1[1] + rnd.gauss(0, 0.25))
            alpha = 0.14 + 0.62 * (1 - u) ** 1.8 + rnd.uniform(-0.04, 0.04)
            out.append((p0, p1, 0.28 + 0.3 * (1 - u), max(0.07, alpha), u))
            if k % 4 == 0:  # a second, opposite family of veins: the webbed texture
                q0 = edge_b(a0, u)
                q1 = edge_a(a0, min(1.0, u + skew * 0.7))
                out.append((q0, q1, 0.2, max(0.05, alpha * 0.45), u))
    return out

def fibre_marks(lines, rnd, scale=1.0):
    """Fibres stay solid near the hub; toward the rim they break up, drawn
    partly as a line and partly as loose square grains."""
    strokes, grains = [], []
    for (x0, y0), (x1, y1), w, a, u in lines:
        if u < 0.55 or rnd.random() < 0.4:
            strokes.append(((x0, y0), (x1, y1), w * scale, a))
            continue
        solid = rnd.uniform(0.2, 0.75)
        xs, ys = x0 + (x1 - x0) * solid, y0 + (y1 - y0) * solid
        strokes.append(((x0, y0), (xs, ys), w * scale, a))
        n = int(math.hypot(x1 - xs, y1 - ys) / (3.0 / scale))
        for _ in range(n):
            t = solid + (1 - solid) * rnd.random()
            s_ = (0.45 + rnd.random() * 0.7) * scale
            grains.append(((x0 + (x1 - x0) * t + rnd.gauss(0, 0.5), y0 + (y1 - y0) * t + rnd.gauss(0, 0.5)), s_, min(1, a * 2.2)))
    return strokes, grains

def rim(rnd, count, scale=1.0):
    """The rim, mostly as grains, dissolving along the left and lower right."""
    g = []
    for _ in range(count):
        a = rnd.uniform(0, 2 * math.pi)
        deg = math.degrees(a) % 360
        loose = 110 <= deg <= 245 or deg <= 70     # these stretches come apart
        r = R + rnd.gauss(0, 2.2 if loose else 0.6) + (rnd.random() ** 3) * (7 if loose else 1.5)
        if loose and rnd.random() < 0.35:
            continue
        if not loose and rnd.random() < 0.55:
            continue
        s = (0.6 + rnd.random() * (1.8 if loose else 0.9)) * scale
        g.append((P(r, a), s, 0.45 + 0.55 * rnd.random()))
    return g

def drips(rnd, scale=1.0):
    """The bottom of the rim sags and lets go of a few drops."""
    out = []
    for deg, length in ((96, 16), (104, 26), (81, 10)):
        a = math.radians(deg)
        x0, y0 = P(R, a)
        y = y0
        k = 0
        while y < y0 + length:
            step = 0.9 + k * 0.18
            y += step
            k += 1
            neck = 1 - (y - y0) / length
            s = (0.7 + 1.1 * neck + rnd.random() * 0.4) * scale
            out.append(((x0 + rnd.gauss(0, 0.25), y), s, 0.85))
        out.append(((x0, y + 4 + length * 0.12), 2.1 * scale, 0.95))   # the drop that fell
    return out

def specks(rnd, count, scale=1.0):
    s = []
    while len(s) < count:
        x, y = rnd.uniform(4, S - 4), rnd.uniform(4, S - 4)
        if math.hypot(x - C, y - C) < R + 8:
            continue
        s.append(((x, y), (0.6 + rnd.random() * 1.0) * scale, 0.3 + 0.5 * rnd.random()))
    return s

def hub_grit(rnd, count):
    """Dark grains packed into the core around the hub."""
    g = []
    for _ in range(count):
        a = rnd.uniform(0, 2 * math.pi)
        r = HUB + 1 + abs(rnd.gauss(0, 7))
        g.append((P(r, a), 0.5 + rnd.random() * 0.9, 0.5 + 0.5 * rnd.random()))
    return g

def core(rnd, count, scale=1.0):
    """String art around the hub: chords tangent to the hole, crowding into a
    dark ring with a clean opening in the middle."""
    out = []
    for k in range(count):
        a = 2 * math.pi * k / count + rnd.uniform(-0.02, 0.02)
        r = HUB + 1.0 + rnd.random() * 2.5          # the chord's nearest approach to the centre
        half = 0.45 + rnd.random() * 0.45
        rr = r / math.cos(half)
        out.append((P(rr, a - half), P(rr, a + half), 0.3 * scale, 0.3 + 0.35 * rnd.random()))
    return out

def rim_lines(rnd, scale=1.0):
    """Hairline strands of the rim, solid where the rim holds together."""
    out = []
    for arc0, arc1 in ((-0.25, 1.85), (4.35, 5.95)):
        for k in range(5):
            r = R + rnd.gauss(0, 0.5)
            a0, a1 = arc0 + rnd.uniform(-0.15, 0.1), arc1 + rnd.uniform(-0.1, 0.15)
            n = 40
            pts = [P(r + math.sin(i * 0.7 + k) * 0.3, a0 + (a1 - a0) * i / n) for i in range(n + 1)]
            for i in range(n):
                out.append((pts[i], pts[i + 1], 0.3 * scale, 0.25 + 0.25 * rnd.random()))
    return out

def svg(strokes, grains, title):
    o = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" role="img" aria-label="%s">' % (S, S + 24, title),
         '<g fill="none" stroke="%s" stroke-linecap="round">' % INK]
    for (x0, y0), (x1, y1), w, a in strokes:
        o.append('<path d="M%.1f %.1fL%.1f %.1f" stroke-width="%.2f" stroke-opacity="%.2f"/>' % (x0, y0, x1, y1, w, a))
    o.append('</g><g fill="%s">' % INK)
    for (x, y), s, a in grains:
        o.append('<rect x="%.1f" y="%.1f" width="%.2f" height="%.2f" fill-opacity="%.2f"/>' % (x - s / 2, y - s / 2, s, s, a))
    o.append('</g></svg>')
    return ''.join(o)

here = os.path.dirname(os.path.abspath(__file__))
site = os.path.join(here, '..', 'site')

rnd = random.Random(23)
full_lines = blades(170, rnd)
strokes, grains = fibre_marks(full_lines, rnd)
strokes += core(rnd, 150) + rim_lines(rnd)
grains += rim(rnd, 300) + drips(rnd) + hub_grit(rnd, 110) + specks(rnd, 44)
open(os.path.join(site, 'logo.svg'), 'w').write(svg(strokes, grains, 'Viscera'))

rnd = random.Random(23)
small = blades(18, rnd)
s_strokes, s_grains = fibre_marks(small, rnd, 3.0)
s_strokes += [(a, b, w * 2.6, min(1.0, al * 1.6)) for a, b, w, al in core(rnd, 26)]
s_strokes = [(a, b, w, min(1.0, al * 2.3)) for a, b, w, al in s_strokes]
s_grains = s_grains[::4] + rim(rnd, 90, 2.4) + drips(rnd, 2.2)[::2]
open(os.path.join(site, 'logo-small.svg'), 'w').write(svg(s_strokes, s_grains, 'Viscera'))

# points for the hero animation: along each fibre (more toward the hub,
# where the membrane is darkest), plus the rim, drips and core
rnd = random.Random(5)
pts = []
for (x0, y0), (x1, y1), w, a, u in full_lines:
    for _ in range(1 + int(a * 4)):
        t = rnd.random() ** 1.4
        pts.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
for (x0, y0), (x1, y1), w, a in core(random.Random(4), 150):
    t = rnd.random(); pts.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
for (x, y), s, a in rim(random.Random(9), 420) + drips(random.Random(9)) + hub_grit(random.Random(9), 60):
    pts.append((x, y))
rnd.shuffle(pts)
open(os.path.join(site, 'mark.js'), 'w').write(
    "/* Generated by tools/make_logo.py: points along the logo's fibres (box %dx%d). */\n"
    "window.VISCERA_MARK={w:%d,h:%d,cx:%d,cy:%d,r:%d,p:[%s]};\n"
    % (S, S + 24, S, S + 24, C, C, R, ','.join('[%.1f,%.1f]' % p for p in pts)))
print(len(strokes), 'strokes,', len(grains), 'grains,', len(pts), 'points')
