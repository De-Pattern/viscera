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
press(B.LFO); for (let i = 0; i < 4; i++) { ctl(0, 0, [0, 0, 0, 0, 0, 0, 1]); paintCheck('mod type ' + i); run(5); }
// the matrix: set a route with the knobs
press(B.LFO); for (let i = 0; i < 6; i++) press(B.LFO);
ctl(0, 0, [0, 0, 0, 5, 0, 0, 0]); ctl(0, 0, [0, 0, 0, 0, 10, 0, 0]); paintCheck('matrix');
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
  press(B.SAVE); press(B.SAVE);                // SETTINGS
  ctl(0, 0, [2, 0, 0, 0, 0, 0, 0]);            // PANEL view
  ctl(0, 0, [0, 0, 0, 0, 1, 0, 0]);            // METRO on (KNOB 3)
  ok(x.device_changed() !== g0, 'changing a panel setting marks the FM-1 settings to keep');
  paintCheck('settings panel'); }
{ // metronome: clicks while playing, with every track silent
  for (let t = 0; t < 4; t++) x.set_param(t, P.P_LEVEL, 0);
  x.set_global(10, 1); if (!x.playing()) press(12);
  let r2 = run(400, false); ok(r2.peak > 0.1, 'metronome clicks: ' + r2.peak.toFixed(2));
  press(12); for (let t = 0; t < 4; t++) x.set_param(t, P.P_LEVEL, 100); x.set_global(10, 0); }

// 7. long run with everything moving: no NaN, no runaway
for (let t = 0; t < 4; t++) { x.set_param(t, P.P_FTYPE, 4); x.set_param(t, P.P_GMIX, 100); x.set_param(t, P.P_GFDBK, 127); }
x.set_global(2, 127); x.set_global(6, 127);
r = run(3000); ok(!r.nan && r.peak <= 1.0, 'long run ' + JSON.stringify(r));
console.log(fails ? `${fails} failures` : 'all engine tests pass');
process.exit(fails ? 1 : 0);
