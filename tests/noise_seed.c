/* noise() is seeded by where it is, so a stream doesn't move when other code
 * does: a compile before it, a line above it, more of the stdlib. A
 * destructuring around it used to take a running count into its hidden
 * name, and with it the seed. */

#include "check.h"

static const char* const Patch =
  "let (x, _) = dho(noise() * 100, 440, 0.02)\n"
  "x\n";

static double first_samples(const char* src, double* out, int n) {
  Voice* v = must_load(src, 48000.0);
  double l, r, sum = 0;
  for (int i = 0; i < n; i++) { tick_at(v, i / 48000.0, &l, &r); out[i] = l; sum += fabs(l); }
  voice_dispose(v);
  return sum;
}

TEST(noise_seed_keeps_its_place) {
  enum { N = 2000 };
  static double a[N], b[N], c[N];
  CHECK(first_samples(Patch, a, N) > 0, "no sound");
  first_samples("let (p, q) = (1, 2)\nlet (r, s) = (3, 4)\np + q + r + s\n", b, 10);   /* other destructurings, compiled between */
  first_samples(Patch, b, N);
  char edited[512];
  snprintf(edited, sizeof edited, "# a comment above\n\nlet unused = 1\n%s", Patch);   /* lines above it */
  first_samples(edited, c, N);
  for (int i = 0; i < N; i++) {
    if (a[i] != b[i]) { CHECK(0, "a recompile moved the noise at sample %d", i); break; }
    if (a[i] != c[i]) { CHECK(0, "a line above moved the noise at sample %d", i); break; }
  }
}
