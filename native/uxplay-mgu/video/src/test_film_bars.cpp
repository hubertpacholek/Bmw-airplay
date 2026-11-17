// SPDX-License-Identifier: GPL-3.0-or-later
/* test_film_bars: video/src/film_bars.cpp off the car.
 *   c++ -O2 -Ivideo/src video/src/test_film_bars.cpp video/src/film_bars.cpp -o /tmp/tfb && /tmp/tfb
 * A 1920x720 frame with the iPhone's picture in its middle 1560x720 (the decoder's bars at the sides): a 2.39:1 film
 * letterboxed in it is found after 6 s, subtitles in its bottom bar keep that bar (1 s), a black frame changes
 * nothing, a bar on one side only is no film's, and the iPhone's screen without bars again is the whole picture (1 s).
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "film_bars.h"

static const unsigned W = 1920, H = 720, PX = 180, PW = 1560;
static std::vector<uint8_t> y(W * H);

/* the picture: black outside x 180..1740 and outside rows top..H-bottom; content textured within */
static void picture(unsigned top, unsigned bottom) {
    for (unsigned r = 0; r < H; r++)
        for (unsigned c = 0; c < W; c++)
            y[r * W + c] = (c >= PX && c < PX + PW && r >= top && r < H - bottom) ? (uint8_t) (60 + (r * 7 + c * 3) % 150) : 16;
}

static int frames(FilmBars *s, unsigned n) {
    int changed = 0;
    for (unsigned i = 0; i < n; i++) changed += film_bars_frame(s, y.data(), W, PX, 0, PW, H);
    return changed;
}

int main() {
    FilmBars s;
    film_bars_reset(&s);
    picture(34, 34);                       /* 1560x652: 2.39:1 */
    frames(&s, 15 * 11);
    assert(s.film[2] == 0);                /* not yet: 11 looks */
    frames(&s, 15);
    assert(s.film[0] == 180 && s.film[1] == 34 && s.film[2] == 1560 && s.film[3] == 652);
    printf("letterboxed film found after 12 looks: %ux%u at %u,%u: ok\n", s.film[2], s.film[3], s.film[0], s.film[1]);

    /* a subtitle in the bottom bar: that bar shrinks, both sides keep the smaller one, within 1 s */
    for (unsigned r = 690; r < 700; r++) for (unsigned c = 800; c < 1100; c++) y[r * W + c] = 235;
    assert(frames(&s, 30) == 1 && s.film[1] == 20 && s.film[3] == 680);
    printf("subtitle in the bar: the film grows to keep it: ok\n");
    picture(34, 34);
    frames(&s, 15 * 11);
    assert(s.film[3] == 680);              /* shrinks again only after 6 s */
    frames(&s, 15);
    assert(s.film[3] == 652);
    printf("subtitle gone: back after 6 s: ok\n");

    /* a black frame says nothing */
    memset(y.data(), 16, y.size());
    assert(frames(&s, 15 * 20) == 0 && s.film[3] == 652);
    printf("black frames: no change: ok\n");

    /* a bar on one side only: no film's */
    FilmBars t;
    film_bars_reset(&t);
    picture(80, 0);
    assert(frames(&t, 15 * 20) == 0 && t.film[2] == 0);
    printf("one-sided bar: none: ok\n");

    /* the iPhone's screen again, no bars: the whole picture within 1 s */
    picture(0, 0);
    assert(frames(&s, 30) == 1 && s.film[2] == 0);
    printf("no bars again: the whole picture: ok\n");
    return 0;
}
