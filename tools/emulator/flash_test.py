#!/usr/bin/env python3
"""Install the real firmware package through the real install page into a
simulated FM-1, in headless Chrome, and check that it works.

For each starting firmware (Ravel-like, stock V15, an older Open build) it
clicks through Use latest build -> Connect -> Install, then checks:
  - the simulated loader got every block in time (no reply slower than the limit)
  - every byte written equals the package image
  - after the reboot the page reads back the new firmware's build ID
Exit status is non-zero on any failure.

Usage: python3 tools/emulator/flash_test.py [--site site] [--fwsc build/viscera.fwsc]
       [--meta build/latest.json] [--deadline 250] [--limit 60]
"""
import argparse, asyncio, base64, functools, http.server, json, os, shutil, socketserver, sys, tempfile, threading

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, '..', '..'))

def package_image(pkg):
    """The package minus the identity bytes interleaved into its first 960 bytes
    (the same transform as inspectPackage in WebUpdate.js)."""
    img = bytearray(len(pkg) - 20)
    for i in range(20):
        img[i * 47:(i + 1) * 47] = pkg[i * 48:i * 48 + 47]
    img[940:] = pkg[960:]
    return bytes(img)

def cipher(b, state):
    b = bytearray(b)
    for i in range(len(b)):
        b[i] ^= state & 0xff
        state = ((state << 1) ^ (0x1021 if state & 0x8000 else 0)) & 0xffff
    return bytes(b)

def boot_head(image):
    """The first 16 KB of the package's flash.bin (its boot head)."""
    h = cipher(image[0:64], 0xffff)
    for i in range(h[8] | h[9] << 8):
        e = cipher(image[64 + i * 80:144 + i * 80], 0xffff)
        if (e[0] | e[1] << 8) == 0:
            off = int.from_bytes(e[8:12], 'little')
            return image[off:off + 0x4000]
    raise ValueError('no flash.bin in package')

