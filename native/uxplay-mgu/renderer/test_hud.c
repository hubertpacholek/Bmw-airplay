/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_hud.c - the picture on the HUD off the car, against a stand-in for cluster-render's socket:
 *   cc -O2 -pthread -Ivideo/src renderer/test_hud.c renderer/mgu_hud.c -o test_hud && ./test_hud
 * A 1920x720 frame file with the picture in its middle 1560x720 (Y 200, U 50, V 220) and the letterbox bars black:
 * wanted, the HUD gets hello, one video layer the picture's shape (800x368, centred) and frames of only the picture
 * in cluster-render's atlas (U and V side by side); a smaller size makes the layer again (dropping the old one), a
 * new place only moves it; a 30 fps film on a 400x184 layer not packed goes at an even 10 a second (every 3rd
 * picture), on a 40 % packed one every picture; mirrored, the decoder's early pictures each 150 ms (the lead) before
 * its screen time; packed blocks where the service offers them; asked no more, it goes off within 3 s; "hud off" at
 * once.
 */
#include <assert.h>
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
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "mgu_hud.h"

static char g_frame_path[256], g_sock_path[256];
const char *mgu_video_path(void) { return g_frame_path; }
void mgu_video_picture(unsigned *x, unsigned *y, unsigned *w, unsigned *h) { *x = 180, *y = 0, *w = 1560, *h = 720; }
unsigned mgu_video_film(unsigned *x, unsigned *y, unsigned *w, unsigned *h) { *x = *y = *w = *h = 0; return 0; }

/* the decoder's early pictures: none (a played video) until g_early_t0 is set; then picture i (Y 40 + i, 640x360)
 * has its screen time at g_early_t0 + i * 33.3 ms + 200 ms (the jitter buffer) and comes out of the decoder 200 ms
 * before it */
static int64_t g_early_t0;
static int g_early_on;
static int64_t mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t) t.tv_sec * 1000000000LL + t.tv_nsec; }
void mgu_video_early(int on) { g_early_on = on; }
int mgu_video_early_take(long long until_ns, unsigned *last, unsigned char *dst, size_t cap, unsigned *w, unsigned *h,
                         unsigned rect[4], unsigned *newest) {
    if (!g_early_t0) return -1;
    const int64_t now = mono();
    int out = (int) ((now - g_early_t0) / 33333333LL);           /* decoded so far */
    if (out < 0) return 0;
    if (out > 150) out = 150;
    *newest = (unsigned) out + 1;
    int best = -1;
    for (int i = 0; i <= out; i++) {
        const int64_t due = g_early_t0 + i * 33333333LL + 200000000LL;
        if ((unsigned) i + 1 > *last && due <= until_ns) best = i;
    }
    if (best < 0 || cap < 640 * 360 * 3 / 2) return 0;
    memset(dst, 40 + best, 640 * 360);
    memset(dst + 640 * 360, 128, 640 * 360 / 2);
    *w = 640, *h = 360;
    rect[0] = rect[1] = 0, rect[2] = 640, rect[3] = 360;
    *last = (unsigned) best + 1;
    return 1;
}

static char g_events[4096];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static void emit(const char *json) {
    pthread_mutex_lock(&g_mu);
    strncat(g_events, json, sizeof g_events - strlen(g_events) - 2);
    strcat(g_events, "\n");
    pthread_mutex_unlock(&g_mu);
}

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int read_full(int fd, void *b, size_t n, double until) {
    unsigned char *p = b;
    while (n) {
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, (int) ((until - now()) * 1000)) <= 0) return -1;
        ssize_t r = read(fd, p, n);
        if (r <= 0) return -1;
        p += r, n -= (size_t) r;
    }
    return 0;
}

/* one message from the client: its type, its body in *body (malloc'd) */
static int next_msg(int fd, char *type, unsigned char **body, uint32_t *len, double until) {
    unsigned char h[5];
    if (read_full(fd, h, 5, until)) return -1;
    *len = (h[0] | h[1] << 8 | h[2] << 16 | (uint32_t) h[3] << 24) - 1;
    *type = (char) h[4];
    *body = malloc(*len + 1);
    if (read_full(fd, *body, *len, until)) return -1;
    (*body)[*len] = 0;
    return 0;
}

