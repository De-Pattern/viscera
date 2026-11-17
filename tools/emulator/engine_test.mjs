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
  BSRC: 42, BDST: 43, BRESON: 44, BDRIVE: 45, BLEVEL: 46, BCLEAR: 47,
  RTGT: 48, RPROF: 49, RMIX: 50, RDECAY: 51, RWIDTH: 52, RSHIFT: 53, RDUST: 54, RG0: 55, EQ0: 63,
  CTHR: 79, CRATIO: 80, CATK: 81, CREL: 82, CMAKE: 83, CMIX: 84, CKEY: 85, CON: 86, SLSRC: 87, SLN: 88, SLSNAP: 89, SLDST: 90, ROB0: 91};
const TRK = (t, k) => 10 + t * 8 + k, S = {TRIM: 0, PAN: 1, LEVEL: 2, MUTE: 3, ARM: 4, SPEED: 5, PITCH: 6, DUB: 7};
const EQ = (b, k) => P.EQ0 + b * 4 + k;
const ROB = (r, k) => P.ROB0 + r * 5 + k, R = {TARGET: 0, RATE: 1, SHAPE: 2, SKEW: 3, DEPTH: 4};
const EV = {NOTE_ON: 0, NOTE_OFF: 1, PLAY: 2, STOP: 3, REC: 4, CLEAR: 5, ERASE: 6, BOUNCE: 7, ARM: 8, CC: 9, PANIC: 10, LOOP: 11};
ok(x.np() === 101, 'parameter count: ' + x.np());
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

// 0. the intro: music, title cards, then any button skips it
x.intro(now); gen = silence;
{ const r = run(150); ok(!r.nan && r.rms > 0.01 && r.peak <= 1, 'the intro plays music ' + JSON.stringify(r)); }
now += 400; x.paint(now); wait(15); { const s = screen(); let lit = 0; for (let i = 0; i < s.length; i++) if (s[i]) lit++; ok(lit > 200, 'the intro draws a card'); }
press(B.PLAY); ok(x.transport() === 0, 'a button skips the intro and does nothing else');
quiet(run(50), 'the intro\'s music stops when skipped');
gen = music;
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
ok(x.get_param(TRK(0, S.ARM)) === 0, 'arming track 2 disarms track 1');
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
  press(B.HOME); press(B.PLAY, 1 << B.ARP);   // ARP (CLEAR) + PLAY: erase
  ok(x.loop() === 0 && has().every(h => !h), 'ARP + PLAY erases the tape');
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
press(B.HOME); press(B.PLAY, 1 << B.ARP);
x.set_param(P.LOOP, 20);   // 1.0 s
gen = music; press(B.REC); wait(80);
ok(x.transport() === 1 && Math.abs(x.loop() - 16000) < 300, 'LOOP 1.0 s closes the take at 1 s: ' + x.loop());
x.set_param(P.LOOP, 0);
press(B.HOME); enc(0, -5);
const lb = x.loop(); enc(4, -4); ok(x.loop() < lb, 'the LOOP knob shortens the loop: ' + lb + ' > ' + x.loop());
enc(4, 2); ok(x.loop() > lb - 4 * 800, 'and lengthens it');
// the LOOP view: START, END, SLIDE, FULL
enc(0, 1); paintCheck('tape, loop view');
enc(2, 20); ok(x.lstart() > 2500 && x.lstart() < 3500, 'START moves in hundredths: ' + x.lstart());
sound(run(100), 'the trimmed loop plays');
const l0 = x.lstart(), e0 = x.lend(); enc(4, 10); ok(x.lstart() === l0 + 1600 && x.lend() === e0 + 1600, 'SLIDE moves both: ' + x.lstart() + ' ' + x.lend());
enc(3, -2); ok(x.lend() === e0 + 1600 - 1600, 'END moves in twentieths');
enc(5, 1); ok(x.lstart() === 0 && x.lend() === x.take(), 'FULL: the whole take again: ' + x.lstart() + '..' + x.lend() + ' of ' + x.take());
enc(0, -5);
const capOk = x.cap() > 40000; ok(capOk, 'tape capacity per track: ' + x.cap());

