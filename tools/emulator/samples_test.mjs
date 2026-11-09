// The install page's sample manager, end to end, in headless Chromium against a
// simulated FM-1 (fm1_samples_sim.js): read an empty FM-1, add audio, write, check
// the bytes on the "device", rename/clear/rewrite, read back, back up, and refuse a
// bank too big for Viscera. Exit status is non-zero on any failure.
// Usage: node tools/emulator/samples_test.mjs [site]
import {createServer} from 'node:http';
import {readFileSync, existsSync, statSync} from 'node:fs';
import {join, extname, dirname} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createRequire} from 'node:module';
const require = createRequire(import.meta.url);
let chromium;
try { ({chromium} = require('playwright')); } catch { ({chromium} = require(join(process.env.NODE_PATH || '/usr/lib/node_modules', 'playwright'))); }

const here = dirname(fileURLToPath(import.meta.url));
const site = process.argv[2] || join(here, '..', '..', 'site');
const types = {'.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.css': 'text/css', '.png': 'image/png', '.svg': 'image/svg+xml', '.json': 'application/json', '.gif': 'image/gif'};
const server = createServer((req, res) => {
  let p = join(site, decodeURIComponent(req.url.split('?')[0]));
  if (existsSync(p) && statSync(p).isDirectory()) p = join(p, 'index.html');
  if (!existsSync(p)) { res.writeHead(404); return res.end(); }
  res.writeHead(200, {'content-type': types[extname(p)] || 'application/octet-stream'}); res.end(readFileSync(p));
}).listen(0);
const url = `http://127.0.0.1:${server.address().port}/`;
const sim = readFileSync(join(here, 'fm1_samples_sim.js'), 'utf8');

let fails = 0;
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) fails++; };
function wav(seconds, channels, rate, freq) {
  const n = Math.round(seconds * rate), data = Buffer.alloc(n * channels * 2);
  for (let i = 0; i < n; i++) for (let c = 0; c < channels; c++) data.writeInt16LE(Math.round(9000 * Math.sin(2 * Math.PI * freq * (c + 1) * i / rate)), (i * channels + c) * 2);
  const h = Buffer.alloc(44);
  h.write('RIFF', 0); h.writeUInt32LE(36 + data.length, 4); h.write('WAVE', 8); h.write('fmt ', 12); h.writeUInt32LE(16, 16);
  h.writeUInt16LE(1, 20); h.writeUInt16LE(channels, 22); h.writeUInt32LE(rate, 24); h.writeUInt32LE(rate * channels * 2, 28);
  h.writeUInt16LE(channels * 2, 32); h.writeUInt16LE(16, 34); h.write('data', 36); h.writeUInt32LE(data.length, 40);
  return Buffer.concat([h, data]);
}
const le32 = (d, at) => (d[at] | d[at + 1] << 8 | d[at + 2] << 16 | d[at + 3] << 24) >>> 0;
const names = d => Array.from({length: 8}, (_, i) => Buffer.from(d.slice(44 + i * 28, 59 + i * 28)).toString('latin1').replace(/\0.*$/s, ''));
const frames = d => Array.from({length: 8}, (_, i) => le32(d, 36 + i * 28));

const browser = await chromium.launch();
async function page(bank) {
  const ctx = await browser.newContext({acceptDownloads: true});
  const p = await ctx.newPage();
  const errors = []; p.on('pageerror', e => errors.push(e.message));
  await p.addInitScript(`window.__SMP_SIM = ${JSON.stringify({bank})};\n${sim}`);
  await p.goto(url);
  p.errors = errors;
  return p;
}
const idle = p => p.waitForFunction(() => !document.getElementById('smp-read').disabled, null, {timeout: 120000});
const status = p => p.$eval('#smp-status', e => e.textContent);
const error = p => p.$eval('#smp-error', e => e.textContent);
const flash = p => p.evaluate(() => window.__SMP_SIM.flash ? Array.from(window.__SMP_SIM.flash) : null);
async function write(p) { await p.click('#smp-write'); await p.click('#smp-write'); await p.waitForTimeout(50); await idle(p); }
async function put(p, slot, name, buf) {
  await p.setInputFiles(`#smp-slots .slot:nth-child(${slot}) .pick input`, {name, mimeType: 'audio/wav', buffer: buf});
  await p.waitForTimeout(50); await idle(p);
}

