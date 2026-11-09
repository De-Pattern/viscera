// The sample codec the install page uses: SEA5 round trip on a test tone.
import {readFileSync} from 'node:fs';
const x = (await WebAssembly.instantiate(readFileSync(process.argv[2] || 'site/vendor/sea.wasm'))).instance.exports;
const N = 32000 * 2 + 77, src = new Int16Array(N);
for (let i = 0; i < N; i++) src[i] = Math.round(12000 * Math.sin(i * 2 * Math.PI * 440 / 32000) + 4000 * Math.sin(i * 0.31));
{ const at = x.pcm_buffer(N); new Int16Array(x.memory.buffer, at, N).set(src); }   // the call may grow memory
const p = x.encode(), n = x.sea_bytes();
if (!p || n !== Math.ceil(N / 320) * 228) throw new Error('encode size ' + n);
const sea = new Uint8Array(x.memory.buffer, p, n).slice();
for (let c = 0; c < n; c += 228) if (sea[c] !== 1 || sea[c + 1] !== 0x45 || sea[c + 2] !== 20 || sea[c + 3] !== 0x5a) throw new Error('chunk header ' + c);
{ const at = x.sea_buffer(N); new Uint8Array(x.memory.buffer, at, n).set(sea); }
const q = x.decode(); if (!q) throw new Error('decode failed');
const out = new Int16Array(x.memory.buffer, q, N);
let s = 0, e = 0; for (let i = 0; i < N; i++) { s += src[i] * src[i]; e += (src[i] - out[i]) ** 2; }
const snr = 10 * Math.log10(s / e);
if (snr < 20) throw new Error('round trip SNR ' + snr.toFixed(1) + ' dB');
console.log('sea ok: ' + n + ' bytes, SNR ' + snr.toFixed(1) + ' dB');
