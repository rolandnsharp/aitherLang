/* pixel(w, h) and pixel_index(w, h) follow the engine clock: the
 * engine's first sample is pixel 0 and the index steps one a sample,
 * wrapping at w*h, so a patch draws on the grid tv counts. If it slipped
 * by a sample the picture would shear. Run through tick_block, the
 * engine's path, which sets t from its own second/fraction clock. */

#include "check.h"

/* n samples from the engine's start, in blocks of 7 so block edges fall
 * everywhere. */
static void run(const char* src, double sr, long n, double* l, double* r) {
  Voice* v = must_load(src, sr);
  if (!v) return;
  VoiceHeader* hdr = v->cur.state;
  hdr->time_sec = 0.0;
  hdr->time_frac = 0.0;
  hdr->fade_gain = 1.0;
  hdr->fade_delta = 0.0;
  hdr->in_buf = NULL;
  for (long i = 0; i < n;) {
    long k = n - i < 7 ? n - i : 7;
    voice_tick_block(v, k, &l[i], &r[i]);
    i += k;
  }
}

/* The index: 0, 1, 2 ... wrapping at w*h. */
TEST(pixel_steps) {
  double idx[40] = {0}, r[40];
  run("[pixel_index(5, 3), 0]", 48000.0, 40, idx, r);
  for (int i = 0; i < 40; i++) CHECK(idx[i] == (double)(i % 15), "sample %d: %g", i, idx[i]);
}

/* The frame: 0 for the first w*h samples, then 1, ... */
TEST(pixel_frames) {
  double fr[40] = {0}, r[40];
  run("[frame_number(5, 3), 0]", 48000.0, 40, fr, r);
  for (int i = 0; i < 40; i++) CHECK(fr[i] == (double)(i / 15), "sample %d: %g", i, fr[i]);
}

/* The centre of each pixel: x across -1..1, y up, square pixels. */
TEST(pixel_centres) {
  double x[16] = {0}, y[16] = {0};
  run("pixel(4, 2)", 48000.0, 16, x, y);
  for (int i = 0; i < 16; i++) {
    int col = i % 4, row = (i / 4) % 2;
    CHECK(fabs(x[i] - ((double)(2 * col + 1) / 4 - 1)) < 1e-12, "x at %d: %g", i, x[i]);
    CHECK(fabs(y[i] - ((double)(2 - 2 * row - 1) / 4)) < 1e-12, "y at %d: %g", i, y[i]);
  }
}

/* At the 1.152 MHz a 160x120 tv takes, over more than a second, so the
 * clock's fraction wraps: not one sample slips. */
TEST(pixel_fast) {
  long n = 1200000;
  double* idx = calloc(n, sizeof *idx);
  double* r = calloc(n, sizeof *r);
  run("[pixel_index(160, 120), 0]", 1152000.0, n, idx, r);
  for (long i = 0; i < n; i++)
    if (idx[i] != (double)(i % 19200)) {
      CHECK(0, "slipped at sample %ld: %g", i, idx[i]);
      break;
    }
  free(idx);
  free(r);
}
