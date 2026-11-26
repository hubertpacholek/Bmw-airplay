// SPDX-License-Identifier: GPL-3.0-or-later
/* mgu_video.cpp - stage 2 of the AirPlay receiver: the mirrored H.264 on the GPU, into mgu-render's frame file.
 *
 * UxPlay's renderer (renderer/mgu_renderer.c) hands every access unit it gets (Annex B, decrypted, SPS+PPS
 * before each IDR) to mgu_video_frame(). The first one opens the frame file (MGVF, 3 slots, I420) and the
 * Intel Media SDK decoder (decoder_msdk.cpp, from waze-on-cluster through Moonride: VA-API on
 * /dev/dri/renderD128, VPP fits the picture into the output size, black bars around); each decoded picture is
 * converted NV12 -> I420 into the next slot. The app (apps/hp_airplay) names the file to mgu-render
 * and draws it over the screen while the car is parked.
 *
 * MGU_VIDEO_PATH  the frame file (default /dev/shm/mgu-video-hp-airplay; mgu-render takes it when nobody owns it)
 * MGU_VIDEO_SIZE  WxH, the output (default 1920x720, the centre screen)
 * MGU_VIDEO_OFF   set: stage 1 behaviour, nothing decoded (a PC without the unit's GPU)
 *
 * One mutex: frames come from UxPlay's mirror thread, stop from another.
 */
#include <atomic>
#include <errno.h>
#include <mutex>
#include <vector>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>

#include "decoder_msdk.h"
#include "film_bars.h"
#include "mgvf.h"
#include "mgu_video.h"
#include "mfx/mfxvideo.h"

/* decoder_msdk.cpp asks this for HEVC only (Moonride's probe.cpp loads the unit's HEVC plugin); screen mirroring is
 * H.264 (the receiver does not offer H.265, features bit 42), so there is no HEVC here */
int hevc_plugin(mfxSession s, void *plugin) { (void) s; (void) plugin; return -1; }

