/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * mgu_hud.c - the picture on the head-up display, through cluster-render (robertnisipeanu/idrive-apps,
 * native/cluster-render: PROTOCOL.md, version 1).
 *
 * Only while wanted: "hud on" on stdin, again at least every 3 s. The app asks each second while the picture may show
 * (its setting "Picture only while parked", as for the screen: the HUD of the car it was made for is on the passenger's
 * side) and stops at once when it may not; if the app or the service stops asking, the HUD goes dark 3 s later.
 * Wanted, a thread connects to cluster-render's socket (its group, tm_oap_app, is one of the receiver's), makes one
 * video layer on the target "hud" (800x480 on iDrive 7) and sends it the frame file's newest picture
 * (video/src/mgu_video.cpp's, read through a read-only mapping of its own), the letterbox bars left out, fitted into
 * the HUD, at most MGU_HUD_FPS (default 15, 1..30) times a second. A picture that does not change is repeated once
 * a second for 5 s (cluster-render stalls a video layer after 6 s without one), then the layer stalls and the HUD
 * goes back to the car's own until the next picture. Not wanted, the connection closes: the layer goes with it, and
 * cluster-render fades the HUD back after its linger.
 *
 * Events, when they change: {"ev":"hud","state":"off|connecting|waiting|shown|unavailable|error","reason":"..."}.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "mgu_hud.h"
#include "mgu_video.h"

#define MS 1000000LL
#define WANT_NS (3000 * MS)
#define RETRY_NS (2000 * MS)
#define REPEAT_NS (1000 * MS)
#define REPEAT_FOR_NS (5000 * MS)
#define LAYER 1
#define MGVF_MAGIC 0x4656474du

static const char *g_sock = "/dev/shm/cluster-render/render.sock";
static void (*g_emit)(const char *json);
static volatile int64_t g_want_at;   /* the last "hud on", CLOCK_MONOTONIC ns; 0: not wanted */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* ---------------- the state, as events ---------------- */

static char g_state[24] = "", g_reason[160] = "";

static void set_state(const char *state, const char *reason) {
    if (!reason) reason = "";
    if (!strcmp(state, g_state) && !strcmp(reason, g_reason)) return;
    snprintf(g_state, sizeof g_state, "%s", state);
    size_t j = 0;
    for (size_t i = 0; reason[i] && j + 1 < sizeof g_reason; i++) {
        const unsigned char c = (unsigned char) reason[i];
        if (c >= 0x20 && c != '"' && c != '\\') g_reason[j++] = (char) c;
    }
    g_reason[j] = 0;
    char line[256];
    snprintf(line, sizeof line, "{\"ev\":\"hud\",\"state\":\"%s\",\"reason\":\"%s\"}", g_state, g_reason);
    if (g_emit) g_emit(line);
}

/* ---------------- flat JSON (cluster-render's events) ---------------- */

/* the string value of "key", or 0 */
static int jstr(const char *j, const char *key, char *out, size_t n) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(j, pat);
    if (!p) return 0;
    p += strlen(pat);
    size_t k = 0;
    while (*p && *p != '"' && k + 1 < n) {
        if (*p == '\\' && p[1]) p++;
        out[k++] = *p++;
    }
    out[k] = 0;
    return 1;
}

static long jnum(const char *j, const char *key, long def) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(j, pat);
    return p ? strtol(p + strlen(pat), NULL, 10) : def;
}

/* ---------------- the connection ---------------- */

static int g_fd = -1;
static unsigned g_hw, g_hh;          /* the HUD target's frame size, from the service's hello */
static int g_layer, g_can_repeat;
static unsigned char g_in[1 << 16];
static size_t g_in_len;

static void disconnect(void) {
    if (g_fd >= 0) close(g_fd);
    g_fd = -1;
    g_hw = g_hh = 0;
    g_layer = g_can_repeat = 0;
    g_in_len = 0;
}