// 6. clearing: HOME + key
x.set_param(TRK(1, S.ARM), 1); x.set_param(TRK(0, S.ARM), 0);
press(B.REC); wait(10); press(B.REC);
ok(x.has(1) === 1, 'track 2 recorded');
key(TK[1], 1 << B.ARP); ctl(); ok(x.has(1) === 0 && x.has(0) === 1, 'ARP + key 2 clears track 2 only');
key(TK[2], 1 << B.HOME); ctl(); ok(x.get_param(TRK(2, S.ARM)) === 0, 'HOME + key 3 selects track 3 without arming it');
enc(2, 20); ok(x.get_param(TRK(2, S.SPEED)) > 64, 'HOME, TRACK view: KNOB 1 is the selected track\'s SPEED');
x.set_param(TRK(2, S.SPEED), 64); press(B.HOME);

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
press(B.HOME); press(B.PLAY, 1 << B.ARP);
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

// 12b. each track at its own speed; DUB level; the master EQ and compressor
press(B.HOME); press(B.PLAY, 1 << B.ARP);
gen = music; x.set_param(TRK(0, S.ARM), 1); x.set_param(TRK(3, S.ARM), 0); press(B.REC); wait(40); press(B.REC);
x.set_param(TRK(0, S.ARM), 0); x.set_param(TRK(1, S.ARM), 1); press(B.REC); wait(40); press(B.REC); x.set_param(TRK(1, S.ARM), 0);
gen = silence;
x.set_param(TRK(1, S.SPEED), 127); run(200);
const tp0 = x.loop(); // (positions: through the meter's tpos via the screen is not exported; check the sound instead)
sound(run(200), 'track 2 at double speed plays');
x.set_param(TRK(1, S.SPEED), 0); sound(run(200), 'track 2 at half speed plays');
x.set_param(TRK(1, S.SPEED), 64);
x.set_param(TRK(0, S.PITCH), 24); x.set_param(TRK(1, S.PITCH), 0); sound(run(200), 'per-track pitch, up and down');
x.set_param(TRK(0, S.PITCH), 12); x.set_param(TRK(1, S.PITCH), 12);
// DUB 0: a DUB pass keeps nothing of what was there (track 2 punched with silence goes quiet)
x.set_param(TRK(1, S.DUB), 0); x.set_param(TRK(1, S.ARM), 1); x.set_param(TRK(0, S.MUTE), 1);
press(B.PLAY); press(B.PLAY); press(B.REC); run(Math.ceil(tp0 / 16000 * 44100 / 128) + 40); press(B.REC);
x.set_param(TRK(1, S.ARM), 0);
const dub0 = run(300); ok(dub0.rms < 0.01, 'DUB at 0 with silence in: the track is wiped ' + JSON.stringify(dub0));
x.set_param(TRK(1, S.DUB), 127); x.set_param(TRK(0, S.MUTE), 0);
// the EQ: a low cut at the top of the range thins the sound; a +15 dB bell louder
const flat = run(200);
x.set_param(EQ(0, 3), 2); x.set_param(EQ(0, 0), 110); const cut = run(200);
ok(cut.rms < flat.rms * 0.7, 'EQ band 1 LO CUT high up takes the body out: ' + flat.rms.toFixed(4) + ' > ' + cut.rms.toFixed(4));
x.set_param(EQ(0, 3), 0); x.set_param(EQ(0, 0), 30);
x.set_param(EQ(1, 1), 127); x.set_param(EQ(1, 0), 30); clean(run(200), 'EQ +15 dB stays in range');
x.set_param(EQ(1, 1), 64);
for (let b = 0; b < 4; b++) for (const ty of [0, 1, 2]) { x.set_param(EQ(b, 3), ty); x.set_param(EQ(b, 1), 100); x.set_param(EQ(b, 2), 127); clean(run(20), 'EQ band ' + b + ' type ' + ty); }
for (let b = 0; b < 4; b++) { x.set_param(EQ(b, 3), 0); x.set_param(EQ(b, 1), 64); x.set_param(EQ(b, 2), 31); }
// the compressor: on, a low threshold, 10:1: the level drops
x.set_param(TRK(0, S.TRIM), 110); x.set_param(TRK(1, S.TRIM), 110);
const open_ = run(300);
x.set_param(P.CON, 1); x.set_param(P.CTHR, 0); x.set_param(P.CRATIO, 2); x.set_param(P.CATK, 0);
const comp = run(300);
ok(comp.rms < open_.rms * 0.8, 'the compressor pulls the level down: ' + open_.rms.toFixed(4) + ' > ' + comp.rms.toFixed(4));
x.set_param(P.CKEY, 1); clean(run(200), 'keyed by track 1');
x.set_param(P.CKEY, 5); x.set_param(P.IMON, 0); gen = music; { const ducked = run(300); gen = silence; const free = run(300);
  ok(ducked.rms < free.rms * 0.85, 'KEY INPUT: the input ducks the tape (not heard): ' + ducked.rms.toFixed(4) + ' < ' + free.rms.toFixed(4)); }