namespace {
std::mutex g_mu;
woc::Decoder *g_dec = nullptr;
ytdec::Mgvf g_mgvf;
bool g_open = false, g_failed = false, g_reported = false, g_file = false;
/* stage 4: a played video's own shape: the decoder outputs the stream's size, and the frame file is made at the
 * first picture with that size (the app places it: fit, zoom, fill); mirroring keeps MGU_VIDEO_SIZE, fitted */
bool g_native = false;
unsigned long long g_shown = 0, g_errors = 0;
uint32_t g_w = 1920, g_h = 720;
std::string g_path = "/dev/shm/mgu-video-hp-airplay", g_err;
/* where the picture is in the frame file's frames, the letterbox bars left out (the HUD shows only it) */
volatile uint32_t g_pic[4] = {0, 0, 0, 0};
/* where a film is within the picture, its own black bars left out (film_bars.cpp): w 0 none; seq counts changes */
FilmBars g_bars;
volatile uint32_t g_film[4] = {0, 0, 0, 0};
volatile unsigned g_film_seq = 0;
/* The HUD's early pictures (renderer/mgu_hud.c): while it wants them, each mirrored picture is also kept here as it
 * comes out of the decoder, before it waits for its time on the screen, with that time; at half size when it is 1600
 * or wider (the HUD shows 800 at most). The HUD sends each ahead of the screen by the cluster's own delay, so both show
 * it together. A ring of kEarly (12: 400 ms at 30 fps, more than the jitter buffer holds), its own lock. */
const int kEarly = 12;
struct EarlyPic {
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0, rect[4] = {0, 0, 0, 0};
    int64_t due_ns = 0;
    unsigned seq = 0;
};
std::mutex g_early_mu;
EarlyPic g_early[kEarly];
unsigned g_early_seq = 0;
int64_t g_early_at = 0;                    /* when the last one came, CLOCK_MONOTONIC ns */
std::atomic<bool> g_early_on(false);

int64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* the decoder's NV12 picture into the ring, I420, halved when wide; its rect (the film's, else the picture's) too */
void early_put(const woc::VideoFrame &f) {
    const uint32_t k = f.w >= 1600 ? 2 : 1, w = (f.w / k) & ~1u, h = (f.h / k) & ~1u;
    if (w < 16 || h < 16) return;
    std::lock_guard<std::mutex> l(g_early_mu);
    EarlyPic &e = g_early[g_early_seq % kEarly];
    e.px.resize((size_t) w * h * 3 / 2);
    uint8_t *y = e.px.data(), *u = y + (size_t) w * h, *v = u + (size_t) (w / 2) * (h / 2);
    for (uint32_t r = 0; r < h; r++) {
        const uint8_t *s0 = f.y + (size_t) (r * k) * f.pitch, *s1 = f.y + (size_t) (r * k + k - 1) * f.pitch;
        uint8_t *o = y + (size_t) r * w;
        if (k == 1) { memcpy(o, s0, w); continue; }
        for (uint32_t c = 0; c < w; c++) o[c] = (uint8_t) ((s0[2 * c] + s0[2 * c + 1] + s1[2 * c] + s1[2 * c + 1] + 2) >> 2);
    }
    for (uint32_t r = 0; r < h / 2; r++) {
        const uint8_t *s0 = f.uv + (size_t) (r * k) * f.pitch, *s1 = f.uv + (size_t) (r * k + k - 1) * f.pitch;
        uint8_t *ou = u + (size_t) r * (w / 2), *ov = v + (size_t) r * (w / 2);
        for (uint32_t c = 0; c < w / 2; c++) {
            if (k == 1) { ou[c] = s0[2 * c]; ov[c] = s0[2 * c + 1]; continue; }
            const uint8_t *a = s0 + 4 * c, *b = s1 + 4 * c;
            ou[c] = (uint8_t) ((a[0] + a[2] + b[0] + b[2] + 2) >> 2);
            ov[c] = (uint8_t) ((a[1] + a[3] + b[1] + b[3] + 2) >> 2);
        }
    }
    uint32_t x = g_film[2] ? g_film[0] : f.pic_x, yy = g_film[2] ? g_film[1] : f.pic_y;
    uint32_t rw = g_film[2] ? g_film[2] : f.pic_w, rh = g_film[2] ? g_film[3] : f.pic_h;
    if (!rw || !rh || x + rw > f.w || yy + rh > f.h) x = yy = 0, rw = f.w, rh = f.h;
    e.rect[0] = (x / k) & ~1u, e.rect[1] = (yy / k) & ~1u, e.rect[2] = (rw / k) & ~1u, e.rect[3] = (rh / k) & ~1u;
    if (e.rect[0] + e.rect[2] > w) e.rect[2] = (w - e.rect[0]) & ~1u;
    if (e.rect[1] + e.rect[3] > h) e.rect[3] = (h - e.rect[1]) & ~1u;
    e.w = w, e.h = h;
    e.due_ns = f.pts_ms >= 0 ? (int64_t) f.pts_ms * 1000000LL : mono_ns();
    e.seq = ++g_early_seq;
    g_early_at = mono_ns();
}

void film_reset() {
    film_bars_reset(&g_bars);
    if (g_film[2]) { g_film[0] = g_film[1] = g_film[2] = g_film[3] = 0; g_film_seq++; }
}

/* opens the frame file and the decoder; false (once, with g_err) when it cannot */
bool open_locked() {
    if (g_open) return true;
    if (g_failed) return false;
    if (getenv("MGU_VIDEO_OFF")) { g_failed = true; g_err = "off (MGU_VIDEO_OFF)"; return false; }
    const char *p = getenv("MGU_VIDEO_PATH");
    if (p && *p) g_path = p;
    const char *sz = getenv("MGU_VIDEO_SIZE");
    unsigned w = 0, h = 0;
    if (sz && sscanf(sz, "%ux%u", &w, &h) == 2 && w >= 16 && h >= 16 && w <= 2048 && h <= 2048) g_w = w & ~1u, g_h = h & ~1u;
    std::string err, warn;
    if (!g_file && !g_native) {
        if (!g_mgvf.open(g_path.c_str(), g_w, g_h, 3, &err, &warn)) { g_failed = true; g_err = "frame file: " + err; return false; }
        g_file = true;
    }
    if (!g_dec) g_dec = new woc::Decoder();
    if (!g_dec->init("/dev/dri/renderD128", g_native ? 0 : g_w, g_native ? 0 : g_h, false, &err)) {
        g_failed = true;
        g_err = "decoder: " + err;
        g_mgvf.close();
        g_file = false;
        return false;
    }
    g_open = true;
    return true;
}
}  // namespace

