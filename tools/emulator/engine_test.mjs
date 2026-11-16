// Viscera 4-Track engine and UI test, run on the WebAssembly build of the same core the
// FM-1 runs: the first take, punches, DUB and REPLACE, clearing, the tape speeds, the
// loop, varispeed and pitch, the preamps, bounces (through RESON), robots and gestures,
// MIDI learn, every page and knob. Nothing may produce NaN, silence where sound is
// expected, or runaway level; the tape must give back what went on it.
import fs from 'node:fs';
const wasm = fs.readFileSync(process.argv[2] || 'site/emulator/viscera.wasm');
const {instance} = await WebAssembly.instantiate(wasm, {});
const x = instance.exports;
const mem = () => x.memory.buffer;
let fails = 0;
const ok = (c, m) => { if (!c) { fails++; console.log('FAIL', m); } };
// the parameters, in vsc.h's order
const P = {SPEED: 0, PITCH: 1, LOOP: 2, TLEVEL: 3, TAPESPD: 4, ISRC: 5, ITRIM: 6, IDRIVE: 7, IMON: 8, IREC: 9,
  BSRC: 30, BDST: 31, BRESON: 32, BDRIVE: 33, BLEVEL: 34, BCLEAR: 35,
  RTGT: 36, RPROF: 37, RMIX: 38, RDECAY: 39, RWIDTH: 40, RSHIFT: 41, RDUST: 42, RG0: 43, ROB0: 51};
const TRK = (t, k) => 10 + t * 5 + k, S = {TRIM: 0, PAN: 1, LEVEL: 2, MUTE: 3, ARM: 4};
const ROB = (r, k) => P.ROB0 + r * 5 + k, R = {TARGET: 0, RATE: 1, SHAPE: 2, SKEW: 3, DEPTH: 4};
const EV = {NOTE_ON: 0, NOTE_OFF: 1, PLAY: 2, STOP: 3, REC: 4, CLEAR: 5, ERASE: 6, BOUNCE: 7, ARM: 8, CC: 9, PANIC: 10, LOOP: 11};
ok(x.np() === 61, 'parameter count: ' + x.np());
x.init();
const inbuf = new Float32Array(mem(), x.input_buffer(), 256), usbbuf = new Float32Array(mem(), x.usb_buffer(), 256);
let fr = 0;
const music = () => { for (let i = 0; i < 128; i++, fr++) { const t = fr / 44100, tb = t % 0.5;
  const v = 0.25 * Math.sin(2 * Math.PI * 220 * t) * Math.exp(-tb * 3) + 0.25 * Math.exp(-tb * 30) * (Math.random() * 2 - 1);
  inbuf[2 * i] = inbuf[2 * i + 1] = v; } };
const silence = () => inbuf.fill(0);
let gen = music;
function run(blocks, live = 1) {
  let peak = 0, nan = false, sum = 0;
  for (let b = 0; b < blocks; b++) {
    if (live & 1) gen();
    x.render(live);
    const o = new Float32Array(mem(), x.output_buffer(), 256);
    for (let i = 0; i < 256; i++) { const v = o[i]; if (!Number.isFinite(v)) nan = true; const a = Math.abs(v); if (a > peak) peak = a; sum += v * v; }
  }
  return {peak, nan, rms: Math.sqrt(sum / (blocks * 256))};
}
const sound = (r, m, min = 1e-3) => ok(!r.nan && r.rms > min && r.peak <= 1.0, m + ' ' + JSON.stringify(r));
const quiet = (r, m) => ok(!r.nan && r.rms < 2e-3, m + ' ' + JSON.stringify(r));
const clean = (r, m) => ok(!r.nan && r.peak <= 1.0, m + ' ' + JSON.stringify(r));
let now = 0;
// each control pass is 20 ms and renders that much audio, as on the FM-1
function ctl(keys = 0, buttons = 0, e = [0, 0, 0, 0, 0, 0, 0]) { now += 20; const r = x.control(keys, buttons, ...e, now); run(7); return r; }
const B = {OCTD: 0, OCTU: 1, FX: 2, SCL: 3, ENV: 4, LFO: 5, EDIT: 6, GLO: 7, HOME: 8, SAVE: 9, ARP: 10, SEQ: 11, PLAY: 12, REC: 13};
function press(btn, held = 0) { ctl(0, held | 1 << btn); return ctl(0, held); }
function key(k, held = 0) { ctl(1 << k, held); ctl(0, held); }
const TK = [0, 2, 4, 6];   // the tracks' keys: the first four white keys
const enc = (k, d, held = 0) => { const e = [0, 0, 0, 0, 0, 0, 0]; e[k] = d; return ctl(0, held, e); };
const wait = n => { for (let i = 0; i < n; i++) ctl(); };
const screen = () => new Uint16Array(mem(), x.screen(), 240 * 240);
function paintCheck(label) { x.paint(now); const s = screen(); let lit = 0; for (let i = 0; i < s.length; i++) if (s[i] !== s[0]) lit++; ok(lit > 3000, 'screen drawn: ' + label + ' (' + lit + ')'); }
const has = () => [0, 1, 2, 3].map(t => x.has(t));