static void send_json(int fd, const char *j) {
    const uint32_t len = (uint32_t) strlen(j) + 1;
    unsigned char h[5] = { (unsigned char) len, (unsigned char) (len >> 8), (unsigned char) (len >> 16), (unsigned char) (len >> 24), 'J' };
    assert(write(fd, h, 5) == 5 && write(fd, j, strlen(j)) == (ssize_t) strlen(j));
}

/* the frame file: 1920x720, one slot, the picture in x 180..1740 */
static unsigned char *g_map;
static size_t g_len;
static void make_frame_file(void) {
    const unsigned w = 1920, h = 720;
    g_len = 64 + (size_t) w * h * 3 / 2;
    int fd = open(g_frame_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && ftruncate(fd, (off_t) g_len) == 0);
    g_map = mmap(NULL, g_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(g_map != MAP_FAILED);
    close(fd);
    uint32_t head[16] = { 0x4656474d, 1, w, h, 1, 0, 1, 1, 0 };
    memcpy(g_map, head, sizeof head);
    unsigned char *y = g_map + 64, *u = y + w * h, *v = u + (w / 2) * (h / 2);
    for (unsigned r = 0; r < h; r++)
        for (unsigned c = 0; c < w; c++) y[r * w + c] = c >= 180 && c < 1740 ? 200 : 16;
    for (unsigned r = 0; r < h / 2; r++)
        for (unsigned c = 0; c < w / 2; c++) {
            const int in = c >= 90 && c < 870;
            u[r * (w / 2) + c] = in ? 50 : 128;
            v[r * (w / 2) + c] = in ? 220 : 128;
        }
}
/* a new picture: the header's seq + 1 */
static void bump(void) { __atomic_add_fetch((uint32_t *) (g_map + 16), 1, __ATOMIC_RELEASE); }

/* the next JSON message, F messages before it skipped (their count in *frames) */
static char *next_json(int c, int *frames) {
    char type;
    unsigned char *body;
    uint32_t len;
    for (;;) {
        assert(next_msg(c, &type, &body, &len, now() + 3) == 0);
        if (type == 'J') return (char *) body;
        if (frames) (*frames)++;
        free(body);
    }
}

static volatile int g_bumping;
static void *bumper(void *arg) {
    (void) arg;
    while (g_bumping) { bump(); mgu_hud_want(1); usleep(33333); }
    return NULL;
}

static double g_asked;
static void want(void) { mgu_hud_want(1); g_asked = now(); }

int main(void) {
    char dir[] = "/tmp/test_hud_XXXXXX";
    assert(mkdtemp(dir));
    snprintf(g_frame_path, sizeof g_frame_path, "%s/frame", dir);
    snprintf(g_sock_path, sizeof g_sock_path, "%s/render.sock", dir);
    make_frame_file();
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%.100s", g_sock_path);
    assert(bind(ls, (struct sockaddr *) &a, sizeof a) == 0 && listen(ls, 2) == 0);
    setenv("MGU_HUD_SOCKET", g_sock_path, 1);
    setenv("MGU_HUD_FPS", "30", 1);

    mgu_hud_init(emit);
    usleep(300000);
    struct pollfd pl = { ls, POLLIN, 0 };
    assert(poll(&pl, 1, 300) == 0);   /* not wanted: no connection */

    want();
    assert(poll(&pl, 1, 2000) == 1);
    int c = accept(ls, NULL, NULL);
    char type, *j;
    unsigned char *body;
    uint32_t len;
    j = next_json(c, NULL);
    assert(strstr(j, "\"op\":\"hello\""));
    free(j);
    send_json(c, "{\"ev\":\"hello\",\"proto\":1,\"backend\":\"id7\",\"targets\":\"map:1088x480,panel:1920x720,hud:800x480\",\"repeat\":1}");
    /* the layer: the picture's shape, 1560x720 fitted into 800x480 = 800x368, centred */
    j = next_json(c, NULL);
    assert(strstr(j, "\"op\":\"layer\"") && strstr(j, "\"ref\":1,") && strstr(j, "\"target\":\"hud\"") && strstr(j, "\"vw\":800,\"vh\":368,\"x\":0,\"y\":56,\"w\":800,\"h\":368"));
    assert(!strstr(j, "always_packed"));
    free(j);
    send_json(c, "{\"ev\":\"layer\",\"ref\":1,\"state\":\"shown\"}");
    assert(next_msg(c, &type, &body, &len, now() + 2) == 0 && type == 'F' && len == 14 + 800 * 368 * 3 / 2);
    uint32_t ref;
    memcpy(&ref, body, 4);
    assert(ref == 1 && body[12] == 0 && body[13] == 0);
    const unsigned char *y = body + 14, *ch = y + 800 * 368;
    assert(y[0] == 200 && y[800 * 368 - 1] == 200 && y[184 * 800 + 400] == 200);        /* only the picture */
    assert(ch[0] == 50 && ch[400] == 220 && ch[183 * 800 + 399] == 50 && ch[183 * 800 + 799] == 220);   /* U | V */
    free(body);
    printf("layer and frame: only the picture, 800x368 at 0,56: ok\n");

    /* the picture does not change: a repeat about once a second */
    want();
    j = next_json(c, NULL);
    assert(strstr(j, "\"op\":\"repeat\",\"ref\":1"));
    free(j);
    printf("repeat: ok\n");

    /* half the size, right edge, top: the layer made again (the old one dropped) */
    mgu_hud_layout(50, 100, 0, 1);
    want();
    bump();
    j = next_json(c, NULL);
    assert(strstr(j, "\"op\":\"drop\",\"ref\":1"));
    free(j);
    j = next_json(c, NULL);
    assert(strstr(j, "\"ref\":2,") && strstr(j, "\"vw\":400,\"vh\":184,\"x\":400,\"y\":0"));
    free(j);
    assert(next_msg(c, &type, &body, &len, now() + 2) == 0 && type == 'F' && len == 14 + 400 * 184 * 3 / 2);
    free(body);
    /* the same size at the left edge, bottom: only moved */
    mgu_hud_layout(50, 0, 100, 1);
    want();
    bump();
    j = next_json(c, NULL);
    assert(strstr(j, "\"op\":\"set\",\"ref\":2,\"x\":0,\"y\":296"));
    free(j);
    printf("size: made again; place: moved: ok\n");

    /* a 30 fps film on a 400x184 layer, not packed (110 KB a picture: 11 a second by MGU_HUD_KBPS 1200): an even 10 a
     * second (every 3rd picture) */
    g_bumping = 1;
    pthread_t bt;
    pthread_create(&bt, NULL, bumper, NULL);
    double t0 = now();
    int frames = 0;
    while (now() - t0 < 1.5) {   /* the rate is measured first */
        struct pollfd pf = { c, POLLIN, 0 };
        if (poll(&pf, 1, 100) == 1) { assert(next_msg(c, &type, &body, &len, now() + 2) == 0); free(body); }
    }
    t0 = now();
    while (now() - t0 < 2.0) {
        struct pollfd pf = { c, POLLIN, 0 };
        if (poll(&pf, 1, 100) == 1) { assert(next_msg(c, &type, &body, &len, now() + 2) == 0); if (type == 'F') frames++; free(body); }
    }
    g_bumping = 0;
    pthread_join(bt, NULL);
    want();
    printf("30 fps film: %d frames in 2 s\n", frames);
    assert(frames >= 18 && frames <= 22);

    /* asked no more: off 3 s after the last ask (the connection closes) */
    char buf[1 << 16];
    for (;;) {
        struct pollfd pf = { c, POLLIN, 0 };
        assert(poll(&pf, 1, 5000) == 1);
        if (read(c, buf, sizeof buf) <= 0) break;
    }
    const double off_after = now() - g_asked;
    assert(off_after > 2.9 && off_after < 3.5);
    close(c);
    printf("off %.1f s after the last ask: ok\n", off_after);

    /* a service with packed blocks: the layer asks for them, its height a multiple of 4 */
    mgu_hud_layout(100, 50, 50, 1);
    want();
    assert(poll(&pl, 1, 3000) == 1);
    c = accept(ls, NULL, NULL);
    j = next_json(c, NULL);
    free(j);
    send_json(c, "{\"ev\":\"hello\",\"proto\":1,\"backend\":\"id7\",\"targets\":\"hud:800x480\",\"repeat\":1,\"always_packed\":1}");
    bump();
    j = next_json(c, NULL);
    assert(strstr(j, "\"vw\":800,\"vh\":368,") && strstr(j, "\"always_packed\":1"));
    free(j);
    printf("packed blocks where offered: ok\n");

    /* 40 % of the HUD, packed: about 23 KB a picture, so every picture of a 30 fps film (MGU_HUD_FPS 30 here) */
    mgu_hud_layout(40, 50, 50, 1);
    g_bumping = 1;
    pthread_create(&bt, NULL, bumper, NULL);
    t0 = now();
    int layer40 = 0;
    while (now() - t0 < 1.5) {
        struct pollfd pf = { c, POLLIN, 0 };
        if (poll(&pf, 1, 100) == 1) {
            assert(next_msg(c, &type, &body, &len, now() + 2) == 0);
            if (type == 'J' && strstr((char *) body, "\"vw\":320,\"vh\":144,")) layer40 = 1;
            free(body);
        }
    }
    frames = 0;
    t0 = now();
    while (now() - t0 < 2.0) {
        struct pollfd pf = { c, POLLIN, 0 };
        if (poll(&pf, 1, 100) == 1) { assert(next_msg(c, &type, &body, &len, now() + 2) == 0); if (type == 'F') frames++; free(body); }
    }
    g_bumping = 0;
    pthread_join(bt, NULL);
    want();
    printf("40 %% packed: layer 320x144: %s, %d frames in 2 s\n", layer40 ? "ok" : "MISSING", frames);
    assert(layer40 && frames >= 54 && frames <= 64);

    /* mirrored: the decoder's early pictures, each sent 150 ms (the lead) before its screen time */
    mgu_hud_lead(150);
    g_early_t0 = mono();
    int checked = 0;
    double worst = 0;
    t0 = now();
    while (now() - t0 < 2.5) {
        if (now() - g_asked > 1) want();
        struct pollfd pf = { c, POLLIN, 0 };
        if (poll(&pf, 1, 50) != 1) continue;
        assert(next_msg(c, &type, &body, &len, now() + 2) == 0);
        if (type == 'F' && len > 14 + 1000) {
            const int64_t at = mono();
            const int i = body[14] - 40;                                   /* the picture's number, from its luma */
            const double off = (double) (at - (g_early_t0 + i * 33333333LL + 200000000LL - 150000000LL)) / 1e6;
            if (i > 6) { checked++; if (off < 0 ? -off > worst : off > worst) worst = off < 0 ? -off : off; }
        }
        free(body);
    }
    printf("early pictures: %d checked, each within %.0f ms of its screen time less 150 ms\n", checked, worst);
    assert(checked >= 40 && worst < 30 && g_early_on);
    g_early_t0 = 0;

    /* "hud off" at once */
    usleep(200000);
    const double t = now();
    mgu_hud_want(0);
    for (;;) {
        struct pollfd pf = { c, POLLIN, 0 };
        assert(poll(&pf, 1, 2000) == 1);
        if (read(c, buf, sizeof buf) <= 0) break;
    }
    assert(now() - t < 0.5);
    close(c);
    printf("hud off: closed in %.2f s: ok\n", now() - t);

    pthread_mutex_lock(&g_mu);
    assert(strstr(g_events, "\"state\":\"shown\"") && strstr(g_events, "\"state\":\"off\""));
    pthread_mutex_unlock(&g_mu);
    printf("events: ok\n");
    munmap(g_map, g_len);
    unlink(g_frame_path);
    unlink(g_sock_path);
    rmdir(dir);
    return 0;
}
