/* Viscera emulator: the audio thread. Runs the same C core as the FM-1
 * (compiled to WebAssembly), one 128-frame block per process() call, and
 * owns all instrument state: controls come in as messages, the screen goes
 * out as RGB565 pixels. */
class Viscera extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.x = new WebAssembly.Instance(options.processorOptions.module, {}).exports;
    this.x.init();
    this.start = currentTime;
    this.port.onmessage = e => this.message(e.data);
  }
  mem() { return this.x.memory.buffer; }
  message(m) {
    const x = this.x;
    if (m.type === 'control') {
      const e = m.enc;
      const req = x.control(m.keys >>> 0, m.buttons >>> 0, e[0], e[1], e[2], e[3], e[4], e[5], e[6], this.now());
      if (req) this.port.postMessage({type: 'host', req, arg: x.host_arg()});
    } else if (m.type === 'paint') {
      x.paint(this.now());
      const px = new Uint16Array(this.mem(), x.screen(), 240 * 240).slice();
      this.port.postMessage({type: 'screen', px}, [px.buffer]);
    } else if (m.type === 'bank') {
      const n = Math.min(m.pcm.length, x.bank_stage_frames());
      new Int16Array(this.mem(), x.bank_stage(), n).set(m.pcm.subarray(0, n));
      const name = new Uint8Array(this.mem(), x.bank_name(), 16); name.fill(0);
      for (let i = 0; i < Math.min(15, m.name.length); i++) name[i] = m.name.charCodeAt(i) & 127;
      const r = x.bank_commit(m.slot, n);
      this.port.postMessage({type: 'bank', slot: m.slot, ok: r === 0});
    } else if (m.type === 'host-status') {
      x.set_host(m.input ? 1 : 0, m.input ? 1 : 0, 0);
      for (const [slot, used, name] of m.slots || []) {
        const b = new Uint8Array(this.mem(), x.name_buffer(), 16); b.fill(0);
        for (let i = 0; i < Math.min(11, name.length); i++) b[i] = name.charCodeAt(i) & 127;
        x.slot_info(slot, used, x.name_buffer());
      }
    } else if (m.type === 'save') {
      x.project_seal(m.slot);
      const bytes = new Uint8Array(this.mem(), x.project(), x.project_bytes()).slice();
      this.port.postMessage({type: 'saved', slot: m.slot, bytes}, [bytes.buffer]);
    } else if (m.type === 'load') {
      const p = new Uint8Array(this.mem(), x.project(), x.project_bytes());
      const keep = p.slice();
      if (m.bytes.length !== p.length) { this.port.postMessage({type: 'loaded', ok: false}); return; }
      x.stop_all(); p.set(m.bytes);
      const ok = x.project_check() === 1;
      if (!ok) p.set(keep);
      this.port.postMessage({type: 'loaded', ok});
    } else if (m.type === 'midi') {
      x.midi(m.data[0], m.data[1] || 0, m.data[2] || 0);
    }
  }
  now() { return Math.floor((currentTime - this.start) * 1000) >>> 0; }
  process(inputs, outputs) {
    const x = this.x, out = outputs[0];
    const input = inputs[0];
    const live = input && input.length > 0 && input[0].length === 128;
    if (live) {
      const buf = new Float32Array(this.mem(), x.input_buffer(), 256);
      const l = input[0], r = input[1] || input[0];
      for (let i = 0; i < 128; i++) { buf[2 * i] = l[i]; buf[2 * i + 1] = r[i]; }
    }
    x.render(live ? 1 : 0);
    const o = new Float32Array(this.mem(), x.output_buffer(), 256);
    const L = out[0], R = out[1] || out[0];
    for (let i = 0; i < 128; i++) { L[i] = o[2 * i]; R[i] = o[2 * i + 1]; }
    return true;
  }
}
registerProcessor('viscera', Viscera);
