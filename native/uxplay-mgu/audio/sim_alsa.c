/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * sim_alsa.c - a stand-in libasound.so.2 for test_follow: one playback device that plays in real time like the car's
 * SmartXbar input (a 64 ms buffer of 3 periods, started when full, non-blocking, an underrun when a period finds too
 * little), at a rate off by SIM_PPM parts per million, as a sound card's clock is off the system's.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct snd_pcm { int dummy; } snd_pcm_t;

enum { PREPARED, RUNNING, XRUN };
static snd_pcm_t pcm;
static int state = PREPARED;
static long fill, buffer, period, start_at;
static double rate, next_period;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* the device's periods up to now */
static void advance(void) {
    const double t = now();
    while (state == RUNNING && t >= next_period) {
        if (fill < period) { state = XRUN; fill = 0; return; }
        fill -= period;
        next_period += period / rate;
    }
}

int snd_pcm_open(snd_pcm_t **p, const char *name, int stream, int mode) {
    (void) name; (void) stream; (void) mode;
    *p = &pcm;
    state = PREPARED;
    fill = 0;
    return 0;
}

int snd_pcm_set_params(snd_pcm_t *p, int format, int access, unsigned channels, unsigned r, int resample, unsigned latency_us) {
    (void) p; (void) format; (void) access; (void) channels; (void) resample;
    const char *e = getenv("SIM_PPM");
    rate = r * (1.0 + (e ? atof(e) : 0) / 1e6);
    buffer = (long) ((double) r * latency_us / 1e6);
    period = buffer / 3;
    start_at = (buffer / period) * period;   /* as snd_pcm_set_params: a full buffer */
    return 0;
}

long snd_pcm_writei(snd_pcm_t *p, const void *buf, unsigned long frames) {
    (void) p; (void) buf;
    advance();
    if (state == XRUN) return -EPIPE;
    long room = buffer - fill;
    if (room <= 0) return -EAGAIN;
    long n = (long) frames < room ? (long) frames : room;
    fill += n;
    if (state == PREPARED && fill >= start_at) { state = RUNNING; next_period = now() + period / rate; }
    return n;
}

int snd_pcm_delay(snd_pcm_t *p, long *d) {
    (void) p;
    advance();
    if (state == XRUN) return -EPIPE;
    *d = fill;
    return 0;
}

int snd_pcm_recover(snd_pcm_t *p, int err, int silent) {
    (void) p; (void) err; (void) silent;
    state = PREPARED;
    fill = 0;
    return 0;
}

int snd_pcm_close(snd_pcm_t *p) { (void) p; return 0; }
const char *snd_strerror(int e) { return strerror(-e); }