/* one access unit; with `wait`, each decoded picture is held until wait(pts_ms) says (stage 4: a played stream's
 * pictures at their time; wait returns 0 to show it, 1 to drop it, too late) */
static int frame_locked(const unsigned char *data, int len, long long pts_ms, int (*wait)(long long, void *), void *arg,
                        unsigned long long *shown, unsigned long long *errors) {
    int ret = 0;
    if (!open_locked()) {
        ret = g_reported ? 0 : -1;   /* -1 once: the caller reports mgu_video_error() */
        g_reported = true;
    } else {
        std::string err;
        const bool ok = g_dec->feed(data, (size_t) len, [&](const woc::VideoFrame &f) {
            if (!f.y) return;
            if (!g_native && g_early_on.load()) early_put(f);   /* mirroring: the HUD's copy, before the wait */
            if (wait && wait(f.pts_ms, arg) != 0) return;
            if (g_native && (!g_file || (f.w & ~1u) != g_w || (f.h & ~1u) != g_h)) {
                /* the stream's own size: the frame file made (again) for it */
                std::string ferr, fwarn;
                if (g_file) { g_mgvf.close(); g_file = false; }
                const uint32_t w = f.w & ~1u, h = f.h & ~1u;
                if (w < 16 || h < 16 || w > 2048 || h > 2048 || !g_mgvf.open(g_path.c_str(), w, h, 3, &ferr, &fwarn)) {
                    g_errors++;
                    if (g_errors <= 3) fprintf(stderr, "mgu_video: frame file %ux%u: %s\n", w, h, ferr.c_str());
                    return;
                }
                g_file = true; g_w = w; g_h = h;
                film_reset();
            }
            if (!g_file) return;
            const uint32_t fw = g_native ? g_w : f.w, fh = g_native ? g_h : f.h;
            uint8_t *dst = g_mgvf.begin();
            ytdec::nv12_to_i420(f.y, f.uv, f.pitch, fw, fh, dst);
            g_pic[0] = f.pic_x, g_pic[1] = f.pic_y, g_pic[2] = f.pic_w, g_pic[3] = f.pic_h;
            if (f.pic_x + f.pic_w <= fw && f.pic_y + f.pic_h <= fh &&
                film_bars_frame(&g_bars, dst, fw, f.pic_x, f.pic_y, f.pic_w, f.pic_h)) {
                g_film[0] = g_bars.film[0], g_film[1] = g_bars.film[1], g_film[2] = g_bars.film[2], g_film[3] = g_bars.film[3];
                g_film_seq++;
            }
            g_mgvf.commit();
            g_shown++;
        }, &err, true, true, wait ? (int64_t) pts_ms : -1);
        if (!ok) { g_errors++; if (g_errors <= 3 || g_errors % 100 == 0) fprintf(stderr, "mgu_video: %s\n", err.c_str()); }
        if (!g_reported) { g_reported = true; ret = 1; }   /* 1 once: opened */
    }
    if (shown) *shown = g_shown;
    if (errors) *errors = g_errors;
    return ret;
}

