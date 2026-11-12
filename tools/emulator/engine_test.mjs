// Viscera PUTTY engine and UI test, run on the WebAssembly build of the same core the
// FM-1 runs. Captures, bounces, the three putty modes, robots, two hands, the reel, every
// page and view, the modulators and matrix, MIDI, the listener: nothing may produce NaN,
// silence where sound is expected, or runaway level.
import fs from 'node:fs';
const wasm = fs.readFileSync(process.argv[2] || 'site/emulator/viscera.wasm');
// parameter indices, read from the core's header
const enumOf = (name, end) => {
  const h = fs.readFileSync(new URL('../../core/vsc.h', import.meta.url), 'utf8');
  const body = h.slice(h.indexOf(name) + name.length, h.indexOf(end)).replace(/\/\*[\s\S]*?\*\//g, '');
  const idx = {}; let i = 0;
  for (let n of body.split(',').map(s => s.trim()).filter(Boolean)) {
    if (n.includes('=')) { const [a] = n.split('='); n = a.trim(); i = idx.P_MOD0 + 40; }
    idx[n] = i++;
  }
  return idx;
};
const P = enumOf('enum vsc_tparam {', 'VSC_NP');
const G = enumOf('enum vsc_gparam {', 'VSC_NG');
const {instance} = await WebAssembly.instantiate(wasm, {});
const x = instance.exports;
const mem = () => x.memory.buffer;
let fails = 0;
const ok = (c, m) => { if (!c) { fails++; console.log('FAIL', m); } };
ok(P.P_ENGINE > 0 && P.P_SEQM === P.P_MOD0 + 40 && x.np() === P.P_RGATE + 1, 'parameter table read: ' + x.np());
x.init();
const SRC = {CAPTURE: 0, REEL: 1, THRU: 2}, ENG = {HANDS: 0, SHARDS: 1, SPECTRA: 2};
const inbuf = new Float32Array(mem(), x.input_buffer(), 256);
let ph = 0, fr = 0;
const music = () => { for (let i = 0; i < 128; i++, fr++) { const t = fr / 44100, tb = t % 0.5;
  const v = 0.25 * Math.sin(ph += 0.031) + 0.2 * Math.sin(2 * Math.PI * 330 * t) * Math.exp(-tb * 4) + 0.3 * Math.exp(-tb * 30) * (Math.random() * 2 - 1);
  inbuf[2 * i] = inbuf[2 * i + 1] = v; } };
function run(blocks, live = true, gen = music) {
  let peak = 0, nan = false, sum = 0;
  for (let b = 0; b < blocks; b++) {
    if (live) gen();
    x.render(live ? 1 : 0);
    const o = new Float32Array(mem(), x.output_buffer(), 256);
    for (let i = 0; i < 256; i++) { const v = o[i]; if (!Number.isFinite(v)) nan = true; const a = Math.abs(v); if (a > peak) peak = a; sum += v * v; }
  }
  return {peak, nan, rms: Math.sqrt(sum / (blocks * 256))};
}
const sound = (r, m, min = 1e-3) => ok(!r.nan && r.rms > min && r.peak <= 1.0, m + ' ' + JSON.stringify(r));
const clean = (r, m) => ok(!r.nan && r.peak <= 1.0, m + ' ' + JSON.stringify(r));
let now = 0;
// each control pass is 20 ms and renders that much audio, as on the FM-1
function ctl(keys = 0, buttons = 0, e = [0, 0, 0, 0, 0, 0, 0]) { now += 20; const r = x.control(keys, buttons, ...e, now); run(7); return r; }
function press(btn, held = 0) { ctl(0, held | 1 << btn); return ctl(0, held); }
const enc = (k, d, held = 0) => { const e = [0, 0, 0, 0, 0, 0, 0]; e[k] = d; return ctl(0, held, e); };
const B = {OCTD: 0, OCTU: 1, FX: 2, SCL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8, SAVE: 9, ARP: 10, SEQ: 11, PLAY: 12, REC: 13};
const screen = () => new Uint16Array(mem(), x.screen(), 240 * 240);
function paintCheck(label) { x.paint(now); const s = screen(); let lit = 0; for (let i = 0; i < s.length; i++) if (s[i] !== s[0]) lit++; ok(lit > 500, 'screen drawn: ' + label); }
const track = (t) => { enc(1, -8); enc(1, t); };
const home = () => { press(B.HOME); enc(0, -20); };   // HANDS, its first view (HOME again steps the views)
const solo = (t) => { for (let u = 0; u < 4; u++) x.set_param(u, P.P_MUTE, u === t ? 0 : 1); };

// 1. capture on HANDS: tap REC, tap again; the capture plays
track(0); home();
ok(x.get_param(0, P.P_ENGINE) === ENG.HANDS, 'HOME is HANDS');
solo(0);
press(B.REC); ok(x.cap_state(0) === 1, 'REC starts a capture: ' + x.cap_state(0));
paintCheck('recording');
for (let i = 0; i < 25; i++) ctl();
press(B.REC); ok(x.cap_state(0) === 2 && x.src_len(0) > 5000, 'REC again ends it: ' + x.cap_state(0) + ' ' + x.src_len(0));
let r = run(200); sound(r, 'the capture plays');
// the playhead moves; FREEZE stops it
{ const h0 = x.head(0); run(40); ok(x.head(0) !== h0, 'the playhead moves'); }
x.set_param(0, P.P_STRETCH, 127); run(10); { const h0 = x.head(0); r = run(60); ok(x.head(0) === h0, 'STRETCH at the top freezes'); sound(r, 'frozen still sounds'); }
x.set_param(0, P.P_STRETCH, 50);
// a full buffer ends a capture on its own
x.set_param(0, P.P_RATE, 0); press(B.REC); run(700); ok(x.cap_state(0) === 2, 'a full buffer ends the capture');
x.set_param(0, P.P_RATE, 1); press(B.REC); for (let i = 0; i < 25; i++) ctl(); press(B.REC);
// HANDS: every stretch and pitch, grain and jitter
for (const [st, pi, gr, ji] of [[0, 0, 0, 0], [25, 48, 127, 127], [100, 12, 60, 30], [127, 36, 10, 0], [75, 24, 127, 127]]) {
  x.set_param(0, P.P_STRETCH, st); x.set_param(0, P.P_PITCH, pi); x.set_param(0, P.P_GRAIN, gr); x.set_param(0, P.P_JITTER, ji);
  sound(run(120), `HANDS stretch ${st} pitch ${pi} grain ${gr}`);
}
x.set_param(0, P.P_REV, 1); sound(run(100), 'HANDS reversed'); x.set_param(0, P.P_REV, 0);
x.set_param(0, P.P_POS, 100); x.set_param(0, P.P_SIZE, 3); sound(run(100), 'a tiny window late in the capture');
x.set_param(0, P.P_POS, 0); x.set_param(0, P.P_SIZE, 127); x.set_param(0, P.P_STRETCH, 50); x.set_param(0, P.P_PITCH, 24);

// 2. overdub: hold REC
ctl(0, 1 << B.REC); for (let i = 0; i < 30; i++) ctl(0, 1 << B.REC);
ok(x.cap_state(0) === 3, 'holding REC overdubs: ' + x.cap_state(0));
ctl(0, 0); ok(x.cap_state(0) === 2, 'letting go ends the overdub');

// 3. a robot: hold REC and turn a knob; it loops around the knob
{ const base = x.get_param(0, P.P_STRETCH);
  ctl(0, 1 << B.REC);
  for (let i = 0; i < 20; i++) { const e = [0, 0, i < 10 ? 2 : -2, 0, 0, 0, 0]; ctl(0, 1 << B.REC, e); }
  ctl(0, 0);
  let slot = -1; for (let k = 0; k < 4; k++) if (x.robot_param(0, k) === P.P_STRETCH) slot = k;
  ok(slot >= 0, 'REC + knob records a robot');
  ok(x.get_param(0, P.P_STRETCH) === base, 'the knob is back where it was: ' + x.get_param(0, P.P_STRETCH));
  ok(x.cap_state(0) === 2, 'turning a knob ended no capture and started no overdub');
  const seen = new Set(); for (let i = 0; i < 40; i++) { ctl(); seen.add(x.robot_off(0, slot).toFixed(3)); }
  ok(seen.size > 5, 'the robot moves: ' + seen.size);
  paintCheck('robot');
  // REC + PLAY clears the capture and the robots
  ctl(0, 1 << B.REC); ctl(0, 1 << B.REC | 1 << B.PLAY); ctl(0, 0);
  ok(x.cap_state(0) === 0 && x.src_len(0) === 0, 'REC + PLAY clears the capture');
  ok([0, 1, 2, 3].every(k => x.robot_param(0, k) === 255), 'and its robots');
  ok(!x.playing(), 'REC + PLAY starts nothing');
  press(B.REC); for (let i = 0; i < 30; i++) ctl(); press(B.REC);
}

// 4. B captures what A plays (BOUNCE IN, the default), and PING passes it back
x.set_param(0, P.P_MUTE, 0); x.set_param(1, P.P_MUTE, 0);
track(1); ok(x.get_param(1, P.P_BOUNCE) === 1, 'B bounces A by default');
press(B.REC); for (let i = 0; i < 25; i++) ctl(); press(B.REC);
ok(x.cap_state(1) === 2 && x.src_len(1) > 3000, 'B caught A');
solo(1); sound(run(150), 'B plays what it caught'); x.set_param(0, P.P_MUTE, 0);
x.set_param(0, P.P_BOUNCE, 2); x.set_param(1, P.P_BOUNCE, 2);
press(B.REC); for (let i = 0; i < 20; i++) ctl(); press(B.REC);
ok(x.cap_state(0) === 1, 'PING: when B ends, A takes it back: ' + x.cap_state(0));
run(600); ok(x.cap_state(0) === 2, 'A ends as long');
x.set_param(0, P.P_BOUNCE, 0); x.set_param(1, P.P_BOUNCE, 1);
paintCheck('ping');

// 5. two hands: OCT- mirrors the pair, OCT+ opposes it; neither moves the octave
track(0); home();
{ const a0 = x.get_param(0, P.P_POS), b0 = x.get_param(1, P.P_POS);
  ctl(0, 1 << B.OCTD); enc(4, 5, 1 << B.OCTD); ctl(0, 0);
  ok(x.get_param(0, P.P_POS) > a0 && x.get_param(1, P.P_POS) > b0, 'OCT- + knob moves both the same way');
  const a1 = x.get_param(0, P.P_POS), b1 = x.get_param(1, P.P_POS);
  ctl(0, 1 << B.OCTU); enc(4, 3, 1 << B.OCTU); ctl(0, 0);
  ok(x.get_param(0, P.P_POS) > a1 && x.get_param(1, P.P_POS) < b1, 'OCT+ + knob moves them apart');
  const l = new Uint8Array(mem(), x.leds(), 41); ok(l[B.OCTD] !== 2 && l[B.OCTU] !== 2, 'the octave stayed put');
  x.set_param(0, P.P_POS, 0); x.set_param(1, P.P_POS, 0); }

// 6. SHARDS: the page is the mode; every trigger, every shape
press(B.FX); ok(x.get_param(0, P.P_ENGINE) === ENG.SHARDS, 'FX is SHARDS');
solo(0);
for (const trig of [0, 1, 2, 3]) for (const shape of [0, 50, 90, 127]) {
  x.set_param(0, P.P_TRIG, trig); x.set_param(0, P.P_SHAPE, shape); x.set_param(0, P.P_DENS, 100); x.set_param(0, P.P_SPREAD, 60); x.set_param(0, P.P_SREV, 60);
  if (trig === 3 && !x.playing()) press(B.PLAY);
  if (trig === 2) for (let i = 0; i < 6; i++) { ctl(1 << (i * 3)); ctl(0); }
  r = run(120); if (trig === 0) sound(r, `SHARDS free shape ${shape}`); else clean(r, `SHARDS trig ${trig} shape ${shape}`);
}
if (x.playing()) press(B.PLAY);
x.set_param(0, P.P_TRIG, 0);
paintCheck('shards');

// 7. SPECTRA: two vocoders; a third track falls back to HANDS
press(B.SCL); ok(x.get_param(0, P.P_ENGINE) === ENG.SPECTRA, 'SCL is SPECTRA');
run(10); ok(x.spectra_on(0) === 1, 'A has a vocoder');
sound(run(150), 'SPECTRA plays');
for (const [p, v] of [['P_BLUR', 127], ['P_SHIFT', 0], ['P_SHIFT', 48], ['P_GATE', 127], ['P_STILT', 0], ['P_HOLD', 127], ['P_SPHASE', 127], ['P_STRETCH', 127], ['P_STRETCH', 0]])
  { x.set_param(0, P[p], v); clean(run(80), `SPECTRA ${p} ${v}`); }
for (const p of ['P_BLUR', 'P_GATE', 'P_HOLD', 'P_SPHASE']) x.set_param(0, P[p], 0);
x.set_param(0, P.P_SHIFT, 24); x.set_param(0, P.P_STILT, 64); x.set_param(0, P.P_STRETCH, 50);
track(1); press(B.SCL); x.set_param(1, P.P_CROSS, 100); x.set_param(1, P.P_MUTE, 0); run(10);
ok(x.spectra_on(1) === 1, 'B has the other vocoder');
solo(1); sound(run(150), 'B crosses A'); x.set_param(0, P.P_MUTE, 0);
x.set_param(2, P.P_SRC, SRC.CAPTURE); track(2); press(B.REC); for (let i = 0; i < 20; i++) ctl(); press(B.REC);
press(B.SCL); run(10); ok(x.spectra_on(2) === 0, 'a third SPECTRA has no vocoder');
paintCheck('spectra, none free');
solo(2); sound(run(100), 'and plays as HANDS'); x.set_param(2, P.P_ENGINE, ENG.HANDS);
for (let t = 0; t < 4; t++) x.set_param(t, P.P_MUTE, 0);

// 8. KEEP on the reel; D plays it; the reel clears from SETTINGS
track(0); home(); x.set_param(0, P.P_ENGINE, ENG.HANDS);
ok(x.reel_count() === 0 && x.reel_free() === 100, 'the reel starts empty');
ctl(0, 1 << B.SAVE); ctl(0, 1 << B.SAVE | 1 << B.REC); ctl(0, 1 << B.SAVE); ctl(0, 0);
ok(x.reel_count() === 1 && x.reel_free() < 100, 'SAVE + REC keeps the capture: ' + x.reel_count() + ' ' + x.reel_free());
ok(x.src_len(0) > 0, 'keeping leaves the capture');
track(3); ok(x.get_param(3, P.P_SRC) === SRC.REEL, 'D plays the reel by default');
solo(3); r = run(150); sound(r, 'D plays the kept splice'); ok(x.src_len(3) > 0, 'the reel has length');
x.set_param(3, P.P_SPLICE, 1); sound(run(80), 'D plays splice 1');
paintCheck('reel on HANDS');
press(B.EDIT); paintCheck('source, reel');
for (let i = 0; i < 6; i++) { track(0); home(); ctl(0, 1 << B.SAVE); ctl(0, 1 << B.SAVE | 1 << B.REC); ctl(0, 1 << B.SAVE); ctl(0, 0); }
ok(x.reel_count() >= 3 && x.reel_free() < 20, 'the reel fills: ' + x.reel_count() + ' ' + x.reel_free());
press(B.GLO); press(B.GLO); press(B.GLO); enc(0, 3);   // SETTINGS, PANEL
enc(5, 1); press(B.REC);
ok(x.reel_count() === 0 && x.reel_free() === 100, 'REEL turned and REC erases the reel');
for (let t = 0; t < 4; t++) x.set_param(t, P.P_MUTE, 0);

// 9. THRU and KEYS
x.set_param(2, P.P_SRC, SRC.THRU); solo(2); sound(run(100), 'C plays the input live');
for (const keys of [1, 2, 3]) { x.set_param(2, P.P_KEYS, keys); for (let i = 0; i < 4; i++) { x.midi(0x92, 55 + i * 3, 100); run(20); x.midi(0x82, 55 + i * 3, 0); } clean(run(20), 'KEYS ' + keys); }
x.set_param(2, P.P_KEYS, 1); x.set_param(2, P.P_SRC, SRC.CAPTURE);
for (let t = 0; t < 4; t++) x.set_param(t, P.P_MUTE, 0);

// 10. SEQ: steps with SLICE, PITCH, LEN, PROB; CAROM
track(0); press(B.SEQ);
for (const k of [0, 3, 6, 9, 12]) { ctl(1 << k); ctl(0); }
enc(6, -20); enc(2, 5); enc(3, 7); enc(4, 3); enc(5, -20);   // step 1: SLICE, PITCH, LEN, PROB
press(B.PLAY); sound(run(400), 'steps play slices');
paintCheck('seq');
enc(0, 2); enc(3, 1); paintCheck('seq, view 3'); r = run(200); clean(r, 'CAROM');
enc(3, -1); enc(0, -5);
press(B.PLAY);

// 11. COLOR, every mode, everything up
for (let cm = 0; cm < 3; cm++) for (let nt = 0; nt < 3; nt++) {
  for (let t = 0; t < 4; t++) for (const [p, v] of [['P_DRIVE', 127], ['P_COMP', 127], ['P_CRUSH', 100], ['P_CMODE', cm], ['P_NOISE', 127], ['P_NTYPE', nt], ['P_TILT', 127], ['P_XOVER', (cm + nt) % 3]]) x.set_param(t, P[p], v);
  clean(run(60), `color ${cm}/${nt}`);
}
for (let t = 0; t < 4; t++) for (const p of ['P_DRIVE', 'P_COMP', 'P_CRUSH', 'P_NOISE']) x.set_param(t, P[p], 0);

// 12. modulators: every type in every slot, routed to a putty knob, level and a global
for (let ty = 0; ty < 4; ty++) {
  for (let t = 0; t < 4; t++) for (let m = 0; m < 4; m++) x.set_param(t, P.P_MOD0 + m * 10, ty);
  x.set_route(0, 0, 0, P.P_POS, 100, 0); x.set_route(0, 1, 1, P.P_LEVEL, -60, 1); x.set_route(0, 2, 2, 128 + 5, 80, 1);
  ctl(1); r = run(120); ctl(0); r = run(60);
  const vals = []; for (let m = 0; m < 4; m++) vals.push(x.mod_value(0, m));
  ok(!r.nan && vals.every(v => v >= 0 && v <= 1), 'mod type ' + ty + ' ' + vals.map(v => v.toFixed(2)));
}
for (let m = 0; m < 4; m++) x.set_param(0, P.P_MOD0 + m * 10, 0);
x.set_param(0, P.P_MOD0 + 1, 120);
{ const seen = new Set(); for (let i = 0; i < 60; i++) { run(4); seen.add(x.mod_value(0, 0).toFixed(2)); } ok(seen.size > 10, 'WAVE moves: ' + seen.size); }
for (let t = 0; t < 4; t++) for (let r2 = 0; r2 < 16; r2++) x.set_route(t, r2, 0, 255, 0, 0);

// 13. every page and every view paints, and every knob turns without trouble
const pages = [B.HOME, B.EDIT, B.FX, B.SCL, B.ENV, B.LFO, B.GLO, B.ARP, B.SEQ, B.SAVE];
for (const t of [0, 1, 2, 3]) {
  track(t);
  for (const pg of pages) {
    press(pg);
    for (let v = 0; v < 14; v++) {
      for (let k = 2; k < 6; k++) { enc(k, 1); enc(k, -1); }
      paintCheck(`track ${t} page ${pg} view ${v}`);
      enc(0, 1);
    }
    enc(0, -20);
  }
  for (let k = 0; k < 4; k++) x.set_param(t, P.P_MUTE, 0);
}
press(B.EDIT); for (let i = 0; i < 3; i++) { enc(6, 1); paintCheck('source ' + i); } enc(6, -5);
press(B.LFO); press(B.LFO); for (let i = 0; i < 4; i++) { enc(6, 1); paintCheck('mod type ' + i); }

// 14. the matrix: LFO opens it first; it lists every route on every track, then NEW
{ const NP = x.np(), ROUTES = 32 + 4 * NP + x.ng() + 4;
  const routes = () => { const b = new Uint8Array(mem(), x.project() + ROUTES, 4 * 16 * 4), out = [];
    for (let t = 0; t < 4; t++) for (let r2 = 0; r2 < 16; r2++) { const o = (t * 16 + r2) * 4; if (b[o + 1] !== 255) out.push({t, src: b[o], param: b[o + 1], depth: (b[o + 2] << 24) >> 24, flags: b[o + 3]}); }
    return out; };
  for (let t = 0; t < 4; t++) for (let r2 = 0; r2 < 16; r2++) x.set_route(t, r2, 0, 255, 0, 0);
  track(0); x.set_param(0, P.P_ENGINE, ENG.HANDS);
  home(); press(B.LFO); paintCheck('matrix, empty');
  enc(6, -40);                                  // PRESETS: the NEW row
  enc(2, 1);                                    // a knob on NEW makes a route
  let rs = routes();
  ok(rs.length === 1 && rs[0].t === 0 && rs[0].param === P.P_STRETCH && rs[0].src === 0, 'a knob on NEW makes A WAV1 > A STRETCH: ' + JSON.stringify(rs));
  enc(2, 5);
  rs = routes(); ok(rs[0].src === (16 | 1 << 2 | 1), 'SOURCE reaches another track\'s modulator: ' + rs[0].src);
  enc(3, 1);                                    // DEVICE: A PUTTY -> A COLOR
  rs = routes(); ok(rs[0].t === 0 && rs[0].param === P.P_DRIVE, 'DEVICE moves to the next device, its first knob: ' + rs[0].param);
  enc(3, 3);                                    // COLOR, MIX, MODS -> B SOURCE
  rs = routes(); ok(rs.length === 1 && rs[0].t === 1 && rs[0].src === 1, 'DEVICE onto another track moves the route there: ' + JSON.stringify(rs[0]));
  enc(3, 30);
  rs = routes(); ok(rs[0].param >= 128, 'DEVICE reaches the globals: ' + rs[0].param);
  enc(5, 10);
  rs = routes(); ok(rs[0].depth === 52, 'AMOUNT: ' + rs[0].depth);
  press(B.REC); rs = routes(); ok(rs[0].flags === 1, 'REC: bipolar');
  paintCheck('matrix, one route');
  clean(run(40), 'cross-track and global routes run');
  enc(4, -60);
  ok(routes().length === 0, 'PARAM turned past the first removes the route');
  // LFO + knob on MIX: A's modulator onto C's level
  press(B.EDIT); press(B.EDIT); press(B.EDIT); press(B.EDIT); enc(0, -9);
  ctl(0, 1 << B.LFO); enc(4, 4, 1 << B.LFO); ctl(0, 0);
  rs = routes(); ok(rs.length === 1 && rs[0].t === 2 && rs[0].param === P.P_LEVEL, 'LFO + knob maps across tracks: ' + JSON.stringify(rs));
  for (let t = 0; t < 4; t++) for (let r2 = 0; r2 < 16; r2++) x.set_route(t, r2, 0, 255, 0, 0);
}

// 15. MIDI: learn, the fixed CC map, program change, clock and transport
home();
ctl(0, 1 << B.SAVE); enc(2, 1, 1 << B.SAVE); ctl(0, 0);
x.midi(0xb5, 21, 99); run(1);
const le = x.learn_entry(0); ok((le & 0xff) === 5 && ((le >> 8) & 0xff) === 21, 'learned ' + le.toString(16));
const target = le >>> 24, tk = (le >> 16) & 0xff;
x.midi(0xb5, 21, 0); run(1); const lo = x.get_param(tk, target);
x.midi(0xb5, 21, 127); run(1); const hi = x.get_param(tk, target);
ok(hi > lo, `learned CC moves ${target}: ${lo} -> ${hi}`);
x.midi(0xb1, 70, 10); run(1); ok(x.get_param(1, P.P_BLUR) === 10, 'CC70 ch2 -> B BLUR');
x.midi(0xb0, 16, 0); run(1); ok(x.get_param(0, P.P_STRETCH) === 0, 'CC16 ch1 -> A STRETCH');
x.midi(0xbf, 7, 64); run(1); ok(x.get_global(G.G_MAIN) === 64, 'CC7 ch16 -> main');
x.set_param(0, P.P_STRETCH, 50);
if (x.playing()) { press(B.PLAY); run(2); }
ok(!x.playing(), 'stopped');
x.midi(0xfa, 0, 0); run(1); ok(x.playing(), 'MIDI start plays');
{ const fpc = 44100 * 60 / (140 * 24); let acc = 0;
  for (let b = 0; b < 2000; b++) { acc += 128; while (acc >= fpc) { acc -= fpc; x.midi(0xf8, 0, 0); } run(1, false); }
  ok(Math.abs(x.tempo_now() - 140) < 2, 'clock tempo ' + x.tempo_now().toFixed(1)); }
x.midi(0xfc, 0, 0); run(1); ok(!x.playing(), 'MIDI stop stops');
x.midi(0xfb, 0, 0); run(1); ok(x.playing(), 'MIDI continue plays');
x.set_global(G.G_CLOCK, 1); x.midi(0xfc, 0, 0); run(1); ok(x.playing(), 'INT ignores MIDI stop');
x.set_global(G.G_CLOCK, 0);
x.midi(0x92, 60, 100); r = run(40); x.midi(0x82, 60, 0); clean(r, 'midi note');
x.midi(0xcf, 1, 0); run(2);
if (x.playing()) press(B.PLAY);

// 16. the panel: LEDs, knob CCs with pickup, metronome, the FM-1's own settings
press(B.SCL);
{ const l = new Uint8Array(mem(), x.leds(), 41); ok(l[B.SCL] === 2 && l[B.FX] !== 2, 'the open page lights its button'); }
const dev = () => new Uint8Array(mem(), x.device(), 20);
dev()[17] = 1; dev()[18] = 0;
track(0); home(); enc(0, -20);
x.set_param(0, P.P_STRETCH, 64);
x.midi(0xb0, 110, 10); run(1); ctl(); ok(x.get_param(0, P.P_STRETCH) === 64, 'knob CC far from the value waits (pickup)');
x.midi(0xb0, 110, 63); run(1); ctl(); x.midi(0xb0, 110, 90); run(1); ctl();
ok(Math.abs(x.get_param(0, P.P_STRETCH) - 90) <= 1, 'knob CC picks up and follows: ' + x.get_param(0, P.P_STRETCH));
x.set_param(0, P.P_STRETCH, 50);
{ const g0 = x.device_changed();
  press(B.GLO); press(B.GLO); press(B.GLO); enc(0, 3); enc(4, 1);
  ok(x.device_changed() !== g0, 'changing a panel setting marks the FM-1 settings to keep');
  paintCheck('settings panel'); }
{ for (let t = 0; t < 4; t++) x.set_param(t, P.P_LEVEL, 0);
  x.set_global(G.G_CLOCK, 1); if (!x.playing()) press(B.PLAY);
  const r2 = run(400, false); ok(r2.peak > 0.1, 'metronome clicks: ' + r2.peak.toFixed(2));
  press(B.PLAY); for (let t = 0; t < 4; t++) x.set_param(t, P.P_LEVEL, 100); x.set_global(G.G_CLOCK, 0); enc(4, -1); }

// 17. SOUNDS under SAVE: store a track's sound, change it, load it back; mix stays
{ if (x.playing()) press(B.PLAY);
  track(0);
  x.set_param(0, P.P_BLUR, 33); x.set_param(0, P.P_DENS, 21); x.set_param(0, P.P_LEVEL, 90);
  press(B.SAVE); press(B.SAVE); enc(6, -20);
  const g0 = x.device_changed();
  ctl(0, 1 << B.REC); const req = ctl(0, 0);
  ok(x.device_changed() !== g0 && req === 10, 'REC on an empty sound slot saves it (req ' + req + ')');
  paintCheck('sounds');
  x.set_param(0, P.P_BLUR, 100); x.set_param(0, P.P_DENS, 99); x.set_param(0, P.P_LEVEL, 40);
  press(B.REC);
  ok(x.get_param(0, P.P_BLUR) === 33 && x.get_param(0, P.P_DENS) === 21, 'REC on a sound loads it onto the track');
  ok(x.get_param(0, P.P_LEVEL) === 40, 'loading a sound leaves the mix alone');
  ctl(0, 1 << B.SAVE); ctl(0, (1 << B.SAVE) | (1 << B.PLAY)); ctl(0, 1 << B.SAVE); ctl(0, 0); run(2);
  ok(!x.playing(), 'SAVE + PLAY saves without starting playback'); }

// 18. DISSECT: the listener, and the shards it drives
{ for (let t = 0; t < 4; t++) { x.set_param(t, P.P_SRC, SRC.THRU); x.set_param(t, P.P_INPUT, 0); x.set_param(t, P.P_BOUNCE, 0); x.set_param(t, P.P_ENGINE, ENG.HANDS);
    x.set_param(t, P.P_TRIG, 0); x.set_param(t, P.P_MUTE, 1); x.set_param(t, P.P_LEVEL, 0);
    for (let m = 0; m < 4; m++) x.set_param(t, P.P_MOD0 + m * 10, m === 2 ? 0 : m); }
  x.set_param(0, P.P_MUTE, 0); x.set_param(0, P.P_LEVEL, 100);
  x.set_global(G.G_ASRC, 0); x.set_global(G.G_ASENS, 80); x.set_global(G.G_AFOCUS, 0); x.set_global(G.G_AHITS, 0);
  let f2 = 0;
  const drums = () => { for (let i = 0; i < 128; i++, f2++) { const t = f2 / 44100, tb = t % 0.5, th = (t + 0.25) % 0.5;
      const v = 0.7 * Math.exp(-tb * 25) * Math.sin(2 * Math.PI * (50 + 80 * Math.exp(-tb * 40)) * tb) + 0.25 * Math.exp(-th * 60) * (Math.random() * 2 - 1) + 0.08 * Math.sin(2 * Math.PI * 220 * t);
      inbuf[2 * i] = inbuf[2 * i + 1] = v; } };
  const tone = (f, a = 0.4) => () => { for (let i = 0; i < 128; i++, f2++) { const v = a * Math.sin(2 * Math.PI * f * f2 / 44100); inbuf[2 * i] = inbuf[2 * i + 1] = v; } };
  track(0); home();
  run(40, true, drums); const h0 = x.ana_hits(); run(700, true, drums); ok(x.ana_hits() === h0, 'the listener sleeps while nothing uses it');
  press(B.GLO); press(B.GLO); paintCheck('dissect');
  run(20, true, drums); const h1 = x.ana_hits(); run(689, true, drums);
  ok(Math.abs(x.ana_hits() - h1 - 8) <= 1, 'hits: a kick and a hat a beat for 2 s: ' + (x.ana_hits() - h1));
  run(120, true, tone(220)); ok(Math.abs(x.ana_pitch() - 220) < 2 && x.ana_conf() > 0.5, 'pitch: 220 Hz: ' + x.ana_pitch().toFixed(1) + ' ' + x.ana_conf().toFixed(2));
  run(120, true, tone(880)); ok(Math.abs(x.ana_pitch() - 880) < 8, 'pitch: 880 Hz (no octave slip): ' + x.ana_pitch().toFixed(1));
  { const h2 = x.ana_hits(); run(300, true, tone(147)); ok(x.ana_hits() - h2 <= 1, 'a steady tone makes no hits: ' + (x.ana_hits() - h2)); }
  for (let v = 0; v < 2; v++) { for (let k = 2; k < 6; k++) { enc(k, 1); enc(k, -1); } paintCheck('dissect view ' + v); enc(0, 1); }
  enc(0, -9);
  // REC on DISSECT: SHARDS on the hits
  press(B.REC); ok(x.get_param(0, P.P_ENGINE) === ENG.SHARDS && x.get_param(0, P.P_TRIG) === 1, 'REC on DISSECT: shards on the hits');
  x.set_param(0, P.P_SRC, SRC.CAPTURE); press(B.REC); for (let i = 0; i < 30; i++) ctl(); press(B.REC);
  x.set_param(0, P.P_DENS, 80);
  r = run(700, true, drums); sound(r, 'SHARDS HIT bursts on hits');
  x.set_param(0, P.P_TRIG, 0);
  // FOLLOW on the LOW band moves
  x.set_param(0, P.P_MOD0 + 20, 2); x.set_param(0, P.P_MOD0 + 25, 8);
  { const seen = new Set(); for (let i = 0; i < 60; i++) { run(4, true, drums); seen.add(x.mod_value(0, 2).toFixed(2)); } ok(seen.size > 5, 'FOLLOW LOW moves with the music: ' + seen.size); }
  // HITS STEPS: the music clocks the sequencer
  x.set_global(G.G_AHITS, 2); press(B.PLAY); run(100, true, tone(147));
  { const s0 = x.step_now(0); run(300, true, tone(147)); const s1 = x.step_now(0); run(345, true, drums); const s2 = x.step_now(0);
    ok(s1 === s0 && s2 !== s1, 'HITS STEPS: no hits, no steps; hits step it: ' + [s0, s1, s2]); }
  x.set_global(G.G_AHITS, 1); x.set_param(1, P.P_SEQM, 1); clean(run(300, true, drums), 'HITS BALLS');
  press(B.PLAY); x.set_global(G.G_AHITS, 0); x.set_param(1, P.P_SEQM, 0);
  home();
}

// 19. projects: this format loads; anything else is refused
{ const p = new Uint8Array(mem(), x.project(), x.project_bytes()), keep = p.slice();
  x.project_seal(0); ok(x.project_check() === 1, 'a sealed project checks');
  const dv = new DataView(mem(), x.project(), 16); dv.setUint32(4, 4, true);
  ok(x.project_check() !== 1, 'a 0.x project (format 4) is refused');
  p.set(keep); }

// 20. long run with everything moving: no NaN, no runaway
for (let t = 0; t < 4; t++) { x.set_param(t, P.P_SRC, SRC.CAPTURE); x.set_param(t, P.P_MUTE, 0); x.set_param(t, P.P_LEVEL, 110); x.set_param(t, P.P_SEND, 100); x.set_param(t, P.P_SOS, 127); }
x.set_param(0, P.P_ENGINE, ENG.SPECTRA); x.set_param(1, P.P_ENGINE, ENG.SPECTRA); x.set_param(1, P.P_CROSS, 127); x.set_param(2, P.P_ENGINE, ENG.SHARDS); x.set_param(2, P.P_DENS, 127);
x.set_param(0, P.P_BOUNCE, 2); x.set_param(1, P.P_BOUNCE, 2);
x.set_global(G.G_DFEED, 127); x.set_global(G.G_RLEVEL, 127);
track(0); home(); press(B.REC); for (let i = 0; i < 20; i++) ctl(); press(B.REC);
ctl(0, 1 << B.REC); for (let i = 0; i < 100; i++) ctl(0, 1 << B.REC); ctl(0, 0);
r = run(3000); ok(!r.nan && r.peak <= 1.0 && r.rms > 1e-3, 'long run ' + JSON.stringify(r));
console.log(fails ? `${fails} failures` : 'all engine tests pass');
process.exit(fails ? 1 : 0);
