/* lp1 / hp1 take hertz: the -3 dB point of lp1(x, fc) is at fc. They
 * used to take a coefficient (a = cutoff / sr), which put lp1(x, 4000)
 * at 665 Hz; every patch was converted to the hertz it already
 * produced. hp1 is the input minus lp1 (so lp1 + hp1 = the input,
 * exactly); that shape lands within 0.6 dB of -3 dB at fc up to 1 kHz. */

#include "check.h"

/* Steady-state gain of a sine at f through the patch (its `x`). */
static double gain_at(const char* expr, double f) {
  char src[512];
  snprintf(src, sizeof src, "let x = sin(TAU * phasor(%.17g))\n%s", f, expr);
  Voice* v = must_load(src, 48000.0);
  if (!v) return NAN;
  double in_e = 0.0, out_e = 0.0;
  for (int i = 0; i < 96000; i++) {
    double l, r;
    tick_at(v, (double)i / 48000.0, &l, &r);
    if (i >= 48000) {
      double x = sin(2.0 * M_PI * f * (double)(i + 1) / 48000.0);
      in_e += x * x;
      out_e += l * l;
    }
  }
  return sqrt(out_e / in_e);
}

TEST(lp1_hertz) {
  char expr[128];
  /* lowpass at -3 dB */
  const double lp_fc[] = {100.0, 1000.0, 4000.0};
  for (int k = 0; k < 3; k++) {
    snprintf(expr, sizeof expr, "lp1(x, %.17g)", lp_fc[k]);
    double g = 20 * log10(gain_at(expr, lp_fc[k]));
    CHECK(fabs(g + 3.0) < 0.35, "lp1 at %g Hz: %g dB, want -3", lp_fc[k], g);
  }
  /* highpass at -3 dB */
  const double hp_fc[] = {100.0, 1000.0};
  for (int k = 0; k < 2; k++) {
    snprintf(expr, sizeof expr, "hp1(x, %.17g)", hp_fc[k]);
    double g = 20 * log10(gain_at(expr, hp_fc[k]));
    CHECK(fabs(g + 3.0) < 0.7, "hp1 at %g Hz: %g dB, want -3", hp_fc[k], g);
  }
  /* pass and stop */
  double g = gain_at("lp1(x, 1000)", 50);
  CHECK(g > 0.99, "lp1(x, 1000) at 50 Hz passes: gain %g", g);
  g = gain_at("lp1(x, 100)", 5000);
  CHECK(g < 0.03, "lp1(x, 100) at 5 kHz is 6 dB/octave down: gain %g", g);
}
