'use strict';
/**
 * AirPlay HP - the head unit as an AirPlay receiver ("BMW iDrive") for an iPhone.
 *
 *   Screen mirroring  Control Center > Screen Mirroring > BMW iDrive: the iPhone's screen on the centre screen and
 *                     its sound on the car's speakers, with wireless CarPlay connected (UxPlay's protocol presented
 *                     as an Apple TV 5,4; the GPU decodes the picture).
 *   A video           the AirPlay button in a video (YouTube) > BMW iDrive: the head unit plays it itself from the
 *                     internet (mgu-uplink), with its controls on the iDrive (controls.js); iOS plays it this way
 *                     only while the iPhone is not in a CarPlay session (setting "AirPlay video button", on).
 * The picture shows while the car is parked (setting "Picture only while parked", on), over the whole screen
 * (mirror_view.js), a film in it without its black bars (setting "Film on the whole screen", on), and on the HUD too
 * (setting "Picture on the HUD": off, only while parked, or also while driving: this car's HUD is moved to the
 * passenger's side); the iDrive controller's buttons hide a mirrored screen, "Start AirPlay" shows it again.
 *
 * The receiver runs in the service (native/hp-airplay, a root tool) from "Start AirPlay" for an hour, longer
 * while an iPhone uses it; the app talks to it over /run/hp-airplay.sock (a JSON object per line).
 *
 * The page's texts are Polish (app.json "language": "pl" gives the kit's own texts in Polish too); the log stays
 * English.
 */
const net = require('net');
const mgu = require('./kit/mgu');
const { withAlpha } = require('./kit/canvas');
const G = require('./kit/gauges');
const fs = require('fs');
const record = require('./kit/record');
const Parked = require('./kit/parked');
const { RenderClient } = require('./mgu_render_client');
const { createMirrorView } = require('./mirror_view');
const { createControls, videoRect } = require('./controls');
const { createCarAudio } = require('./car_audio');

const W = 1600, H = 520;
const SOCK = process.env.MYAIRPLAY_SOCK || '/run/hp-airplay.sock';
// the receiver's time from "Start AirPlay"; while an iPhone uses it, it is lengthened before it ends
const MINUTES = 60;
const EXTEND_BELOW_MS = 10 * 60000;
// the service version this app is built with: an older one still running (its files were updated, the
// process not) is asked to end, and the socket starts the new one at the next request
const SERVICE_VERSION = '1.5.0';
const older = (a, b) => { const x = String(a).split('.').map(Number), y = String(b).split('.').map(Number); for (let i = 0; i < 3; i++) { if ((x[i] || 0) !== (y[i] || 0)) return (x[i] || 0) < (y[i] || 0); } return false; };
const GREEN = '#2fd27a', AMBER = '#ffb020', RED = '#ef2b2d';

/** one request: the service's status, or null when it does not answer */
function ask(cmd, extra) {
    return new Promise((res) => {
        let buf = '', done = false;
        const end = (v) => { if (!done) { done = true; clearTimeout(t); try { c.destroy(); } catch (e) { /* gone */ } res(v); } };
        const c = net.connect(SOCK);
        const t = setTimeout(() => end(null), 4000);
        c.setEncoding('utf8');
        c.on('connect', () => c.write(JSON.stringify(Object.assign({ cmd }, extra || {})) + '\n'));
        c.on('data', (d) => {
            buf += d;
            const i = buf.indexOf('\n');
            if (i < 0) return;
            try { end(JSON.parse(buf.slice(0, i))); } catch (e) { end(null); }
        });
        c.on('error', () => end(null));
    });
}