extern "C" int mgu_video_frame(const unsigned char *data, int len, unsigned long long *shown, unsigned long long *errors) {
    std::lock_guard<std::mutex> l(g_mu);
    return frame_locked(data, len, -1, nullptr, nullptr, shown, errors);
}

extern "C" int mgu_video_frame_timed(const unsigned char *data, int len, long long pts_ms, int (*wait)(long long, void *), void *arg,
                                     unsigned long long *shown, unsigned long long *errors) {
    std::lock_guard<std::mutex> l(g_mu);
    return frame_locked(data, len, pts_ms, wait, arg, shown, errors);
}

/* stage 4: a played video has B frames: its pictures in display order (from the decoder's next start) */
extern "C" void mgu_video_display_order(int on) {
    std::lock_guard<std::mutex> l(g_mu);
    woc::Decoder::set_display_order(on != 0);
}

/* stage 4: 1 the stream's own size (the frame file made at the first picture), 0 MGU_VIDEO_SIZE (mirroring); from
 * the next open */
extern "C" void mgu_video_native(int on) {
    std::lock_guard<std::mutex> l(g_mu);
    g_native = on != 0;
}

/* a seek: the decoder starts again at the next SPS; the frame file and its last picture stay */
extern "C" void mgu_video_reset(void) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_open && g_dec) g_dec->shutdown();
    g_open = false;
    g_failed = false;
}

/* the stream ended: the decoder is let go (a new stream starts at its first SPS); the file keeps the last
 * picture until mgu_video_close() */
extern "C" void mgu_video_stop(void) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_open && g_dec) { g_dec->shutdown(); }
    g_open = false;
    g_failed = false;
    g_reported = false;
    g_mgvf.close();
    g_file = false;
    film_reset();
}

/* at exit: no frame file left behind */
extern "C" void mgu_video_close(void) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_dec) { g_dec->shutdown(); delete g_dec; g_dec = nullptr; }
    g_open = false;
    g_mgvf.close();
    g_file = false;
    unlink(g_path.c_str());
}

extern "C" void mgu_video_picture(unsigned *x, unsigned *y, unsigned *w, unsigned *h) {
    *x = g_pic[0], *y = g_pic[1], *w = g_pic[2], *h = g_pic[3];
}

extern "C" unsigned mgu_video_film(unsigned *x, unsigned *y, unsigned *w, unsigned *h) {
    *x = g_film[0], *y = g_film[1], *w = g_film[2], *h = g_film[3];
    return g_film_seq;
}

extern "C" void mgu_video_early(int on) { g_early_on.store(on != 0); }

extern "C" int mgu_video_early_take(long long until_ns, unsigned *last, unsigned char *dst, size_t cap, unsigned *w, unsigned *h,
                                    unsigned rect[4], unsigned *newest) {
    std::lock_guard<std::mutex> l(g_early_mu);
    if (!g_early_seq || mono_ns() - g_early_at > 1000000000LL) return -1;
    *newest = g_early_seq;
    const EarlyPic *best = nullptr;
    for (int i = 0; i < kEarly; i++) {
        const EarlyPic &e = g_early[i];
        if (e.seq && e.seq > *last && e.due_ns <= until_ns && (!best || e.seq > best->seq)) best = &e;
    }
    if (!best || best->px.size() > cap) return 0;
    memcpy(dst, best->px.data(), best->px.size());
    *w = best->w, *h = best->h;
    for (int i = 0; i < 4; i++) rect[i] = best->rect[i];
    *last = best->seq;
    return 1;
}

extern "C" const char *mgu_video_error(void) { return g_err.c_str(); }
extern "C" const char *mgu_video_path(void) { return g_path.c_str(); }
extern "C" void mgu_video_size(unsigned *w, unsigned *h) { if (w) *w = g_w; if (h) *h = g_h; }
