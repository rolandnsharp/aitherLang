/* Runaway guard: the engine mutes a voice whose output passes ±8
 * (+18 dBFS). This pins the threshold from both sides: a genuinely
 * unstable patch must trip it, and loud-but-healthy shipped patches
 * (bush_doof runs close to 0 dBFS) must never trip it. */

#include "check.h"

enum { SR = 48000 };

/* engine.c's RUNAWAY_LIMIT and its test (static there). */
static const double RunawayLimit = 8.0;
static int is_runaway(double l, double r) { return fabs(l) > RunawayLimit || fabs(r) > RunawayLimit; }

/* The sample index where the guard trips, or -1. */
static int first_runaway(Voice* v, double seconds) {
  double l, r;
  for (int i = 0; i < (int)(seconds * SR); i++) {
    tick_at(v, (double)i / SR, &l, &r);
    if (is_runaway(l, r)) return i;
  }
  return -1;
}

/* Unstable: grows 0.1% per sample, passes 8 after ~2100 samples. */
static const char* Unstable =
  "$x = 0.01\n"
  "$x = $x * 1.001\n"
  "$x\n";

TEST(runaway_guard) {
  Voice* u = must_load(Unstable, SR);
  if (!u) return;
  int hit = first_runaway(u, 1.0);
  CHECK(hit >= 0, "unstable patch never tripped the runaway guard");

  CHECK(!is_runaway(1.0, -1.0), "full scale is not a runaway");
  CHECK(!is_runaway(RunawayLimit, 0.0), "limit itself is allowed");
  CHECK(is_runaway(0.0, -RunawayLimit * 1.01), "past the limit is a runaway");
  CHECK(is_runaway(INFINITY, 0.0), "Inf is a runaway");

  const char* healthy[] = {"patches/bush_doof.aither", "patches/qigong_drift.aither"};
  for (int i = 0; i < 2; i++) {
    char* src = slurp(healthy[i]);
    CHECK(src, "can't read %s", healthy[i]);
    if (!src) continue;
    Voice* v = must_load(src, SR);
    if (!v) continue;
    int at = first_runaway(v, 3.0);
    CHECK(at < 0, "%s tripped the runaway guard at sample %d", healthy[i], at);
    free(src);
  }
  printf("  unstable tripped at sample %d\n", hit);
}
