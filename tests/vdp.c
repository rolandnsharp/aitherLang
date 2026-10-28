/* vdp: a self-sustaining oscillator grows from its start to a circle of
 * radius 1 at its frequency, and two detuned ones on a spring lock. */

#include "check.h"

/* The left side of `secs` of the patch, its length in *n. */
static double* left_of(const char* src, double secs, long* n) {
  Rendered r;
  char err[4096];
  if (render_src(src, secs, 48000, &r, err, sizeof err) != 0) {
    CHECK(0, "doesn't render: %s", err);
    *n = 0;
    return calloc(1, sizeof(double));
  }
  double* l = r.left;
  *n = r.n;
  r.left = NULL;
  rendered_free(&r);
  return l;
}

static int crossings(const double* x, long a, long b) {
  int n = 0;
  for (long i = a + 1; i < b; i++)
    if ((x[i - 1] < 0.0) != (x[i] < 0.0)) n++;
  return n;
}

/* Two at 220 and 223 Hz on a spring of stiffness k. */
static double* sum_of(const char* k, long* n) {
  char src[512];
  snprintf(src, sizeof src,
           "$pa = 0.0\n$pb = 0.0\nlet a = vdp(220, push = %s * ($pb - $pa))\n"
           "let b = vdp(223, push = %s * ($pa - $pb))\n$pa = a[0]\n$pb = b[0]\na[0] + b[0]",
           k, k);
  return left_of(src, 6.0, n);
}

/* How much the sum's peak level varies over the last 2 s, in 50 ms
 * windows. */
static double swing(const double* x, long n) {
  double lo = 1e9, hi = 0.0;
  for (long i = 4 * 48000; i + 2400 <= n; i += 2400) {
    double p = 0.0;
    for (long j = i; j < i + 2400; j++) p = fmax(p, fabs(x[j]));
    lo = fmin(lo, p);
    hi = fmax(hi, p);
  }
  return hi - lo;
}

TEST(vdp) {
  /* Alone: a sine at 220 Hz, peak about 1 once grown, from 0.01. */
  long n;
  double* one = left_of("vdp(220)[0]", 2.0, &n);
  if (n >= 96000) {
    CHECK(fabs(one[0] - 0.01) < 0.001, "starts at %g", one[0]);
    double peak = 0.0;
    for (long i = 48000; i < 96000; i++) peak = fmax(peak, fabs(one[i]));
    CHECK(fabs(peak - 1.0) < 0.02, "radius %g", peak);
    int c = crossings(one, 48000, 96000);
    CHECK(abs(c - 440) <= 2, "%d crossings in a second", c);
  } else CHECK(0, "%ld samples", n);
  free(one);

  /* Two at 220 and 223 Hz: apart they beat, a 3 Hz swell; on a spring
   * of 0.1 they lock, and the sum's loudness holds still. */
  double* a = sum_of("0", &n);
  double apart = swing(a, n);
  free(a);
  double* b = sum_of("0.1", &n);
  double locked = swing(b, n);
  free(b);
  CHECK(apart > 1.0, "apart they should beat: %g", apart);
  CHECK(locked < 0.05, "on a spring they should lock: %g", locked);
  printf("  vdp beat swing %g, locked %g\n", apart, locked);
}
