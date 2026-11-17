'use strict';
/**
 * hud_test.js - a test picture on the HUD while its size and place are set: where the film will go (its shape, size
 * and place worked out as the receiver does, native/uxplay-mgu/renderer/mgu_hud.c) for 10 s after the last change.
 * The app draws it itself through cluster-render (cluster_render_client.js, MPL-2.0, from robertnisipeanu/idrive-apps
 * native/cluster-render/js), so it shows whether the receiver runs or not, over the film when one is on.
 *
 *   const t = createHudTest({ ClusterRender, log, now });
 *   t.show({ size, x, y, aspect })   // shows it (or moves it) for the next 10 s
 *   t.tick(allowed)                  // each second: gone when its time is up or the HUD may not show
 *   t.close()
 */
const SHOW_MS = 10000;

/* the receiver's layout: the shape fitted into size % of the HUD (hw x hh), at x %, y % of the room left */
function layout(hw, hh, size, x, y, aspect) {
    const bw = Math.floor(hw * size / 100), bh = Math.floor(hh * size / 100);
    let w, h;
    if (aspect > bw / bh) { w = bw; h = Math.floor(bw / aspect); } else { h = bh; w = Math.floor(bh * aspect); }
    w &= ~3; h &= ~1;
    return { x: Math.floor((hw - w) * x / 100) & ~1, y: Math.floor((hh - h) * y / 100) & ~1, w, h };
}

/* colour bars above, a grey ramp below, a white frame and cross: w x h RGBA */
function pattern(w, h) {
    const px = Buffer.alloc(w * h * 4);
    const BARS = [[235, 235, 235], [235, 235, 16], [16, 235, 235], [16, 235, 16], [235, 16, 235], [235, 16, 16], [16, 16, 235]];
    const edge = Math.max(2, Math.round(Math.min(w, h) / 60)), cx = w >> 1, cy = h >> 1;
    for (let y = 0; y < h; y++) {
        for (let x = 0; x < w; x++) {
            let c;
            if (x < edge || y < edge || x >= w - edge || y >= h - edge) c = [255, 255, 255];
            else if (Math.abs(x - cx) < edge / 2 + 0.5 || Math.abs(y - cy) < edge / 2 + 0.5) c = [255, 255, 255];
            else if (y < h * 0.7) c = BARS[Math.min(6, Math.floor(x * 7 / w))];
            else { const g = Math.round(16 + 219 * x / (w - 1)); c = [g, g, g]; }
            const i = (y * w + x) * 4;
            px[i] = c[0]; px[i + 1] = c[1]; px[i + 2] = c[2]; px[i + 3] = 255;
        }
    }
    return px;
}

function createHudTest(o) {
    const now = o.now || Date.now;
    let cr = null, ref = 0, rect = null, want = null, until = 0, hw = 800, hh = 480;

    function place() {
        if (!cr || !want) return;
        const r = layout(hw, hh, want.size, want.x, want.y, want.aspect);
        if (ref && rect && (r.w !== rect.w || r.h !== rect.h)) { cr.drop(ref); ref = 0; }
        if (!ref) {
            // over the film (class overlay), and over the car's own pop-ups on the HUD while it shows (yield 0)
            ref = cr.image({ target: 'hud', klass: 'overlay', yield: 0, x: r.x, y: r.y, w: r.w, h: r.h, iw: r.w, ih: r.h });
            if (ref) cr.pixels(ref, null, pattern(r.w, r.h));
        } else if (r.x !== rect.x || r.y !== rect.y) cr.set(ref, { x: r.x, y: r.y });
        rect = r;
    }

    function close() {
        if (cr) { try { cr.close(); } catch (e) { /* gone */ } if (o.log) o.log.info('hud test: off'); }
        cr = null; ref = 0; rect = null; want = null;
    }

    return {
        show(w) {
            want = w;
            until = now() + SHOW_MS;
            if (!cr) {
                cr = new o.ClusterRender('hp_airplay');
                cr.on('ready', () => {
                    const m = /(?:^|,)hud:(\d+)x(\d+)/.exec(cr.targets || '');
                    if (m) { hw = Number(m[1]); hh = Number(m[2]); }
                    place();
                });
                cr.connect();
                if (o.log) o.log.info('hud test: on');
            }
            if (cr.ready) place();
        },
        tick(allowed) { if (cr && (!allowed || now() > until)) close(); },
        close,
        state: () => ({ on: !!cr, rect }),
    };
}

module.exports = { createHudTest, layout, pattern, SHOW_MS };