def serve(directory):
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=directory)
    handler.log_message = lambda *a: None
    httpd = socketserver.TCPServer(('127.0.0.1', 0), handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd

async def run_case(browser, url, start, cfg_js, limit, meta):
    page = await browser.new_page(viewport={'width': 1280, 'height': 900})
    errors = []
    page.on('pageerror', lambda e: errors.append(str(e)))
    await page.add_init_script(cfg_js.replace('__START__', start))
    await page.add_init_script(path=os.path.join(here, 'fm1_device.js'))
    await page.goto(url)
    await page.wait_for_function("!document.getElementById('use-latest').disabled", timeout=10000)
    await page.click('#use-latest')
    await page.wait_for_function("document.getElementById('pkg-meta').textContent.includes('build')", timeout=15000)
    if await page.query_selector('#switch') and start != 'open':
        await page.check('#switch')
    await page.click('#connect')
    await page.wait_for_function("document.getElementById('dev-meta').textContent.startsWith('Connected')", timeout=15000)
    await page.click('#install-btn')
    await page.click('#install-btn')
    try:
        await page.wait_for_function(
            "/^Done|install stopped/i.test(document.getElementById('status').textContent) || document.getElementById('error').textContent",
            timeout=420000)
    except Exception:
        pass
    status = await page.text_content('#status')
    err = await page.text_content('#error')
    rep = await page.evaluate('window.__FM1_SIM_REPORT')
    # reconnect: the page should show the installed build matching the package
    reread = ''
    if rep.get('outcome') == 'installed ' + meta['product']:
        await page.wait_for_timeout(1500)
        await page.evaluate("document.getElementById('dev-meta').textContent = ''")
        await page.click('#connect')
        try:
            await page.wait_for_function("document.getElementById('dev-meta').textContent.includes('installed build')", timeout=15000)
        except Exception:
            pass
        reread = await page.text_content('#dev-meta')
    await page.close()
    lat = sorted(rep.get('latencies') or [0])
    p = lambda q: lat[min(len(lat) - 1, int(len(lat) * q))]
    ok = (rep.get('outcome') == 'installed ' + meta['product'] and (status or '').startswith('Done') and lat[-1] <= limit
          and meta['build_id'][:16] in reread and 'Matches this package' in reread and not errors)
    return ok, {'start': start, 'outcome': rep.get('outcome'), 'status': status, 'error': err, 'blocks': len(lat),
                'reply_ms': {'median': p(0.5), 'p99': p(0.99), 'max': lat[-1]}, 'after_reconnect': reread,
                'page_errors': errors, 'events': rep.get('events', [])[-4:]}

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--site', default=os.path.join(root, 'site'))
    ap.add_argument('--fwsc', default=os.path.join(root, 'build', 'viscera.fwsc'))
    ap.add_argument('--meta', default=os.path.join(root, 'build', 'latest.json'))
    ap.add_argument('--deadline', type=int, default=250, help='ms the simulated loader waits per block')
    ap.add_argument('--limit', type=float, default=60, help='slowest acceptable reply, ms')
    ap.add_argument('--cases', default='ravel,stock,open,loader,ravel-slow')
    ap.add_argument('--stock', default=os.path.join(root, 'site', 'firmware', 'stock', 'FM-1_015.fwsc'))
    a = ap.parse_args()
    from playwright.async_api import async_playwright

    meta = json.load(open(a.meta))
    fwsc = open(a.fwsc, 'rb').read()
    tmp = tempfile.mkdtemp()
    shutil.copytree(a.site, os.path.join(tmp, 'site'))
    os.makedirs(os.path.join(tmp, 'site', 'firmware'), exist_ok=True)
    open(os.path.join(tmp, 'site', meta['url']), 'wb').write(fwsc)
    json.dump(meta, open(os.path.join(tmp, 'site', 'firmware', 'latest.json'), 'w'))
    httpd = serve(os.path.join(tmp, 'site'))
    url = f'http://127.0.0.1:{httpd.server_address[1]}/?installer-test'
    b64 = lambda b: "Uint8Array.from(atob('%s'),c=>c.charCodeAt(0))" % base64.b64encode(b).decode()
    vis_img = package_image(fwsc)
    stock_img = package_image(open(a.stock, 'rb').read())
    packages = "[{product:%s,buildId:%s,stock:false,image:%s,head:%s},{product:'FM-1_015',buildId:'',stock:true,image:%s,head:%s}]" % (
        json.dumps(meta['product']), json.dumps(meta['build_id']), b64(vis_img), b64(boot_head(vis_img)),
        b64(stock_img), b64(boot_head(stock_img)))
    # a Felucca-based or stock FM-1 still carries stock V15's boot head; an Open one carries Viscera's
    # 'ravel-slow': the loader takes 70 s to show up on USB; 'loader': the FM-1 is already in
    # M-VAVE's update mode (dark screen) when the page opens, after an install that stopped
    cfg_js = ("window.__FM1_SIM={start:('__START__'==='ravel-slow'?'ravel':'__START__'),loaderDelay:('__START__'==='ravel-slow'?70000:900),"
              "deadline:%d,packages:%s,head:('__START__'==='open'?%s:%s)};"
              % (a.deadline, packages, b64(boot_head(vis_img)), b64(boot_head(stock_img))))
    chrome = shutil.which('google-chrome') or shutil.which('chromium')
    all_ok = True
    async with async_playwright() as p:
        browser = await p.chromium.launch(executable_path=chrome, args=['--no-sandbox']) if chrome else await p.chromium.launch()
        for start in a.cases.split(','):
            ok, info = await run_case(browser, url, start, cfg_js, a.limit, meta)
            all_ok &= ok
            print(('PASS ' if ok else 'FAIL ') + json.dumps(info, ensure_ascii=False))
        await browser.close()
    httpd.shutdown()
    sys.exit(0 if all_ok else 1)

if __name__ == '__main__':
    asyncio.run(main())
