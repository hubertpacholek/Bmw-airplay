/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * mgu_jitter.c - the jitter buffer for screen mirroring.
 *
 * The iPhone stamps every access unit and sound packet with the time it is meant for; Wi-Fi delivers them unevenly
 * (a few at once, then a gap). Shown and played the moment they arrive, a film stutters. Each queue holds its packets
 * and hands them on, in order, at an even pace: due = arrival + (base + delay - transit), where transit is
 * now - ntp_time (UxPlay's local clock) and base the shortest transit of the last 2 to 4 s. A packet that came
 * through fastest waits the whole delay, a slow one less; one slower than base + delay goes at once. Base follows
 * the two clocks' drift and the network by itself; a jump of 2 s or more (the car setting its clock, a dead spot)
 * starts it again. Video and sound each have their own queue, so each keeps the timing it had without the buffer,
 * shifted by the same delay.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mgu_jitter.h"

#define MS 1000000LL
#define RESET_NS (2000 * MS)
#define BUCKET_NS (2000 * MS)
#define LATE_NS (40 * MS)
#define MAX_QUEUED 1000          /* more than this: handed on without waiting until it drains */

typedef struct item {
    struct item *next;
    int64_t due;
    int len;
    unsigned char data[];
} item_t;

struct mgu_jitter {
    pthread_mutex_t mu;
    pthread_cond_t cv;           /* CLOCK_MONOTONIC: a new packet, a flush, the end of a hand-on */
    item_t *head, *tail;
    unsigned count, gen;
    bool busy;
    mgu_jitter_fn fn;
    bool wait_first;
    bool have;
    int64_t cur, prev, bucket_start;
    unsigned long long late;
};

static volatile int g_delay_ms = 200;

int64_t mgu_jitter_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t realtime_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void mgu_jitter_set_delay(int ms) { g_delay_ms = ms < 0 ? 0 : ms > 1000 ? 1000 : ms; }
int mgu_jitter_delay(void) { return g_delay_ms; }

static int64_t base_locked(const mgu_jitter_t *q) { return q->cur < q->prev ? q->cur : q->prev; }

/* the transit of a packet arriving now -> its due time */
static int64_t due_locked(mgu_jitter_t *q, uint64_t ntp_time, int64_t now, int64_t delay) {
    const int64_t transit = realtime_ns() - (int64_t) ntp_time;
    if (!q->have || transit < base_locked(q) - RESET_NS || transit > base_locked(q) + RESET_NS) {
        q->have = true;
        q->cur = q->prev = transit;
        q->bucket_start = now;
    } else if (now - q->bucket_start >= BUCKET_NS) {
        q->prev = q->cur;
        q->cur = transit;
        q->bucket_start = now;
    } else if (transit < q->cur) {
        q->cur = transit;
    }
    int64_t wait = base_locked(q) + delay - transit;
    if (wait < 0) wait = 0;
    if (wait > delay + 100 * MS) wait = delay + 100 * MS;
    return now + wait;
}

static void *worker(void *arg) {
    mgu_jitter_t *q = arg;
    pthread_mutex_lock(&q->mu);
    for (;;) {
        while (!q->head) pthread_cond_wait(&q->cv, &q->mu);
        item_t *it = q->head;
        q->head = it->next;
        if (!q->head) q->tail = NULL;
        q->count--;
        const unsigned gen = q->gen;
        const bool catch_up = q->count > MAX_QUEUED;
        q->busy = true;
        pthread_mutex_unlock(&q->mu);
        int drop = 0;
        if (q->wait_first && !catch_up) drop = mgu_jitter_wait(q, it->due, gen);
        if (!drop) {
            if (q->wait_first && mgu_jitter_now() > it->due + LATE_NS) mgu_jitter_late(q);
            q->fn(it->data, it->len, catch_up ? mgu_jitter_now() : it->due, q, gen);
        }
        free(it);
        pthread_mutex_lock(&q->mu);
        q->busy = false;
        pthread_cond_broadcast(&q->cv);
    }
    return NULL;
}

mgu_jitter_t *mgu_jitter_new(mgu_jitter_fn fn, bool wait_first) {
    mgu_jitter_t *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    pthread_mutex_init(&q->mu, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&q->cv, &ca);
    pthread_condattr_destroy(&ca);
    q->fn = fn;
    q->wait_first = wait_first;
    pthread_t t;
    if (pthread_create(&t, NULL, worker, q) != 0) { free(q); return NULL; }
    pthread_detach(t);
    return q;
}

void mgu_jitter_push(mgu_jitter_t *q, const unsigned char *data, int len, uint64_t ntp_time) {
    if (!q || !data || len <= 0) return;
    const int64_t now = mgu_jitter_now(), delay = (int64_t) g_delay_ms * MS;
    pthread_mutex_lock(&q->mu);
    const int64_t due = due_locked(q, ntp_time, now, delay);
    if (delay == 0 && !q->head && !q->busy) {
        /* no buffer: handed on here, as it arrives (busy keeps a flush waiting for it) */
        const unsigned gen = q->gen;
        q->busy = true;
        pthread_mutex_unlock(&q->mu);
        q->fn(data, len, now, q, gen);
        pthread_mutex_lock(&q->mu);
        q->busy = false;
        pthread_cond_broadcast(&q->cv);
        pthread_mutex_unlock(&q->mu);
        return;
    }
    pthread_mutex_unlock(&q->mu);
    item_t *it = malloc(sizeof *it + (size_t) len);
    if (!it) return;
    it->next = NULL;
    it->due = due;
    it->len = len;
    memcpy(it->data, data, (size_t) len);
    pthread_mutex_lock(&q->mu);
    if (q->tail) q->tail->next = it; else q->head = it;
    q->tail = it;
    q->count++;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
}

void mgu_jitter_flush(mgu_jitter_t *q) {
    if (!q) return;
    pthread_mutex_lock(&q->mu);
    q->gen++;
    item_t *it = q->head;
    q->head = q->tail = NULL;
    q->count = 0;
    q->have = false;   /* a new stream: UxPlay starts its clock offset again */
    pthread_cond_broadcast(&q->cv);
    while (q->busy) pthread_cond_wait(&q->cv, &q->mu);
    pthread_mutex_unlock(&q->mu);
    while (it) { item_t *n = it->next; free(it); it = n; }
}

int mgu_jitter_wait(mgu_jitter_t *q, int64_t due_ns, unsigned gen) {
    struct timespec ts = { (time_t) (due_ns / 1000000000LL), (long) (due_ns % 1000000000LL) };
    pthread_mutex_lock(&q->mu);
    while (q->gen == gen && mgu_jitter_now() < due_ns)
        if (pthread_cond_timedwait(&q->cv, &q->mu, &ts) == ETIMEDOUT) break;
    const int flushed = q->gen != gen;
    pthread_mutex_unlock(&q->mu);
    return flushed;
}

void mgu_jitter_late(mgu_jitter_t *q) {
    pthread_mutex_lock(&q->mu);
    q->late++;
    pthread_mutex_unlock(&q->mu);
}

void mgu_jitter_stats(mgu_jitter_t *q, mgu_jitter_stats_t *s) {
    if (!s) return;
    memset(s, 0, sizeof *s);
    s->delay_ms = g_delay_ms;
    if (!q) return;
    pthread_mutex_lock(&q->mu);
    s->queued = q->count;
    s->late = q->late;
    s->base_ms = q->have ? base_locked(q) / 1e6 : 0;
    pthread_mutex_unlock(&q->mu);
}
