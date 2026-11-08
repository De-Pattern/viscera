/* A simulated FM-1 for testing the install page, injected before the page's
 * scripts. It replaces navigator.requestMIDIAccess with one USB-MIDI port pair
 * connected to a device running in a Web Worker, so the device keeps its own
 * clock: if the page is slow to answer a block request, the device notices,
 * exactly as real hardware would.
 *
 * Configure with window.__FM1_SIM before the page loads:
 *   start:    'ravel' (non-Open firmware, no capability reply), 'stock' (FM-1_015)
 *             or 'open' (an Open-family firmware such as an older Viscera)
 *   deadline: ms the loader waits for each block before failing (default 250)
 *   packages: [{image, head, product, buildId, stock}] the packages the page may send;
 *             head is the package's 16 KB boot head
 *   head:     the boot head already on the device (stock V15's for Felucca-based firmware)
 * Results: window.__FM1_SIM_REPORT (latencies, outcome, bytes written). */
(() => {
  const cfg = Object.assign({start: 'ravel', deadline: 250, chunk: 512}, window.__FM1_SIM || {});
  const worker = new Worker(URL.createObjectURL(new Blob([`(${device.toString()})()`], {type: 'text/javascript'})));
  const report = window.__FM1_SIM_REPORT = {latencies: [], outcome: 'idle', events: []};

  class Port extends EventTarget {
    constructor(type) { super(); this.type = type; this.name = 'FM1'; this.manufacturer = 'M-VAVE'; this.id = type + '-fm1'; this.state = 'connected'; this.connection = 'closed'; }
    open() { this.connection = 'open'; return Promise.resolve(this); }
    close() { this.connection = 'closed'; return Promise.resolve(this); }
    send(bytes) { if (this.state !== 'connected') throw new Error('disconnected'); worker.postMessage({type: 'host', bytes: Array.from(bytes)}); }
  }
  const input = new Port('input'), output = new Port('output');
  const access = new EventTarget();
  access.inputs = new Map([[input.id, input]]);
  access.outputs = new Map([[output.id, output]]);
  access.sysexEnabled = true;

  worker.onmessage = ev => {
    const m = ev.data;
    if (m.type === 'dev') {
      const e = new Event('midimessage'); e.data = Uint8Array.from(m.bytes); input.dispatchEvent(e);
    } else if (m.type === 'state') {
      input.state = output.state = m.connected ? 'connected' : 'disconnected';
      const e = new Event('statechange'); e.port = input; access.dispatchEvent(e);
    } else if (m.type === 'report') {
      Object.assign(report, m.report);
    }
  };
  worker.postMessage({type: 'config', cfg: {start: cfg.start, deadline: cfg.deadline, chunk: cfg.chunk,
    packages: (cfg.packages || []).map(p => ({product: p.product, buildId: p.buildId, stock: !!p.stock,
      image: Array.from(p.image), head: Array.from(p.head)})),
    head: Array.from(cfg.head || [])}});
  navigator.requestMIDIAccess = () => Promise.resolve(access);

  /* ---- the device, running in the worker ---- */
  function device() {
    let cfg, mode = 'app', identity, capBuild = null, armed = false, image = null, flash = null, head = null, family = 'open';
    let staged = null, stagedBytes = null, current = null;
    let pending = null, timer = 0, seq = 0, queue = [], phase = '', lat = [], outcome = 'idle', events = [];
    const now = () => performance.now();
    const pack7 = bytes => { const out = []; let bits = 0, v = 0; for (const b of bytes) { v |= b << bits; bits += 8; while (bits >= 7) { out.push(v & 127); v >>>= 7; bits -= 7; } } if (bits) out.push(v & 127); return out; };
    const frame = (cmd, payload = []) => { let sum = 0; for (const b of payload) sum = (sum + b) & 255;
      return [240, ...pack7([0, 89, cmd, payload.length & 255, payload.length >> 8, 0, ...payload, (~sum) & 255]), 247]; };
    const parse = bytes => {
      if (bytes[0] !== 240 || bytes[bytes.length - 1] !== 247) return null;
      const raw = []; let bits = 0, v = 0;
      for (let i = 1; i < bytes.length - 1; i++) { v |= bytes[i] << bits; bits += 7; if (bits >= 8) { raw.push(v & 255); v >>>= 8; bits -= 8; } }
      if (raw.length < 7 || raw[0] !== 0 || raw[1] !== 89) return null;
      return {command: raw[2], data: raw.slice(6, -1)};
    };
    const le = (p, at, n) => { let v = 0; for (let i = 0; i < n; i++) v += p[at + i] * 2 ** (i * 8); return v; };
    const put = (n, w) => Array.from({length: w}, (_, i) => Math.floor(n / 2 ** (8 * i)) & 255);
    const send = bytes => postMessage({type: 'dev', bytes});
    const report = () => postMessage({type: 'report', report: {latencies: lat, outcome, events, mode, identity,
      written: flash ? flash.filter(x => x >= 0).length : 0}});
    const log = e => { events.push(e); report(); };
    const reboot = (to, ms = 900) => {
      postMessage({type: 'state', connected: false});
      setTimeout(() => {
        mode = to;
        postMessage({type: 'state', connected: true});
        log('booted ' + mode + ' as ' + identity);
      }, ms);
    };
    const nextRequest = () => {
      const req = queue.shift();
      if (!req) return;
      pending = {...req, seq: seq = (seq + 1) & 127, sent: now()};
      send(frame(48, [pending.seq, ...put(req.addr, 4), ...put(req.bytes, 3)]));
      /* the loader gives up if the page doesn't answer in time */
      timer = setTimeout(() => {
        if (!pending) return;
        outcome = 'failed: no data for ' + cfg.deadline + ' ms at 0x' + pending.addr.toString(16) + ' (' + phase + ')';
        log(outcome); mode = 'error'; pending = null; queue = [];
      }, cfg.deadline);
    };
    const startTransfer = (which) => {
      phase = which; queue = [];
      if (which === 'write') image = staged;
      const n = which === 'prepare' ? 32768 : image.length;
      for (let a = 0; a < n; a += cfg.chunk) queue.push({addr: a, bytes: Math.min(cfg.chunk, n - a)});
      queue.push({addr: which === 'prepare' ? 0xe0000000 : 0xf0000000, bytes: 8});
      if (which === 'write') flash = new Array(image.length).fill(-1);
      log('requesting data: ' + which);
      setTimeout(nextRequest, 5);
    };
    onmessage = ev => {
      const m = ev.data;
      if (m.type === 'config') {
        cfg = m.cfg; head = cfg.head;
        family = cfg.start === 'ravel' ? 'felucca' : cfg.start;          /* felucca, stock or open */
        identity = cfg.start === 'stock' ? 'FM-1_015' : cfg.start === 'open' ? 'FM-1_8600099' : 'FM-1_82000401';
        capBuild = cfg.start === 'open' ? '00'.repeat(32) : null;
        log('booted app as ' + identity);
        return;
      }
      const b = m.bytes;
      if (mode === 'error') return;                               /* shows an update error; silent until power-cycled */
      if (b.length === 6 && b[1] === 34 && b[2] === 36 && b[3] === 53 && b[4] === 127) {
        if (mode === 'loader') startTransfer('write');
        else if (mode === 'app' && (armed || cfg.start !== 'open')) startTransfer('prepare');
        return;
      }
      const f = parse(b);
      if (!f) return;
      if (f.command === 17) {
        const id = [...identity].map(c => c.charCodeAt(0)); while (id.length < 27) id.push(0);
        send(frame(17, id));
      } else if (f.command === 49 && mode === 'app' && capBuild) {
        const c = new Array(47).fill(0);
        [70, 77, 67, 49].forEach((x, i) => c[i] = x);              /* 'FMC1' */
        c[4] = 1; c[5] = 7; c[6] = 0; c[7] = 1;
        put(512, 2).forEach((x, i) => c[8 + i] = x); put(1048576, 4).forEach((x, i) => c[10 + i] = x);
        capBuild.match(/../g).forEach((h, i) => c[14 + i] = parseInt(h, 16));
        send(frame(49, c));
      } else if (f.command === 50 && mode === 'app' && capBuild) {
        armed = true; send(frame(50, [0]));
      } else if ((f.command === 33 || f.command === 34) && family === 'stock' && mode === 'app') {
        send(frame(0, [0]));                                        /* voice bank restore: acknowledged */
      } else if (f.command === 48 && pending && f.data[0] === pending.seq && le(f.data, 1, 4) === pending.addr) {
        clearTimeout(timer);
        const took = now() - pending.sent; lat.push(Math.round(took * 10) / 10);
        const body = f.data.slice(8);
        if (pending.addr >= 0xe0000000) {
          pending = null;
          if (phase === 'prepare') {
            const sb = stagedBytes || []; stagedBytes = null;
            const pkg = cfg.packages.find(p => sb.length && sb.every((x, i) => x === undefined || x === p.image[i]));
            if (!pkg) { outcome = 'failed: unknown package'; log(outcome); mode = 'error'; return; }
            /* Felucca-based firmware accepts the official loader only when the
             * package's boot head equals the one already on the device */
            if (family === 'felucca' && pkg.head.some((x, i) => x !== head[i])) {
              outcome = 'failed: refused by the firmware (boot head differs, Felucca -5): ' + pkg.product;
              log(outcome); mode = 'error'; return;
            }
            staged = pkg.image; current = pkg;
            identity = 'ota-FM-1_015'; log('accepted ' + pkg.product + '; rebooting into the loader'); reboot('loader');
          }
          else {
            const missing = flash.indexOf(-1), wrong = flash.findIndex((x, i) => x !== image[i]);
            if (missing >= 0 || wrong >= 0) { outcome = 'failed: flash mismatch at ' + Math.max(missing, wrong); log(outcome); mode = 'error'; return; }
            identity = current.product; capBuild = current.stock ? null : current.buildId; head = current.head;
            family = current.stock ? 'stock' : 'open'; armed = false;
            outcome = 'installed ' + current.product; log('written and verified; rebooting');
            reboot('app', 1200);
          }
          return;
        }
        if (phase === 'write') body.forEach((x, i) => flash[pending.addr + i] = x);
        else {
          /* staging: keep what arrives, to tell which package this is */
          if (pending.addr === 0) staged = null;
          stagedBytes = stagedBytes || [];
          body.forEach((x, i) => stagedBytes[pending.addr + i] = x);
        }
        pending = null;
        setTimeout(nextRequest, 1);                                /* the loader asks for the next block */
      }
    };
  }
})();