// 1. the first take: track 1 is armed; REC starts it, REC ends it and the loop plays
x.set_param(P.IMON, 0);
ok(x.get_param(TRK(0, S.ARM)) === 1, 'track 1 armed at the start');
paintCheck('tape, empty');
press(B.REC);
ok(x.transport() === 2 && x.recording() === 1, 'REC on an empty tape starts the first take');
wait(50);
press(B.REC);
ok(x.transport() === 1 && x.loop() > 10000 && x.recording() === 0, 'REC again: the loop is set and plays: ' + x.loop());
const loop1 = x.loop();
gen = silence;
sound(run(200), 'the first take plays back');
paintCheck('tape, playing');

// 2. PLAY stops (back to the start) and plays
press(B.PLAY);
ok(x.transport() === 0 && x.pos() === 0, 'PLAY stops, at the start');
quiet(run(50), 'stopped: silence');
press(B.PLAY);
ok(x.transport() === 1, 'PLAY plays');

// 3. a punch on track 2 (key 2 arms it), DUB
key(TK[1]);
ok(x.get_param(TRK(1, S.ARM)) === 1, 'key 2 arms track 2');
key(TK[0]);
ok(x.get_param(TRK(0, S.ARM)) === 0, 'key 1 disarms track 1');
gen = music;
press(B.REC); ok(x.recording() === 2, 'REC punches in the armed track');
wait(20);
press(B.REC); ok(x.recording() === 0, 'REC punches out');
ok(x.has(1) === 1, 'track 2 holds something');
gen = silence;
x.set_param(TRK(0, S.MUTE), 1);
sound(run(200), 'track 2 plays alone');
x.set_param(TRK(0, S.MUTE), 0);

