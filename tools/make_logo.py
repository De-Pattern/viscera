# Generates the Viscera mark: a drop of grains, dense at the crown and
# thinning into drips. Deterministic (seeded), so the logo never changes.
import math, random, sys
random.seed(7)
W, H = 120, 160
dots = []
def inside(x, y):
    # teardrop: circle of radius 34 centred at (60,92) joined to a point at (60,22)
    cx, cy, r = 60, 92, 34
    if y >= cy:
        return (x-cx)**2 + (y-cy)**2 <= r*r
    if y < 22: return False
    t = (cy - y) / (cy - 22)           # 0 at circle centre, 1 at tip
    half = r * (1 - t) ** 1.35
    return abs(x - cx) <= half
tries = 0
while len(dots) < 900 and tries < 120000:
    tries += 1
    x, y = random.uniform(20, 100), random.uniform(18, 128)
    if not inside(x, y): continue
    depth = (y - 18) / 110             # 0 top .. 1 bottom
    # density falls toward the base: grains loosen and separate
    if random.random() < 0.55 * depth * depth: continue
    r = 0.45 + random.random() * (0.9 + 0.5 * depth)
    pad = 0.25 + 2.4 * depth ** 1.6
    if any((x-a)**2 + (y-b)**2 < (r+c+pad)**2 for a, b, c in dots): continue
    dots.append((x, y, r))
# drips falling from the base
for (x, y0, gaps) in [(50, 126, [2.6, 3.4, 5.5, 8.0]), (61, 127, [2.2, 2.8, 3.6, 5.2, 7.4, 10.5]), (70, 125, [3.0, 4.6, 7.8])]:
    y = y0
    for i, g in enumerate(gaps):
        y += g
        r = (1.55 if i == len(gaps) - 1 else 1.05 - i * 0.1)  # the falling drop is the heaviest
        dots.append((x + random.uniform(-0.5, 0.5), y, max(0.4, r)))
out = ['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" role="img" aria-label="Viscera">' % (W, H)]
for x, y, r in dots:
    out.append('<circle cx="%.1f" cy="%.1f" r="%.2f"/>' % (x, y, r))
out.append('</svg>')
sys.stdout.write(''.join(out))