try {
  // 1. an FM-1 with no samples: read, add two, write
  let p = await page(null);
  await p.click('#smp-read'); await idle(p);
  ok(/no sample bank yet/i.test(await status(p)), 'reads an FM-1 with no samples: ' + await status(p));
  await put(p, 1, 'Kick 01.wav', wav(1.5, 1, 44100, 60));
  ok(/Slot 1: Kick 01\.wav, 1\.50 s/.test(await status(p)), 'converts a 44.1 kHz mono WAV: ' + await status(p));
  await put(p, 2, 'long pad.wav', wav(20, 2, 48000, 220));
  ok(/cut from 20\.00 s/.test(await status(p)), 'a too-long stereo WAV is cut to fit: ' + await status(p));
  ok(await p.$eval('#smp-write', b => !b.disabled), 'write is offered');
  await write(p);
  ok(/Written and checked/.test(await status(p)), 'writes and reads back: ' + await status(p) + ' ' + await error(p));
  let d = await flash(p);
  ok(d && d.length <= 376832, 'the bank on the FM-1 fits Viscera: ' + (d && d.length));
  ok(d && names(d)[0] === 'Kick 01' && names(d)[1] === 'long pad', 'names on the FM-1: ' + (d && names(d).slice(0, 2)));
  ok(d && frames(d)[0] === 48000, 'sample 1 is 1.5 s at 32 kHz: ' + (d && frames(d)[0]));
  ok(d && frames(d).slice(2).every(f => f === 0), 'the other slots stay empty');

  // 2. rename one, clear the other, write again
  await p.fill('#smp-slots .slot:nth-child(1) .nm', 'BD long name over fifteen');
  await p.click('#smp-slots .slot:nth-child(2) .clear');
  await write(p);
  d = await flash(p);
  ok(d && names(d)[0] === 'BD long name ov' && frames(d)[1] === 0 && frames(d)[0] === 48000, 'rename (cut to 15) and clear reach the FM-1: ' + (d && names(d)[0]));

  // 3. play decodes; backup downloads a bank the page can open again
  await p.click('#smp-slots .slot:nth-child(1) .play'); await p.waitForTimeout(200);
  ok(!(await error(p)), 'play decodes the sample: ' + await error(p));
  const [dl] = await Promise.all([p.waitForEvent('download'), p.click('#smp-backup')]);
  const backup = readFileSync(await dl.path());
  ok(Buffer.compare(backup, Buffer.from(d)) === 0, 'the backup is the bank on the FM-1, byte for byte');
  ok(p.errors.length === 0, 'no page errors: ' + p.errors.join('; '));
  await p.context().close();

  // 4. a fresh page reads that bank back
  p = await page(d);
  await p.click('#smp-read'); await idle(p);
  ok(/Read 1 sample\./.test(await status(p)) && await p.$eval('#smp-slots .slot:nth-child(1) .nm', e => e.value) === 'BD long name ov', 'reads the bank back: ' + await status(p));
  await p.setInputFiles('#smp-open-file', {name: 'backup.fm1s', mimeType: 'application/octet-stream', buffer: backup});
  await idle(p);
  ok(/Opened/.test(await status(p)), 'opens a backup: ' + await status(p));
  await p.context().close();

  // 5. a stock-sized bank (too big for Viscera): read it, refuse to write it as is
  // build one directly: eight 2.3 s samples, about 25 KB over Viscera's limit
  p = await page(null);
  const over = await p.evaluate(async () => {
    const one = n => { const d = new Uint8Array(Math.ceil(n / 320) * 228); for (let c = 0; c < d.length; c += 228) d.set([1, 0x45, 20, 0x5a], c); return d; };
    const e = Array.from({length: 8}, (_, i) => ({name: 'S' + (i + 1), frames: 73600, data: one(73600)}));
    return Array.from(FM1Samples.pack(e));
  });
  await p.context().close();
  ok(over.length > 376832, 'test bank is over Viscera\'s limit: ' + over.length);
  p = await page(over);
  await p.click('#smp-read'); await idle(p);
  ok(/larger than Viscera allows/.test(await status(p)), 'warns about a bank too big for Viscera: ' + await status(p));
  ok(await p.$eval('#smp-write', b => b.disabled), 'and won\'t write it until it fits');
  await p.click('#smp-slots .slot:nth-child(8) .clear');
  ok(await p.$eval('#smp-write', b => !b.disabled), 'clearing a sample makes it fit');
  await p.context().close();
} finally {
  await browser.close(); server.close();
}
console.log(fails ? fails + ' failures' : 'all sample manager tests pass');
process.exit(fails ? 1 : 0);
