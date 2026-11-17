#!/usr/bin/env python3
"""Adds glyphs to the baked font atlases (kit/runtime/fonts/*.json) without baking the others again.

    python3 kit/tools/add_glyphs.py 'ąćęłńóśźżĄĆĘŁŃÓŚŹŻ„”'

Every size that has the text set (bake_fonts.TEXT: it has 'q', which the digit sets lack) gets the characters it
lacks, rendered from the family's font as bake_fonts.py renders them and packed below its atlas; the glyphs already
there keep their pixels and places, so text drawn before looks the same. Needs Pillow and the Ubuntu font (bake_fonts.FONTDIR).
"""
import base64, json, os, sys, zlib
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bake_fonts  # noqa: E402


def add(entry, font, chars):
    W, H = entry['atlasW'], entry['atlasH']
    atlas = Image.frombytes('L', (W, H), zlib.decompress(base64.b64decode(entry['alpha'])))
    new = []
    for ch in dict.fromkeys(chars):
        if str(ord(ch)) in entry['glyphs']:
            continue
        bbox = font.getbbox(ch, anchor='ls')
        w, h = max(0, bbox[2] - bbox[0]), max(0, bbox[3] - bbox[1])
        im = None
        if w and h:
            im = Image.new('L', (w, h), 0)
            ImageDraw.Draw(im).text((-bbox[0], -bbox[1]), ch, font=font, fill=255, anchor='ls')
        new.append((ch, im, bbox, font.getlength(ch)))
    if not new:
        return 0
    x, y, rowh, place = 0, H, 0, {}
    for ch, im, bbox, adv in new:
        if im is None:
            place[ch] = (0, 0, 0, 0)
            continue
        if x + im.width > W:
            x, y, rowh = 0, y + rowh + 1, 0
        place[ch] = (x, y, im.width, im.height)
        x += im.width + 1
        rowh = max(rowh, im.height)
    H2 = y + rowh + 1
    out = Image.new('L', (W, H2), 0)
    out.paste(atlas, (0, 0))
    for ch, im, bbox, adv in new:
        px, py, pw, ph = place[ch]
        if im is not None:
            out.paste(im, (px, py))
        entry['glyphs'][str(ord(ch))] = [px, py, pw, ph, bbox[0], bbox[1], round(adv, 2)]
    entry['atlasH'] = H2
    entry['alpha'] = base64.b64encode(zlib.compress(out.tobytes(), 9)).decode()
    return len(new)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    chars = sys.argv[1]
    for fam, (fname, var, sizes) in bake_fonts.FAMILIES.items():
        p = os.path.join(bake_fonts.OUT, fam + '.json')
        data = json.load(open(p, encoding='utf-8'))
        added = []
        for entry in data['sizes']:
            if str(ord('q')) not in entry['glyphs']:
                continue
            n = add(entry, bake_fonts.load(fname, var, entry['size']), chars)
            if n:
                added.append('%d: %d' % (entry['size'], n))
        if added:
            json.dump(data, open(p, 'w', encoding='utf-8', newline='\n'), separators=(',', ':'))
            print('%s: %s' % (fam, ', '.join(added)))


if __name__ == '__main__':
    main()
