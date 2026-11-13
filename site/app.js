/* Viscera site: the flasher. Uses M-VAVE's FM1 updater (vendor/WebUpdate.js),
 * the same protocol and checks as the official workbench.
 *
 * The route depends on what the FM-1 runs now:
 *   Open family (FM-1_8xxxxxx: Viscera, M-VAVE Open)  -> install directly
 *   M-VAVE stock V15 (FM-1_015)                        -> install through V15's updater
 *   anything else (Ravel, SLOOP, other Felucca-based)  -> M-VAVE's V15 first, then Viscera
 * Felucca-based firmwares accept only M-VAVE's own packages (their boot head
 * must match the one already on the FM-1), so Viscera can't go on directly. */
(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const fx = window.VisceraFX || {state: {}};
  const en = text => (window.FM1I18n ? FM1I18n.t(String(text), 'en') : String(text));
  const STOCK = {schema: 1, kind: 'stock', product: 'FM-1_015', profile: 'M-VAVE Original V15', family: 7, release_version: 'V15',
    build_id: 'db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a',
    sha256: 'db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a', bytes: 699956,
    loader_sha256: 'ef1f2db89b011fa53e71df363f0122de0ea896618c53fb53e8da676322004541',
    url: 'firmware/stock/FM-1_015.fwsc'};
  const OPEN = /^(FM1_OFFICIAL_OPEN|FM1_COMMUNITY_\d+|FM-1_8\d{6})$/;
  let prepared = null, access = null, latest = null, busy = false, route = null, identity = '', versions = [];

  function status(text, progress) {
    $('status').textContent = en(text);
    if (progress !== undefined) {
      fx.state.progress = Math.max(0, Math.min(1, progress / 100));
      fx.sediment && fx.sediment.kick();
    }
  }
  /* M-VAVE's update loader runs with the FM-1's screen dark: say so, so nobody unplugs a working update */
  const DARK = ' The FM-1’s screen goes dark now: that is M-VAVE’s update loader starting. It can take a minute. Leave it plugged in.';
  const stepStatus = (step, steps) => (text, progress) => {
    const t = en(text);
    status((steps > 1 ? `Step ${step} of ${steps}: ` : '') + t + (/waiting for the update device/i.test(t) ? DARK : ''), progress);
  };
  /* how long the page waits for the loader (and for the FM-1 to come back): generous, the
   * loader's USB can take a while to appear, and giving up early leaves people guessing */
  const TIMING = {reconnect: 180000};
  function error(e) { $('error').textContent = en(e && e.message ? e.message : String(e)); }
  function clearError() { $('error').textContent = ''; }
  function stage(n) { $('s2').classList.toggle('off', n < 2); $('s3').classList.toggle('off', n < 3); }

  async function sha256(bytes) {
    const d = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
    return Array.from(d, b => b.toString(16).padStart(2, '0')).join('');
  }
  function base64(bytes) {
    let s = '';
    for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(s);
  }

  /* firmware/versions.json (newest first) and latest.json are written by the deploy */
  function pick() {
    const v = versions[+$('ver').value] || versions[0];
    latest = v || null;
    $('ver-note').textContent = v ? (v.date ? v.date + ' · ' : '') + (v.note || '') : '';
  }
  async function findLatest() {
    try {
      let r = await fetch('firmware/versions.json', {cache: 'no-cache'});
      if (r.ok) versions = await r.json();
      else {
        r = await fetch('firmware/latest.json', {cache: 'no-cache'});
        if (!r.ok) throw 0;
        versions = [await r.json()];
      }
      if (!versions.length) throw 0;
      const sel = $('ver');
      sel.innerHTML = '';
      versions.forEach((v, i) => {
        const o = document.createElement('option');
        o.value = i; o.textContent = 'Viscera ' + (v.version || v.product) + (i === 0 ? ' · newest' : '');
        sel.appendChild(o);
      });
      sel.disabled = false;
      pick();
      $('release-text').textContent = 'Newest: Viscera ' + (versions[0].version || versions[0].product) + ' (' + versions.length + ' version' + (versions.length > 1 ? 's' : '') + ' to choose from). Early firmware, being tested on hardware now: back up first.';
      $('use-latest').disabled = false;
    } catch {
      $('ver').innerHTML = '<option>No published version</option>';
      $('release-text').textContent = 'No build is published yet. You can still open a .fwsc file you built.';
    }
  }
  $('ver').addEventListener('change', pick);
  /* which published version an identity or build is */
  function versionOf(id, build) {
    const v = versions.find(m => (build && m.build_id && m.build_id.startsWith(build)) || m.product === id);
    return v ? 'Viscera ' + v.version : '';
  }
  function describe(meta) {
    $('pkg-meta').innerHTML = '<b>' + meta.product + '</b><br>' +
      (meta.version ? 'version ' + meta.version + ' · ' : '') + Math.round(meta.bytes / 1024) + ' KB<br>build ' + meta.build_id.slice(0, 16);
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

  async function midi() {
    if (!navigator.requestMIDIAccess) throw new Error('This browser has no Web MIDI. Use Chrome or Edge on a computer.');
    return access = access || await navigator.requestMIDIAccess({sysex: true});
  }
  /* Connect without writing anything: identify the FM-1 and pick the route. */
  $('connect').addEventListener('click', async () => {
    if (!prepared || busy) return;
    clearError(); busy = true;
    try {
      status('Looking for the FM-1…');
      const u = new FM1Web.Updater(await midi(), prepared, status);
      const device = await u.connect({force: false});
      identity = device.identity;
      await u.disconnect().catch(() => {});
      if (device.resume || /^ota-/i.test(identity)) route = 'resume';
      else if (OPEN.test(identity) && !device.migration) route = 'direct';
      else if (identity === 'FM-1_015') route = 'stock';
      else if (OPEN.test(identity)) route = 'direct-force';    /* Open identity, other family */
      else route = 'bridge';
      fx.state.connected = 1;
      const now = device.current ? String(device.current).slice(0, 16) : '';
      const which = versionOf(identity, now);
      $('dev-meta').innerHTML = 'Connected: <b>' + identity + '</b>' + (which ? ' (' + which + ')' : '') +
        (now ? '<br>installed build ' + now : '') +
        (device.same ? '<br>Matches this package: already installed.' : '');
      $('route').textContent = {
        resume: 'The FM-1 is waiting in M-VAVE’s update mode (its screen is dark). Install Viscera finishes the update; nothing else is needed.',
        direct: 'Viscera installs directly.',
        'direct-force': 'Viscera installs directly, replacing this Open-family firmware.',
        stock: 'Installs through M-VAVE’s V15 updater.',
        bridge: 'This FM-1 runs Ravel or another Felucca-based firmware. The page will not install from it: handing over to M-VAVE’s updater from these firmwares can switch the FM-1 off part-way. Put M-VAVE’s V15 back first, then connect again.'
      }[route];
      if (route === 'bridge') { stage(2); status('Not installing: this FM-1 runs a firmware the page cannot safely install from.'); return; }
      stage(3);
      status(device.same ? 'This build is already on the FM-1.' : 'Connected. Ready to install.');
    } catch (e) { fx.state.connected = 0; route = null; error(e); status('Could not connect. Switch the FM-1 off and on, close other MIDI apps, then connect again.'); }
    finally { busy = false; }
  });

  /* One install of one package, connecting first. force covers non-Open firmware. */
  async function installOne(pkg, force, report) {
    const u = new FM1Web.Updater(await midi(), pkg, report, TIMING);
    const device = await u.connect({force});
    if (device.migration && !force) { await u.disconnect().catch(() => {}); return installOne(pkg, true, report); }
    await u.install({force});
    return device;
  }
  async function writing(job) {
    if (document.visibilityState !== 'visible') { error('Keep this tab in front while installing.'); return false; }
    clearError(); busy = true;
    /* Writing: stop every animation so each block request from the FM-1 is
     * answered at once, keep the screen awake, and watch for the tab hiding. */
    fx.pause && fx.pause();
    fx.state.rain = 0.9; fx.state.progress = 0.01;
    let lock = null;
    try { lock = navigator.wakeLock ? await navigator.wakeLock.request('screen') : null; } catch { lock = null; }
    const hidden = () => { if (document.visibilityState !== 'visible') $('inst-meta').textContent = 'Bring this tab back to the front: the FM-1 is waiting for data.'; };
    document.addEventListener('visibilitychange', hidden);
    try { await job(); return true; }
    catch (e) {
      error(e);
      status('The install stopped. Nothing is lost: if the FM-1’s screen is dark, it is waiting in M-VAVE’s update mode. ' +
             'Leave it plugged in (or unplug and replug it), press Connect, then Install Viscera: it finishes from there. ' +
             'If it shows an update error, switch it off and on first.');
      $('inst-meta').textContent = 'Stopped.';
      return false;
    } finally {
      busy = false; fx.state.rain = 0.15;
      document.removeEventListener('visibilitychange', hidden);
      try { lock && await lock.release(); } catch {}
      fx.resume && fx.resume();
    }
  }
  function arm(btn, label) {
    /* two presses: the first asks, the second installs */
    if (!btn.dataset.armed) {
      btn.dataset.armed = '1'; btn.textContent = 'Press again to install';
      $('inst-meta').textContent = 'Keep the FM-1 on, connected and this tab in front until it restarts.';
      setTimeout(() => { if (!busy) { delete btn.dataset.armed; btn.textContent = label; } }, 6000);
      return false;
    }
    delete btn.dataset.armed; btn.textContent = label;
    return true;
  }

  $('install-btn').addEventListener('click', async () => {
    if (!prepared || !route || busy || !arm($('install-btn'), 'Install Viscera')) return;
    $('inst-meta').textContent = 'Writing… don’t unplug.';
    const ok = await writing(async () => {
      if (route === 'bridge') {
        /* Never: from Felucca-based firmware the hand-over to M-VAVE's loader can cut the FM-1's power */
        throw new Error('Not installing from this firmware. Put M-VAVE’s V15 back first, then connect again.');
      } else {
        await installOne(prepared, route !== 'direct' && route !== 'resume', stepStatus(1, 1));
      }
    });
    if (ok) {
      status('Done. The FM-1 restarts into Viscera. Press Connect to check the installed build.', 100);
      $('inst-meta').textContent = 'Installed.';
      route = 'direct';
    }
  });
  $('stock-btn').addEventListener('click', async () => {
    if (!route || route === 'bridge' || busy || !arm($('stock-btn'), 'Put M-VAVE’s V15 back')) return;
    $('inst-meta').textContent = 'Writing M-VAVE’s V15… don’t unplug.';
    const ok = await writing(async () => {
      const stock = await FM1Web.prepare({...STOCK});
      await installOne(stock, true, stepStatus(1, 1));
    });
    if (ok) { status('Done. The FM-1 runs M-VAVE’s V15 again.', 100); $('inst-meta').textContent = 'V15 installed.'; route = 'stock'; }
  });

  findLatest();
})();
