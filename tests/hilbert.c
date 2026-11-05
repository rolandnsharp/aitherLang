/* analytic(x) is x as a spin: the pair's second side the first a quarter
 * turn behind, at every frequency in the band, the two the same size, so
 * the point goes round a circle and doesn't wobble along a line (SPEC.md,
 * "freq_shift"). Measured through a compiled patch, so the way round the
 * compiler gives it is checked too. Until
 * 2026-11-05 the Hilbert stages used Niemitalo's coefficients unsquared
 * and missed his one-sample delay: 4.5 degrees off at 441 Hz, 85 at
 * 10 kHz. */

#include <complex.h>
#include "check.h"

TEST(hilbert) {
  const double sr = 48000.0;
  const double freqs[] = {30, 100, 441, 1000, 3000, 5000, 10000, 15000, 20000};
  for (size_t k = 0; k < sizeof freqs / sizeof *freqs; k++) {
    double f = freqs[k];
    char src[128];
    snprintf(src, sizeof src, "analytic(sin(TAU * phasor(%g)))", f);
    Voice* v = must_load(src, sr);
    if (!v) return;
    /* Past the filters' start, then whole cycles of about 0.2 s. */
    long skip = (long)(0.25 * sr), n = (long)lround(sr / f * floor(0.2 * f + 0.5));
    double complex first = 0, second = 0;      /* each side's component at f */
    double l, r;
    for (long i = 0; i < skip + n; i++) {
      tick_at(v, i / sr, &l, &r);
      if (i < skip) continue;
      double complex w = cexp(-I * 2 * M_PI * f * i / sr);
      first += l * w;
      second += r * w;
    }
    double behind = carg(second / first) * 180 / M_PI;      /* -90: a quarter turn behind */
    double size = cabs(second) / cabs(first);
    CHECK(fabs(behind + 90) < 1.0, "%g Hz: the second side is %.2f degrees from the first, not -90 (a quarter turn behind)", f, behind);
    CHECK(fabs(size - 1) < 0.01, "%g Hz: the sides' sizes differ by %.3f", f, size);
  }
}