// 4. the tape gives back what went on it: a sine, REPLACE, at each speed
function fidelity(spd) {
  press(B.HOME); press(B.PLAY, 1 << B.HOME);   // HOME + PLAY: erase
  ok(x.loop() === 0 && has().every(h => !h), 'HOME + PLAY erases the tape');
  x.set_param(P.TAPESPD, spd); x.set_param(P.IDRIVE, 0); x.set_param(P.IREC, 1);
  for (let t = 0; t < 4; t++) x.set_param(TRK(t, S.ARM), t === 0 ? 1 : 0);
  let ph = 0; gen = () => { for (let i = 0; i < 128; i++) { inbuf[2 * i] = inbuf[2 * i + 1] = 0.3 * Math.sin(ph); ph += 2 * Math.PI * 300 / 44100; } };
  x.event(EV.REC, 0, 0); run(300); x.event(EV.REC, 0, 0);
  gen = silence; run(10);
  const out = []; for (let b = 0; b < 120; b++) { x.render(0); const o = new Float32Array(mem(), x.output_buffer(), 256); for (let i = 0; i < 128; i++) out.push(o[2 * i]); }
  let s = 0, c = 0; const N = 12000, w = 2 * Math.PI * 300 / 44100;
  for (let i = 0; i < N; i++) { s += out[i] * Math.sin(w * i); c += out[i] * Math.cos(w * i); }
  const A = 2 * s / N, Bc = 2 * c / N; let res = 0, sig = 0;
  for (let i = 0; i < N; i++) { const m = A * Math.sin(w * i) + Bc * Math.cos(w * i); res += (out[i] - m) ** 2; sig += m * m; }
  return {amp: Math.hypot(A, Bc), snr: 10 * Math.log10(sig / res), loop: x.loop()};
}
const fid = [0, 1, 2].map(fidelity);
fid.forEach((f, s) => ok(f.amp > 0.27 && f.amp < 0.33 && f.snr > 30, 'tape speed ' + s + ' gives the sine back: ' + JSON.stringify(f)));
ok(fid[0].loop > fid[1].loop && fid[1].loop > fid[2].loop, 'faster tape: more samples for the same take');
x.set_param(P.IREC, 0); x.set_param(P.TAPESPD, 1);

// 5. a set loop length closes the first take by itself; the LOOP knob trims it after
press(B.HOME); press(B.PLAY, 1 << B.HOME);
x.set_param(P.LOOP, 20);   // 1.0 s
gen = music; press(B.REC); wait(80);
ok(x.transport() === 1 && Math.abs(x.loop() - 16000) < 300, 'LOOP 1.0 s closes the take at 1 s: ' + x.loop());
x.set_param(P.LOOP, 0);
press(B.HOME);
const lb = x.loop(); enc(4, -4); ok(x.loop() < lb, 'the LOOP knob shortens the loop: ' + lb + ' > ' + x.loop());
enc(4, 2); ok(x.loop() > lb - 4 * 800, 'and lengthens it');
const capOk = x.cap() > 40000; ok(capOk, 'tape capacity per track: ' + x.cap());

// 6. clearing: HOME + key
x.set_param(TRK(1, S.ARM), 1); x.set_param(TRK(0, S.ARM), 0);
press(B.REC); wait(10); press(B.REC);
ok(x.has(1) === 1, 'track 2 recorded');
key(TK[1], 1 << B.HOME); ctl(); ok(x.has(1) === 0 && x.has(0) === 1, 'HOME + key 2 clears track 2 only');

// 7. varispeed, pitch, the preamps
gen = silence;
x.set_param(P.SPEED, 127); run(4); const r1 = x.rate(); x.set_param(P.SPEED, 0); run(4); const r0 = x.rate();
ok(r1 > 1.9 * 16000 * 0.97 && r0 < 0.51 * 16000, 'SPEED: half to double the tape rate: ' + r0 + ' .. ' + r1);
x.set_param(P.SPEED, 64);
for (const p of [0, 6, 18, 24]) { x.set_param(P.PITCH, p); sound(run(100), 'pitch ' + (p - 12) + ' st'); }
x.set_param(P.PITCH, 12); clean(run(50), 'pitch back to 0');
x.set_param(TRK(0, S.TRIM), 127); x.set_param(TRK(0, S.LEVEL), 127); x.set_param(P.TLEVEL, 127);
const hot = run(100); clean(hot, 'a hot strip stays in range');
ok(x.sat(0) > 0, 'the strip\'s preamp lamp lights when pushed');
x.set_param(TRK(0, S.TRIM), 80); x.set_param(TRK(0, S.LEVEL), 100); x.set_param(P.TLEVEL, 100);
x.set_param(P.ITRIM, 127); x.set_param(P.IDRIVE, 127); gen = music; x.set_param(P.IMON, 1);
clean(run(100), 'the input preamp pushed, monitored'); ok(x.sat(4) > 0, 'the input lamp lights');
x.set_param(P.ITRIM, 80); x.set_param(P.IDRIVE, 30); x.set_param(P.IMON, 2);

