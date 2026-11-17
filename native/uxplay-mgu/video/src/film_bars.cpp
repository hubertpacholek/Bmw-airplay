// SPDX-License-Identifier: GPL-3.0-or-later
/* film_bars.cpp - where a film is within the picture, its black bars left out.
 *
 * Mirrored, a film comes inside the iPhone's screen, and the iPhone puts black bars around it where the shapes differ
 * (a 2.39:1 film on a 19.5:9 screen: bars above and below); the head unit's screen, wider still, adds its own at the
 * sides. Every 15th frame the rows and columns at the picture's edges are looked at (every 4th pixel): a row or
 * column of black only (luma 32 at most, limited range) is a bar. Bars count as a film's when they are on both
 * opposite sides, each at least 2 % of the picture; the film's rect is then the picture less the smaller bar of each
 * pair (centred, as players put films; subtitles in a bar keep that side). A frame black almost all over (a dark
 * scene, a fade) says nothing. The rect changes once a look has found the same for long enough: 1 s when the film
 * grows (bars go: the iPhone's own screen again, or subtitles in a bar), 6 s when it shrinks (bars come), so a
 * dark scene does not zoom in and subtitles that come and go do not zoom in and out.
 */
#include "film_bars.h"

#include <string.h>

namespace {
const unsigned kEvery = 15;          /* frames between two looks */
const unsigned kGrowLooks = 2;       /* ~1 s at 30 fps */
const unsigned kShrinkLooks = 12;    /* ~6 s */
const uint8_t kBlack = 32;

bool row_black(const uint8_t *y, uint32_t pitch, uint32_t r, uint32_t x0, uint32_t x1) {
    const uint8_t *p = y + (size_t) r * pitch;
    for (uint32_t x = x0; x < x1; x += 4)
        if (p[x] > kBlack) return false;
    return true;
}

bool col_black(const uint8_t *y, uint32_t pitch, uint32_t c, uint32_t y0, uint32_t y1) {
    for (uint32_t r = y0; r < y1; r += 4)
        if (y[(size_t) r * pitch + c] > kBlack) return false;
    return true;
}

uint64_t area(const uint32_t *r) { return (uint64_t) r[2] * r[3]; }
bool near(const uint32_t *a, const uint32_t *b) {
    for (int i = 0; i < 4; i++) {
        const uint32_t d = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        if (d > 8) return false;
    }
    return true;
}
}  // namespace

void film_bars_reset(FilmBars *s) { memset(s, 0, sizeof *s); }

bool film_bars_frame(FilmBars *s, const uint8_t *y, uint32_t pitch, uint32_t px, uint32_t py, uint32_t pw, uint32_t ph) {
    if (!y || pw < 64 || ph < 64) return false;
    if (++s->frames < kEvery) return false;
    s->frames = 0;
    uint32_t top = 0, bottom = 0, left = 0, right = 0;
    while (top < ph / 2 && row_black(y, pitch, py + top, px, px + pw)) top++;
    while (bottom < ph / 2 && row_black(y, pitch, py + ph - 1 - bottom, px, px + pw)) bottom++;
    if ((uint64_t) (top + bottom) * 10 >= (uint64_t) ph * 8) return false;   /* black almost all over */
    const uint32_t r0 = py + top, r1 = py + ph - bottom;
    while (left < pw / 2 && col_black(y, pitch, px + left, r0, r1)) left++;
    while (right < pw / 2 && col_black(y, pitch, px + pw - 1 - right, r0, r1)) right++;
    if ((uint64_t) (left + right) * 10 >= (uint64_t) pw * 8) return false;
    /* a film's bars: on both opposite sides, at least 2 % each; the smaller of the pair on both */
    uint32_t v = top < bottom ? top : bottom, h = left < right ? left : right;
    if ((uint64_t) v * 50 < ph) v = 0;
    if ((uint64_t) h * 50 < pw) h = 0;
    uint32_t c[4] = {0, 0, 0, 0};
    if (v || h) { c[0] = (px + h) & ~1u; c[1] = (py + v) & ~1u; c[2] = (pw - 2 * h) & ~1u; c[3] = (ph - 2 * v) & ~1u; }
    if (near(c, s->cand)) s->same++;
    else { memcpy(s->cand, c, sizeof c); s->same = 1; }
    if (near(c, s->film)) return false;
    /* the film's area now: none (w 0) is the whole picture */
    const uint64_t now = s->film[2] ? area(s->film) : (uint64_t) pw * ph, next = c[2] ? area(c) : (uint64_t) pw * ph;
    if (s->same < (next > now ? kGrowLooks : kShrinkLooks)) return false;
    memcpy(s->film, c, sizeof c);
    return true;
}
