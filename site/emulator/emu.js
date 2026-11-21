/* Viscera emulator: the page. Builds the panel, turns pointer and keyboard
 * input into FM-1 control snapshots for the audio thread, draws the screen it
 * sends back, and handles live input and saved projects. */
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
  const ENC_FN = ['view', 'reson · robot', 'param 1', 'param 2', 'param 3', 'param 4', 'page\'s own'];
  const BTN_FN = {FX: 'master', SEL: 'motion · slice', ARP: 'clear', ENV: 'reson · tape', LFO: 'robots', EDIT: 'mixer', GLO: 'settings',
    HOME: 'tape', SAVE: 'projects', SEQ: 'bounce · splice', PLAY: 'play · stop', REC: 'rec · punch'};
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

  const btnEls = [];
  function makeBtn(name, parent, extra) {
    const i = BUTTONS.indexOf(name), b = document.createElement('button');
    b.className = 'btn' + (extra ? ' ' + extra : '');
    b.innerHTML = '<span class="l">' + name + '</span>' + (BTN_FN[name] ? '<span class="f">' + BTN_FN[name] + '</span>' : '');
    b.setAttribute('aria-label', name.replace('−', '-'));
    btnEls[i] = b;
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
  ROW2.forEach(n => makeBtn(n, $('btnrow2'), n === 'REC' ? 'rec' : ''));

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

  /* ---------- the meter bridge: four LED ladders, as on a 424 ---------- */
  /* 12 segments from -20 to +6 dB (0 VU at the 9th); peak-reading, with a short hold */
  const SEG_DB = [-20, -15, -10, -7, -5, -3, -2, -1, 0, 2, 4, 6];
  const ladders = [], lamps = [], held = [0, 0, 0, 0];
  {
    const m = $('meters');
    for (let t = 0; t < 4; t++) {
      const d = document.createElement('div'); d.className = 'mtr';
      const lad = document.createElement('div'); lad.className = 'lad';
      const segs = SEG_DB.map(db => { const g = document.createElement('div'); g.className = 'seg ' + (db > 2 ? 'r' : db >= 0 ? 'a' : 'g'); lad.appendChild(g); return g; });
      const lamp = document.createElement('div'); lamp.className = 'lamp';
      const nm = document.createElement('div'); nm.className = 'nm'; nm.textContent = 'TRK ' + (t + 1);
      d.append(lad, lamp, nm); m.appendChild(d); ladders.push(segs); lamps.push(lamp);
    }
    const sc = document.createElement('div'); sc.className = 'mscale'; sc.innerHTML = '<span>+6</span><span>0</span><span>-20</span>';
    m.appendChild(sc);
  }
  function meters(v) {
    if (!v) return;
    for (let t = 0; t < 4; t++) {
      const pk = v.pk[t], db = pk > 1e-5 ? 20 * Math.log10(pk / 0.5) : -99;   /* 0 VU: -6 dBFS */
      held[t] = Math.max(db, held[t] - 1.2);
      ladders[t].forEach((g, i) => g.classList.toggle('on', held[t] >= SEG_DB[i]));
      lamps[t].className = 'lamp' + (v.rec >> t & 1 ? ' rec' : v.arm >> t & 1 ? ' arm' : '');
    }
  }

  /* ---------- the panel's LEDs: 2 lit, 1 the backlight's glow ---------- */
  function lights(l) {
    for (let i = 0; i < 14; i++) if (btnEls[i]) { btnEls[i].classList.toggle('lit', l[i] === 2); btnEls[i].classList.toggle('glow', l[i] === 1); }
    for (let k = 0; k < 27; k++) if (keyEls[k]) { keyEls[k].classList.toggle('lit', l[14 + k] === 2); keyEls[k].classList.toggle('glow', l[14 + k] === 1); }
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

  /* ---------- live input ---------- */
  const KINDS = ['none', 'file', 'pc', 'mic'];
  function setInput(kind) {
    KINDS.forEach(k => $('in-' + k).setAttribute('aria-pressed', String(k === kind)));
    inputKind = kind; hostStatus();
  }
  async function useSource(make, label) {
    if (!ctx) { note('Switch the FM-1 on first.'); return false; }
    try {
      const s = await make();
      if (source) {
        try { source.disconnect(); } catch {}
        try { if (source.stop) source.stop(); } catch {}
        if (source.mediaStream) source.mediaStream.getTracks().forEach(t => t.stop());
        if (source.mediaElement) source.mediaElement.pause();
      }
      source = s; if (s) s.connect(node);
      $('in-name').textContent = label;
      return true;
    } catch (e) { note(e && e.userMessage ? e.userMessage : 'That input is not available here.'); setInput('none'); $('in-name').textContent = 'No live input.'; return false; }
  }
  const CATCH = ' REC starts the first take on the armed track, REC again loops it.';

  $('in-none').addEventListener('click', () => { setInput('none'); useSource(async () => null, 'No live input.'); });

  /* microphone, or any input device: a virtual cable brings in a DAW or any app */
  async function useMic() {
    setInput('mic');
    const id = $('in-dev').value;
    const ok = await useSource(async () => ctx.createMediaStreamSource(await navigator.mediaDevices.getUserMedia({audio: {
      deviceId: id ? {exact: id} : undefined, echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: 2}})),
      ($('in-dev').selectedOptions[0]?.textContent || 'Microphone') + '.' + CATCH);
    if (ok) listDevices();
  }
  async function listDevices() {
    try {
      const sel = $('in-dev'), cur = sel.value;
      const devs = (await navigator.mediaDevices.enumerateDevices()).filter(d => d.kind === 'audioinput');
      sel.innerHTML = '<option value="">Default</option>' + devs.filter(d => d.deviceId && d.deviceId !== 'default')
        .map((d, i) => `<option value="${d.deviceId}">${(d.label || 'Input ' + (i + 1)).replace(/[<>&]/g, '')}</option>`).join('');
      if ([...sel.options].some(o => o.value === cur)) sel.value = cur;
    } catch {}
  }
  $('in-mic').addEventListener('click', useMic);
  $('in-dev').addEventListener('focus', listDevices);
  $('in-dev').addEventListener('change', useMic);
  if (navigator.mediaDevices && navigator.mediaDevices.addEventListener) navigator.mediaDevices.addEventListener('devicechange', listDevices);

  /* the computer's own audio, through the screen-share prompt: a tab anywhere, the whole
     computer in Chrome or Edge on Windows */
  $('in-pc').addEventListener('click', async () => {
    if (!navigator.mediaDevices || !navigator.mediaDevices.getDisplayMedia) { note('This browser cannot share audio. Try Chrome or Edge, or a virtual cable as the input device.'); return; }
    setInput('pc');
    await useSource(async () => {
      const st = await navigator.mediaDevices.getDisplayMedia({video: true, audio: {echoCancellation: false, noiseSuppression: false, autoGainControl: false},
        systemAudio: 'include', restrictOwnAudio: true, selfBrowserSurface: 'exclude', surfaceSwitching: 'include'});
      st.getVideoTracks().forEach(t => t.stop());
      const tracks = st.getAudioTracks();
      if (!tracks.length) { const e = new Error('no audio'); e.userMessage = 'No audio was shared: share again and turn on "Share audio" (a tab, or Entire screen on Windows).'; throw e; }
      tracks[0].addEventListener('ended', () => { if (inputKind === 'pc') { setInput('none'); $('in-name').textContent = 'Sharing stopped. No live input.'; } });
      return ctx.createMediaStreamSource(new MediaStream(tracks));
    }, 'Computer audio (shared).' + CATCH + ' (Sharing the whole computer can feed the FM-1 back into itself: headphones help.)');
  });

  $('in-file').addEventListener('click', () => $('in-file-pick').click());
  $('in-file-pick').addEventListener('change', e => {
    const f = e.target.files[0]; e.target.value = '';
    if (!f) return;
    setInput('file');
    useSource(async () => {
      const a = new Audio(URL.createObjectURL(f)); a.loop = true; await a.play();
      return ctx.createMediaElementSource(a);
    }, f.name + ', looping.' + CATCH);
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
        if (m.type === 'screen') { pending = false; draw(m.px); if (m.leds) lights(m.leds); meters(m.meters); }
        else if (m.type === 'device') { try { localStorage.setItem('viscera-device', toB64(m.bytes)); } catch {} }
        else if (m.type === 'host') host(m.req, m.arg);
        else if (m.type === 'saved') {
          try { localStorage.setItem(slotKey(m.slot), toB64(m.bytes)); note('Saved project ' + (m.slot + 1) + ' in this browser.'); }
          catch { note('This browser would not store the project.'); }
          hostStatus();
        } else if (m.type === 'loaded') note(m.ok ? 'Project loaded.' : 'That project could not be loaded.');
      };
      await ctx.resume();
      hostStatus();
      try { const d = localStorage.getItem('viscera-device'); if (d) node.port.postMessage({type: 'device-load', bytes: fromB64(d)}); } catch {}
      $('power').hidden = true;
      (function loop() { if (!pending && node) { pending = true; node.port.postMessage({type: 'paint'}); } setTimeout(loop, 33); })();
      setInterval(send, 50);       /* keeps held-button timings (like a long REC) moving */
      midiIn();
      setInput('none');
      note('Viscera 4-Track is on. Choose an input (a file, your computer\'s audio or a microphone), then REC for the first take on track 1.');
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