// 8. USB as the source
x.set_param(P.ISRC, 1); gen = silence;
for (let i = 0; i < 256; i++) usbbuf[i] = 0.3 * Math.sin(i * 0.1);
x.set_param(TRK(0, S.ARM), 1);
let u = {peak: 0}; for (let b = 0; b < 50; b++) { x.render(2); } ok(x.vu(4) > 0.05, 'USB reaches the input: ' + x.vu(4));
x.set_param(P.ISRC, 2); x.set_param(TRK(0, S.ARM), 0);

// 9. bounces: 1 + 2 to a free track, through RESON; the errors
press(B.HOME); press(B.PLAY, 1 << B.HOME);
gen = music;
x.set_param(TRK(0, S.ARM), 1); press(B.REC); wait(40); press(B.REC);
x.set_param(TRK(0, S.ARM), 0); x.set_param(TRK(1, S.ARM), 1); press(B.REC); wait(40); press(B.REC);
x.set_param(TRK(1, S.ARM), 0);
ok(has().join() === '1,1,0,0', 'two tracks for the bounce: ' + has());
press(B.SEQ); paintCheck('bounce');
key(TK[2]);   // drop 3 from the sources (it is empty anyway)
ok(x.get_param(P.BSRC) === 3, 'keys pick the sources: ' + x.get_param(P.BSRC));
x.set_param(P.BRESON, 1); x.set_param(P.BCLEAR, 0);
press(B.REC);
ok((x.bounce() & 255) === 1, 'REC on BOUNCE prints: ' + x.bounce().toString(16));
paintCheck('bounce, printing');
gen = silence;
for (let i = 0; i < 80 && (x.bounce() & 255); i++) ctl();
ok((x.bounce() & 255) === 0 && x.has(2) === 1 && x.has(0) && x.has(1), 'the bounce lands on track 3, sources kept: ' + has());
for (let t = 0; t < 2; t++) x.set_param(TRK(t, S.MUTE), 1);
sound(run(200), 'the bounced track plays');
for (let t = 0; t < 2; t++) x.set_param(TRK(t, S.MUTE), 0);
x.set_param(P.BSRC, 7); x.set_param(P.BCLEAR, 1); press(B.REC);
for (let i = 0; i < 80 && (x.bounce() & 255); i++) ctl();
ok(has().join() === '0,0,0,1', 'bounce 1+2+3 to 4, sources cleared: ' + has());
x.set_param(P.BSRC, 8); x.set_param(P.BDST, 4); press(B.REC);
ok(((x.bounce() >> 8) & 255) === 2 || (x.bounce() & 255) === 0, 'a bounce onto its own source is refused');
x.set_param(P.BDST, 0); x.set_param(P.BCLEAR, 0);

// 10. RESON: each profile on a track and on the input; pings from the keys
press(B.ENV); paintCheck('reson, off');
enc(1, 1); ok(x.get_param(P.RTGT) === 1, 'ALGORITHM puts RESON on track 1');
x.set_param(P.RTGT, 4);
gen = music;
for (let p = 0; p < 6; p++) { enc(2, p === 0 ? -9 : 1); sound(run(150), 'RESON profile ' + x.get_param(P.RPROF) + ' on track 4'); }
const pings = x.reson_pings(); key(1); run(20); ok(x.reson_pings() > pings, 'a key pings RESON');
x.set_param(P.RTGT, 5); x.set_param(P.IMON, 1); sound(run(150), 'RESON on the input');
x.set_param(P.RTGT, 0); x.set_param(P.IMON, 2);
paintCheck('reson');

