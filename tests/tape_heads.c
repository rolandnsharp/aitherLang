/* tape(sig, on, w, r, max_time, keep): a piece of tape with a write
 * head and a read head at positions given as signals. A delay, a loop,
 * half speed, reverse and sound on sound are choices of the heads; the
 * pool holds a tape far longer than the old fixed 4 MB. */

#include "check.h"

#define SR 48000.0

/* `n` samples of the patch; a failed check (and r.n = 0) if it doesn't
 * render. */
static Rendered both(const char* src, long n) {
  Rendered r = {0};
  char err[4096];
  if (render_src(src, (double)n / SR, 48000, &r, err, sizeof err) != 0) {
    CHECK(0, "doesn't render: %s\n%s", err, src);
    r = (Rendered){0};
  }
  CHECK(r.n >= n, "%ld samples of %ld from\n%s", r.n, n, src);
  return r;
}

TEST(tape_heads) {
  /* A delay: the write head at now, the read head 3 samples behind. */
  Rendered d = both("let x = if t < dt then 1 else 0\ntape(x, 1, t, t - 3 / sr, 0.01)", 8);
  if (d.n >= 8) {
    double sum = 0.0;
    for (long i = 0; i < d.n; i++) sum += d.left[i];
    CHECK(fabs(d.left[3] - 1.0) < 1e-9 && fabs(sum - 1.0) < 1e-9, "delay: [3] is %g, sum %g",
          d.left[3], sum);
  }
  rendered_free(&d);

  /* A loop: half a second recorded, then played round and round,
   * exactly. Left is what went in, right is the tape, which reads before
   * it writes: silence the first time round, then what was recorded
   * half a second ago. */
  Rendered lp = both("let x = noise()\n[x, tape(x, if t < 0.5 then 1 else 0, t, t, 0.5)]", 72000);
  if (lp.n >= 72000) {
    for (long i = 0; i < 24000; i++)
      if (lp.right[i] != 0.0) {
        CHECK(0, "fresh tape at %ld: %g", i, lp.right[i]);
        break;
      }
    for (long i = 24000; i < 72000; i++)
      if (!(fabs(lp.right[i] - lp.left[i % 24000]) < 1e-9)) {
        CHECK(0, "loop at %ld: %g, recorded %g", i, lp.right[i], lp.left[i % 24000]);
        break;
      }
  }
  rendered_free(&lp);

  /* Half speed: a 1 kHz tone read at half the rate it was written is
   * 500 Hz. */
  Rendered hs = both("tape(sin(TAU * 1000 * t), 1, t, t * 0.5, 1)", 48000);
  if (hs.n >= 48000) {
    int crossings = 0;
    for (long i = 24001; i < 48000; i++)
      if ((hs.left[i - 1] < 0.0) != (hs.left[i] < 0.0)) crossings++;
    CHECK(abs(crossings - 500) <= 2, "half speed: %d crossings in 0.5 s", crossings);
  }
  rendered_free(&hs);

  /* Reverse: a second of the sample count written, then read backwards. */
  Rendered rv = both("tape(t * sr, if t < 1 then 1 else 0, t, 2 - t, 1)", 96000);
  if (rv.n >= 96000)
    for (long i = 48010; i < 95990; i++)
      if (!(fabs(rv.left[i] - (96000.0 - (double)i)) < 1e-6)) {
        CHECK(0, "reverse at %ld: %g", i, rv.left[i]);
        break;
      }
  rendered_free(&rv);

  /* keep: sound on sound. An impulse goes round a 100-sample tape
   * (0.0021 s at 48 kHz), each pass written over itself at half: 1,
   * 0.5, 0.25. */
  Rendered ov = both("let x = if t < dt then 1 else 0\ntape(x, 1, t, t, 0.0021, keep = 0.5)", 320);
  if (ov.n >= 320)
    CHECK(fabs(ov.left[100] - 1.0) < 1e-9 && fabs(ov.left[200] - 0.5) < 1e-9 &&
          fabs(ov.left[300] - 0.25) < 1e-9, "sound on sound: %g %g %g", ov.left[100],
          ov.left[200], ov.left[300]);
  rendered_free(&ov);

  /* A pair: two tapes on the same heads, each side its own. */
  Rendered pr = both("let x = if t < dt then 1 else 0\ntape([x, x * 2], 1, t, t - 5 / sr, 0.01)", 8);
  if (pr.n >= 8)
    CHECK(fabs(pr.left[5] - 1.0) < 1e-9 && fabs(pr.right[5] - 2.0) < 1e-9, "pair: %g, %g",
          pr.left[5], pr.right[5]);
  rendered_free(&pr);

  /* Thirty seconds of stereo: 2.88 million slots, seven times the old
   * pool. */
  Rendered lg = both("let x = if t < dt then 1 else 0\ntape([x, x], 1, t, t - 10 / sr, 30)", 16);
  if (lg.n >= 16)
    CHECK(fabs(lg.left[10] - 1.0) < 1e-9 && fabs(lg.right[10] - 1.0) < 1e-9, "long: %g, %g",
          lg.left[10], lg.right[10]);
  rendered_free(&lg);
}
