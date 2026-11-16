#!/usr/bin/env python3
"""The GitHub release `catalog`'s files, from catalog.json and packages/:

    python3 tools/release_catalog.py <owner>/<repo> <out dir>

<out dir> gets catalog.json, with each artifact's url the release asset's download
(https://github.com/<owner>/<repo>/releases/download/catalog/<name>) and the icon the asset's name, and the
packages and icon it names. The Store then reads the repository as a GitHub source on Releases
(Store > Settings > Add source > <owner>/<repo>). Run by .github/workflows/catalog.yml.
"""
import json, os, shutil, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    if len(sys.argv) != 3 or sys.argv[1].count('/') != 1:
        sys.exit(__doc__)
    repo, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    cat = json.load(open(os.path.join(ROOT, 'catalog.json')))
    for c in cat['components']:
        a = c['artifact']
        shutil.copy(os.path.join(ROOT, a['url']), os.path.join(out, a['name']))
        a['url'] = 'https://github.com/%s/releases/download/catalog/%s' % (repo, a['name'])
        if 'icon' in c:
            shutil.copy(os.path.join(ROOT, c['icon']), os.path.join(out, os.path.basename(c['icon'])))
            c['icon'] = os.path.basename(c['icon'])
    with open(os.path.join(out, 'catalog.json'), 'w') as f:
        json.dump(cat, f, indent=2, ensure_ascii=False)
        f.write('\n')
    print('\n'.join(sorted(os.listdir(out))))


if __name__ == '__main__':
    main()
