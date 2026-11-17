'use strict';
/**
 * The HUD's test picture (apps/hp_airplay/hud_test.js): node tests/airplay_hud_test.test.js
 * Against a fake cluster-render client: the layout as the receiver works it out, the picture made where the film will
 * go, moved for a new place, made again for a new size, gone after 10 s or when the HUD may not show.
 */
const assert = require('assert');
const path = require('path');
const EventEmitter = require('events');
const { createHudTest, layout, pattern, SHOW_MS } = require(path.join(__dirname, '..', 'apps', 'hp_airplay', 'hud_test'));

class FakeCR extends EventEmitter {
    constructor(app) { super(); FakeCR.last = this; this.app = app; this.calls = []; this.ready = false; this.targets = ''; this.next = 1; }
    connect() { this.calls.push('connect'); }
    close() { this.calls.push('close'); }
    image(o) { this.calls.push(['image', o.target, o.klass, o.x, o.y, o.w, o.h, o.iw, o.ih].join(' ')); return this.next++; }
    pixels(ref, rects, px) { this.calls.push('pixels ' + ref + ' ' + px.length); }
    set(ref, o) { this.calls.push('set ' + ref + ' ' + o.x + ',' + o.y); }
    drop(ref) { this.calls.push('drop ' + ref); }
    hello(targets) { this.ready = true; this.targets = targets; this.emit('ready'); }
}

const tests = [];
const test = (n, f) => tests.push({ n, f });

test('layout: the film shape fitted into size % of the HUD, placed in the room left', () => {
    assert.deepStrictEqual(layout(800, 480, 40, 50, 50, 16 / 9), { x: 240, y: 150, w: 320, h: 180 });
    assert.deepStrictEqual(layout(800, 480, 40, 0, 0, 16 / 9), { x: 0, y: 0, w: 320, h: 180 });
    assert.deepStrictEqual(layout(800, 480, 40, 100, 100, 16 / 9), { x: 480, y: 300, w: 320, h: 180 });
    assert.deepStrictEqual(layout(800, 480, 100, 50, 50, 1560 / 720), { x: 0, y: 56, w: 800, h: 368 });   // as the receiver
});

test('pattern: RGBA, a white frame', () => {
    const p = pattern(40, 30);
    assert.strictEqual(p.length, 40 * 30 * 4);
    assert.deepStrictEqual([p[0], p[1], p[2], p[3]], [255, 255, 255, 255]);
});

test('shown where the film goes, moved, made again, gone after 10 s or when it may not show', () => {
    let t = 1000;
    const h = createHudTest({ ClusterRender: FakeCR, now: () => t });
    h.show({ size: 40, x: 50, y: 50, aspect: 16 / 9 });
    const cr = FakeCR.last;
    assert.strictEqual(cr.app, 'hp_airplay');
    assert.deepStrictEqual(cr.calls, ['connect']);
    cr.hello('map:1088x480,panel:1920x720,hud:800x480');
    assert.deepStrictEqual(cr.calls.slice(1), ['image hud overlay 240 150 320 180 320 180', 'pixels 1 ' + 320 * 180 * 4]);
    h.show({ size: 40, x: 0, y: 100, aspect: 16 / 9 });
    assert.strictEqual(cr.calls[cr.calls.length - 1], 'set 1 0,300');
    h.show({ size: 60, x: 0, y: 100, aspect: 16 / 9 });
    assert.deepStrictEqual(cr.calls.slice(-3), ['drop 1', 'image hud overlay 0 210 480 270 480 270', 'pixels 2 ' + 480 * 270 * 4]);
    t += SHOW_MS - 1000;
    h.tick(true);
    assert.ok(h.state().on);
    t += 2000;
    h.tick(true);
    assert.ok(!h.state().on && cr.calls[cr.calls.length - 1] === 'close');
    // shown again, then the HUD may not show (driving): gone at once
    h.show({ size: 40, x: 50, y: 50, aspect: 16 / 9 });
    FakeCR.last.hello('hud:800x480');
    h.tick(false);
    assert.ok(!h.state().on);
});

(async () => {
    let failed = 0;
    for (const t of tests) {
        try { await t.f(); console.log('ok   ' + t.n); } catch (e) { failed++; console.log('FAIL ' + t.n + '\n     ' + (e && e.stack || e)); }
    }
    console.log(failed ? failed + ' failed' : 'all ' + tests.length + ' passed');
    process.exit(failed ? 1 : 0);
})();
