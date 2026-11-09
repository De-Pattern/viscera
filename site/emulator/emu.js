/* Viscera emulator: the page. Builds the panel, turns pointer and keyboard
 * input into FM-1 control snapshots for the audio thread, draws the screen it
 * sends back, and handles the sample bank, live input and saved projects. */
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const BUTTONS = ['OCT−', 'OCT+', 'FX', 'SEL', 'ENV', 'LFO', 'EDIT', 'GLO', 'HOME', 'SAVE', 'ARP', 'SEQ', 'PLAY', 'REC'];
  const NOTE_NAMES = ['F', 'F#', 'G', 'G#', 'A', 'A#', 'B', 'C', 'C#', 'D', 'D#', 'E'];
  /* computer keys for the 27 keys, F3..G5 */
  /* naturals on Z X C V … / and Q W E R T Y, sharps on the keys just above them */
  const KEYMAP = (() => {
    const w = 'zxcvbnm,./qwerty'.split(''), b = ['s', 'd', 'f', 'h', 'j', 'l', ';', "'", '3', '4', '6'];
    const WH = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26], BL = [1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25];
    const m = []; WH.forEach((k, i) => m[k] = w[i]); BL.forEach((k, i) => m[k] = b[i]); return m;
  })();
  const state = {keys: 0, buttons: 0, enc: [0, 0, 0, 0, 0, 0, 0]};
  let ctx = null, node = null, outGain = null, source = null, inputKind = 'none', pending = false;
  const note = t => { $('note').textContent = t; };

  /* ---------- panel: the FM-1's own layout ---------- */
  /* encoder indices as the FM-1 reports them: 0 SELECT, 1 ALGORITHM, 2-5 KNOB 1-4, 6 PRESETS */
  const ENC_FN = ['page', 'track', 'param 1', 'param 2', 'param 3', 'param 4', 'device / list'];
  const BTN_FN = {FX: 'grain', SEL: 'filter', ENV: 'colour', LFO: 'mods', EDIT: 'material', GLO: 'space',
    HOME: 'mix', SAVE: 'project', ARP: 'scenes', SEQ: 'steps', REC: 'record'};
  const ROW1 = ['FX', 'SEL', 'ENV', 'LFO', 'EDIT', 'GLO'], ROW2 = ['HOME', 'SAVE', 'ARP', 'SEQ', 'PLAY', 'REC'];
  let master = 0.8;
  function makeEnc(role, parent, label, fn) {
    const el = document.createElement('div');
    el.className = 'enc'; el.tabIndex = 0;
    el.setAttribute('role', 'slider'); el.setAttribute('aria-label', label + (fn ? ' (' + fn + ')' : ''));
    el.innerHTML = '<div class="lbl">' + label + '</div><div class="cap"></div><div class="fn">' + (fn || '&nbsp;') + '</div>';
    const cap = el.children[1];
    let angle = 0;
    el.turn = d => { state.enc[role] += d; angle += d * 15; cap.style.transform = `rotate(${angle}deg)`; send(); };
    /* PRESETS steps through rows and tracks: a firmer turn per click, like Ravel's selectors */
    const dragPx = role === 6 ? 18 : 6, wheelPx = role === 6 ? 80 : 30;
    let y0 = null, acc = 0, wacc = 0;
    el.addEventListener('pointerdown', e => { y0 = e.clientY; acc = 0; el.setPointerCapture(e.pointerId); e.preventDefault(); el.focus({preventScroll: true}); });
    el.addEventListener('pointermove', e => {
      if (y0 === null) return;
      acc += y0 - e.clientY; y0 = e.clientY;
      const d = Math.trunc(acc / dragPx); if (d) { acc -= d * dragPx; el.turn(d); }
    });
    const end = () => { y0 = null; };
    el.addEventListener('pointerup', end); el.addEventListener('pointercancel', end);
    el.addEventListener('wheel', e => {
      e.preventDefault();
      wacc -= e.deltaMode === 1 ? e.deltaY * 30 : e.deltaMode === 2 ? e.deltaY * 400 : e.deltaY;
      const d = Math.trunc(wacc / wheelPx); if (d) { wacc -= d * wheelPx; el.turn(Math.max(-3, Math.min(3, d))); }
    }, {passive: false});
    el.addEventListener('keydown', e => {
      if (e.key === 'ArrowUp' || e.key === 'ArrowRight') { el.turn(1); e.preventDefault(); }
      if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') { el.turn(-1); e.preventDefault(); }
    });
    parent.appendChild(el);
    return el;
  }
  const lh = $('lefthand');
  {
    /* MASTER is the FM-1's analogue volume, not an encoder */
    const m = makeEnc(0, lh, 'MASTER', 'volume'), cap = m.children[1];
    const show = () => { cap.style.transform = `rotate(${(master - 0.5) * 270}deg)`; if (outGain) outGain.gain.value = master * master * 1.4; };
    m.turn = d => { master = Math.max(0, Math.min(1, master + d * 0.03)); show(); };
    show();
  }
  makeEnc(0, lh, 'SELECT', ENC_FN[0]);
  makeEnc(6, lh, 'PRESETS', ENC_FN[6]);
  makeEnc(1, lh, 'ALGORITHM', ENC_FN[1]);
  for (let i = 0; i < 4; i++) makeEnc(2 + i, $('knobs4'), 'KNOB ' + (i + 1), ENC_FN[2 + i]);

  function makeBtn(name, parent, extra) {
    const i = BUTTONS.indexOf(name), b = document.createElement('button');
    b.className = 'btn' + (extra ? ' ' + extra : '');
    b.innerHTML = '<span class="l">' + (name === 'PLAY' ? 'PLAY/STOP' : name) + '</span>' + (BTN_FN[name] ? '<span class="f">' + BTN_FN[name] + '</span>' : '');
    b.setAttribute('aria-label', name.replace('−', '-'));
    /* right-click latches the button held (for hold gestures: LFO + knob, ARP + pad, SAVE + knob);
     * a second right-click lets it go. A latched button ignores left clicks. */
    let latched = false;
    const down = () => { state.buttons |= 1 << i; b.classList.add('down'); send(); };
    const up = () => { if (latched) return; state.buttons &= ~(1 << i); b.classList.remove('down'); send(); };
    b.addEventListener('pointerdown', e => {
      e.preventDefault();
      if (e.button === 2) {
        latched = !latched;
        b.classList.toggle('latched', latched);
        b.setAttribute('aria-pressed', String(latched));
        if (latched) down(); else { state.buttons &= ~(1 << i); b.classList.remove('down'); send(); }
        return;
      }
      if (e.button !== 0 || latched) return;
      b.setPointerCapture(e.pointerId); down();
    });
    b.addEventListener('pointerup', e => { if (e.button === 0) up(); });
    b.addEventListener('pointercancel', up);
    b.addEventListener('contextmenu', e => e.preventDefault());
    b.addEventListener('keydown', e => { if ((e.key === 'Enter' || e.key === ' ') && !e.repeat) { e.preventDefault(); e.stopPropagation(); down(); } });
    b.addEventListener('keyup', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); e.stopPropagation(); up(); } });
    parent.appendChild(b);
  }
  const octTray = document.createElement('div'); octTray.className = 'tray'; lh.appendChild(octTray);
  makeBtn('OCT−', octTray, 'oct');
  makeBtn('OCT+', octTray, 'oct');
  ROW1.forEach(n => makeBtn(n, $('btnrow1')));
  ROW2.forEach(n => makeBtn(n, $('btnrow2')));

  /* 27 pads from F3: sharps on the row above, offset half a pad, with gaps as on the FM-1 */
  const WHITE = [0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26], BLACK = [1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25];
  const BLACK_SLOTS = [1, 2, 3, 5, 6, 8, 9, 10, 12, 13, 15];
  const keyEls = new Array(27);
  function makeKey(k, parent, cls) {
    const el = document.createElement('div');
    el.className = cls;
    if (k < 0) { parent.appendChild(el); return; }
    el.innerHTML = '<span>' + NOTE_NAMES[k % 12] + (3 + Math.floor((k + 5) / 12)) + '<i>' + (KEYMAP[k] || '').toUpperCase() + '</i></span>';
    const down = () => { state.keys |= 1 << k; el.classList.add('down'); send(); };
    const up = () => { state.keys &= ~(1 << k); el.classList.remove('down'); send(); };
    el.addEventListener('pointerdown', e => { el.releasePointerCapture?.(e.pointerId); e.preventDefault(); down(); });
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
    el.addEventListener('pointerenter', e => { if (e.buttons & 1) down(); });
    el.addEventListener('pointerleave', e => { if (state.keys & (1 << k)) up(); });
    parent.appendChild(el); keyEls[k] = el;
  }
  {
    let bi = 0;
    for (let s = 0; s < 16; s++) makeKey(BLACK_SLOTS.includes(s) ? BLACK[bi++] : -1, $('blackrow'), BLACK_SLOTS.includes(s) ? 'key b' : 'key b gap');
    WHITE.forEach(k => makeKey(k, $('whiterow'), 'key'));
  }
  addEventListener('keydown', e => {
    if (e.target.closest('input,select,textarea,button,.enc')) return;
    if (e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
    if (e.key === ' ') { e.preventDefault(); state.buttons |= 1 << 12; send(); return; }
    const k = KEYMAP.indexOf(e.key.toLowerCase());
    if (k >= 0) { state.keys |= 1 << k; keyEls[k].classList.add('down'); send(); }
  });
  addEventListener('keyup', e => {
    if (e.key === ' ') { state.buttons &= ~(1 << 12); send(); return; }
    const k = KEYMAP.indexOf(e.key.toLowerCase());
    if (k >= 0) { state.keys &= ~(1 << k); keyEls[k].classList.remove('down'); send(); }
  });

  /* ---------- talking to the audio thread ---------- */
  function send() {
    if (!node) return;
    node.port.postMessage({type: 'control', keys: state.keys, buttons: state.buttons, enc: state.enc.slice()});
    state.enc.fill(0);
  }
  const lcd = $('lcd').getContext('2d'), img = lcd.createImageData(240, 240);
  function draw(px) {
    const d = img.data;
    for (let i = 0, j = 0; i < px.length; i++, j += 4) {
      const c = px[i];
      d[j] = (c >> 8) & 0xf8; d[j + 1] = (c >> 3) & 0xfc; d[j + 2] = (c << 3) & 0xf8; d[j + 3] = 255;
    }
    lcd.putImageData(img, 0, 0);
  }

  /* ---------- projects in this browser ---------- */
  const slotKey = s => 'viscera-project-' + s;
  function slotList() {
    const out = [];
    for (let s = 0; s < 8; s++) {
      let v = null;
      try { v = localStorage.getItem(slotKey(s)); } catch {}
      out.push([s, v ? 1 : 0, v ? 'PROJECT ' + (s + 1) : '']);
    }
    return out;
  }
  function hostStatus() { node && node.port.postMessage({type: 'host-status', input: inputKind !== 'none', slots: slotList()}); }
  const toB64 = b => { let s = ''; for (let i = 0; i < b.length; i += 0x8000) s += String.fromCharCode.apply(null, b.subarray(i, i + 0x8000)); return btoa(s); };
  const fromB64 = s => Uint8Array.from(atob(s), c => c.charCodeAt(0));

  /* ---------- the sample bank ---------- */
  const SR = 32000, slotNames = new Array(16).fill('');
  const slotsEl = $('slots');
  for (let s = 0; s < 16; s++) {
    const b = document.createElement('button');
    b.className = 'slot'; b.dataset.slot = s; b.textContent = 'S' + (s + 1);
    b.addEventListener('click', () => { $('slot-pick').dataset.slot = s; $('slot-pick').click(); });
    b.addEventListener('dragover', e => { e.preventDefault(); b.classList.add('over'); });
    b.addEventListener('dragleave', () => b.classList.remove('over'));
    b.addEventListener('drop', e => { e.preventDefault(); b.classList.remove('over'); const f = e.dataTransfer.files[0]; if (f) loadFile(s, f); });
    slotsEl.appendChild(b);
  }
  $('slot-pick').addEventListener('change', e => { const f = e.target.files[0]; if (f) loadFile(+e.target.dataset.slot, f); e.target.value = ''; });
  function showSlot(s, name) {
    slotNames[s] = name;
    const b = slotsEl.children[s];
    b.textContent = 'S' + (s + 1) + (name ? ' ' + name : '');
    b.classList.toggle('full', !!name);
  }
  function toBank(s, name, f32) {
    const n = Math.min(f32.length, SR * 12), pcm = new Int16Array(n);
    let peak = 0; for (let i = 0; i < n; i++) peak = Math.max(peak, Math.abs(f32[i]));
    const g = peak > 0 ? 0.9 / peak : 1;
    for (let i = 0; i < n; i++) pcm[i] = Math.round(Math.max(-1, Math.min(1, f32[i] * g)) * 32767);
    node.port.postMessage({type: 'bank', slot: s, name: name.toUpperCase().slice(0, 15), pcm}, [pcm.buffer]);
    showSlot(s, name.toUpperCase().slice(0, 15));
  }
  async function loadFile(s, file) {
    if (!ctx) { note('Switch the FM-1 on first.'); return; }
    try {
      const buf = await ctx.decodeAudioData(await file.arrayBuffer());
      /* mono, resampled to the bank's 32 kHz, like M-VAVE's workbench does */
      const n = Math.min(Math.ceil(buf.duration * SR), SR * 12);
      const off = new OfflineAudioContext(1, n, SR);
      const src = off.createBufferSource(); src.buffer = buf; src.connect(off.destination); src.start();
      const out = await off.startRendering();
      toBank(s, file.name.replace(/\.[^.]+$/, ''), out.getChannelData(0));
      note('Loaded ' + file.name + ' into S' + (s + 1) + (buf.duration > 12 ? ' (first 12 s)' : '') + '.');
    } catch (e) { note('That file could not be decoded.'); }
  }
  /* sounds made here, so PLAY and KEYS have something to work with */
  function rnd(seed) { return () => { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 4294967296; }; }
  function made() {
    const r = rnd(9), len = s => new Float32Array(Math.round(SR * s));
    const rain = len(5); { let lp = 0;
      for (let i = 0; i < rain.length; i++) { lp += (r() * 2 - 1 - lp) * 0.08; rain[i] = lp * 0.4; }
      for (let k = 0; k < 140; k++) { const at = Math.floor(r() * (rain.length - 4000)), f = 1800 + r() * 3200, a = 0.2 + r() * 0.6;
        for (let i = 0; i < 3800; i++) rain[at + i] += a * Math.sin(2 * Math.PI * f * i / SR) * Math.exp(-i / (180 + r() * 40)); } }
    const glass = len(4); { const parts = [1, 2.76, 5.4, 8.93, 13.3];
      for (let i = 0; i < glass.length; i++) { let v = 0; parts.forEach((p, j) => v += Math.sin(2 * Math.PI * 523.25 * p * i / SR) * Math.exp(-i / (SR * (1.6 / (j + 1)))) / (j + 1)); glass[i] = v; } }
    const hum = len(5); for (let i = 0; i < hum.length; i++) { const t = i / SR;
      hum[i] = Math.sin(2 * Math.PI * 55 * t) * 0.5 + Math.sin(2 * Math.PI * 110.4 * t) * 0.3 + Math.sin(2 * Math.PI * 165.7 * t) * 0.2 * (0.5 + 0.5 * Math.sin(t * 1.3)) + (r() * 2 - 1) * 0.02; }
    const breath = len(4); { let a = 0, b = 0;
      for (let i = 0; i < breath.length; i++) { const t = i / SR, env = Math.sin(Math.PI * Math.min(1, t / 4)) ** 2;
        const f = 0.02 + 0.05 * (0.5 + 0.5 * Math.sin(t * 2.1)); a += (r() * 2 - 1 - a) * f; b += (a - b) * f; breath[i] = (a - b) * 6 * env; } }
    const ticks = len(3); for (let k = 0; k < 24; k++) { const at = Math.floor(k * ticks.length / 24 + (k % 3) * 400);
      for (let i = 0; i < 600 && at + i < ticks.length; i++) ticks[at + i] += (r() * 2 - 1) * Math.exp(-i / 40) * (k % 4 ? 0.5 : 1); }
    [['RAIN ON TIN', rain], ['GLASS', glass], ['HUM', hum], ['BREATH', breath], ['TICKS', ticks]].forEach(([n, d], s) => toBank(s, n, d));
  }

  /* ---------- live input ---------- */
  function setInput(kind) {
    ['none', 'file', 'mic'].forEach(k => $('in-' + k).setAttribute('aria-pressed', String(k === kind)));
    inputKind = kind; hostStatus();
  }
  async function useSource(make, label) {
    if (!ctx) { note('Switch the FM-1 on first.'); return; }
    try {
      const s = await make();
      if (source) { try { source.disconnect(); } catch {} if (source.mediaStream) source.mediaStream.getTracks().forEach(t => t.stop()); }
      source = s; if (s) s.connect(node);
      $('in-name').textContent = label;
    } catch { note('That input is not available here.'); setInput('none'); }
  }
  $('in-none').addEventListener('click', () => { setInput('none'); useSource(async () => null, 'No live input.'); });
  $('in-mic').addEventListener('click', () => { setInput('mic'); useSource(async () => ctx.createMediaStreamSource(await navigator.mediaDevices.getUserMedia({audio: {echoCancellation: false, noiseSuppression: false}})), 'Microphone. T2 (LOOP) and T4 (THRU) listen to it.'); });
  $('in-file').addEventListener('click', () => $('in-file-pick').click());
  $('in-file-pick').addEventListener('change', e => {
    const f = e.target.files[0]; e.target.value = '';
    if (!f) return;
    setInput('file');
    useSource(async () => {
      const a = new Audio(URL.createObjectURL(f)); a.loop = true; await a.play();
      return ctx.createMediaElementSource(a);
    }, f.name + ', looping. T2 (LOOP) and T4 (THRU) listen to it.');
  });

  /* ---------- power ---------- */
  async function powerOn() {
    $('power-on').disabled = true;
    try {
      ctx = new AudioContext({sampleRate: 44100, latencyHint: 'interactive'});
      const module = await WebAssembly.compileStreaming(fetch('viscera.wasm'));
      await ctx.audioWorklet.addModule('worklet.js');
      node = new AudioWorkletNode(ctx, 'viscera', {numberOfInputs: 1, numberOfOutputs: 1, outputChannelCount: [2], processorOptions: {module}});
      outGain = ctx.createGain(); outGain.gain.value = master * master * 1.4;
      node.connect(outGain).connect(ctx.destination);
      node.port.onmessage = e => {
        const m = e.data;
        if (m.type === 'screen') { pending = false; draw(m.px); }
        else if (m.type === 'host') host(m.req, m.arg);
        else if (m.type === 'saved') {
          try { localStorage.setItem(slotKey(m.slot), toB64(m.bytes)); note('Saved project ' + (m.slot + 1) + ' in this browser.'); }
          catch { note('This browser would not store the project.'); }
          hostStatus();
        } else if (m.type === 'loaded') note(m.ok ? 'Project loaded.' : 'That project could not be loaded.');
      };
      await ctx.resume();
      made(); hostStatus();
      $('power').hidden = true;
      (function loop() { if (!pending && node) { pending = true; node.port.postMessage({type: 'paint'}); } setTimeout(loop, 33); })();
      setInterval(send, 50);       /* keeps held-button timings (like a long REC) moving */
      midiIn();
      note('Viscera is on. Press PLAY, or play the keys.');
    } catch (e) {
      $('power-on').disabled = false;
      note('Could not start audio in this browser: ' + (e && e.message ? e.message : e));
    }
  }
  function host(req, arg) {
    /* HOST_SAVE 1, HOST_LOAD 2; Bluetooth requests have nothing to do here */
    if (req === 1) node.port.postMessage({type: 'save', slot: arg});
    else if (req === 2) {
      let v = null; try { v = localStorage.getItem(slotKey(arg)); } catch {}
      if (v) node.port.postMessage({type: 'load', bytes: fromB64(v)}); else note('That slot is empty.');
    } else if (req >= 3) note('Bluetooth settings only do something on the FM-1.');
  }
  async function midiIn() {
    if (!navigator.requestMIDIAccess) return;
    try {
      const access = await navigator.requestMIDIAccess();
      const hook = p => { p.onmidimessage = e => node && node.port.postMessage({type: 'midi', data: Array.from(e.data)}); };
      access.inputs.forEach(hook);
      access.onstatechange = e => { if (e.port.type === 'input' && e.port.state === 'connected') hook(e.port); };
    } catch {}
  }
  $('power-on').addEventListener('click', powerOn);
  /* a still picture of the screen before power-on */
  lcd.fillStyle = '#121110'; lcd.fillRect(0, 0, 240, 240);
})();
