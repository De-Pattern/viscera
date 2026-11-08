/* Viscera site: the flasher. Uses M-VAVE's FM1 updater (vendor/WebUpdate.js),
 * the same protocol and checks as the official workbench. */
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const fx = window.VisceraFX || {state: {}};
  const en = text => (window.FM1I18n ? FM1I18n.t(String(text), 'en') : String(text));
  let prepared = null, updater = null, access = null, latest = null, busy = false;

  function status(text, progress) {
    $('status').textContent = en(text);
    if (progress !== undefined) {
      fx.state.progress = Math.max(0, Math.min(1, progress / 100));
      fx.sediment && fx.sediment.kick();
    }
  }
  function error(e) {
    const msg = e && e.message ? e.message : String(e);
    $('error').textContent = en(msg);
  }
  function clearError() { $('error').textContent = ''; }
  function stage(n) {
    $('s2').classList.toggle('off', n < 2);
    $('s3').classList.toggle('off', n < 3);
  }

  async function sha256(bytes) {
    const d = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
    return Array.from(d, b => b.toString(16).padStart(2, '0')).join('');
  }
  function base64(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(s);
  }

  /* A published build: firmware/latest.json written by the release workflow:
   * {schema:1, kind:"open", product, build_id, family:7, bytes, sha256,
   *  loader_sha256, url:"firmware/viscera-x.y.z.fwsc", version} */
  async function findLatest() {
    try {
      const r = await fetch('firmware/latest.json', {cache: 'no-cache'});
      if (!r.ok) throw 0;
      latest = await r.json();
      $('release-text').textContent = 'Latest build: ' + (latest.version || latest.product) + '. Early firmware — back up first.';
      $('use-latest').disabled = false;
    } catch {
      $('release-text').textContent = 'No build is published yet. Viscera is in development; once the first build is released it appears here. You can still open a .fwsc file you built.';
    }
  }

  function describe(meta) {
    $('pkg-meta').innerHTML = '<b>' + meta.product + '</b><br>' +
      (meta.version ? 'version ' + meta.version + ' · ' : '') + Math.round(meta.bytes / 1024) + ' KB · sha ' + meta.sha256.slice(0, 12);
  }
  async function choose(meta) {
    clearError();
    status('Checking the firmware package…');
    prepared = await FM1Web.prepare(meta);
    describe(meta);
    fx.state.chosen = 1;
    stage(2);
    status('Firmware checked. Connect the FM-1.');
  }

  $('use-latest').addEventListener('click', async () => {
    if (!latest || busy) return;
    try { await choose({...latest}); } catch (e) { error(e); status('That build could not be used.'); }
  });
  $('pkg-file').addEventListener('change', async ev => {
    const file = ev.target.files && ev.target.files[0];
    if (!file || busy) return;
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      const pkg = FM1Web.inspectPackage(bytes);
      const sum = await sha256(bytes);
      await choose({schema: 1, kind: 'open', product: pkg.product, build_id: sum, family: 7,
        bytes: bytes.length, sha256: sum, loader_sha256: await sha256(pkg.loader), data: base64(bytes),
        version: file.name.replace(/\.fwsc$/i, '')});
    } catch (e) { error(e); status('That file is not an FM-1 firmware package this page can install.'); }
    ev.target.value = '';
  });

  $('connect').addEventListener('click', async () => {
    if (!prepared || busy) return;
    clearError();
    if (!navigator.requestMIDIAccess) { error('This browser has no Web MIDI. Use Chrome or Edge on a computer.'); return; }
    busy = true;
    try {
      status('Asking for MIDI access…');
      access = access || await navigator.requestMIDIAccess({sysex: true});
      if (updater) await updater.disconnect().catch(() => {});
      updater = new FM1Web.Updater(access, prepared, status);
      status('Looking for the FM-1…');
      const device = await updater.connect({force: $('switch').checked});
      if (device.migration) {
        fx.state.connected = 0;
        $('dev-meta').innerHTML = 'Found <b>' + device.identity + '</b>.';
        status('The FM-1 is running other firmware. Tick “Switching from…” and connect again to replace it.');
        return;
      }
      fx.state.connected = 1;
      $('dev-meta').innerHTML = 'Connected: <b>' + device.identity + '</b>' + (device.same ? '<br>This build is already installed.' : '');
      stage(3);
      status(device.resume ? 'The FM-1 is waiting in update mode. Install to finish.' : 'Connected. Ready to install.');
    } catch (e) { fx.state.connected = 0; error(e); status('Could not connect.'); }
    finally { busy = false; }
  });

  $('install-btn').addEventListener('click', async () => {
    if (!updater || busy) return;
    const btn = $('install-btn');
    if (!btn.dataset.armed) {
      /* two presses: the first asks, the second installs */
      btn.dataset.armed = '1'; btn.textContent = 'Press again to install';
      $('inst-meta').textContent = 'Keep the FM-1 on and connected until it restarts.';
      setTimeout(() => { if (!busy) { delete btn.dataset.armed; btn.textContent = 'Install Viscera'; } }, 6000);
      return;
    }
    delete btn.dataset.armed; btn.textContent = 'Install Viscera';
    clearError(); busy = true;
    fx.state.rain = 0.9; fx.state.progress = 0.01; fx.sediment && fx.sediment.kick();
    $('inst-meta').textContent = 'Writing… don’t unplug.';
    try {
      await updater.install({force: $('switch').checked});
      status('Done. The FM-1 restarts into Viscera.', 100);
      $('inst-meta').textContent = 'Installed.';
    } catch (e) {
      error(e); status('The install stopped. Don’t unplug; connect again to resume.');
      $('inst-meta').textContent = 'Stopped.';
    } finally { busy = false; fx.state.rain = 0.15; }
  });

  findLatest();
})();
