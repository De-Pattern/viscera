#!/usr/bin/env python3
"""Put every published version behind the installer, not only the newest.

The Pages site is rebuilt from scratch on each deploy, so the versions it served
before are fetched back from the live site (checked against their SHA-256), the
seeds in firmware-archive/ (versions from before this list existed) are added, and
this build goes on top. Writes site/firmware/versions.json, newest first, each entry
the build's latest.json plus a date and a one-line note.

  python3 tools/site_versions.py --build build --site site --seed firmware-archive \
      --live https://de-pattern.github.io/viscera --changelog CHANGELOG.md
"""
import argparse, datetime, hashlib, json, os, re, shutil, urllib.request


def vkey(v):
    return tuple(int(x) for x in re.findall(r'\d+', v))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def note_for(changelog, version):
    """The section's first line, without its markdown: a short line for the menu."""
    text = open(changelog, encoding='utf-8').read()
    m = re.search(r'^## ' + re.escape(version) + r'\b[^\n]*\n(.*?)(?=^## |\Z)', text, re.S | re.M)
    if not m:
        return ''
    title = re.search(r'^## ' + re.escape(version) + r'\s*[—-]\s*(.*)$', text, re.M)
    line = title.group(1).strip() if title else ''
    return line[:120]


def fetch(url):
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--site', required=True)
    ap.add_argument('--seed', required=True)
    ap.add_argument('--live', required=True)
    ap.add_argument('--changelog', required=True)
    a = ap.parse_args()
    fw = os.path.join(a.site, 'firmware')
    os.makedirs(fw, exist_ok=True)
    have = {}

    def add(meta, data, why):
        if sha(data) != meta['sha256']:
            print(f"skipped {meta['version']} ({why}): its SHA-256 doesn't match")
            return
        name = f"viscera-{meta['version']}.fwsc"
        open(os.path.join(fw, name), 'wb').write(data)
        meta = dict(meta, url='firmware/' + name)
        have[meta['version']] = meta
        print(f"{meta['version']}: {why}")

    # what the live site serves now (lost otherwise: Pages keeps only the last deploy)
    try:
        live = json.loads(fetch(a.live.rstrip('/') + '/firmware/versions.json'))
    except Exception:
        live = []
        try:   # before versions.json existed: its one build
            live = [json.loads(fetch(a.live.rstrip('/') + '/firmware/latest.json'))]
        except Exception as e:
            print('nothing from the live site:', e)
    for meta in live:
        try:
            add(meta, fetch(a.live.rstrip('/') + '/' + meta['url']), 'kept from the live site')
        except Exception as e:
            print(f"couldn't fetch {meta.get('version')}: {e}")

    # the seeds, unless the live site had them
    for meta in json.load(open(os.path.join(a.seed, 'versions.json'))):
        if meta['version'] not in have:
            add(meta, open(os.path.join(a.seed, os.path.basename(meta['url'])), 'rb').read(), 'from the archive')

    # this build, on top
    cur = json.load(open(os.path.join(a.build, 'latest.json')))
    cur['date'] = datetime.date.today().isoformat()
    cur['note'] = note_for(a.changelog, cur['version'])
    old = have.get(cur['version'])
    if old and old['sha256'] != cur['sha256']:
        print(f"{cur['version']} was published with another build: this one replaces it")
    add(cur, open(os.path.join(a.build, 'viscera.fwsc'), 'rb').read(), 'this build')
    for k in ('date', 'note'):   # keep the first date and any note set by hand
        if old and old.get(k) and old['sha256'] == cur['sha256']:
            have[cur['version']][k] = old[k]

    out = sorted(have.values(), key=lambda m: vkey(m['version']), reverse=True)
    json.dump(out, open(os.path.join(fw, 'versions.json'), 'w'), indent=1)
    shutil.copy(os.path.join(a.build, 'latest.json'), os.path.join(fw, 'latest.json'))
    print('versions:', ', '.join(m['version'] for m in out))


if __name__ == '__main__':
    main()