function clip(cv, s, size, w, font) {
    s = String(s);
    if (cv.textWidth(s, size, { font, literal: true }) <= w) return s;
    while (s.length > 1 && cv.textWidth(s + '...', size, { font, literal: true }) > w) s = s.slice(0, -1);
    return s + '...';
}
const hms = (x) => { x = Math.max(0, Math.round(x || 0)); const h = Math.floor(x / 3600), m = Math.floor(x / 60) % 60, sec = x % 60; return (h ? h + ':' + String(m).padStart(2, '0') : m) + ':' + String(sec).padStart(2, '0'); };
const mb = (b) => b >= 1048576 ? (b / 1048576).toFixed(1) + ' MB' : Math.round((b || 0) / 1024) + ' KB';
const NET_NAMES = { 'car-wifi': 'Wi-Fi auta', 'iphone-hotspot': 'hotspot iPhone\'a', uplink: 'łącze uplink', wifi: 'Wi-Fi' };
/* n with its Polish noun: 1 przerwa, 2-4 przerwy (not 12-14), else przerw */
const plural = (n, one, few, many) => n + ' ' + (n === 1 ? one : n % 10 >= 2 && n % 10 <= 4 && (n % 100 < 12 || n % 100 > 14) ? few : many);
/* the receiver's HUD reasons (renderer/mgu_hud.c), in Polish; cluster-render's own pass as they are */
const HUD_REASONS = {
    'cluster-render is not installed: add it from the Store': 'brak cluster-render: dodaj go ze Store (robertnisipeanu/idrive-apps)',
    "no access to cluster-render's socket": 'brak dostępu do cluster-render',
    'cluster-render has no HUD on this car': 'cluster-render nie widzi HUD w tym aucie',
    'cluster-render closed the connection': 'cluster-render zamknął połączenie',
    'cluster-render sent a message too long': 'cluster-render przysłał zbyt długą wiadomość',
    'no picture yet': 'czekam na obraz',
    'no new picture': 'brak nowego obrazu',
    'the HUD is not available': 'HUD niedostępny',
};

/**
 * What the page says: { head: { text, tone }, rows: [{ label, state: 'ok'|'wait'|'bad'|'off', detail }] } from the
 * service's status (null: no service), the picture's state (mirror_view) and the speakers' (car_audio).
 * Exported for the tests and the screenshots.
 */
