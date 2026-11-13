#!/usr/bin/env python3
"""Put every published version behind the installer, not only the newest.

The Pages site is rebuilt from scratch on each deploy, so the published versions
come back from the GitHub releases (each has viscera-X.Y.Z.fwsc and its
viscera-X.Y.Z.json; releases without one, the 0.x ones, aren't offered), checked
against their SHA-256. The seeds in firmware-archive/ fill in any version a release
doesn't have, and this build goes on top. Writes site/firmware/versions.json, newest
first, each entry the build's latest.json plus a date and a one-line note.

  GH_TOKEN=... python3 tools/site_versions.py --build build --site site \
      --seed firmware-archive --repo De-Pattern/viscera --changelog CHANGELOG.md
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


def fetch(url, accept='application/vnd.github+json'):
    h = {'Accept': accept, 'User-Agent': 'viscera-site'}
    if os.environ.get('GH_TOKEN') and url.startswith('https://api.github.com/'):
        h['Authorization'] = 'Bearer ' + os.environ['GH_TOKEN']
    with urllib.request.urlopen(urllib.request.Request(url, headers=h), timeout=60) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', required=True)
    ap.add_argument('--site', required=True)
    ap.add_argument('--seed', required=True)
    ap.add_argument('--repo', required=True)
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

    # the releases (Pages keeps only the last deploy; releases keep every version)
    seeds = {m['version']: m for m in json.load(open(os.path.join(a.seed, 'versions.json')))}
    try:
        rels = json.loads(fetch(f'https://api.github.com/repos/{a.repo}/releases?per_page=100'))
    except Exception as e:
        rels = []
        print('no releases read:', e)
    for rel in rels:
        if rel.get('draft'):
            continue
        assets = {x['name']: x for x in rel.get('assets', [])}
        v = rel['tag_name'].lstrip('v')
        fj, ff = assets.get(f'viscera-{v}.json'), assets.get(f'viscera-{v}.fwsc')
        if not fj or not ff:
            continue
        try:
            meta = json.loads(fetch(fj['browser_download_url'], '*/*'))
            meta['date'] = (rel.get('published_at') or '')[:10]
            meta['note'] = (seeds.get(v) or {}).get('note') or note_for(a.changelog, v)
            add(meta, fetch(ff['browser_download_url'], '*/*'), 'from its release')
        except Exception as e:
            print(f"couldn't fetch {v}: {e}")

    # the seeds, unless a release had them
    for meta in seeds.values():
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
    for k in ('date', 'note'):   # keep the release's date and any note set by hand
        if old and old.get(k) and old['sha256'] == cur['sha256']:
            have[cur['version']][k] = old[k]

    out = sorted(have.values(), key=lambda m: vkey(m['version']), reverse=True)
    json.dump(out, open(os.path.join(fw, 'versions.json'), 'w'), indent=1)
    shutil.copy(os.path.join(a.build, 'latest.json'), os.path.join(fw, 'latest.json'))
    print('versions:', ', '.join(m['version'] for m in out))


if __name__ == '__main__':
    main()
