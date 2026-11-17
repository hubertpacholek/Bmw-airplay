/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * test_jitter.c - the jitter buffer off the car: cc -O2 -pthread renderer/test_jitter.c renderer/mgu_jitter.c && ./a.out
 * 60 fps frames stamped by the sender arrive with 0-120 ms of random delay (bursts and gaps, as over Wi-Fi); with a
 * 200 ms buffer they must come out evenly. Then a flush in the middle of a wait, and sound on the picture's timeline:
 * stamped 0.2 s ahead, as the iPhone stamps it, it waits 0.2 s longer than the picture.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "mgu_jitter.h"

#define N 300
static int64_t out_t[N];
static int out_n = 0;

static int64_t rt(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec; }

static void fn(const unsigned char *d, int len, int64_t due, mgu_jitter_t *q, unsigned gen) {
    (void) len; (void) due; (void) q; (void) gen;
    int i = *(const int *) d;
    assert(i == out_n);   /* in order */
    out_t[out_n++] = mgu_jitter_now();
}

static int64_t last_due;
static void fn_due(const unsigned char *d, int len, int64_t due, mgu_jitter_t *q, unsigned gen) {
    (void) d; (void) len; (void) q; (void) gen;
    last_due = due;
}

static void sleep_until_rt(int64_t t) { int64_t d = t - rt(); if (d > 0) { struct timespec ts = { d / 1000000000LL, d % 1000000000LL }; nanosleep(&ts, NULL); } }

static double sd_ms(const int64_t *t, int n, double *mean) {
    double s = 0, s2 = 0;
    for (int i = 1; i < n; i++) { double d = (t[i] - t[i - 1]) / 1e6; s += d; s2 += d * d; }
    *mean = s / (n - 1);
    return sqrt(s2 / (n - 1) - *mean * *mean);
}

static void run(int delay, double *sd, double *mean) {
    mgu_jitter_set_delay(delay);
    mgu_jitter_t *q = mgu_jitter_new(fn, true);
    out_n = 0;
    srand(7);
    const int64_t t0 = rt() + 50000000LL, frame = 16666667LL;
    /* arrival = stamp + random 0..120 ms, in arrival order (a late frame holds back the ones after it, as TCP does) */
    int64_t arrive_prev = 0;
    for (int i = 0; i < N; i++) {
        int64_t stamp = t0 + i * frame;
        int64_t arrive = stamp + (rand() % 120) * 1000000LL;
        if (arrive < arrive_prev) arrive = arrive_prev;
        arrive_prev = arrive;
        sleep_until_rt(arrive);
        mgu_jitter_push(q, (const unsigned char *) &i, sizeof i, (uint64_t) stamp);
    }
    while (out_n < N) usleep(10000);
    *sd = sd_ms(out_t + 20, N - 20, mean);   /* after the first frames set the base */
}

int main(void) {
    double sd0, m0, sd2, m2;
    run(0, &sd0, &m0);
    run(200, &sd2, &m2);
    printf("no buffer:  interval %.2f ms, jitter (sd) %.2f ms\n", m0, sd0);
    printf("200 ms:     interval %.2f ms, jitter (sd) %.2f ms\n", m2, sd2);
    assert(sd2 < 3.0 && sd0 > 10.0 && fabs(m2 - 16.67) < 0.5);

    /* a flush wakes a packet waiting and drops the rest */
    mgu_jitter_set_delay(500);
    mgu_jitter_t *q = mgu_jitter_new(fn, true);
    out_n = 0;
    for (int i = 0; i < 5; i++) mgu_jitter_push(q, (const unsigned char *) &i, sizeof i, (uint64_t) rt());
    usleep(50000);
    int64_t t = mgu_jitter_now();
    mgu_jitter_flush(q);
    assert(mgu_jitter_now() - t < 20000000LL && out_n == 0);
    mgu_jitter_stats_t s;
    mgu_jitter_stats(q, &s);
    assert(s.queued == 0);
    printf("flush: ok\n");

    /* sound anchored on the picture: a picture arriving at its stamp waits the delay; sound stamped 200 ms ahead waits
     * 200 ms more, and on its own (no picture yet) only the delay */
    mgu_jitter_set_delay(200);
    mgu_jitter_t *v = mgu_jitter_new(fn_due, false), *a = mgu_jitter_new(fn_due, true);
    mgu_jitter_anchor(a, v);
    int64_t t0 = mgu_jitter_now();
    mgu_jitter_push(a, (const unsigned char *) "a", 1, (uint64_t) (rt() + 200000000LL));
    usleep(300000);
    double alone = (last_due - t0) / 1e6;
    mgu_jitter_push(v, (const unsigned char *) "v", 1, (uint64_t) rt());
    usleep(250000);
    t0 = mgu_jitter_now();
    mgu_jitter_push(a, (const unsigned char *) "a", 1, (uint64_t) (rt() + 200000000LL));
    usleep(500000);
    double anchored = (last_due - t0) / 1e6;
    printf("sound alone waits %.0f ms, on the picture's timeline %.0f ms\n", alone, anchored);
    assert(fabs(alone - 200) < 15 && fabs(anchored - 400) < 15);
    return 0;
}