x.set_param(P.IMON, 2);
press(B.FX); paintCheck('master, EQ'); for (let v = 0; v < 6; v++) { paintCheck('master view ' + v); enc(0, 1); }
x.set_param(P.CON, 0); x.set_param(P.CKEY, 0); x.set_param(TRK(0, S.TRIM), 80); x.set_param(TRK(1, S.TRIM), 80);

// 12c. MOTION (SEL): REC, then a knob turned over the loop plays back each pass
press(B.HOME); press(B.PLAY, 1 << B.ARP);
gen = music; x.set_param(TRK(0, S.ARM), 1); press(B.REC); wait(60); press(B.REC); x.set_param(TRK(0, S.ARM), 0);
gen = silence;
press(B.SCL); paintCheck('motion');
press(B.REC); ok(x.transport() === 1, 'MOTION REC plays the tape');
for (let i = 0; i < 40; i++) enc(2, i < 20 ? 3 : -3);
press(B.REC);
x.set_param(P.SPEED, 64);
{ const seen = new Set(); for (let i = 0; i < 60; i++) { run(5); seen.add(Math.round(x.rate())); }
  ok(seen.size > 8, 'the recorded SPEED motion plays back over the loop: ' + seen.size + ' speeds'); }
ctl(0, 1 << B.ARP); enc(2, 1, 1 << B.ARP); ctl(0, 0);
{ const seen = new Set(); for (let i = 0; i < 30; i++) { run(5); seen.add(Math.round(x.rate())); } ok(seen.size <= 2, 'ARP + the knob erases its motion: ' + seen.size); }
// 12d. SLICE (SEL, SEL, SEL): track 1 cut on the white keys; REC records them to a free track
press(B.SCL); press(B.SCL); paintCheck('slice');
x.set_param(P.SLSRC, 0); x.set_param(P.SLN, 0); ctl();
quiet(run(60), 'the sliced track is muted on its page (nothing played)');
key(0); sound(run(30), 'white key 1 plays slice 1');
key(4); key(7); sound(run(30), 'more slices');
press(B.REC); ok((x.recording() & 2) === 2, 'REC on SLICE records into the free track 2: ' + x.recording());
for (let i = 0; i < 12; i++) { key([0, 2, 4, 6][i % 4]); wait(3); }
wait(260);   // (laps go by with no slices played before REC keeps it: they mustn't wipe the take)
press(B.REC); ok(x.has(1) === 1 && (x.recording() & 2) === 0, 'the take is on track 2');
{ const lv = x.get_param(TRK(0, S.LEVEL)); x.set_param(TRK(0, S.LEVEL), 0);
  sound(run(200), 'the take keeps the slices, though laps went by after them (track 1 down)');
  x.set_param(TRK(0, S.LEVEL), lv); }
press(B.REC, 1 << B.ARP); ctl(); ok(x.has(1) === 0, 'ARP + REC clears the take to start over');
press(B.HOME); ctl();
sound(run(100), 'leaving SLICE: track 1 plays again');

