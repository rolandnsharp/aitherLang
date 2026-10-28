/* The background -O2 build: same C as the TCC fast path, compiled by cc
 * and linked in memory by TCC. It must drop into the loaded voice (same
 * state layout), sound the same, and be refused once the voice has been
 * reloaded. Also prints the speedup, since that's the point of it. */

#include <time.h>
#include "check.h"

enum { SR = 48000 };

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* The left channel of samples [from, from + n). */
static double* render(Voice* v, int from, int n) {
  double* out = calloc(n, sizeof *out);
  double r;
  for (int i = 0; i < n; i++) tick_at(v, (double)(from + i) / SR, &out[i], &r);
  return out;
}

TEST(optimized_build) {
  const char* path = getenv("AITHER_OPT_PATCH");   /* the Nim test's argument */
  if (!path) path = "patches/bush_doof.aither";
  char* src = slurp(path);
  CHECK(src, "can't read %s", path);
  if (!src) return;

  /* Two identical voices, run in lockstep; then upgrade one mid-stream. */
  Voice* a = must_load(src, SR);
  Voice* b = must_load(src, SR);
  if (!a || !b) return;
  int warm = SR / 2;
  free(render(a, 0, warm));
  free(render(b, 0, warm));

  void* lib;
  TickFn fn;
  char err[2048];
  int rc = voice_compile_optimized(b->cur.csrc, b->cur.state_size, 0, &lib, &fn, err, sizeof err);
  CHECK(rc == 0, "the -O2 build failed: %s", err);
  if (rc != 0) return;
  CHECK(voice_upgrade(b, b->gen, lib, fn), "upgrade refused for the current generation");
  CHECK(b->cur.optimized, "not optimized after the upgrade");

  int n = SR * 3;
  double t0 = now();
  double* slow = render(a, warm, n);
  double tcc_time = now() - t0;
  t0 = now();
  double* fast = render(b, warm, n);
  double opt_time = now() - t0;

  /* cc may fuse multiply-adds, so allow rounding-level drift, nothing audible. */
  double worst = 0.0;
  for (int i = 0; i < n; i++) worst = fmax(worst, fabs(slow[i] - fast[i]));
  CHECK(worst < 1e-6, "optimized output drifted by %g", worst);

  /* A stale build must not replace newer code. */
  reload(b, src, SR);
  CHECK(!voice_upgrade(b, b->gen - 1, lib, fn), "stale upgrade was accepted");
  CHECK(!b->cur.optimized, "optimized after a stale upgrade");

  double budget = 3.0;                 /* seconds of audio rendered */
  printf("  %s: max drift %g\n", path, worst);
  printf("  tcc: %.0f ms (%d%% of one core)\n", tcc_time * 1e3, (int)(tcc_time * 1e3 / (budget * 10)));
  printf("  -O2: %.0f ms (%d%% of one core)\n", opt_time * 1e3, (int)(opt_time * 1e3 / (budget * 10)));
}
