// SPDX-License-Identifier: GPL-3.0-or-later
/* film_bars: where a film is within the picture, its black bars left out (video/src/film_bars.cpp). */
#ifndef FILM_BARS_H
#define FILM_BARS_H
#include <stdint.h>

struct FilmBars {
    uint32_t film[4];    /* the film's rect in the frame (x, y, w, h); w 0: no bars, the whole picture */
    uint32_t cand[4];    /* what the last look found */
    unsigned same;       /* looks in a row that found cand */
    unsigned frames;     /* frames since the last look */
};

/* starts again (a new stream) */
void film_bars_reset(FilmBars *s);
/* one frame's luma plane (pitch bytes a row) and where the picture is in it (px, py, pw, ph: the decoder's letterbox
 * left out); every 15th frame is looked at. True when s->film changed. */
bool film_bars_frame(FilmBars *s, const uint8_t *y, uint32_t pitch, uint32_t px, uint32_t py, uint32_t pw, uint32_t ph);

#endif