static int send_all(const void *p, size_t n) {
    const unsigned char *b = p;
    while (n) {
        ssize_t w = send(g_fd, b, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        b += w;
        n -= (size_t) w;
    }
    return 0;
}

/* one message: u32 length (LE, the bytes after it), u8 type, body */
static int send_msg(char type, const void *head, size_t head_len, const void *body, size_t body_len) {
    unsigned char h[5];
    const uint32_t len = (uint32_t) (1 + head_len + body_len);
    h[0] = (unsigned char) len, h[1] = (unsigned char) (len >> 8), h[2] = (unsigned char) (len >> 16), h[3] = (unsigned char) (len >> 24);
    h[4] = (unsigned char) type;
    if (send_all(h, 5) || (head_len && send_all(head, head_len)) || (body_len && send_all(body, body_len))) {
        disconnect();
        set_state("error", "cluster-render closed the connection");
        return -1;
    }
    return 0;
}

static int send_json(const char *json) { return send_msg('J', json, strlen(json), NULL, 0); }

static int connect_service(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", g_sock);
    if (connect(fd, (struct sockaddr *) &a, sizeof a) != 0) { const int e = errno; close(fd); return -e; }
    struct timeval tv = { 2, 0 };   /* a service that stops reading cannot hold this thread for long */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

/* one event from the service */
static void on_event(const char *j) {
    char ev[24] = "", s[32] = "", reason[160] = "";
    if (!jstr(j, "ev", ev, sizeof ev)) return;
    jstr(j, "reason", reason, sizeof reason);
    if (!strcmp(ev, "hello")) {
        char targets[256] = "";
        jstr(j, "targets", targets, sizeof targets);
        const char *h = strstr(targets, "hud:");
        unsigned w = 0, hh = 0;
        if (!h || sscanf(h + 4, "%ux%u", &w, &hh) != 2 || w < 16 || hh < 16) {
            set_state("unavailable", "cluster-render has no HUD on this car");
            return;
        }
        g_hw = w & ~3u, g_hh = hh & ~1u;   /* a video frame: width a multiple of 4, height even */
        g_can_repeat = jnum(j, "repeat", 0) == 1;
    } else if (!strcmp(ev, "layer") && jnum(j, "ref", 0) == LAYER) {
        jstr(j, "state", s, sizeof s);
        if (!strcmp(s, "shown")) set_state("shown", "");
        else if (!strcmp(s, "error")) set_state("error", reason);
        else if (!strcmp(s, "stalled")) set_state("waiting", "no new picture");
        else set_state("waiting", reason[0] ? reason : s);
    } else if (!strcmp(ev, "target")) {
        char t[16] = "";
        jstr(j, "target", t, sizeof t);
        jstr(j, "state", s, sizeof s);
        if (!strcmp(t, "hud") && !strcmp(s, "unavailable")) set_state("unavailable", reason[0] ? reason : "the HUD is not available");
    } else if (!strcmp(ev, "error")) {
        set_state("error", reason);
    }
}

/* the events waiting: read them all (a client that does not is disconnected) */
static void read_events(void) {
    for (;;) {
        ssize_t r = recv(g_fd, g_in + g_in_len, sizeof g_in - g_in_len, MSG_DONTWAIT);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { disconnect(); set_state("error", "cluster-render closed the connection"); return; }
        g_in_len += (size_t) r;
        while (g_in_len >= 5) {
            const uint32_t len = g_in[0] | g_in[1] << 8 | g_in[2] << 16 | (uint32_t) g_in[3] << 24;
            if (len < 1 || len > sizeof g_in - 4) { disconnect(); set_state("error", "cluster-render sent a message too long"); return; }
            if (g_in_len < 4 + (size_t) len) break;
            if (g_in[4] == 'J') {
                char j[4096];
                const size_t n = len - 1 < sizeof j - 1 ? len - 1 : sizeof j - 1;
                memcpy(j, g_in + 5, n);
                j[n] = 0;
                on_event(j);
            }
            memmove(g_in, g_in + 4 + len, g_in_len - 4 - len);
            g_in_len -= 4 + len;
        }
        if (g_fd < 0) return;
    }
}

/* ---------------- the picture ---------------- */

struct mgvf_head { uint32_t magic, version, w, h, seq, slot, nslot, matrix, full, pad[7]; };

static int g_vfd = -1;
static const unsigned char *g_map;
static size_t g_map_len;
static ino_t g_ino;

static void unmap(void) {
    if (g_map) munmap((void *) g_map, g_map_len);
    if (g_vfd >= 0) close(g_vfd);
    g_map = NULL, g_map_len = 0, g_vfd = -1, g_ino = 0;
}

/* the frame file mapped as it is now (made again for a played video's size: a new file) */
static const struct mgvf_head *frame_file(void) {
    struct stat st;
    if (stat(mgu_video_path(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < (off_t) sizeof(struct mgvf_head)) { unmap(); return NULL; }
    if (!g_map || st.st_ino != g_ino || (size_t) st.st_size != g_map_len) {
        unmap();
        g_vfd = open(mgu_video_path(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (g_vfd < 0) return NULL;
        void *m = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_SHARED, g_vfd, 0);
        if (m == MAP_FAILED) { close(g_vfd); g_vfd = -1; return NULL; }
        g_map = m, g_map_len = (size_t) st.st_size, g_ino = st.st_ino;
    }
    const struct mgvf_head *h = (const struct mgvf_head *) g_map;
    if (__atomic_load_n(&h->magic, __ATOMIC_ACQUIRE) != MGVF_MAGIC || !h->nslot || h->w < 16 || h->h < 16 ||
        sizeof *h + (size_t) h->nslot * h->w * h->h * 3 / 2 > g_map_len)
        return NULL;
    return h;
}

/* the area average of src's (sx, sy, sw, sh) into dst's (dx, dy, dw, dh): sharper than nearest, no aliasing */
static void box_scale(const unsigned char *src, unsigned spitch, unsigned sx, unsigned sy, unsigned sw, unsigned sh,
                      unsigned char *dst, unsigned dpitch, unsigned dx, unsigned dy, unsigned dw, unsigned dh) {
    static unsigned x0[2048], x1[2048];
    if (!dw || !dh || !sw || !sh || dw > 2048) return;
    for (unsigned x = 0; x < dw; x++) {
        x0[x] = sx + x * sw / dw;
        x1[x] = sx + (x + 1) * sw / dw;
        if (x1[x] <= x0[x]) x1[x] = x0[x] + 1;
    }
    for (unsigned y = 0; y < dh; y++) {
        unsigned y0 = sy + y * sh / dh, y1 = sy + (y + 1) * sh / dh;
        if (y1 <= y0) y1 = y0 + 1;
        unsigned char *o = dst + (size_t) (dy + y) * dpitch + dx;
        for (unsigned x = 0; x < dw; x++) {
            unsigned sum = 0;
            for (unsigned yy = y0; yy < y1; yy++) {
                const unsigned char *r = src + (size_t) yy * spitch;
                for (unsigned xx = x0[x]; xx < x1[x]; xx++) sum += r[xx];
            }
            o[x] = (unsigned char) (sum / ((y1 - y0) * (x1[x] - x0[x])));
        }
    }
}

/* the I420 frame's picture (px, py, pw, ph) fitted into the atlas: Y (w x h), then U and V side by side (w/2 each)
 * in h/2 rows; black around it */
static void to_atlas(const unsigned char *frame, unsigned fw, unsigned fh, unsigned px, unsigned py, unsigned pw, unsigned ph,
                     unsigned char *atlas, unsigned w, unsigned h) {
    memset(atlas, 16, (size_t) w * h);
    memset(atlas + (size_t) w * h, 128, (size_t) w * h / 2);
    unsigned ow, oh;
    if ((uint64_t) pw * h > (uint64_t) ph * w) { ow = w; oh = (unsigned) ((uint64_t) ph * w / pw) & ~1u; }
    else { oh = h; ow = (unsigned) ((uint64_t) pw * h / ph) & ~1u; }
    if (ow < 2 || oh < 2) return;
    const unsigned ox = (w - ow) / 2 & ~1u, oy = (h - oh) / 2 & ~1u;
    const unsigned char *u = frame + (size_t) fw * fh, *v = u + (size_t) (fw / 2) * (fh / 2);
    box_scale(frame, fw, px, py, pw, ph, atlas, w, ox, oy, ow, oh);
    unsigned char *c = atlas + (size_t) w * h;
    box_scale(u, fw / 2, px / 2, py / 2, pw / 2, ph / 2, c, w, ox / 2, oy / 2, ow / 2, oh / 2);
    box_scale(v, fw / 2, px / 2, py / 2, pw / 2, ph / 2, c + w / 2, w, ox / 2, oy / 2, ow / 2, oh / 2);
}

/* ---------------- the thread ---------------- */

static void *hud_thread(void *arg) {
    (void) arg;
    const char *e = getenv("MGU_HUD_FPS");
    int fps = e && atoi(e) > 0 ? atoi(e) : 15;
    if (fps > 30) fps = 30;
    const int64_t period = 1000000000LL / fps;
    int64_t next_try = 0, last_sent = 0, last_new = 0;
    uint32_t last_seq = 0;
    uint64_t frame_id = 0;
    unsigned char *atlas = NULL;
    size_t atlas_len = 0;
    set_state("off", "");
    for (;;) {
        const int64_t t = now_ns(), want = g_want_at;
        if (!want || t - want > WANT_NS) {
            if (g_fd >= 0) disconnect();
            unmap();
            set_state("off", "");
            usleep(100000);
            continue;
        }
        if (g_fd < 0) {
            if (t < next_try) { usleep(100000); continue; }
            next_try = t + RETRY_NS;
            const int fd = connect_service();
            if (fd < 0) {
                set_state("unavailable", fd == -ENOENT ? "cluster-render is not installed: add it from the Store" :
                                         fd == -EACCES ? "no access to cluster-render's socket" : strerror(-fd));
                continue;
            }
            g_fd = fd;
            last_seq = 0;
            set_state("connecting", "");
            if (send_json("{\"op\":\"hello\",\"app\":\"hp-airplay\",\"proto\":1}")) continue;
        }
        struct pollfd p = { g_fd, POLLIN, 0 };
        if (poll(&p, 1, 20) > 0) read_events();
        if (g_fd < 0) continue;
        if (g_hw && !g_layer) {
            char j[256];
            snprintf(j, sizeof j, "{\"op\":\"layer\",\"ref\":%d,\"target\":\"hud\",\"kind\":\"video\",\"vw\":%u,\"vh\":%u,\"fps\":%d}",
                     LAYER, g_hw, g_hh, fps);
            if (send_json(j)) continue;
            g_layer = 1;
            set_state("waiting", "no picture yet");
            const size_t n = (size_t) g_hw * g_hh * 3 / 2;
            if (n > atlas_len) { free(atlas); atlas = malloc(n); atlas_len = atlas ? n : 0; }
        }
        if (!g_layer || !atlas || now_ns() - last_sent < period) continue;
        const struct mgvf_head *h = frame_file();
        const uint32_t seq = h ? __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE) : 0;
        if (h && seq && seq != last_seq) {
            const uint32_t slot = __atomic_load_n(&h->slot, __ATOMIC_ACQUIRE) % h->nslot, fw = h->w, fh = h->h;
            const unsigned char *frame = g_map + sizeof *h + (size_t) slot * fw * fh * 3 / 2;
            unsigned px = 0, py = 0, pw = 0, ph = 0;
            mgu_video_picture(&px, &py, &pw, &ph);
            if (!pw || !ph || px + pw > fw || py + ph > fh) px = py = 0, pw = fw, ph = fh;
            px &= ~1u, py &= ~1u, pw &= ~1u, ph &= ~1u;
            to_atlas(frame, fw, fh, px, py, pw, ph, atlas, g_hw, g_hh);
            unsigned char head[14];
            const uint32_t ref = LAYER;
            const uint64_t id = ++frame_id;
            memcpy(head, &ref, 4);       /* little-endian, as the head unit */
            memcpy(head + 4, &id, 8);
            head[12] = head[13] = 0;     /* no rects: the whole atlas */
            if (send_msg('F', head, sizeof head, atlas, (size_t) g_hw * g_hh * 3 / 2)) continue;
            last_seq = seq;
            last_sent = last_new = now_ns();
        } else if (g_can_repeat && last_new && now_ns() - last_sent >= REPEAT_NS && now_ns() - last_new < REPEAT_FOR_NS) {
            char j[64];
            snprintf(j, sizeof j, "{\"op\":\"repeat\",\"ref\":%d}", LAYER);
            if (send_json(j)) continue;
            last_sent = now_ns();
        }
    }
    return NULL;
}

static void start_thread(void) {
    const char *s = getenv("MGU_HUD_SOCKET");
    if (s && *s) g_sock = s;
    pthread_t t;
    if (pthread_create(&t, NULL, hud_thread, NULL) == 0) pthread_detach(t);
}

void mgu_hud_init(void (*emit_json)(const char *json)) {
    g_emit = emit_json;
    pthread_once(&g_once, start_thread);
}

void mgu_hud_want(int on) { g_want_at = on ? now_ns() : 0; }
