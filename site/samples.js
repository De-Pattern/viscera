/* Viscera site: the sample manager. Reads the FM-1's sample bank over USB MIDI,
 * lets you replace, rename, clear and audition the eight slots, and writes the
 * whole bank back, read back and compared byte for byte. It uses M-VAVE's own
 * sample protocol (vendor/WebUpdate.js, FM1Web.Library) and bank format: the
 * same one the workbench writes, which Viscera and M-VAVE's firmware both read.
 *
 * Audio is converted here: decoded by the browser, mixed to mono at 32 kHz,
 * and encoded to SEA5 by vendor/sea.wasm (the SDK's encoder). */
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const en = text => (window.FM1I18n ? FM1I18n.t(String(text), 'en') : String(text));
  const SLOTS = 8, DISK_SLOTS = 16, HEADER = 512, RATE = 32000, CHUNK_FRAMES = 320, CHUNK_BYTES = 228;
  const REGION = 466944;
  /* Viscera keeps its projects, sounds and settings at the end of the sample area,
   * so a bank for Viscera stops 68 KiB short of M-VAVE's limit (446464). */
  const LIMIT = 376832;
  const MAX_FILE = 64 * 1024 * 1024;
  const check = (ok, message) => { if (!ok) throw new Error(message); };

  /* ---- the bank format (as M-VAVE's Samples.js) -------------------------------- */
  const zero8 = new Uint8Array(8);
  function crc(data) {
    let v = FM1Web.crc32(data.subarray(0, 12));
    v = FM1Web.crc32(zero8.subarray(0, Math.max(0, Math.min(8, data.length - 12))), v);
    return FM1Web.crc32(data.subarray(20), v);
  }
  function read(data) {
    check(data instanceof Uint8Array && data.length >= HEADER && data.length <= REGION, 'The sample bank has the wrong length.');
    const v = new DataView(data.buffer, data.byteOffset, data.byteLength);
    check(v.getUint32(0, true) === 0x53314d46 && v.getUint32(4, true) === 1 && v.getUint32(8, true) === data.length &&
      v.getUint32(16, true) === 0 && v.getUint32(20, true) === RATE && v.getUint32(24, true) === 0 && v.getUint32(28, true) === 0,
      'This is not an FM-1 sample bank.');
    check(v.getUint32(12, true) === crc(data), 'The sample bank failed its checksum.');
    check(data.slice(480, 512).every(b => b === 0), 'The sample bank header is damaged.');
    let next = HEADER; const entries = [];
    for (let i = 0; i < DISK_SLOTS; i++) {
      const at = 32 + i * 28, offset = v.getUint32(at, true), frames = v.getUint32(at + 4, true), bytes = v.getUint32(at + 8, true);
      const name = data.slice(at + 12, at + 28);
      check(offset === next && bytes === Math.ceil(frames / CHUNK_FRAMES) * CHUNK_BYTES && bytes <= data.length - next &&
        name[15] === 0 && name.every(b => b === 0 || (b >= 32 && b <= 126)), 'Sample slot ' + (i + 1) + ' is damaged.');
      for (let p = offset; p < offset + bytes; p += CHUNK_BYTES) check(v.getUint32(p, true) === 0x5a144501, 'Sample ' + (i + 1) + ' has a damaged block.');
      entries.push({name: String.fromCharCode(...name).replace(/\0.*$/s, ''), frames, data: data.slice(offset, offset + bytes)});
      next += bytes;
    }
    check(next === data.length, 'The sample bank has extra data at the end.');
    check(entries.slice(SLOTS).every(s => !s.frames && !s.name), 'This bank has more than eight samples.');
    return entries.slice(0, SLOTS);
  }
  function pack(entries, capacity = REGION) {
    check(entries.length === SLOTS, 'A bank has eight slots.');
    const size = HEADER + entries.reduce((n, s) => n + s.data.length, 0);
    check(size <= capacity, 'The samples don’t fit: shorten or clear one.');
    const data = new Uint8Array(size), v = new DataView(data.buffer);
    v.setUint32(0, 0x53314d46, true); v.setUint32(4, 1, true); v.setUint32(8, size, true); v.setUint32(20, RATE, true);
    let offset = HEADER;
    entries.forEach((s, i) => {
      const at = 32 + i * 28;
      v.setUint32(at, offset, true); v.setUint32(at + 4, s.frames, true); v.setUint32(at + 8, s.data.length, true);
      const name = cleanName(s.name);
      data.set(Array.from(name, c => c.charCodeAt(0)), at + 12);
      data.set(s.data, offset); offset += s.data.length;
    });
    for (let i = SLOTS; i < DISK_SLOTS; i++) v.setUint32(32 + i * 28, offset, true);
    v.setUint32(12, crc(data), true);
    read(data);
    return data;
  }
  function status(info) {
    check(info instanceof Uint8Array && info.length === 9 && info[0] <= 4, 'The FM-1 gave an unexpected sample status.');
    const view = new DataView(info.buffer, info.byteOffset, info.byteLength), size = view.getUint32(1, true);
    check(view.getUint32(5, true) === REGION && (info[0] === 2 ? size >= HEADER && size <= REGION : size === 0), 'The FM-1’s sample area isn’t the expected size.');
    return info[0];
  }
  /* WebUpdate.js's Library checks banks with these before it writes */
  globalThis.FM1Samples = {read, pack, crc, status, capacity: LIMIT, slots: SLOTS};
  const cleanName = s => String(s || '').replace(/[^\x20-\x7e]/g, '_').slice(0, 15);
  const empty = () => ({name: '', frames: 0, data: new Uint8Array(0)});
  const used = list => HEADER + list.reduce((n, s) => n + s.data.length, 0);

  /* ---- the codec --------------------------------------------------------------- */
  let codec = null;
  async function sea() {
    if (!codec) codec = (await WebAssembly.instantiate(await (await fetch('vendor/sea.wasm')).arrayBuffer())).instance.exports;
    return codec;
  }
  async function encode(pcm) {
    const x = await sea();
    const at = x.pcm_buffer(pcm.length);   /* may grow memory: take the buffer after */
    check(at, 'Not enough memory to convert this sample.');
    new Int16Array(x.memory.buffer, at, pcm.length).set(pcm);
    const p = x.encode(), n = x.sea_bytes();
    check(p && n === Math.ceil(pcm.length / CHUNK_FRAMES) * CHUNK_BYTES, 'Converting the sample failed.');
    return new Uint8Array(x.memory.buffer, p, n).slice();
  }
  async function decode(s) {
    const x = await sea();
    const at = x.sea_buffer(s.frames);
    new Uint8Array(x.memory.buffer, at, s.data.length).set(s.data);
    const p = x.decode();
    check(p, 'This sample could not be decoded.');
    return new Int16Array(x.memory.buffer, p, s.frames).slice();
  }

  /* ---- state --------------------------------------------------------------------- */
  let lib = null, access = null, entries = null, onDevice = null, source = '', busy = false, player = null, ctx = null;
  const changed = () => entries && onDevice && (onDevice.length !== used(entries) ||
    entries.some((s, i) => s.name !== onDevice.names[i] || s.data !== onDevice.data[i]));
  function say(text) { $('smp-status').textContent = en(text); }
  function fail(e) { $('smp-error').textContent = en(e && e.message ? e.message : String(e)); }
  function progress(v) { $('smp-progress').style.width = Math.round(Math.max(0, Math.min(1, v)) * 100) + '%'; }
  function remember() { onDevice = {length: used(entries), names: entries.map(s => s.name), data: entries.map(s => s.data)}; }
  async function job(text, fn) {
    if (busy) return; busy = true; $('smp-error').textContent = ''; progress(0); render();
    try { if (text) say(text); await fn(); }
    catch (e) { fail(e); }
    /* let go of the USB MIDI ports after every job, so the installer above can use them */
    finally { await disconnect(); busy = false; render(); }
  }
  async function library() {
    if (lib && lib.link && !lib.link.lost) return lib;
    if (!navigator.requestMIDIAccess) throw new Error('This browser has no Web MIDI. Use Chrome or Edge on a computer.');
    access = access || await navigator.requestMIDIAccess({sysex: true});
    lib = new FM1Web.Library(access);
    try { await lib.connect(); }
    catch (e) { lib = null; throw new Error('Couldn’t reach the FM-1’s sample bank. Is it on, connected by USB, and running Viscera or M-VAVE’s firmware? Close other MIDI software, then try again. (' + en(e.message) + ')'); }
    return lib;
  }
  async function disconnect() { if (lib) { await lib.disconnect().catch(() => {}); lib = null; } }

  /* ---- drawing ------------------------------------------------------------------- */
  const secs = frames => (frames / RATE).toFixed(2) + ' s';
  function render() {
    const have = !!entries;
    const size = have ? used(entries) : HEADER;
    const free = Math.max(0, Math.floor((LIMIT - size) / CHUNK_BYTES) * CHUNK_FRAMES);
    $('smp-space-bar').style.width = Math.min(100, (size - HEADER) / (LIMIT - HEADER) * 100) + '%';
    $('smp-space-bar').classList.toggle('over', size > LIMIT);
    $('smp-space').textContent = have
      ? (size > LIMIT ? 'Over by ' + ((size - LIMIT) / 1024).toFixed(1) + ' KiB: shorten or clear a sample before writing.'
                      : secs(entries.reduce((n, s) => n + s.frames, 0)) + ' used · ' + secs(free) + ' free')
      : 'No bank loaded.';
    $('smp-source').textContent = have ? source + (changed() ? ' · changes not on the FM-1 yet' : '') : '';
    const list = $('smp-slots');
    list.querySelectorAll('.slot').forEach((row, i) => {
      const s = have ? entries[i] : empty();
      const nm = row.querySelector('.nm');
      if (document.activeElement !== nm) nm.value = s.name;
      nm.disabled = busy || !have;
      nm.placeholder = s.frames ? 'name' : 'empty';
      row.querySelector('.len').textContent = s.frames ? secs(s.frames) : '—';
      row.classList.toggle('filled', !!s.frames);
      row.querySelector('.play').disabled = busy || !s.frames;
      row.querySelector('.play').textContent = player && player.slot === i ? 'Stop' : 'Play';
      row.querySelector('.clear').disabled = busy || !s.frames;
      row.querySelector('.pick').classList.toggle('off', busy || !have);
    });
    $('smp-read').disabled = busy;
    $('smp-write').disabled = busy || !have || size > LIMIT;
    $('smp-backup').disabled = busy || !have || size > REGION;
    $('smp-open').classList.toggle('off', busy);
    $('smp-new').disabled = busy;
  }
  function build() {
    const list = $('smp-slots');
    for (let i = 0; i < SLOTS; i++) {
      const row = document.createElement('div');
      row.className = 'slot';
      row.innerHTML = '<span class="no">' + (i + 1) + '</span>' +
        '<input class="nm" maxlength="15" spellcheck="false" aria-label="Name of sample ' + (i + 1) + '">' +
        '<span class="len">—</span>' +
        '<button class="play ghost" type="button">Play</button>' +
        '<label class="file ghost pick">Replace<input type="file" accept="audio/*,.wav,.aif,.aiff,.mp3,.flac,.ogg,.m4a"></label>' +
        '<button class="clear ghost" type="button">Clear</button>';
      row.querySelector('.nm').addEventListener('input', ev => { if (entries) { const v = cleanName(ev.target.value); entries[i] = {...entries[i], name: v}; render(); } });
      row.querySelector('.play').addEventListener('click', () => audition(i));
      row.querySelector('.clear').addEventListener('click', () => { stop(); entries[i] = empty(); say('Slot ' + (i + 1) + ' cleared here. Write to the FM-1 to keep it.'); render(); });
      row.querySelector('.pick input').addEventListener('change', ev => { const f = ev.target.files[0]; ev.target.value = ''; if (f) replace(i, f); });
      list.append(row);
    }
  }

  /* ---- actions ------------------------------------------------------------------- */
  function stop() { if (player) { try { player.node.stop(); } catch {} player = null; } }
  async function audition(i) {
    if (player && player.slot === i) { stop(); render(); return; }
    stop();
    try {
      const pcm = await decode(entries[i]);
      ctx = ctx || new AudioContext();
      await ctx.resume();
      const b = ctx.createBuffer(1, pcm.length, RATE), d = b.getChannelData(0);
      for (let k = 0; k < pcm.length; k++) d[k] = pcm[k] / 32768;
      const node = ctx.createBufferSource(); node.buffer = b; node.connect(ctx.destination);
      node.onended = () => { if (player && player.node === node) { player = null; render(); } };
      node.start(); player = {slot: i, node}; render();
    } catch (e) { fail(e); }
  }
  function replace(i, file) {
    job('Converting ' + file.name + '…', async () => {
      check(file.size <= MAX_FILE, 'That file is over 64 MB.');
      const bytes = await file.arrayBuffer();
      let audio;
      try { audio = await new OfflineAudioContext(1, 1, RATE).decodeAudioData(bytes); }
      catch { throw new Error('The browser couldn’t read that file. Try a WAV, AIFF, FLAC or MP3.'); }
      /* mono at 32 kHz (decodeAudioData resampled it), peak-normalised if asked */
      const n0 = audio.length, ch = Array.from({length: audio.numberOfChannels}, (_, c) => audio.getChannelData(c));
      const mono = new Float32Array(n0);
      for (let k = 0; k < n0; k++) { let v = 0; for (const c of ch) v += c[k]; mono[k] = v / ch.length; }
      /* fit: what the other slots leave free */
      const others = HEADER + entries.reduce((n, s, j) => n + (j === i ? 0 : s.data.length), 0);
      const room = Math.floor((LIMIT - others) / CHUNK_BYTES) * CHUNK_FRAMES;
      check(room >= CHUNK_FRAMES, 'No room left: clear or shorten another sample first.');
      const n = Math.min(n0, room);
      let peak = 0; for (let k = 0; k < n; k++) peak = Math.max(peak, Math.abs(mono[k]));
      const gain = $('smp-norm').checked && peak > 0.0001 ? 0.97 / peak : 1;
      const pcm = new Int16Array(n);
      for (let k = 0; k < n; k++) pcm[k] = Math.max(-32768, Math.min(32767, Math.round(mono[k] * gain * 32767)));
      stop();
      const data = await encode(pcm);
      entries[i] = {name: cleanName(file.name.replace(/\.[^.]+$/, '')), frames: n, data};
      say('Slot ' + (i + 1) + ': ' + file.name + ', ' + secs(n) + (n < n0 ? ' (cut from ' + secs(n0) + ' to fit)' : '') +
          '. Write to the FM-1 to keep it.');
    });
  }
  $('smp-read').addEventListener('click', () => job('Reading the sample bank from the FM-1…', async () => {
    stop();
    const l = await library();
    let info = await l.query(11), state = status(info);
    for (let t = 0; t < 20 && state === 1; t++) { await new Promise(r => setTimeout(r, 500)); state = status(info = await l.query(11)); }
    if (state === 0) { entries = Array.from({length: SLOTS}, empty); source = 'The FM-1 has no samples yet'; remember(); onDevice.length = -1; say('The FM-1 has no sample bank yet. Add samples, then write.'); return; }
    check(state === 2, state === 3 ? 'The FM-1 is in the middle of receiving samples. Switch it off and on, then read again.'
                                    : 'The FM-1 couldn’t read its own sample bank. You can write a new one (or a backup).');
    const bank = await l.samplesRead(progress);
    entries = read(bank); source = 'Read from the FM-1'; remember();
    const size = bank.length;
    const count = entries.filter(s => s.frames).length;
    say('Read ' + count + (count === 1 ? ' sample.' : ' samples.') + ' Back it up before you change anything.' +
        (size > LIMIT ? ' This bank is larger than Viscera allows: shorten or clear samples before writing.' : ''));
  }));
  $('smp-write').addEventListener('click', () => {
    const btn = $('smp-write');
    if (!btn.dataset.armed) {
      btn.dataset.armed = '1'; btn.textContent = 'Press again to write';
      say('This replaces the whole sample bank on the FM-1 (projects stay). Playback pauses while it writes.');
      setTimeout(() => { delete btn.dataset.armed; btn.textContent = 'Write to FM-1'; }, 6000);
      return;
    }
    delete btn.dataset.armed; btn.textContent = 'Write to FM-1';
    job('Writing the samples, then reading them back… keep the FM-1 on and connected.', async () => {
      stop();
      const bank = pack(entries, LIMIT);
      const l = await library();
      await l.samplesWrite(bank, progress);
      remember(); source = 'On the FM-1';
      say('Written and checked: the FM-1 has these samples now.');
    });
  });
  $('smp-backup').addEventListener('click', () => {
    try {
      const bank = pack(entries, REGION);
      const a = document.createElement('a'), u = URL.createObjectURL(new Blob([bank]));
      a.href = u; a.download = 'FM1-samples-' + new Date().toISOString().slice(0, 10) + '.fm1s'; a.click();
      setTimeout(() => URL.revokeObjectURL(u), 2000);
      say('Backup saved. M-VAVE’s workbench opens it too.');
    } catch (e) { fail(e); }
  });
  $('smp-open-file').addEventListener('change', ev => {
    const f = ev.target.files[0]; ev.target.value = '';
    if (f) job('Opening ' + f.name + '…', async () => {
      check(f.size <= REGION, 'That file is too big to be a sample bank.');
      stop();
      const list = read(new Uint8Array(await f.arrayBuffer()));
      if (!onDevice) { onDevice = {length: -1, names: [], data: []}; }
      entries = list; source = 'Opened ' + f.name;
      say('Opened. Nothing is on the FM-1 until you write it.');
    });
  });
  $('smp-new').addEventListener('click', () => {
    stop();
    if (!onDevice) onDevice = {length: -1, names: [], data: []};
    entries = Array.from({length: SLOTS}, empty); source = 'A new, empty bank';
    say('A new bank: add samples, then write. Writing it replaces every sample on the FM-1.'); render();
  });
  window.addEventListener('beforeunload', ev => { if (changed()) { ev.preventDefault(); ev.returnValue = ''; } });
  window.addEventListener('pagehide', disconnect);

  build(); render();
  say('Read the bank from the FM-1, open a backup, or start a new bank.');
})();