function view(s, pic, ca, opts) {
    const o = opts || {};
    if (!s) return { head: { text: 'Usługa AirPlay HP nie odpowiada. Instaluje się z aplikacją ze Store: po instalacji uruchom jednostkę ponownie.', tone: 'bad' }, rows: [] };
    const on = s.mode === 'receiver' && s.active;
    const r = on ? (s.rx || {}) : {};
    const st = r.stream || {}, h = r.hls, c = r.client;
    const who = (c && c.name) || 'iPhone';
    const playing = h && h.open && !h.ended;
    const mirroring = !h && r.video && r.videoRunning && st.frames > 0;
    const iface = (s.ifaces || []).find((i) => i.used && i.ip === r.ip);
    let head;
    if (!on) head = { text: 'Wyłączony. Naciśnij „Włącz AirPlay”.', tone: 'idle' };
    else if (r.error && !r.running) head = { text: 'Odbiornik się zatrzymał: ' + r.error, tone: 'bad' };
    else if (s.fw && s.fw.state === 'failed') head = { text: 'Nie udało się otworzyć zapory: ' + (s.fw.error || ''), tone: 'bad' };
    else if (h && h.ended && h.ended.why === 'error') head = { text: 'Nie udało się odtworzyć filmu: ' + h.ended.error, tone: 'bad' };
    else if (playing) head = { text: who + ': film ' + (h.paused ? 'wstrzymany' : h.buffering ? 'się wczytuje' : 'gra') + ', ' + hms(h.pos) + (h.dur ? ' / ' + hms(h.dur) : ''), tone: 'ok' };
    else if (h && !h.ended) head = { text: who + ' wysłał film: otwieram go z internetu', tone: 'wait' };
    else if (mirroring) head = { text: who + ': odbicie lustrzane ekranu', tone: 'ok' };
    else if (c) head = { text: who + ' jest połączony', tone: 'wait' };
    else if (!r.running) head = { text: 'Uruchamianie...', tone: 'wait' };
    else head = { text: 'Gotowy: na iPhonie Odbicie lustrzane ekranu > ' + (r.name || 'BMW iDrive'), tone: 'ready' };
    if (!on) return { head, rows: [] };

    const rows = [];
    rows.push({ label: 'Odbiornik', state: r.running ? 'ok' : r.error ? 'bad' : 'wait',
        detail: r.running ? '„' + (r.name || 'BMW iDrive') + '” w sieci ' + (iface ? NET_NAMES[iface.kind] || iface.name : r.iface || '?') : r.error || 'uruchamianie' });
    rows.push({ label: 'iPhone', state: c || h ? 'ok' : 'off', detail: c ? (c.name || 'iPhone') : h ? 'wysłał film' : 'jeszcze brak' });
    // the picture: a mirrored screen or a video, and why it is not on the screen
    let pd = 'jeszcze brak', ps = 'off';
    if (playing) {
        const op = h.open;
        pd = (op.vcodec ? op.vcodec.toUpperCase() : 'film') + (r.decoder && r.decoder.ok && r.decoder.w ? ' ' + r.decoder.w + 'x' + r.decoder.h : '') + (st.fps > 0 ? ', ' + st.fps + ' kl./s' : '');
        ps = 'ok';
    } else if (mirroring) {
        pd = 'ekran ' + r.video.w + 'x' + r.video.h + (st.fps > 0 ? ', ' + st.fps + ' kl./s' : '');
        ps = 'ok';
    }
    if (r.decoder && !r.decoder.ok) { ps = 'bad'; pd = 'dekoder GPU zawiódł: ' + r.decoder.error; }
    else if (ps === 'ok' && pic && !pic.shown) { ps = 'wait'; pd += '; ' + pic.why; }
    rows.push({ label: 'Obraz', state: ps, detail: pd });
    // the sound
    const snd = r.sound;
    let sd = 'jeszcze brak', ss = 'off';
    if (snd && !snd.ok) { ss = 'bad'; sd = snd.error; }
    else if (snd && r.soundRunning) {
        if (ca && ca.taken) { ss = 'bad'; sd = 'głośniki zajęte przez inną aplikację (Emby)'; }
        else if (ca && ca.error) { ss = 'bad'; sd = ca.error; }
        else if (ca && ca.connected) { ss = 'ok'; sd = 'w głośnikach auta' + (st.agaps ? ', ' + plural(st.agaps, 'przerwa', 'przerwy', 'przerw') : ''); }
        else { ss = 'wait'; sd = 'łączenie głośników'; }
    }
    rows.push({ label: 'Dźwięk', state: ss, detail: sd });
    // the HUD (setting hudMode)
    if (o.hud) {
        const hu = r.hud || {};
        const why = (x) => HUD_REASONS[x] || x;
        let hs = 'off', hd = 'jeszcze brak';
        if (o.driving) hd = 'wyłączony w czasie jazdy';
        else if (hu.state === 'shown') { hs = 'ok'; hd = 'obraz na HUD'; }
        else if (hu.state === 'connecting' || hu.state === 'waiting') { hs = 'wait'; hd = why(hu.reason) || 'łączenie'; }
        else if (hu.state === 'unavailable' || hu.state === 'error') { hs = 'bad'; hd = why(hu.reason) || hu.state; }
        rows.push({ label: 'HUD', state: hs, detail: hd });
    }
    // the internet, for a video sent with the AirPlay button
    if (r.hlsOn) {
        const px = r.proxy || {}, ps2 = px.stats || {};
        rows.push({ label: 'Internet', state: px.state === 'on' ? 'ok' : px.state === 'failed' ? 'bad' : 'wait',
            detail: px.state === 'on' ? (ps2.bytesIn ? mb(ps2.bytesIn) + ' na filmy' : 'gotowy na filmy (mgu-uplink)') : px.error || 'uruchamianie' });
    } else if (o.videoButtonOff) rows.push({ label: 'Filmy', state: 'off', detail: 'przycisk AirPlay w filmie wyłączony w Ustawieniach' });
    return { head, rows };
}

