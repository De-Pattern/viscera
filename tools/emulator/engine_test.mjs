// Viscera engine and UI test, run on the WebAssembly build of the same core the FM-1 runs.
// Every page and view is painted, every device and modulator type is played, MIDI learn,
// clock and transport are exercised, and nothing may produce NaN, silence where sound is
// expected, or runaway level.
import fs from 'node:fs';
const wasm = fs.readFileSync(process.argv[2] || 'site/emulator/viscera.wasm');
// parameter indices, read from the core's header
const P = (() => {
  const h = fs.readFileSync(new URL('../../core/vsc.h', import.meta.url), 'utf8');
  let body = h.slice(h.indexOf('enum vsc_tparam {') + 17, h.indexOf('VSC_NP\n};')).replace(/\/\*[\s\S]*?\*\//g, '');
  const idx = {}; let i = 0;
  for (let n of body.split(',').map(s => s.trim()).filter(Boolean)) {
    if (n.includes('=')) { const [a] = n.split('='); n = a.trim(); i = idx.P_MOD0 + 40; }
    idx[n] = i++;
  }
  return idx;
})();
const {instance} = await WebAssembly.instantiate(wasm, {});
const x = instance.exports;
const mem = () => x.memory.buffer;
let fails = 0;
const ok = (c, m) => { if (!c) { fails++; console.log('FAIL', m); } };
x.init();
// a bank sound: a decaying chord in slot 0 and noise in slot 1
const stage = () => new Int16Array(mem(), x.bank_stage(), x.bank_stage_frames());
{ const s = stage(); for (let i = 0; i < 64000; i++) s[i] = Math.round(12000 * Math.exp(-i / 20000) * (Math.sin(i * 0.05) + Math.sin(i * 0.0631) + Math.sin(i * 0.0749)) / 3);
  new Uint8Array(mem(), x.bank_name(), 16).set([67, 72, 79, 82, 68, 0]); ok(x.bank_commit(0, 64000) === 0, 'bank 0'); }
{ const s = stage(); for (let i = 0; i < 32000; i++) s[i] = Math.round((Math.random() * 2 - 1) * 9000);
  new Uint8Array(mem(), x.bank_name(), 16).set([78, 79, 73, 83, 69, 0]); ok(x.bank_commit(1, 32000) === 0, 'bank 1'); }
const inbuf = new Float32Array(mem(), x.input_buffer(), 256);
let ph = 0;
function run(blocks, live = true) {
  let peak = 0, nan = false, sum = 0;
  for (let b = 0; b < blocks; b++) {
    if (live) for (let i = 0; i < 128; i++) { const v = 0.3 * Math.sin(ph += 0.03) * (Math.random() * 0.5 + 0.5); inbuf[2 * i] = v; inbuf[2 * i + 1] = v; }
    x.render(live ? 1 : 0);
    const o = new Float32Array(mem(), x.output_buffer(), 256);
    for (let i = 0; i < 256; i++) { const v = o[i]; if (!Number.isFinite(v)) nan = true; const a = Math.abs(v); if (a > peak) peak = a; sum += v * v; }
  }
  return {peak, nan, rms: Math.sqrt(sum / (blocks * 256))};
}
let now = 0;
function ctl(keys = 0, buttons = 0, e = [0, 0, 0, 0, 0, 0, 0]) { now += 20; return x.control(keys, buttons, ...e, now); }
function press(btn) { ctl(0, 1 << btn); ctl(0, 0); }
const B = {OCTD: 0, OCTU: 1, FX: 2, SCL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8, SAVE: 9, ARP: 10, SEQ: 11, PLAY: 12, REC: 13};
const screen = () => new Uint16Array(mem(), x.screen(), 240 * 240);
function paintCheck(label) { x.paint(now); const s = screen(); let lit = 0; for (let i = 0; i < s.length; i++) if (s[i] !== s[0]) lit++; ok(lit > 500, 'screen drawn: ' + label); }

// 1. defaults play: hold a note on every track, sequencer running
x.set_param(0, P.P_MODE, 0); x.set_param(0, P.P_SLOT, 0);
x.set_param(2, P.P_MODE, 2); x.set_param(2, P.P_SLOT, 0);
press(B.PLAY);
for (let t = 0; t < 4; t++) { x.set_param(t, P.P_FTYPE, 5); }
ctl(1);
let r = run(200); ok(!r.nan && r.peak > 0.01 && r.peak <= 1.0, 'default sound ' + JSON.stringify(r));
ctl(0);

// 2. every filter device on every track, including RESON with everything turned up
for (const ft of [0, 1, 2, 3, 4, 5]) {
  for (let t = 0; t < 4; t++) { x.set_param(t, P.P_FTYPE, ft); x.set_param(t, P.P_FMIX, 127); }
  if (ft === 4) for (let t = 0; t < 4; t++) for (const [p, v] of [['P_KDECAY', 127], ['P_FRES', 127], ['P_KWAVES', 127], ['P_KNOISE', 127], ['P_KDETUN', 127], ['P_KTILT', 0], ['P_KSCALE', 7], ['P_FCUT', 60]]) x.set_param(t, P[p], v);
  ctl(1 | 1 << 12); r = run(150); ctl(0);
  ok(!r.nan && r.peak <= 1.0, 'filter ' + ft + ' ' + JSON.stringify(r));
  if (ft !== 5) ok(r.rms > 1e-4, 'filter ' + ft + ' makes sound');
}
// RESON: both decay modes and send
for (const dm of [0, 1]) for (const wm of [0, 1]) {
  for (let t = 0; t < 4; t++) { x.set_param(t, P.P_FTYPE, 4); x.set_param(t, P.P_KDMODE, dm); x.set_param(t, P.P_FWMODE, wm); }
  r = run(100); ok(!r.nan && r.peak <= 1.0, `reson dmode ${dm} wmode ${wm}`);
}
// 3. COLOR: every crush mode, noise type, crossover, with everything up
for (let cm = 0; cm < 3; cm++) for (let nt = 0; nt < 3; nt++) {
  for (let t = 0; t < 4; t++) for (const [p, v] of [['P_DRIVE', 127], ['P_COMP', 127], ['P_CRUSH', 100], ['P_CMODE', cm], ['P_NOISE', 127], ['P_NTYPE', nt], ['P_TILT', 127], ['P_XOVER', (cm + nt) % 3]]) x.set_param(t, P[p], v);
  r = run(60); ok(!r.nan && r.peak <= 1.0, `color ${cm}/${nt}`);
}
for (let t = 0; t < 4; t++) for (const p of ['P_DRIVE', 'P_COMP', 'P_CRUSH', 'P_NOISE']) x.set_param(t, P[p], 0);
// 4. modulators: every type in every slot, routed to cutoff, level and a global
for (let ty = 0; ty < 4; ty++) {
  for (let t = 0; t < 4; t++) for (let m = 0; m < 4; m++) x.set_param(t, P.P_MOD0 + m * 10, ty);
  x.set_route(0, 0, 0, P.P_FCUT, 100, 0); x.set_route(0, 1, 1, P.P_LEVEL, -60, 1); x.set_route(0, 2, 2, 128 + 5, 80, 1);
  ctl(1); r = run(120); ctl(0); r = run(60);
  let vals = []; for (let m = 0; m < 4; m++) vals.push(x.mod_value(0, m));
  ok(!r.nan && vals.every(v => v >= 0 && v <= 1), 'mod type ' + ty + ' ' + vals.map(v => v.toFixed(2)));
}
// WAVE moves
for (let m = 0; m < 4; m++) x.set_param(0, P.P_MOD0 + m * 10, 0);
x.set_param(0, P.P_MOD0 + 1, 120);
{ let seen = new Set(); for (let i = 0; i < 60; i++) { run(4); seen.add(x.mod_value(0, 0).toFixed(2)); } ok(seen.size > 10, 'WAVE moves: ' + seen.size); }

// 5. every page and every view paints, and every knob turns without trouble
const pages = [B.HOME, B.EDIT, B.FX, B.SCL, B.ENV, B.LFO, B.GLO, B.ARP, B.SEQ, B.SAVE];
for (const t of [0, 1, 2, 3]) {
  ctl(0, 0, [0, t - 3, 0, 0, 0, 0, 0]); ctl(0, 0, [0, 3, 0, 0, 0, 0, 0]); // track via ALGORITHM
}
for (const pg of pages) {
  press(pg);
  for (let v = 0; v < 14; v++) {
    for (let k = 2; k < 6; k++) { const e = [0, 0, 0, 0, 0, 0, 0]; e[k] = 1; ctl(0, 0, e); e[k] = -1; ctl(0, 0, e); }
    paintCheck('page ' + pg + ' view ' + v);
    run(2);
    ctl(0, 0, [1, 0, 0, 0, 0, 0, 0]);   // SELECT: next view
  }
  ctl(0, 0, [-20, 0, 0, 0, 0, 0, 0]);
}
// PRESETS cycles devices on MATERIAL and FILTER and modulator types
press(B.EDIT); for (let i = 0; i < 6; i++) { ctl(0, 0, [0, 0, 0, 0, 0, 0, 1]); paintCheck('material device ' + i); run(5); }
press(B.SCL); for (let i = 0; i < 6; i++) { ctl(0, 0, [0, 0, 0, 0, 0, 0, -1]); paintCheck('filter device ' + i); run(5); }
press(B.LFO); press(B.LFO); for (let i = 0; i < 4; i++) { ctl(0, 0, [0, 0, 0, 0, 0, 0, 1]); paintCheck('mod type ' + i); run(5); }
// the matrix: LFO opens it first; it lists every route on every track, then NEW
{ const NP = x.np(), ROUTES = 32 + 4 * NP + x.ng() + 4;
  const routes = () => { const b = new Uint8Array(mem(), x.project() + ROUTES, 4 * 16 * 4), out = [];
    for (let t = 0; t < 4; t++) for (let r = 0; r < 16; r++) { const o = (t * 16 + r) * 4; if (b[o + 1] !== 255) out.push({t, src: b[o], param: b[o + 1], depth: (b[o + 2] << 24) >> 24, flags: b[o + 3]}); }
    return out; };
  for (let t = 0; t < 4; t++) for (let r = 0; r < 16; r++) x.set_route(t, r, 0, 255, 0, 0);
  ctl(0, 0, [0, -8, 0, 0, 0, 0, 0]);                       // track 1
  press(B.HOME); press(B.LFO); paintCheck('matrix, empty');
  ctl(0, 0, [0, 0, 0, 0, 0, 0, -40]);                      // PRESETS: the NEW row
  ctl(0, 0, [0, 0, 1, 0, 0, 0, 0]);                        // a knob on NEW makes a route
  let rs = routes();
  ok(rs.length === 1 && rs[0].t === 0 && rs[0].param === P.P_FCUT && rs[0].src === 0, 'a knob on NEW makes T1 WAV1 > T1 CUTOFF: ' + JSON.stringify(rs));
  ctl(0, 0, [0, 0, 5, 0, 0, 0, 0]);                        // SOURCE: five on, T2's modulator 2
  rs = routes(); ok(rs[0].src === (16 | 1 << 2 | 1), 'SOURCE reaches another track\'s modulator: ' + rs[0].src);
  ctl(0, 0, [0, 0, 0, 1, 0, 0, 0]);                        // DEVICE: T1 FILTER -> T1 COLOR
  rs = routes(); ok(rs[0].t === 0 && rs[0].param === P.P_DRIVE, 'DEVICE moves to the next device, its first knob: ' + rs[0].param);
  ctl(0, 0, [0, 0, 0, 3, 0, 0, 0]);                        // COLOR, MIX, MODS -> T2 MATERIAL
  rs = routes(); ok(rs.length === 1 && rs[0].t === 1 && rs[0].src === 1, 'DEVICE onto another track moves the route there (its own modulator now): ' + JSON.stringify(rs[0]));
  ctl(0, 0, [0, 0, 0, 30, 0, 0, 0]);                       // past the tracks: SPACE, PERFORM, MASTER
  rs = routes(); ok(rs[0].param >= 128, 'DEVICE reaches the globals: ' + rs[0].param);
  ctl(0, 0, [0, 0, 0, 0, 0, 10, 0]);                       // AMOUNT
  rs = routes(); ok(rs[0].depth === 52, 'AMOUNT: ' + rs[0].depth);
  press(B.REC); rs = routes(); ok(rs[0].flags === 1, 'REC: bipolar');
  paintCheck('matrix, one route');
  r = run(40); ok(!r.nan, 'cross-track and global routes run');
  ctl(0, 0, [0, 0, 0, 0, -60, 0, 0]);                      // PARAM before the first removes it
  ok(routes().length === 0, 'PARAM turned past the first removes the route');
  // LFO + knob on a page: T1's modulator onto T3's level (MIX lays tracks across)
  press(B.HOME); ctl(0, 0, [-9, 0, 0, 0, 0, 0, 0]);
  ctl(0, 1 << B.LFO); ctl(0, 1 << B.LFO, [0, 0, 0, 0, 4, 0, 0]); ctl(0, 0);
  rs = routes(); ok(rs.length === 1 && rs[0].t === 2 && rs[0].param === P.P_LEVEL && rs[0].src === (16 | 0 << 2 | (rs[0].src & 3)), 'LFO + knob maps across tracks: ' + JSON.stringify(rs));
  for (let t = 0; t < 4; t++) for (let r2 = 0; r2 < 16; r2++) x.set_route(t, r2, 0, 255, 0, 0);
}
// LFO + knob maps a modulator on a device page
press(B.SCL); x.set_param(0, P.P_FTYPE, 4);
ctl(0, 1 << B.LFO); ctl(0, 1 << B.LFO, [0, 0, 5, 0, 0, 0, 0]); ctl(0, 0);
paintCheck('after LFO map');

// 6. MIDI: learn, fixed CC map, program change, clock and transport
press(B.SCL);
ctl(0, 1 << B.SAVE); ctl(0, 1 << B.SAVE, [0, 0, 1, 0, 0, 0, 0]); ctl(0, 0);
x.midi(0xb5, 21, 99); run(1); // CC21 on channel 6: learn
const le = x.learn_entry(0); ok((le & 0xff) === 5 && ((le >> 8) & 0xff) === 21, 'learned ' + le.toString(16));
const target = le >>> 24, tk = (le >> 16) & 0xff;
x.midi(0xb5, 21, 0); run(1); const lo = x.get_param(tk, target);
x.midi(0xb5, 21, 127); run(1); const hi = x.get_param(tk, target);
ok(hi > lo, `learned CC moves ${target}: ${lo} -> ${hi}`);
x.midi(0xb1, 70, 10); run(1); ok(x.get_param(1, P.P_FCUT) === 10, 'CC70 ch2 -> T2 cutoff');
x.midi(0xbf, 7, 64); run(1); ok(x.get_global(9) === 64, 'CC7 ch16 -> main');
// clock: stop, then external clock at 140 BPM in AUTO follows
press(B.PLAY); run(2); if (x.playing()) { press(B.PLAY); run(2); }
ok(!x.playing(), 'stopped');
x.midi(0xfa, 0, 0); run(1); ok(x.playing(), 'MIDI start plays');
const frames_per_clock = 44100 * 60 / (140 * 24);
let acc = 0;
for (let b = 0; b < 2000; b++) { acc += 128; while (acc >= frames_per_clock) { acc -= frames_per_clock; x.midi(0xf8, 0, 0); } run(1, false); }
ok(Math.abs(x.tempo_now() - 140) < 2, 'clock tempo ' + x.tempo_now().toFixed(1));
x.midi(0xfc, 0, 0); run(1); ok(!x.playing(), 'MIDI stop stops');
x.midi(0xfb, 0, 0); run(1); ok(x.playing(), 'MIDI continue plays');
// INT ignores MIDI transport
x.set_global(10, 1); x.midi(0xfc, 0, 0); run(1); ok(x.playing(), 'INT ignores MIDI stop');
x.set_global(10, 0);
// notes in on channel 3 play track 3
x.midi(0x92, 60, 100); r = run(40); x.midi(0x82, 60, 0); ok(!r.nan, 'midi note');
// program change recalls a scene without trouble
x.midi(0xcf, 1, 0); run(2);

// 6b. the panel: LEDs, knob CCs with pickup, metronome, the FM-1's own settings
press(B.SCL);
{ const l = new Uint8Array(mem(), x.leds(), 41); ok(l[B.SCL] === 2 && l[B.FX] !== 2, 'the open page lights its button'); }
const dev = () => new Uint8Array(mem(), x.device(), 20);
dev()[17] = 1; dev()[18] = 0;                  // KNOB CC on, METRO off (the page sweep above toggled them)
x.set_param(0, P.P_FTYPE, 0); x.set_param(0, P.P_FCUT, 64);
ctl(0, 0, [0, -8, 0, 0, 0, 0, 0]);             // track 1
ctl(0, 0, [-20, 0, 0, 0, 0, 0, 0]);            // first view: CUTOFF on KNOB 1
x.midi(0xb0, 110, 10); run(1); ctl(); ok(x.get_param(0, P.P_FCUT) === 64, 'knob CC far from the value waits (pickup)');
x.midi(0xb0, 110, 63); run(1); ctl(); x.midi(0xb0, 110, 90); run(1); ctl();
ok(Math.abs(x.get_param(0, P.P_FCUT) - 90) <= 1, 'knob CC picks up and follows: ' + x.get_param(0, P.P_FCUT));
{ const g0 = x.device_changed();
  press(B.GLO); press(B.GLO); press(B.GLO);    // SPACE, DISSECT, then SETTINGS
  ctl(0, 0, [3, 0, 0, 0, 0, 0, 0]);            // PANEL view
  ctl(0, 0, [0, 0, 0, 0, 1, 0, 0]);            // METRO on (KNOB 3)
  ok(x.device_changed() !== g0, 'changing a panel setting marks the FM-1 settings to keep');
  paintCheck('settings panel'); }
{ // metronome: clicks while playing, with every track silent
  for (let t = 0; t < 4; t++) x.set_param(t, P.P_LEVEL, 0);
  x.set_global(10, 1); if (!x.playing()) press(12);
  let r2 = run(400, false); ok(r2.peak > 0.1, 'metronome clicks: ' + r2.peak.toFixed(2));
  press(12); for (let t = 0; t < 4; t++) x.set_param(t, P.P_LEVEL, 100); x.set_global(10, 0); }

// 6b2. SETTINGS -> AUDIO IN: input meters move, INPUT is the track's, REC opens its MATERIAL
{ ctl(0, 0, [-9, 0, 0, 0, 0, 0, 0]); ctl(0, 0, [1, 0, 0, 0, 0, 0, 0]);   // view 1
  paintCheck('settings audio in');
  ctl(0, 0, [0, 0, 0, 0, 0, 0, -8]);             // PRESETS: track 1
  const before = x.get_param(0, P.P_INPUT);
  ctl(0, 0, [0, 0, 0, 1, 0, 0, 0]);              // KNOB 2: INPUT
  ok(x.get_param(0, P.P_INPUT) === before + 1, 'INPUT on the audio-in view is the track\'s');
  ctl(0, 0, [0, 0, 0, -1, 0, 0, 0]);
  press(B.REC); press(B.SCL);                   // REC opens MATERIAL; then leave it
}

// 6c. SOUNDS under SAVE: store a track's sound, change it, load it back; mix and sequence stay
{ run(2); if (x.playing()) { press(B.PLAY); run(2); }
  ctl(0, 0, [0, -8, 0, 0, 0, 0, 0]);             // track 1
  x.set_param(0, P.P_FCUT, 33); x.set_param(0, P.P_GSIZE, 21); x.set_param(0, P.P_LEVEL, 90);
  press(B.SAVE); press(B.SAVE);                  // PROJECT, then SOUNDS
  ctl(0, 0, [0, 0, 0, 0, 0, 0, -20]);            // slot 1
  const g0 = x.device_changed();
  ctl(0, 1 << B.REC); const req = ctl(0, 0);     // empty slot: REC saves
  ok(x.device_changed() !== g0 && req === 10, 'REC on an empty sound slot saves it (req ' + req + ')');
  paintCheck('sounds');
  x.set_param(0, P.P_FCUT, 100); x.set_param(0, P.P_GSIZE, 99); x.set_param(0, P.P_LEVEL, 40);
  press(B.REC);                                  // used slot: REC loads
  ok(x.get_param(0, P.P_FCUT) === 33 && x.get_param(0, P.P_GSIZE) === 21, 'REC on a sound loads it onto the track');
  ok(x.get_param(0, P.P_LEVEL) === 40, 'loading a sound leaves the mix alone');
  ctl(0, 1 << B.SAVE); ctl(0, (1 << B.SAVE) | (1 << B.PLAY)); ctl(0, 1 << B.SAVE); ctl(0, 0); run(2);
  ok(!x.playing(), 'SAVE + PLAY saves without starting playback'); }

// 8. DISSECT: the listener, and what it drives
{ const G = (() => {
    const h = fs.readFileSync(new URL('../../core/vsc.h', import.meta.url), 'utf8');
    const body = h.slice(h.indexOf('enum vsc_gparam {') + 17, h.indexOf('VSC_NG\n};')).replace(/\/\*[\s\S]*?\*\//g, '');
    const idx = {}; body.split(',').map(s => s.trim()).filter(Boolean).forEach((n, i) => { idx[n] = i; }); return idx; })();
  if (x.playing()) press(B.PLAY);
  for (let t = 0; t < 4; t++) { x.set_param(t, P.P_MODE, 3); x.set_param(t, P.P_INPUT, 0); x.set_param(t, P.P_FTYPE, 5);
    x.set_param(t, P.P_GMIX, 0); x.set_param(t, P.P_GFDBK, 0); x.set_param(t, P.P_MUTE, 1); x.set_param(t, P.P_LEVEL, 0); }
  x.set_param(0, P.P_MUTE, 0); x.set_param(0, P.P_LEVEL, 100);
  for (const p of ['P_TILT', 'P_DJF', 'P_PAN']) x.set_param(0, P[p], 64);
  x.set_global(G.G_ASRC, 0); x.set_global(G.G_ASENS, 80); x.set_global(G.G_AFOCUS, 0); x.set_global(G.G_AHITS, 0);
  x.set_global(2, 50); x.set_global(6, 70);
  let fr = 0;
  const drums = () => { for (let i = 0; i < 128; i++, fr++) { const t = fr / 44100, tb = t % 0.5, th = (t + 0.25) % 0.5;
      const v = 0.7 * Math.exp(-tb * 25) * Math.sin(2 * Math.PI * (50 + 80 * Math.exp(-tb * 40)) * tb) + 0.25 * Math.exp(-th * 60) * (Math.random() * 2 - 1) + 0.08 * Math.sin(2 * Math.PI * 220 * t);
      inbuf[2 * i] = inbuf[2 * i + 1] = v; } };
  const tone = (f, a = 0.4) => () => { for (let i = 0; i < 128; i++, fr++) { const v = a * Math.sin(2 * Math.PI * f * fr / 44100); inbuf[2 * i] = inbuf[2 * i + 1] = v; } };
  const play = (gen, blocks) => { let peak = 0, sum = 0, nan = false;
    for (let b = 0; b < blocks; b++) { gen(); x.render(1); const o = new Float32Array(mem(), x.output_buffer(), 256);
      for (let i = 0; i < 256; i++) { const v = o[i]; if (!Number.isFinite(v)) nan = true; peak = Math.max(peak, Math.abs(v)); sum += v * v; } }
    return {peak, nan, rms: Math.sqrt(sum / (blocks * 256))}; };
  // nothing listens: the listener stays off
  play(drums, 40); const h0 = x.ana_hits(); play(drums, 700); ok(x.ana_hits() === h0, 'the listener sleeps while nothing uses it');
  // the page on screen wakes it: hits and pitch
  press(B.GLO); press(B.GLO); paintCheck('dissect');
  ok(x.ana_hits() === h0, 'opening DISSECT alone changes nothing yet');
  play(drums, 20); const h1 = x.ana_hits(); play(drums, 689);   /* 2 s: four kicks and four hats */
  ok(Math.abs(x.ana_hits() - h1 - 8) <= 1, 'hits: a kick and a hat a beat for 2 s: ' + (x.ana_hits() - h1));
  play(tone(220), 120); ok(Math.abs(x.ana_pitch() - 220) < 2 && x.ana_conf() > 0.5, 'pitch: 220 Hz: ' + x.ana_pitch().toFixed(1) + ' ' + x.ana_conf().toFixed(2));
  play(tone(880), 120); ok(Math.abs(x.ana_pitch() - 880) < 8, 'pitch: 880 Hz (no octave slip): ' + x.ana_pitch().toFixed(1));
  { const h2 = x.ana_hits(); play(tone(147), 300); ok(x.ana_hits() - h2 <= 1, 'a steady tone makes no hits: ' + (x.ana_hits() - h2)); }
  for (let v = 0; v < 5; v++) { for (let k = 2; k < 6; k++) { const e = [0, 0, 0, 0, 0, 0, 0]; e[k] = 1; ctl(0, 0, e); e[k] = -1; ctl(0, 0, e); }
    paintCheck('dissect view ' + v); ctl(0, 0, [1, 0, 0, 0, 0, 0, 0]); }
  ctl(0, 0, [-9, 0, 0, 0, 0, 0, 0]);
  // REBUILD: partials from the pitch heard
  x.set_param(0, P.P_FTYPE, 6); x.set_param(0, P.P_FMIX, 127);
  r = play(tone(330), 300); ok(!r.nan && r.rms > 0.05 && r.peak <= 1, 'REBUILD sounds from the pitch heard ' + JSON.stringify(r));
  { press(B.SCL); paintCheck('rebuild page'); for (let v = 0; v < 3; v++) { ctl(0, 0, [0, 0, 1, 1, 1, 1, 0]); paintCheck('rebuild view ' + v); ctl(0, 0, [1, 0, 0, 0, 0, 0, 0]); } press(B.GLO); press(B.GLO); }
  // RESON EXCITE: the hits strike the bank
  x.set_param(0, P.P_FTYPE, 4); x.set_param(0, P.P_KEXCITE, 127); x.set_param(0, P.P_FCUT, 40); x.set_param(0, P.P_KSCALE, 9); x.set_param(0, P.P_FMIX, 127);
  r = play(drums, 700); ok(!r.nan && r.rms > 0.01 && r.peak <= 1, 'RESON EXCITE rings on hits ' + JSON.stringify(r));
  x.set_param(0, P.P_KFOLLOW, 1); r = play(tone(196), 200); ok(!r.nan, 'RESON FOLLOW');
  x.set_param(0, P.P_KEXCITE, 0); x.set_param(0, P.P_KFOLLOW, 0); x.set_param(0, P.P_FTYPE, 5);
  // GRAIN HITS and NOTES, with TUNE
  x.set_param(0, P.P_GSIZE, 50);
  x.set_param(0, P.P_GTRIG, 1); x.set_param(0, P.P_GMIX, 127); x.set_param(0, P.P_GDENS, 60); x.set_param(0, P.P_GQUANT, 4);
  r = play(drums, 700); ok(!r.nan && r.rms > 0.01 && r.peak <= 1, 'GRAIN HITS bursts on hits ' + JSON.stringify(r));
  x.set_param(0, P.P_GTRIG, 2);
  for (let i = 0; i < 10; i++) { x.midi(0x90, 60 + i, 100); play(drums, 20); x.midi(0x80, 60 + i, 0); }
  r = play(drums, 50); ok(!r.nan, 'GRAIN NOTES');
  x.set_param(0, P.P_GTRIG, 0); x.set_param(0, P.P_GQUANT, 0); x.set_param(0, P.P_GMIX, 0);
  // FOLLOW on the listener's LOW band moves
  x.set_param(0, P.P_MOD0 + 20, 2); x.set_param(0, P.P_MOD0 + 25, 8);
  { const seen = new Set(); for (let i = 0; i < 60; i++) { play(drums, 4); seen.add(x.mod_value(0, 2).toFixed(2)); } ok(seen.size > 5, 'FOLLOW LOW moves with the music: ' + seen.size); }
  // HITS STEPS: the music clocks the sequencer
  x.set_global(G.G_AHITS, 2); press(B.PLAY); play(tone(147), 100);
  { const s0 = x.step_now(0); play(tone(147), 300); const s1 = x.step_now(0); play(drums, 345); const s2 = x.step_now(0);
    ok(s1 === s0 && s2 !== s1, 'HITS STEPS: no hits, no steps; hits step it: ' + [s0, s1, s2]); }
  x.set_global(G.G_AHITS, 1); x.set_param(1, P.P_SEQM, 1); r = play(drums, 300); ok(!r.nan, 'HITS BALLS');
  press(B.PLAY); x.set_global(G.G_AHITS, 0); x.set_param(1, P.P_SEQM, 0);
  press(B.HOME); run(5);
  // a 0.2.x project (format 4) loads: built from this one without the new parameters
  { const NP = x.np(), NG = x.ng(), np4 = NP - 3, ng4 = NG - 4, cur = new Uint8Array(mem(), x.project(), x.project_bytes()).slice();
    x.set_param(2, P.P_FCUT, 77); x.set_global(0, 33);
    const now = new Uint8Array(mem(), x.project(), x.project_bytes()).slice();
    const v4 = new Uint8Array(x.project_v4_bytes()); let o = 0, i = 0;
    const take = (n) => { v4.set(now.subarray(i, i + n), o); o += n; i += n; }, skip = (n) => { i += n; };
    take(32);
    for (let t = 0; t < 4; t++) { take(np4); skip(3); }
    take(ng4); skip(4); take(4 + 4 * 16 * 4 + 8 * 8 * 4 + 4 * 64 * 8);
    for (let sc = 0; sc < 8; sc++) { take(4); for (let t = 0; t < 4; t++) { take(np4); skip(3); } take(8); }
    const dv = new DataView(v4.buffer); dv.setUint32(4, 4, true); dv.setUint32(8, v4.length, true);
    let c = 0xffffffff; for (let k = 16; k < v4.length; k++) { c ^= v4[k]; for (let b = 0; b < 8; b++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1)); }
    dv.setUint32(12, (~c) >>> 0, true);
    const p = new Uint8Array(mem(), x.project(), x.project_bytes());
    p.fill(0xee); p.set(v4);
    ok(x.project_upgrade() === 1, 'a format 4 project upgrades');
    ok(x.get_param(2, P.P_FCUT) === 77 && x.get_global(0) === 33 && x.get_param(3, P.P_GTRIG) === 0, 'its values survive, the new ones are defaults');
    p.set(cur); }
}

// 7. long run with everything moving: no NaN, no runaway
for (let t = 0; t < 4; t++) { x.set_param(t, P.P_FTYPE, 4); x.set_param(t, P.P_GMIX, 100); x.set_param(t, P.P_GFDBK, 127); }
x.set_global(2, 127); x.set_global(6, 127);
r = run(3000); ok(!r.nan && r.peak <= 1.0, 'long run ' + JSON.stringify(r));
console.log(fails ? `${fails} failures` : 'all engine tests pass');
process.exit(fails ? 1 : 0);
