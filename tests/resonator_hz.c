/* resonator(sig, freq, decay) rings at freq Hz, like every frequency in
 * aither. It squared freq as if it were radians per second until
 * 2026-10-18, so resonator(x, 440, ...) rang at 70 Hz and the GUIDE's
 * plucked string and bell were 2 pi too low. */

#include "check.h"

static double fundamental(const char* src) {
  Rendered r;
  char err[4096];
  if (render_src(src, 2.0, 48000, &r, err, sizeof err) != 0) {
    CHECK(0, "doesn't render: %s", err);
    return NAN;
  }
  double* mono = mono_mix(r.left, r.right, r.n);
  SpectrumSummary s;
  analyze(mono, r.n, 48000.0, 8, &s);
  free(mono);
  rendered_free(&r);
  return s.fundamental_hz;
}

TEST(resonator_hz) {
  const double hz[] = {110.0, 440.0, 1000.0};
  for (int k = 0; k < 3; k++) {
    char src[128];
    snprintf(src, sizeof src, "impulse(2) |> resonator(%.17g, 0.5)", hz[k]);
    double f = fundamental(src);
    CHECK(fabs(f - hz[k]) / hz[k] < 0.01, "resonator at %g Hz rings at %g", hz[k], f);
  }
}
