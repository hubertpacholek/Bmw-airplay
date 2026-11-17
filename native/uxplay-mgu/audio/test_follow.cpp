// SPDX-License-Identifier: GPL-3.0-or-later
/* test_follow: mgu_audio.cpp's way to the device off the car, against sim_alsa.c (a device that plays in real time,
 * its clock SIM_PPM off). `test_follow SECONDS` sends ALAC packets (352 frames, stored verbatim: there is no encoder
 * here) at the iPhone's pace, each up to 3 ms late, for SECONDS; once the device has started, it must never run dry
 * nor overflow, and with SIM_PPM of 1000 or more the clock follower must have left out or repeated samples.
 *   make test-follow   (in build.sh's container; runs at +2000, -2000 and 0 ppm) */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vector>
#include "mgu_audio.h"

static const int kFrames = 352, kRate = 44100;

struct Bits {
    std::vector<unsigned char> b;
    int n = 0;
    void put(uint32_t v, int bits) {
        for (int i = bits - 1; i >= 0; i--) {
            if (n % 8 == 0) b.push_back(0);
            if ((v >> i) & 1) b.back() |= (unsigned char) (0x80 >> (n % 8));
            n++;
        }
    }
};

/* one stereo ALAC frame, verbatim: a channel pair element, the samples as they are, the end */
static std::vector<unsigned char> alac_packet(int first) {
    Bits w;
    w.put(1, 3);    // channel pair element
    w.put(0, 4);    // its instance
    w.put(0, 12);   // unused
    w.put(0, 1);    // the frame's samples: the default (352)
    w.put(0, 2);    // no extra bits
    w.put(1, 1);    // verbatim
    for (int i = 0; i < kFrames; i++) {
        const int16_t s = (int16_t) (8000 * sin(2 * M_PI * 440 * (first + i) / kRate));
        w.put((uint16_t) s, 16);
        w.put((uint16_t) s, 16);
    }
    w.put(7, 3);    // end
    return w.b;
}

static double now() { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void sleep_until(double t) {
    double d = t - now();
    if (d <= 0) return;
    struct timespec s = { (time_t) d, (long) ((d - (time_t) d) * 1e9) };
    nanosleep(&s, nullptr);
}

int main(int argc, char **argv) {
    const double secs = argc > 1 ? atof(argv[1]) : 20;
    const double ppm = getenv("SIM_PPM") ? atof(getenv("SIM_PPM")) : 0;
    setenv("MGU_AUDIO", "alsa:sim", 1);
    if (mgu_audio_start(2) != 0) { fprintf(stderr, "start: %s\n", mgu_audio_error()); return 1; }
    srand(3);
    const double t0 = now();
    const int packets = (int) (secs * kRate / kFrames);
    for (int i = 0; i < packets; i++) {
        sleep_until(t0 + (double) i * kFrames / kRate + (rand() % 3000) / 1e6);
        std::vector<unsigned char> p = alac_packet(i * kFrames);
        mgu_audio_packet(p.data(), (int) p.size());
    }
    mgu_audio_stats_t s;
    mgu_audio_stats(&s);
    printf("%+.0f ppm, %.0f s: packets %llu decoded %llu errors %llu, underruns %llu, left out %llu, samples followed %llu, device %.0f ms\n",
           ppm, secs, s.packets, s.decoded, s.errors, s.xruns, s.dropped_full, s.adjusted, s.device_ms);
    mgu_audio_stop();
    const bool ok = s.decoded == s.packets && s.errors == 0 && s.xruns == 0 && s.dropped_full == 0 && (fabs(ppm) < 1000 || s.adjusted > 0);
    printf("%s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
