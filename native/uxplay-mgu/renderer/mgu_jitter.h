/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * mgu_jitter.h - the jitter buffer between UxPlay and the GPU decoder / the sound (renderer/mgu_jitter.c).
 */
#ifndef MGU_JITTER_H
#define MGU_JITTER_H
#include <stdbool.h>
#include <stdint.h>

typedef struct mgu_jitter mgu_jitter_t;

/* Called on the queue's own thread, in arrival order, with the time the packet is due (CLOCK_MONOTONIC, ns) and
 * the queue's generation; `wait_first` queues have already waited for it. */
typedef void (*mgu_jitter_fn)(const unsigned char *data, int len, int64_t due_ns, mgu_jitter_t *q, unsigned gen);

typedef struct mgu_jitter_stats_s {
    unsigned queued;            /* packets waiting now */
    unsigned long long late;    /* packets handed on more than 40 ms after they were due */
    int delay_ms;               /* the buffer's delay */
    double base_ms;             /* the shortest transit seen lately (now - ntp_time), ms */
} mgu_jitter_stats_t;

mgu_jitter_t *mgu_jitter_new(mgu_jitter_fn fn, bool wait_first);
/* a copy of the packet, due at its ntp_time (UxPlay's local clock, CLOCK_REALTIME ns) + the delay, taken from
 * the shortest transit seen in the last 2 to 4 s; with no delay, fn runs here at once */
void mgu_jitter_push(mgu_jitter_t *q, const unsigned char *data, int len, uint64_t ntp_time);
/* drops what waits and returns once nothing is being handed on (a packet already in fn has returned) */
void mgu_jitter_flush(mgu_jitter_t *q);
/* waits until due_ns; 0 when due, 1 when the queue was flushed meanwhile (the packet is to be dropped) */
int mgu_jitter_wait(mgu_jitter_t *q, int64_t due_ns, unsigned gen);
void mgu_jitter_late(mgu_jitter_t *q);
void mgu_jitter_stats(mgu_jitter_t *q, mgu_jitter_stats_t *s);

/* the delay of every queue, 0..1000 ms (0: each packet handed on as it arrives) */
void mgu_jitter_set_delay(int ms);
int mgu_jitter_delay(void);
int64_t mgu_jitter_now(void);   /* CLOCK_MONOTONIC, ns */
#endif