// 11. robots: an LFO on SPEED moves the tape's rate; a gesture recorded with HOME + turn
press(B.LFO); paintCheck('robots');
x.set_param(ROB(0, R.TARGET), 0); x.set_param(ROB(0, R.RATE), 100); x.set_param(ROB(0, R.DEPTH), 127);
const rates = new Set(); for (let i = 0; i < 40; i++) { run(10); rates.add(Math.round(x.rate())); }
ok(rates.size > 10, 'robot 1 sweeps SPEED: ' + rates.size + ' rates');
x.set_param(ROB(0, R.DEPTH), 0);
press(B.HOME);
ctl(0, 1 << B.HOME);
for (let i = 0; i < 30; i++) enc(2, i < 15 ? 2 : -2, 1 << B.HOME);
ctl(0, 0);
ok(x.get_param(ROB(0, R.SHAPE)) === 6 && x.get_param(ROB(0, R.DEPTH)) === 127 && x.get_param(P.SPEED) === 64, 'HOME + turning SPEED records a gesture on a robot, and SPEED goes back');
const gs = new Int8Array(mem(), x.gest(0), 48); ok(Math.max(...gs) > 20, 'the gesture holds the movement: ' + Math.max(...gs));
const rr = new Set(); for (let i = 0; i < 30; i++) { run(10); rr.add(Math.round(x.rate())); } ok(rr.size > 5, 'the gesture plays back');
x.set_param(ROB(0, R.DEPTH), 0);

// 12. MIDI learn: SAVE + a knob, then a CC; SAVE + REC, then a note
press(B.EDIT);
ctl(0, 1 << B.SAVE); enc(2, 1, 1 << B.SAVE); ctl(0, 0);
x.midi(0xb0, 74, 0); run(2); x.midi(0xb0, 74, 20); run(2);
ok((x.learn_entry(0) & 0xffff) === (74 << 8) && (x.learn_entry(0) >>> 24) === TRK(0, S.LEVEL), 'CC 74 learned to track 1 level: ' + x.learn_entry(0).toString(16));
x.midi(0xb0, 74, 30); run(2); x.midi(0xb0, 74, 100); run(2);
ok(x.get_param(TRK(0, S.LEVEL)) === 100, 'the learned CC moves it (after pickup): ' + x.get_param(TRK(0, S.LEVEL)));
ctl(0, 1 << B.SAVE); ctl(0, 1 << B.SAVE | 1 << B.REC); ctl(0, 0);
x.midi(0x90, 36, 100); run(2);
const tr0 = x.transport(), rec0 = x.recording();
x.set_param(TRK(3, S.ARM), 1);
x.midi(0x90, 36, 100); run(2);
ok(x.recording() !== rec0 || x.transport() !== tr0, 'a learned note works REC');
x.midi(0x90, 36, 100); run(2);
x.set_param(TRK(3, S.ARM), 0);

// 13. every page and view drawn, every knob both ways, nothing breaks
gen = music;
for (const b of [B.HOME, B.EDIT, B.FX, B.SEQ, B.ENV, B.LFO, B.SAVE, B.GLO]) {
  press(b); paintCheck('page ' + b);
  for (let v = 0; v < 3; v++) {
    for (let k = 2; k < 6; k++) { enc(k, 5); enc(k, -9); enc(k, 4); }
    enc(6, 1); enc(6, -1);
    paintCheck('page ' + b + ' view ' + v);
    enc(0, 1);
  }
  enc(0, -5);
}
clean(run(100), 'after every knob');

// 14. a project seals and checks; the device settings too
x.project_seal(0); ok(x.project_check() === 1, 'a sealed project checks');
x.device_seal(); ok(x.device_check() === 1, 'the device settings check');

// 15. long run with everything moving: no NaN, in range
x.set_param(P.RTGT, 2); x.set_param(P.PITCH, 19); x.set_param(ROB(1, R.TARGET), 1); x.set_param(ROB(1, R.DEPTH), 90);
x.set_param(TRK(0, S.ARM), 1); press(B.HOME); press(B.REC);
clean(run(2000), 'two minutes... (46 s) of everything');
press(B.REC);
if (fails) { console.log(fails + ' failures'); process.exit(1); }
console.log('engine test passed');
