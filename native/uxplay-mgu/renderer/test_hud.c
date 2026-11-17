/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_hud.c - the picture on the HUD off the car, against a stand-in for cluster-render's socket:
 *   cc -O2 -pthread -Ivideo/src renderer/test_hud.c renderer/mgu_hud.c -o test_hud && ./test_hud
 * A 1920x720 frame file with the picture in its middle 1560x720 (Y 200, U 50, V 220) and the letterbox bars black:
 * wanted, the HUD gets hello, one video layer of 800x480 and frames with the picture fitted (800x368, centred, the
 * bars left out) in cluster-render's atlas (U and V side by side); asked no more, it goes off within 3 s; "hud off"
 * at once.
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
static void make_frame_file(void) {
    const unsigned w = 1920, h = 720;
    const size_t len = 64 + (size_t) w * h * 3 / 2;
    int fd = open(g_frame_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && ftruncate(fd, (off_t) len) == 0);
    unsigned char *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(m != MAP_FAILED);
    uint32_t head[16] = { 0x4656474d, 1, w, h, 1, 0, 1, 1, 0 };
    memcpy(m, head, sizeof head);
    unsigned char *y = m + 64, *u = y + w * h, *v = u + (w / 2) * (h / 2);
    for (unsigned r = 0; r < h; r++)
        for (unsigned c = 0; c < w; c++) y[r * w + c] = c >= 180 && c < 1740 ? 200 : 16;
    for (unsigned r = 0; r < h / 2; r++)
        for (unsigned c = 0; c < w / 2; c++) {
            const int in = c >= 90 && c < 870;
            u[r * (w / 2) + c] = in ? 50 : 128;
            v[r * (w / 2) + c] = in ? 220 : 128;
        }
    munmap(m, len);
    close(fd);
}

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
    setenv("MGU_HUD_FPS", "10", 1);

    mgu_hud_init(emit);
    usleep(300000);
    struct pollfd pl = { ls, POLLIN, 0 };
    assert(poll(&pl, 1, 300) == 0);   /* not wanted: no connection */

    const double asked_last = now();
    mgu_hud_want(1);
    assert(poll(&pl, 1, 2000) == 1);
    int c = accept(ls, NULL, NULL);
    char type;
    unsigned char *body;
    uint32_t len;
    assert(next_msg(c, &type, &body, &len, now() + 2) == 0 && type == 'J' && strstr((char *) body, "\"op\":\"hello\""));
    free(body);
    send_json(c, "{\"ev\":\"hello\",\"proto\":1,\"backend\":\"id7\",\"targets\":\"map:1088x480,panel:1920x720,hud:800x480\",\"repeat\":1}");
    assert(next_msg(c, &type, &body, &len, now() + 2) == 0 && type == 'J');
    assert(strstr((char *) body, "\"op\":\"layer\"") && strstr((char *) body, "\"target\":\"hud\"") &&
           strstr((char *) body, "\"vw\":800") && strstr((char *) body, "\"vh\":480"));
    free(body);
    send_json(c, "{\"ev\":\"layer\",\"ref\":1,\"state\":\"shown\"}");

    /* the first frame: the whole atlas */
    assert(next_msg(c, &type, &body, &len, now() + 2) == 0 && type == 'F');
    assert(len == 14 + 800 * 480 * 3 / 2);
    uint32_t ref;
    memcpy(&ref, body, 4);
    assert(ref == 1 && body[12] == 0 && body[13] == 0);
    const unsigned char *y = body + 14, *ch = y + 800 * 480;
    /* 1560x720 fitted into 800x480: 800x368 (even), from row 56 */
    assert(y[10 * 800 + 400] == 16 && y[470 * 800 + 400] == 16);       /* the bars above and below */
    assert(y[240 * 800 + 5] == 200 && y[240 * 800 + 795] == 200);      /* the picture edge to edge */
    assert(y[57 * 800 + 400] == 200 && y[422 * 800 + 400] == 200);
    assert(ch[120 * 800 + 200] == 50 && ch[120 * 800 + 400 + 200] == 220);   /* U, then V, side by side */
    assert(ch[5 * 800 + 200] == 128 && ch[5 * 800 + 600] == 128);
    free(body);
    printf("frame: 800x480 atlas, the picture fitted without its bars: ok\n");

    /* the picture does not change: a repeat about once a second */
    assert(next_msg(c, &type, &body, &len, now() + 2.5) == 0 && type == 'J' && strstr((char *) body, "\"op\":\"repeat\""));
    free(body);
    printf("repeat: ok\n");

    /* asked no more: off 3 s after the last ask (the connection closes) */
    char buf[1 << 16];
    for (;;) {
        struct pollfd pf = { c, POLLIN, 0 };
        assert(poll(&pf, 1, 5000) == 1);
        if (read(c, buf, sizeof buf) <= 0) break;
    }
    const double off_after = now() - asked_last;
    assert(off_after > 2.9 && off_after < 3.5);
    close(c);
    printf("off %.1f s after the last ask: ok\n", off_after);

    /* "hud off" at once */
    mgu_hud_want(1);
    assert(poll(&pl, 1, 3000) == 1);
    c = accept(ls, NULL, NULL);
    assert(next_msg(c, &type, &body, &len, now() + 2) == 0);
    free(body);
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
    unlink(g_frame_path);
    unlink(g_sock_path);
    rmdir(dir);
    return 0;
}
