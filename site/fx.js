/* Viscera site: particle animations. One field of grains that passes through
 * five states (vapour, rain, swell, melt, settle), small studies for each
 * install step, and a sediment progress bar. Canvas 2D, no libraries. */
(() => {
  'use strict';
  const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const PAPER = '#f3efe7';
  const TAU = Math.PI * 2;

  function rng(seed) {
    return () => {
      seed |= 0; seed = seed + 0x6D2B79F5 | 0;
      let t = Math.imul(seed ^ seed >>> 15, 1 | seed);
      t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t;
      return ((t ^ t >>> 14) >>> 0) / 4294967296;
    };
  }
  function hash(x, y) {
    let h = Math.imul(x | 0, 374761393) + Math.imul(y | 0, 668265263);
    h = Math.imul(h ^ (h >>> 13), 1274126177);
    return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
  }
  function noise(x, y) {
    const xi = Math.floor(x), yi = Math.floor(y), xf = x - xi, yf = y - yi;
    const u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    const a = hash(xi, yi), b = hash(xi + 1, yi), c = hash(xi, yi + 1), d = hash(xi + 1, yi + 1);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
  }
  const smooth = x => x <= 0 ? 0 : x >= 1 ? 1 : x * x * (3 - 2 * x);
  function fit(c) {
    const r = c.getBoundingClientRect(), d = Math.min(2, window.devicePixelRatio || 1);
    const w = Math.max(1, Math.round(r.width * d)), h = Math.max(1, Math.round(r.height * d));
    if (c.width !== w || c.height !== h) { c.width = w; c.height = h; }
    return d;
  }
  function visible(el, cb) {
    if (!('IntersectionObserver' in window)) { cb(true); return; }
    new IntersectionObserver(es => cb(es[0].isIntersecting), {rootMargin: '80px'}).observe(el);
  }

  /* The logo's drop, as points in a 120 x 160 box (same shape as logo.svg). */
  function dropPoints(n) {
    const r = rng(7), pts = [];
    const inside = (x, y) => {
      const cx = 60, cy = 92, rad = 34;
      if (y >= cy) return (x - cx) ** 2 + (y - cy) ** 2 <= rad * rad;
      if (y < 22) return false;
      const t = (cy - y) / (cy - 22);
      return Math.abs(x - cx) <= rad * Math.pow(1 - t, 1.35);
    };
    let guard = 0;
    while (pts.length < n && guard++ < n * 60) {
      const x = 20 + r() * 80, y = 18 + r() * 110;
      if (!inside(x, y)) continue;
      const depth = (y - 18) / 110;
      if (r() < 0.6 * depth * depth) continue;
      pts.push([x, y]);
    }
    const drips = [[50, 126, [2.6, 3.4, 5.5, 8]], [61, 127, [2.2, 2.8, 3.6, 5.2, 7.4, 10.5]], [70, 125, [3, 4.6, 7.8]]];
    for (const [x, y0, gaps] of drips) { let y = y0; for (const g of gaps) { y += g; pts.push([x, y]); } }
    return pts;
  }

  /* ---------------------------------------------------------------- field */
  const PHASES = ['vapour', 'rain', 'swell', 'melt', 'settle'];
  let meltBreak = 0;
  const HOLD = 7.5, BLEND = 2.6;

  function field(canvas) {
    const ctx = canvas.getContext('2d', {alpha: false});
    let MARK = {w: 120, h: 144};
    let n = 0, X, Y, S1, S2, S3, SZ, TONE, ST, AL, logo = [], W = 0, H = 0, dpr = 1;
    const label = document.getElementById('phase-name'), num = document.getElementById('phase-n');
    let shown = -1;

    function build() {
      dpr = fit(canvas); W = canvas.width; H = canvas.height;
      n = Math.min(2600, Math.max(700, Math.round(W * H / (190 * dpr * dpr))));
      const r = rng(11);
      X = new Float32Array(n); Y = new Float32Array(n);
      S1 = new Float32Array(n); S2 = new Float32Array(n); S3 = new Float32Array(n);
      SZ = new Float32Array(n); TONE = new Uint8Array(n); ST = new Float32Array(n); AL = new Uint8Array(n);
      for (let i = 0; i < n; i++) {
        S1[i] = r(); S2[i] = r(); S3[i] = r();
        X[i] = S1[i] * W; Y[i] = S2[i] * H;
        const k = r();
        SZ[i] = (k < 0.7 ? 0.8 + r() * 0.9 : 1.6 + r() * 1.6) * dpr;
        TONE[i] = r() < 0.55 ? 0 : r() < 0.6 ? 1 : 2;
      }
      /* reel fibres (site/mark.js); fall back to the drop if it didn't load */
      const src = window.VISCERA_MARK;
      if (src && src.p.length) {
        MARK = src;
        const want = Math.min(Math.round(n * 0.6), 1500), step = src.p.length / want;
        logo = []; for (let k = 0; k < want; k++) logo.push(src.p[Math.floor(k * step) % src.p.length]);
      } else { MARK = {w: 120, h: 160}; logo = dropPoints(Math.min(Math.round(n * 0.55), 1300)); }
    }

    /* Target for particle i in phase p at time t: [x, y, stretch, alpha] */
    const out = [0, 0, 0, 1];
    function target(p, i, t) {
      const u = S1[i], v = S2[i], w = S3[i];
      switch (p) {
      case 0: { /* vapour: slow rising, curling gas */
        const span = H * 1.15;
        let y = H * 1.05 - ((v * span + t * H * (0.012 + 0.03 * w)) % span);
        const curl = noise(u * 3 + t * 0.05, y / H * 2.2 + w * 7) - 0.5;
        out[0] = u * W + curl * W * 0.35 + Math.sin(t * 0.21 + v * 9) * W * 0.02;
        out[1] = y; out[2] = 0;
        out[3] = 0.25 + 0.75 * Math.sin(Math.PI * Math.min(1, Math.max(0, y / H)));
        return out;
      }
      case 1: { /* rain: streaks falling at different speeds, pooling below */
        const span = H * 1.25, speed = 0.28 + 0.75 * w * w;
        let y = (v * span + t * H * speed) % span - H * 0.12;
        const floor = H * (0.9 + 0.05 * noise(u * 20, t * 0.4));
        let s = 2 + 9 * w;
        if (y > floor) { s = 0; y = floor + (y - floor) * 0.08; }
        out[0] = u * W + (y / H) * W * 0.045; out[1] = y; out[2] = s;
        out[3] = 0.5 + 0.5 * w;
        return out;
      }
      case 2: { /* swell: grains riding layered waves */
        const band = Math.floor(w * 9), x = u * W;
        const amp = H * 0.05 * (1 + 0.6 * Math.sin(t * 0.17 + band * 1.3));
        const y = H * (0.16 + band * 0.078) + Math.sin(x / W * 7.5 + t * 0.85 + band * 0.62) * amp
          + Math.sin(x / W * 19 - t * 1.3 + band) * amp * 0.25 + (v - 0.5) * 7 * dpr;
        out[0] = x; out[1] = y; out[2] = 0; out[3] = 0.35 + 0.65 * (1 - band / 9);
        return out;
      }
      case 3: { /* melt: a mass along the top, sagging into drips */
        const cols = 13, col = Math.floor(u * cols);
        const cx = (col + 0.5) / cols * W + (hash(col, 3) - 0.5) * W * 0.04;
        if (v < 0.5) {
          const x = u * W;
          let sag = 0;
          for (let c = Math.max(0, col - 1); c <= Math.min(cols - 1, col + 1); c++) {
            const dx = (x - ((c + 0.5) / cols * W)) / (W / cols * 0.45);
            sag += Math.exp(-dx * dx) * (0.5 + 0.5 * Math.sin(t * (0.3 + hash(c, 1) * 0.4) + c));
          }
          out[0] = x; out[1] = H * 0.05 + (v / 0.5) * H * 0.13 + sag * H * 0.07 * (v / 0.5);
          out[2] = 0; out[3] = 1;
          return out;
        }
        const rate = 0.07 + 0.09 * hash(col, 9);
        /* unbroken streams first ... */
        const d1 = (t * rate + w * 0.999 + hash(col, 5)) % 1;
        const sx = cx + (v - 0.75) * (10 * (1 - d1) + 2) * dpr, sy = H * 0.2 + d1 * d1 * H * 0.95;
        /* ... then the necks thin and let go of drops */
        const group = Math.floor(w * 3), within = (w * 3) % 1;
        const d = (t * rate + group / 3 + hash(col, 5) + within * (0.03 + 0.05 * hash(col, group))) % 1;
        const fall = d < 0.25 ? d * 0.25 : 0.0625 + (d - 0.25) * (d - 0.25) * 1.75;
        const sway = Math.sin(d * 5 + col * 1.7 + t * 0.4) * 5 * dpr * d;
        const dx = cx + sway + (v - 0.75) * (14 * (1 - d) * (1 - d) + 2) * dpr, dy = H * 0.19 + fall * H * 0.95;
        const k = meltBreak;
        out[0] = sx + (dx - sx) * k; out[1] = sy + (dy - sy) * k;
        out[2] = d1 * 5 * (1 - k) + (d > 0.25 ? (d - 0.25) * 6 : 0) * k;
        out[3] = (1 - d1 * 0.6) * (1 - k) + (1 - d * 0.5) * k;
        return out;
      }
      default: { /* settle: grains gather into the reel, which turns slowly */
        if (i < logo.length) {
          const mw = MARK.w, mh = MARK.h;
          const narrow = W < H * 0.9;
          const sc = Math.min(W, H) / (mh * 1.25) * (1 + 0.02 * Math.sin(t * 0.8));
          const ox = (narrow ? W * 0.5 : W * 0.72) - mw / 2 * sc, oy = (narrow ? H * 0.38 : H * 0.48) - mh / 2 * sc;
          /* the reel turns slowly; the hanging tape (below the rim) stays put */
          let [px, py] = logo[i];
          const cx = MARK.cx || mw / 2, cy = MARK.cy || mh / 2, rim = (MARK.r || mw * 0.4) + 3;
          const rx = px - cx, ry = py - cy;
          if (rx * rx + ry * ry < rim * rim) {
            const ang = t * 0.22, c = Math.cos(ang), s2 = Math.sin(ang);
            px = cx + rx * c - ry * s2; py = cy + rx * s2 + ry * c;
          }
          const jitter = (noise(i * 0.37, t * 0.6) - 0.5) * 1.6 * dpr;
          out[0] = ox + px * sc + jitter;
          out[1] = oy + py * sc + (noise(i * 0.11 + 9, t * 0.5) - 0.5) * 1.6 * dpr;
          out[2] = 0; out[3] = 1;
          return out;
        }
        const y = H - ((v * H + t * H * 0.01) % H);
        out[0] = u * W + (noise(u * 5, t * 0.05) - 0.5) * W * 0.2; out[1] = y; out[2] = 0; out[3] = 0.22;
        return out;
      }
      }
    }

    const cycle = PHASES.length * (HOLD + BLEND);
    let t0 = performance.now(), running = true, raf = 0, last = 0;
    function frame(now) {
      raf = 0;
      if (!running) return;
      const t = (now - t0) / 1000;
      if (canvas.width !== Math.round(canvas.getBoundingClientRect().width * dpr)) build();
      const pos = t % cycle, idx = Math.floor(pos / (HOLD + BLEND)), local = pos - idx * (HOLD + BLEND);
      const a = idx, b = (idx + 1) % PHASES.length;
      const e = smooth((local - HOLD) / BLEND);
      meltBreak = a === 3 ? smooth((local - 1.5) / 3.5) : 0;
      if (a !== shown && label) { shown = a; label.textContent = PHASES[a]; num.textContent = '0' + (a + 1) + ' / 0' + PHASES.length; }

      /* trails: wash the paper over the last frame instead of clearing */
      ctx.globalAlpha = a === 1 || (a === 0 && e > 0) ? 0.5 : 0.32;
      ctx.fillStyle = PAPER; ctx.fillRect(0, 0, W, H);
      ctx.globalAlpha = 1;
      const springBase = a === 1 && e < 0.5 ? 0.3 : 0.07;
      const inks = ['rgba(26,25,23,', 'rgba(70,67,61,', 'rgba(150,143,130,'];
      for (let i = 0; i < n; i++) {
        target(a, i, t); const ax = out[0], ay = out[1], as = out[2], aa = out[3];
        let tx = ax, ty = ay, st = as, al = aa;
        if (e > 0) {
          target(b, i, t);
          tx = ax + (out[0] - ax) * e; ty = ay + (out[1] - ay) * e;
          st = as + (out[2] - as) * e; al = aa + (out[3] - aa) * e;
        }
        const dx = tx - X[i], dy = ty - Y[i];
        if (Math.abs(dy) > H * 0.45 && e === 0) { X[i] = tx; Y[i] = ty; }
        else { const k = springBase * (0.6 + 0.8 * S3[i]); X[i] += dx * k; Y[i] += dy * k; }
        ST[i] = st; AL[i] = al > 0.6 ? 1 : 0;
      }
      for (let tone = 0; tone < 3; tone++) {
        for (let q = 0; q < 2; q++) {
          ctx.fillStyle = inks[tone] + (q ? 0.85 : 0.4) + ')';
          for (let i = 0; i < n; i++) {
            if (TONE[i] !== tone || AL[i] !== q) continue;
            const s = SZ[i], st = ST[i];
            if (st > 0.5) ctx.fillRect(X[i], Y[i] - st * dpr, Math.max(dpr * 0.8, s * 0.6), st * dpr + s);
            else ctx.fillRect(X[i], Y[i], s, s);
          }
        }
      }
      /* loose grit: specks that appear for one frame */
      ctx.fillStyle = 'rgba(26,25,23,0.18)';
      for (let k = 0; k < 140; k++) ctx.fillRect(Math.random() * W, Math.random() * H, dpr, dpr);
      last = now;
      if (!reduce) raf = requestAnimationFrame(frame);
    }
    build();
    addEventListener('resize', () => build());
    if (reduce) {
      /* one still frame of the settled drop */
      t0 = performance.now() - (4 * (HOLD + BLEND) + 2) * 1000;
      for (let k = 0; k < 90; k++) frame(performance.now());
      return;
    }
    visible(canvas, on => { running = on && !document.hidden; if (running && !raf) raf = requestAnimationFrame(frame); });
    document.addEventListener('visibilitychange', () => { running = !document.hidden; if (running && !raf) raf = requestAnimationFrame(frame); });
    raf = requestAnimationFrame(frame);
  }

  /* ------------------------------------------------------- small studies */
  const state = {connected: 0, chosen: 0, rain: 0.15, progress: 0};
  function mini(canvas) {
    const kind = canvas.dataset.mini, ctx = canvas.getContext('2d');
    const r = rng(kind.length * 31), N = 170;
    const P = Array.from({length: N}, () => ({x: r(), y: r(), u: r(), v: r(), w: r()}));
    let dpr = fit(canvas), W = canvas.width, H = canvas.height, on = true, raf = 0, mix = 0;
    function draw(now) {
      raf = 0; if (!on) return;
      const t = now / 1000;
      if (canvas.width !== Math.round(canvas.getBoundingClientRect().width * dpr)) { dpr = fit(canvas); W = canvas.width; H = canvas.height; }
      ctx.fillStyle = 'rgba(243,239,231,0.42)'; ctx.fillRect(0, 0, W, H);
      ctx.fillStyle = 'rgba(26,25,23,0.8)';
      if (kind === 'sift') {
        /* grains sift through a narrow opening, faster once a build is chosen */
        mix += ((state.chosen ? 1 : 0) - mix) * 0.04;
        for (const p of P) {
          p.y += (0.002 + p.w * 0.004) * (1 + mix * 2);
          const gate = 0.5;
          const neck = Math.abs(p.y - gate) < 0.08;
          if (neck) p.x += (0.5 - p.x) * 0.12;
          else if (p.y > gate) p.x += (p.u - p.x) * 0.02;
          if (p.y > 1) { p.y = 0; p.x = p.u; }
          const s = (p.w < 0.8 ? 1 : 2) * dpr;
          ctx.fillRect(p.x * W, p.y * H, s, s);
        }
      } else if (kind === 'gather') {
        /* two loose clouds; they gather into one when the FM-1 answers */
        mix += ((state.connected ? 1 : 0) - mix) * 0.03;
        for (const p of P) {
          const side = p.u < 0.5 ? 0.22 : 0.78;
          const ang = p.v * TAU + t * (0.3 + p.w * 0.4) * (p.u < 0.5 ? 1 : -1);
          const rad = (0.08 + p.w * 0.14) * (1 - mix * 0.55);
          const cx = side + (0.5 - side) * mix;
          const tx = cx + Math.cos(ang) * rad * (H / W) * 2.4, ty = 0.5 + Math.sin(ang) * rad * 2;
          p.x += (tx - p.x) * 0.08; p.y += (ty - p.y) * 0.08;
          const s = (p.w < 0.75 ? 1 : 2) * dpr;
          ctx.fillRect(p.x * W, p.y * H, s, s);
        }
      } else {
        /* drizzle, heavier while installing */
        for (const p of P) {
          const active = p.w < state.rain;
          p.y += active ? 0.02 + p.v * 0.03 : 0.003;
          if (p.y > 1) { p.y = -0.1; p.x = r(); }
          const len = active ? (3 + p.v * 6) * dpr : dpr;
          ctx.fillRect(p.x * W, p.y * H, dpr, len);
        }
      }
      if (!reduce) raf = requestAnimationFrame(draw);
    }
    visible(canvas, v => { on = v; if (on && !raf) raf = requestAnimationFrame(draw); });
    if (reduce) { for (let k = 0; k < 60; k++) draw(k * 16); return; }
    raf = requestAnimationFrame(draw);
  }

  /* ------------------------------------------------------ sediment bar */
  function sediment(canvas) {
    const ctx = canvas.getContext('2d');
    let dpr = fit(canvas), W = canvas.width, H = canvas.height;
    const cols = 160, heights = new Float32Array(cols), falling = [];
    let raf = 0;
    function reset() { heights.fill(0); falling.length = 0; }
    function draw() {
      raf = 0;
      if (canvas.width !== Math.round(canvas.getBoundingClientRect().width * dpr)) { dpr = fit(canvas); W = canvas.width; H = canvas.height; }
      const p = state.progress;
      if (p <= 0 && heights.some(h => h > 0)) reset();
      /* the filled width follows progress; grains land and pile up there */
      const edge = Math.floor(p * cols);
      const want = 7;
      for (let k = 0; k < 6 && p > 0; k++) {
        const c = Math.floor(Math.random() * Math.max(1, edge + 2));
        if (c < cols && heights[c] < want + Math.random() * 3) falling.push({c, y: -Math.random() * 10, v: 0.6 + Math.random()});
      }
      ctx.fillStyle = '#e6dfd1'; ctx.fillRect(0, 0, W, H);
      const cw = W / cols, gh = 2 * dpr;
      ctx.fillStyle = 'rgba(26,25,23,0.85)';
      for (let i = falling.length - 1; i >= 0; i--) {
        const g = falling[i]; g.v += 0.25; g.y += g.v * dpr;
        const floor = H - heights[g.c] * gh;
        if (g.y >= floor) {
          /* slide off taller neighbours, like dry grain */
          let c = g.c;
          if (c > 0 && heights[c - 1] < heights[c] - 1) c--;
          else if (c < cols - 1 && heights[c + 1] < heights[c] - 1) c++;
          heights[c] += 1; falling.splice(i, 1);
        } else ctx.fillRect(g.c * cw, g.y, Math.max(1, cw * 0.7), gh);
      }
      for (let c = 0; c < cols; c++) {
        const h = heights[c];
        for (let k = 0; k < h; k++) {
          const jitter = hash(c, k) < 0.3 ? 0.5 : 0;
          ctx.globalAlpha = 0.55 + 0.45 * hash(k, c);
          ctx.fillRect(c * cw + jitter, H - (k + 1) * gh, Math.max(1, cw * 0.82), gh * 0.85);
        }
      }
      ctx.globalAlpha = 1;
      if (!reduce || falling.length) raf = requestAnimationFrame(draw);
    }
    const kick = () => { if (!raf) raf = requestAnimationFrame(draw); };
    kick();
    return {kick, reset};
  }

  window.VisceraFX = {state, start() {
    const f = document.getElementById('field');
    if (f) field(f);
    document.querySelectorAll('canvas[data-mini]').forEach(mini);
    const s = document.getElementById('sediment');
    const sed = s ? sediment(s) : null;
    this.sediment = sed;
  }};
  window.VisceraFX.start();
})();
