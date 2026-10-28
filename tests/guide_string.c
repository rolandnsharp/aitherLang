/* The GUIDE's "Spatial physics with arrays" string: stable, pitched,
 * and it rings. Keeps the documented example honest. */

#include "check.h"

static const char snippet[] =
  "def string(pluck, len, damp):\n"
  "  $y = array(len)                    # displacement\n"
  "  $v = array(len)                    # velocity\n"
  "  each(len, i =>\n"
  "    let force = ($y[i - 1] - 2 * $y[i] + $y[i + 1]) * 2e8\n"
  "    let v = ($v[i] + force * dt) * damp\n"
  "    $v[i] = v\n"
  "    $y[i] = $y[i] + v * dt)\n"
  "  $v[2] = $v[2] + pluck               # strike near one end\n"
  "  $y[len - 3]                         # listen near the other\n"
  "\n"
  "play guitar:\n"
  "  string(impulse(1) * 3000, 48, 0.99998) * 0.3\n"
  "\n"
  "guitar\n";

TEST(guide_string) {
  /* impulse(1) first fires at t = 1 s; listen from there. */
  Rendered r;
  char err[4096];
  if (render_src(snippet, 2.0, 48000, &r, err, sizeof err) != 0) {
    CHECK(0, "doesn't render: %s", err);
    return;
  }
  const double* sig = r.left;
  long n = r.n;
  double peak = 0.0, late = 0.0;
  for (long i = 0; i < n; i++) {
    if (sig[i] != sig[i]) {
      CHECK(0, "NaN at %ld", i);
      break;
    }
    peak = fmax(peak, fabs(sig[i]));
    if (i >= 48000 + 36000) late = fmax(late, fabs(sig[i]));
  }
  printf("  string peak %.3f late peak %.4f\n", peak, late);
  CHECK(peak > 0.01 && peak < 1.0, "peak %g", peak);
  CHECK(late > peak * 0.05, "the string should still ring 0.75 s after the pluck (late %g)", late);
  /* Period by autocorrelation: c/dx = sqrt(2e8) = 14142 /s over 49
   * gaps, ~144 Hz, a period of ~330 samples. */
  int best = 0;
  double best_r = -1e300;
  for (int lag = 200; lag <= 500; lag++) {
    double c = 0.0;
    for (long i = 52800; i < n - lag; i++) c += sig[i] * sig[i + lag];
    if (c > best_r) {
      best_r = c;
      best = lag;
    }
  }
  printf("  string period %d samples (%.1f Hz)\n", best, 48000.0 / (double)best);
  CHECK(best > 300 && best < 360, "period %d", best);
  rendered_free(&r);
}
