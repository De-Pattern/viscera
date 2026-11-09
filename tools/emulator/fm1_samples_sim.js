/* A simulated FM-1 sample bank for testing the install page's sample manager,
 * injected before the page's scripts. It speaks M-VAVE's file protocol the way
 * the firmware's transfer.c and samples.c do (commands 0, 21, 22 and 11-16),
 * including Viscera's smaller bank limit, and keeps its flash in
 * window.__SMP_SIM.flash so a test can check exactly what was written.
 *
 * window.__SMP_SIM = {bank: [byte...] or null, limit: 376832} before load. */
(() => {
  const cfg = Object.assign({bank: null, limit: 376832}, window.__SMP_SIM || {});
  const REGION = 466944;
  const sim = window.__SMP_SIM = {flash: cfg.bank ? Uint8Array.from(cfg.bank) : null, writes: 0, commands: []};
  let state = sim.flash ? 2 : 0, total = sim.flash ? sim.flash.length : 0, position = 0, incoming = null;

  const crcTable = (() => { const t = []; for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t.push(c >>> 0); } return t; })();
  const crc32 = (bytes, prev = 0) => { let c = ~prev; for (const b of bytes) c = crcTable[(c ^ b) & 255] ^ (c >>> 8); return (~c) >>> 0; };
  const bankCrc = d => crc32(d.subarray(20), crc32(new Uint8Array(8), crc32(d.subarray(0, 12))));
  const le32 = (d, at) => (d[at] | d[at + 1] << 8 | d[at + 2] << 16 | d[at + 3] << 24) >>> 0;
  const put32 = v => [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, v >>> 24];
  function valid(d) {
    if (le32(d, 0) !== 0x53314d46 || le32(d, 4) !== 1 || le32(d, 8) !== d.length || le32(d, 16) !== 0 || le32(d, 20) !== 32000) return false;
    if (le32(d, 12) !== bankCrc(d)) return false;
    let next = 512;
    for (let i = 0; i < 16; i++) {
      const at = 32 + i * 28, off = le32(d, at), frames = le32(d, at + 4), bytes = le32(d, at + 8);
      if (off !== next || bytes !== Math.ceil(frames / 320) * 228 || (i >= 8 && frames)) return false;
      for (let p = off; p < off + bytes; p += 228) if (le32(d, p) !== 0x5a144501) return false;
      next += bytes;
    }
    return next === d.length;
  }
  function command(cmd, data) {
    sim.commands.push(cmd);
    if (cmd === 0) return [0, 4, 16, 0, 4064 & 255, 4064 >> 8, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    if (cmd === 21) return [0, 0, 4264 & 255, 4264 >> 8, ...put32(REGION - 4096)];
    if (cmd === 22) return [0, 1, 128, 4, 0, ...put32(446464), 3];
    if (cmd === 11) return [0, state, ...put32(state === 2 ? total : 0), ...put32(REGION)];
    if (cmd === 12) {
      const n = le32(data, 0);
      if (n < 512 || n > cfg.limit) return [71];
      state = 3; total = n; position = 0; incoming = new Uint8Array(n); return [0];
    }
    if (cmd === 13) {
      const off = le32(data, 0), part = data.slice(4);
      if (state !== 3 || off !== position || !part.length || part.length > 128 || part.length > total - position) return [71];
      incoming.set(part, off); position += part.length; sim.writes += part.length; return [0];
    }
    if (cmd === 14) {
      if (state !== 3 || position !== total || !valid(incoming)) { state = 4; return [71]; }
      sim.flash = incoming; incoming = null; state = 2; return [0];
    }
    if (cmd === 15) {
      const off = le32(data, 0), n = data[4];
      if (state !== 2 || n > 64 || off + n > total) return [71];
      return [0, ...sim.flash.slice(off, off + n)];
    }
    if (cmd === 16) { if (state === 3) state = 4; return [0]; }
    return [71];
  }
  /* framing: as transfer.c (7-bit packing, checksum, the reply echoes the request head) */
  function reply(msg) {
    const n = msg.length;
    if (n < 10 || msg[0] !== 240 || msg[1] !== 125 || msg[2] !== 70 || msg[3] !== 77 || msg[4] !== 2) return null;
    let sum = 0; for (let i = 1; i < n - 1; i++) sum += msg[i]; if (sum & 127) return null;
    const data = []; let value = 0, bits = 0;
    for (let i = 8; i + 2 < n; i++) { value |= msg[i] << bits; bits += 7; if (bits >= 8) { data.push(value & 255); value >>>= 8; bits -= 8; } }
    const out = command(msg[5], Uint8Array.from(data));
    const r = [...msg.slice(0, 8)]; r[5] |= 64;
    value = 0; bits = 0;
    for (const b of out) { value |= b << bits; bits += 8; while (bits >= 7) { r.push(value & 127); value >>>= 7; bits -= 7; } }
    if (bits) r.push(value & 127);
    let s = 0; for (let i = 1; i < r.length; i++) s += r[i];
    r.push((0 - s) & 127, 247);
    return Uint8Array.from(r);
  }

  class Port extends EventTarget {
    constructor(type) { super(); this.type = type; this.name = 'FM1'; this.manufacturer = 'M-VAVE'; this.id = type + '-fm1'; this.state = 'connected'; this.connection = 'closed'; this.onmidimessage = null; }
    open() { this.connection = 'open'; return Promise.resolve(this); }
    close() { this.connection = 'closed'; return Promise.resolve(this); }
    send(bytes) {
      const r = reply(Uint8Array.from(bytes));
      if (r) setTimeout(() => { const e = new Event('midimessage'); e.data = r; input.dispatchEvent(e); if (input.onmidimessage) input.onmidimessage(e); }, 1);
    }
  }
  const input = new Port('input'), output = new Port('output');
  const access = new EventTarget();
  access.inputs = new Map([[input.id, input]]); access.outputs = new Map([[output.id, output]]); access.sysexEnabled = true;
  navigator.requestMIDIAccess = () => Promise.resolve(access);
})();