mgu.run({
    async start(ctx) {
        const { ui, log } = ctx;
        let st = null, busy = null, polling = false, askedExit = false, stAt = Date.now();

        async function poll() {
            if (polling) return;
            polling = true;
            try {
                st = await ask('status');
                stAt = Date.now();
                // an iPhone using the receiver: its time is lengthened before it ends
                if (st && st.active && st.mode === 'receiver' && st.rx && (st.rx.client || st.rx.hls) && st.endsAt - st.now < EXTEND_BELOW_MS) {
                    const r = await ask('start', startArgs());
                    if (r) { st = r; log.info('myairplay: an iPhone is using it: the receiver goes on for another ' + MINUTES + ' min'); }
                }
                if (st && st.version && older(st.version, SERVICE_VERSION) && !st.active && !askedExit) {
                    askedExit = true;   // once: a service whose update failed is not stopped again and again
                    log.info('myairplay: service ' + st.version + ' running, ' + SERVICE_VERSION + ' installed: asking it to end');
                    busy = 'Aktualizacja usługi...';
                    await ask('exit');
                    st = await ask('status');
                    busy = null;
                }
            } finally { polling = false; }
        }
        async function command(cmd, label, extra) {
            busy = label;
            const r = await ask(cmd, cmd === 'start' ? Object.assign({ minutes: MINUTES }, extra || {}) : null);
            busy = null;
            if (r) st = r;
            log.info('myairplay: ' + cmd + ' -> ' + (r ? r.verdict.code : 'no service'));
            return render();
        }
        // the receiver as it worked for everything in the car: Apple TV 5,4 "BMW iDrive" on the car Wi-Fi first
        const startArgs = () => ({ mode: 'receiver', hls: ctx.settings.get('videoButton') !== false, identity: 'carplay', net: 'car' });
        ui.onClick('BTN_RX', () => { mirror.wantAgain(); return command('start', 'Uruchamianie...', startArgs()); });

        /* ---------- stage 2: the picture over the screen, parked only ---------- */
        const parked = Parked.watch(ctx, log, { strict: true });
        // "Picture only while parked" (on by default) and the car moving: no picture, on the screen or on the HUD
        const driving = () => ctx.settings.get('parkedOnly') !== false && parked.moving();
        // the HUD's own setting: off, only while parked, or also while driving (this car's HUD is moved to the
        // passenger's side, where only the passenger sees it)
        const hudMode = () => (['parked', 'always'].indexOf(ctx.settings.get('hudMode')) >= 0 ? ctx.settings.get('hudMode') : 'off');
        const hudDriving = () => hudMode() === 'parked' && parked.moving();
        const mirrorOpts = { RenderClient, Recorder: record.Recorder, kit: record, appId: ctx.spec.id, log,
            overlay: () => videoOverlay(),
            // a played video's controls (controls.js, Emby's): touch and the controller go to them while it shows;
            // screen mirroring keeps the controller's buttons for hiding the picture
            onTouch: (phase, x, y, pl) => { if (pl && videoOn() && controls.touch(phase, x, y, pl, barInfo())) mirror.tick(); },
            onCcu: (c) => { if (!videoOn()) return false; const r = controls.ccu(c); mirror.tick(); return r; },
            onShown: (on) => pageTouch(!on),
            statIno: (p) => { const f = fs.statSync(p); return f.isFile() ? f.ino : null; } };
        Object.defineProperty(mirrorOpts, 'hintText', { get: () => (videoOn()
            ? 'Dotknij obrazu lub użyj kontrolera iDrive, by sterować' : 'Naciśnij przycisk kontrolera iDrive, by wrócić') });
        const mirror = createMirrorView(mirrorOpts);
        // The app's page is drawn by the kit on its own connection to mgu-render, which claims the page and its bar for
        // touch (found by Emby, 0.3.0 in the car: with the film over it, a touch went to the page's buttons underneath).
        // While the picture shows, the page claims nothing.
        const disp = ctx.display;
        let pageClaims = true;
        if (disp && typeof disp._claimTabs === 'function') {
            const claimTabs = disp._claimTabs.bind(disp);
            disp._claimTabs = (rects, r) => {
                if (pageClaims) return claimTabs(rects, r);
                disp.tabs = [];
                if (disp.gpu && disp.gpu.claim) disp.gpu.claim([]);
                return undefined;
            };
        }
        function pageTouch(on) {
            if (on === pageClaims) return;
            pageClaims = on;
            try {
                if (!on) { if (disp && disp.gpu && disp.gpu.claim) disp.gpu.claim([]); }
                else if (disp) { disp.lastWords = null; if (disp._repaint) disp._repaint(); }
            } catch (e) { log.warn('airplay page touch: ' + e); }
        }

        /* ---------- a played video's controls (controls.js) and shape: fit, zoom, fill ---------- */
        // the video plays at its own size in the frame file (the player's decoder: stage 4), placed here
        const ASPECTS = ['fit', 'zoom', 'fill'];
        const aspect = () => (ASPECTS.indexOf(ctx.settings.get('aspect')) >= 0 ? ctx.settings.get('aspect') : 'fill');
        const hlsOf = () => { const r = st && st.mode === 'receiver' && st.active ? st.rx : null; return r && r.hls && r.hls.open && !r.hls.ended ? r.hls : null; };
        const videoOn = () => !!hlsOf();
        // the position between two statuses (one a second): on from the last one while it plays
        function posNow() {
            const h = hlsOf();
            if (!h) return 0;
            return Math.max(0, (h.pos || 0) + (h.paused || h.buffering ? 0 : (Date.now() - stAt) / 1000));
        }
        // a seek from the bar: the target at once; it goes once the buttons rest (several taps on +30 s, one seek)
        let seekTo = null, seekTimer = null, seekSentAt = 0;
        function seekBy(d) { seekAt((seekTo != null ? seekTo : posNow()) + d); }
        function seekAt(t) {
            const h = hlsOf();
            seekTo = Math.max(0, h && h.dur ? Math.min(t, h.dur - 2) : t);
            clearTimeout(seekTimer);
            seekTimer = setTimeout(() => {
                seekSentAt = Date.now();
                ask('video', { op: 'seek', to: seekTo }).then((r) => { if (r) { st = r; stAt = Date.now(); } });
            }, 800);
            mirror.tick();
        }
        const controls = createControls({
            // no Stop: from the iDrive it ended the iPhone's session, and YouTube went to "Something went wrong"
            buttons: ['back10', 'playpause', 'fwd30', 'aspect', 'exit'],
            act: (a) => {
                const h = hlsOf();
                if (a && a.seek != null) return seekAt(a.seek);
                if (a === 'back10') return seekBy(-10);
                if (a === 'fwd30') return seekBy(30);
                if (a === 'playpause') return ask('video', { op: h && h.paused ? 'resume' : 'pause' }).then((r) => { if (r) { st = r; stAt = Date.now(); } mirror.tick(); });
                if (a === 'aspect') { ctx.settings.set('aspect', ASPECTS[(ASPECTS.indexOf(aspect()) + 1) % ASPECTS.length]); log.info('airplay: aspect ' + aspect()); return mirror.tick(); }
                if (a === 'exit') return mirror.hide('the controls');
            },
        });
        function barInfo() {
            const h = hlsOf() || {};
            if (seekTo != null && seekSentAt && (Date.now() - seekSentAt > 15000 || (Math.abs((h.pos || 0) - seekTo) < 5 && Date.now() - seekSentAt > 1500))) seekTo = null;
            return { title: 'Film AirPlay', pos: posNow(), dur: h.dur, paused: !!h.paused, aspect: aspect(), seeking: seekTo };
        }
        let wasPaused = false;
        // a film's rect without its black bars (the receiver finds it, video/src/film_bars.cpp), while "Film na cały
        // ekran" is on
        const filmOf = () => (ctx.settings.get('filmFill') !== false && st && st.rx && st.rx.film) || null;
        function videoOverlay() {
            const d = st && st.rx && st.rx.decoder, fw = d && d.ok ? d.w : 0, fh = d && d.ok ? d.h : 0, mode = aspect(), film = filmOf();
            if (!videoOn()) {
                // screen mirroring: the whole screen as the decoder fitted it, or a film in it placed by its shape
                if (!film || !fw) return null;
                return { key: ['mirror', mode, fw, fh, film.x, film.y, film.w, film.h].join('#'), rect: (p) => videoRect(mode, fw, fh, p, film) };
            }
            const info = barInfo();
            if (info.paused !== wasPaused) { wasPaused = info.paused; controls.keepOpen(info.paused); }   // paused: the bar stays
            const bar = controls.key(info);
            return {
                key: [mode, fw, fh, bar, film ? [film.x, film.y, film.w, film.h].join(',') : ''].join('#'),
                rect: (p) => videoRect(mode, fw, fh, p, film),
                draw: (rec, p) => { if (bar) controls.draw(rec, p, info); },
            };
        }
        // the bar's time moves on or it hides: a new frame (only then)
        setInterval(() => { if (mirror.state().shown) mirror.tick(); }, 100);
        // leaving the app (HOME, another app, CarPlay's tile) ends the iPhone's mirroring, so CarPlay is free at once
        // (setting endOnLeave, on): 1.5 s after the page went, once per session
        let goneAt = 0, ended = false, hudAsked = false;
        // stage 3: the speakers while the receiver has an iPhone with sound and the app is up (leaving it ends the session)
        const carAudio = createCarAudio(ctx.signals, log, { file: ctx.dataDir ? require('path').join(ctx.dataDir, 'car-audio.json') : null });
        function syncMirror() {
            const rxOn = st && st.mode === 'receiver' && st.active ? st.rx : null;
            // "Picture only while parked" (on by default): off, the picture stays while driving (for passengers)
            mirror.update({ rx: rxOn, pageVisible: shown(), driving: driving() });
            // the speakers while the iPhone's sound plays: stopped on the phone, they go back to the source before at once
            carAudio.want(!!(rxOn && rxOn.running && rxOn.soundRunning && shown()));
            // the HUD (setting hudMode): asked for each second while it should show, and off 3 s after the last ask
            // (renderer/mgu_hud.c)
            const picture = !!(rxOn && rxOn.running && (rxOn.videoRunning || (rxOn.hls && rxOn.hls.open && !rxOn.hls.ended)));
            const hudWant = hudMode() !== 'off' && picture && !hudDriving();
            if (hudWant || hudAsked) {
                hudAsked = hudWant;
                ask('hud', { on: hudWant }).then((r) => { if (r) st = r; });
            }
            if (shown()) { goneAt = 0; ended = false; return; }
            if (!rxOn || !rxOn.client || ended || ctx.settings.get('endOnLeave') === false) return;
            if (!goneAt) goneAt = Date.now();
            if (Date.now() - goneAt >= 1500) {
                ended = true;
                log.info('airplay: the app left the front: ending the iPhone\'s mirroring');
                ask('end-session', { why: 'the app left the front' }).then((r) => { if (r) st = r; });
            }
        }
        if (ctx.app && ctx.app.on) ctx.app.on('shutdown', () => { mirror.close(); carAudio.want(false); if (hudAsked) ask('hud', { on: false }); });
        ui.onClick('BTN_STOP', () => command('stop', 'Zatrzymywanie...'));

        function render() {
            const t = ctx.theme();
            const cv = ctx.canvas(W, H).clear(t.bg);
            const on = st && st.active && st.mode === 'receiver';
            const left = on ? Math.max(0, Math.round((st.endsAt - st.now) / 60000)) : null;
            const inUse = on && st.rx && (st.rx.client || st.rx.hls);
            G.frame(cv, t, ['AirPlay HP'], 0, [busy && { text: busy, color: 'accent' }, on && !inUse && left <= 10 && { text: 'WYŁ. ZA ' + left + ' MIN' }]);
            draw(cv, t);
            return ui.image('MAIN_GFX', cv);
        }

        function draw(cv, t) {
            const v = view(st, mirror.state(), carAudio.state(), { videoButtonOff: ctx.settings.get('videoButton') === false,
                hud: hudMode() !== 'off', driving: hudDriving() });
            const TONE = { ok: GREEN, wait: AMBER, bad: RED, ready: GREEN, idle: t.accent };
            const hc = TONE[v.head.tone] || t.accent;
            // the state, across the page
            cv.fillRect(24, 58, 1552, 84, withAlpha(hc, 0.14));
            cv.fillRect(24, 58, 6, 84, hc);
            cv.text(clip(cv, v.head.text, 30, 1500, 'bold'), 52, 112, 30, t.text, { font: 'bold', literal: true });
            // off: what "Start AirPlay" does
            if (!v.rows.length && st) {
                ['„Włącz AirPlay” zmienia jednostkę w odbiornik AirPlay', '„BMW iDrive” na godzinę; dłużej, gdy iPhone go używa.']
                    .forEach((l, i) => cv.text(l, 52, 200 + i * 34, 24, t.dim, { literal: true }));
            }
            // the rows
            const COL = { ok: GREEN, wait: AMBER, bad: RED, off: withAlpha('#ffffff', 0.3) };
            v.rows.forEach((r, i) => {
                const y = 200 + i * 56;
                cv.circle(46, y - 9, 10, COL[r.state]);
                cv.text(r.label, 70, y, 26, t.text, { font: 'bold', literal: true });
                cv.text(clip(cv, r.detail || '', 24, 650, 'label'), 250, y, 24, r.state === 'off' ? t.dim : t.text, { literal: true });
            });
            // how to use it
            const x = 960;
            cv.fillRect(x - 20, 166, 1.5, 320, withAlpha('#ffffff', 0.15));
            const step = (y, title, lines) => {
                cv.text(title, x, y, 24, t.accent, { font: 'bold', literal: true });
                lines.forEach((l, k) => cv.text(l, x, y + 34 + k * 30, 22, t.text, { literal: true }));
            };
            step(200, 'ODBICIE LUSTRZANE EKRANU', ['iPhone: Centrum sterowania > Odbicie lustrzane', '> BMW iDrive. Działa przy połączonym CarPlay.']);
            step(330, 'FILM (YOUTUBE)', ['Przycisk AirPlay w filmie > BMW iDrive.', 'Tylko gdy iPhone nie jest w CarPlay.']);
            cv.text('Dotknij obrazu lub użyj kontrolera, by sterować filmem', x, 470, 18, t.dim, { literal: true });
        }

        // only while the page shows: in the background the app leaves the service alone, so it can end
        // 10 minutes after the last request
        function shown() { return !ui.isVisible || ui.isVisible('MAIN'); }
        setInterval(() => { if (shown()) poll().catch(() => {}); syncMirror(); }, 1000);
        ui.onVisible(() => syncMirror());
        await poll();
        ctx.loop('MAIN', 2, render);
        render().then(() => log.info('AirPlay HP first frame done')).catch((e) => log.warn('first render: ' + e));
    },
});

module.exports = { view, ask };