// 12e. the mixer's keys: F3-B3 mute, C4-F4 solo
press(B.EDIT); enc(0, -5);
gen = silence; x.set_param(TRK(0, S.MUTE), 0);
key(0); ok(x.get_param(TRK(0, S.MUTE)) === 1, 'on the mixer, F3 mutes track 1'); quiet(run(60), 'muted: track 1 silent');
key(0); ok(x.get_param(TRK(0, S.ARM)) === 0, 'and doesn\'t arm it');
key(7); sound(run(60), 'C4 solos track 1: it plays');
key(7); key(9); quiet(run(60), 'D4 solos (empty) track 2 alone: track 1 is silent');
key(9); sound(run(60), 'solo off: track 1 plays');
// 13. every page and view drawn, every knob both ways, nothing breaks
gen = music;
for (const b of [B.HOME, B.EDIT, B.FX, B.SCL, B.SEQ, B.ENV, B.LFO, B.SAVE, B.GLO]) {
  press(b); paintCheck('page ' + b);
  for (let v = 0; v < 6; v++) {
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

// 14b. the LFO matrix: hold LFO and turn a knob on any page; the matrix view edits the rows
{
  const rt = () => new Int8Array(mem(), x.route(), 32);
  for (let i = 0; i < 32; i++) rt()[i] = 0;
  x.set_param(ROB(0, R.SHAPE), 3); x.set_param(ROB(0, R.RATE), 127); x.set_param(ROB(0, R.DEPTH), 0); x.set_param(ROB(0, R.SKEW), 64);
  press(B.EDIT); enc(0, -5);                       // the mixer, LEVEL: knob 2 is track 2's level
  const lv = x.get_param(TRK(1, S.LEVEL));
  ctl(0, 1 << B.LFO); enc(3, 5, 1 << B.LFO); ctl(0, 0);
  ok(x.get_param(TRK(1, S.LEVEL)) === lv, 'LFO+TURN leaves the knob where it was');
  ok(rt()[0] === 0 && rt()[1] === TRK(1, S.LEVEL) && rt()[2] === 20, 'LFO+TURN routes LFO 1 to T2 LEVEL by the turn: ' + [...rt().slice(0, 4)]);
  // and it moves: the value in use swings either side of the knob
  x.set_param(TRK(1, S.LEVEL), 60);
  let lo = 999, hi = -1; for (let b = 0; b < 400; b++) { run(1); const v = x.param_live(TRK(1, S.LEVEL)); lo = Math.min(lo, v); hi = Math.max(hi, v); }
  ok(lo < 55 && hi > 65, 'the route moves T2 LEVEL ' + lo.toFixed(1) + '..' + hi.toFixed(1));
  // the release opened nothing (the button was used); a tap opens ROBOTS
  press(B.LFO); enc(0, 2); paintCheck('LFO matrix');
  enc(6, 1); enc(2, 1); ok(rt()[4] === 0 && rt()[6] === 32, 'an empty row starts from FROM: ' + [...rt().slice(4, 8)]);
  enc(2, 1); ok(rt()[4] === 1, 'FROM: LFO 2');
  enc(3, 1); ok(rt()[5] !== TRK(0, S.LEVEL) && rt()[5] < 101, 'TO steps to the next knob an LFO can move: ' + rt()[5]);
  enc(4, -40); ok(rt()[6] === -8, 'DEPTH is bipolar: ' + rt()[6]);
  enc(4, 8); ok(rt()[6] === 1, 'DEPTH skips 0 (0 is an empty row): ' + rt()[6]);
  enc(5, 1); ok(rt()[6] === 0, 'DELETE empties the row');
  // a knob an LFO can't move: no row
  press(B.FX); ctl(0, 1 << B.LFO); enc(2, 3, 1 << B.LFO); ctl(0, 0);
  ok(rt()[6] === 0 && rt()[10] === 0, 'the EQ (worked out on change) is not a route target');
  // 2.4's projects (format 10, no matrix) load with an empty one
  x.project_seal(0);
  const hv = new Uint32Array(mem(), x.project(), 4), bytes = x.project_bytes();
  const old = bytes - 32; rt()[0] = 1; rt()[2] = 50;
  const u8 = new Uint8Array(mem(), x.project(), bytes);
  let c = 0xffffffff; for (let i = 16; i < old; i++) { c ^= u8[i]; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1)); }
  hv[1] = 10; hv[2] = old; hv[3] = (~c) >>> 0;
  ok(x.project_check() === 1, 'a format 10 project checks (upgraded)');
  ok(hv[1] === 11 && hv[2] === bytes && rt()[2] === 0, 'and gets this format and an empty matrix');
  for (let i = 0; i < 32; i++) rt()[i] = 0;
  x.set_param(ROB(0, R.SHAPE), 0); x.set_param(ROB(0, R.RATE), 40);
}

// 15. long run with everything moving: no NaN, in range
x.set_param(P.RTGT, 2); x.set_param(P.PITCH, 19); x.set_param(ROB(1, R.TARGET), 1); x.set_param(ROB(1, R.DEPTH), 90);
x.set_param(TRK(0, S.ARM), 1); press(B.HOME); press(B.REC);
clean(run(2000), 'two minutes... (46 s) of everything');
press(B.REC);
if (fails) { console.log(fails + ' failures'); process.exit(1); }
console.log('engine test passed');
