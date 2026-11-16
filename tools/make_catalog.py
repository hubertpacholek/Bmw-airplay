#!/usr/bin/env python3
"""Builds the Store catalog: packages/ and catalog.json at the repository root.

    sh native/uxplay-mgu/build.sh      # the receiver (Docker), once per change to native/uxplay-mgu
    python3 tools/make_catalog.py

The app (kit/tools/build.py) and the service (tools/pack_tools.py) go to packages/, and catalog.json lists them
with relative addresses, so the Store reads it as an HTTPS catalog:
https://raw.githubusercontent.com/<owner>/<repo>/main/catalog.json
"""
import glob, hashlib, json, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACKAGES = os.path.join(ROOT, 'packages')
APP, TOOL = 'hp_airplay', 'hp-airplay'
ICON = 'hp_airplay.png'

TEXTS = {
    APP: {
        'pl': {'title': 'AirPlay HP',
               'description': 'Odbiornik AirPlay dla iPhone\'a: odbicie lustrzane ekranu na środkowym ekranie z dźwiękiem w głośnikach auta, '
                              'wyrównane buforem, żeby filmy nie przycinały. Obraz tylko na postoju. Wymaga usługi AirPlay HP.'},
    },
    TOOL: {
        'pl': {'title': 'Usługa AirPlay HP',
               'description': 'Odbiornik AirPlay (protokół UxPlay) dla aplikacji AirPlay HP: obraz dekodowany na GPU, dźwięk do głośników auta, '
                              'bufor wyrównujący dla odbicia lustrzanego ekranu.'},
    },
}


def sha256(p):
    with open(p, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def artifact(name):
    p = os.path.join(PACKAGES, name)
    return {'name': name, 'size': os.path.getsize(p), 'sha256': sha256(p), 'url': 'packages/' + name}


def main():
    if not os.path.exists(os.path.join(ROOT, 'native', 'uxplay-mgu', 'build', 'uxplay-mgu')):
        sys.exit('native/uxplay-mgu/build/uxplay-mgu is missing: run sh native/uxplay-mgu/build.sh first')
    subprocess.check_call([sys.executable, os.path.join(ROOT, 'kit', 'tools', 'build.py'), APP], cwd=ROOT)
    subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'pack_tools.py')], cwd=ROOT)
    app = json.load(open(os.path.join(ROOT, 'apps', APP, 'app.json')))
    tool = json.load(open(os.path.join(ROOT, 'native', TOOL, 'component.json')))
    app_pkg = '%s@%s.tgz' % (APP, app['version'])
    for old in glob.glob(os.path.join(PACKAGES, APP + '@*.tgz')):
        os.remove(old)
    shutil.copy(os.path.join(ROOT, 'dist', app_pkg), os.path.join(PACKAGES, app_pkg))
    components = [
        {'id': TOOL, 'kind': 'tool', 'version': tool['version'], 'title': tool['title'], 'description': tool['description'],
         'author': 'hubertpacholek, aurelmarius', 'mguHmi': tool['mguHmi'], 'artifact': artifact('%s@%s.tgz' % (TOOL, tool['version'])),
         'requires': tool.get('requires', {}), 'uninstallable': True, 'texts': TEXTS[TOOL]},
        {'id': APP, 'kind': 'app', 'version': app['version'], 'title': app['title'], 'description': app['description'],
         'author': 'hubertpacholek, aurelmarius', 'mguHmi': ['ID7'], 'artifact': artifact(app_pkg),
         'requires': app.get('requires', {}), 'uninstallable': True, 'icon': ICON, 'iconSha256': sha256(os.path.join(ROOT, ICON)),
         'texts': TEXTS[APP]},
    ]
    with open(os.path.join(ROOT, 'catalog.json'), 'w') as f:
        json.dump({'schema': 2, 'name': 'AirPlay HP', 'components': components}, f, indent=2, ensure_ascii=False)
        f.write('\n')
    print('catalog.json: %s' % ', '.join('%s %s' % (c['id'], c['version']) for c in components))


if __name__ == '__main__':
    main()
