/* A hot-swap that blows up can be rolled back: the previous code comes
 * back with its state as of the swap, and file-level `$vars` that went
 * NaN re-init instead of migrating (they used to poison every resend). */

#include "check.h"

/* n samples from t0 + 1; the last one's left. */
static double tick_n(Voice* v, int n, int t0) {
  double l = 0.0, r;
  for (int i = 1; i <= n; i++) tick_at(v, (double)(t0 + i) / 48000.0, &l, &r);
  return l;
}

static int can_rollback(Voice* v) { return v->prev.state != NULL; }

static const char* Good =
  "$n = 0\n"
  "$n = $n + 1\n"
  "sin(TAU * phasor(220)) * 0.2\n";
static const char* Bad =
  "$n = 0\n"
  "$n = $n + 1\n"
  "$x = 0.01\n"
  "$x = $x * 1.01\n"
  "sin(TAU * phasor(220)) * 0.2 + $x\n";

/* A NaN'd file-level var re-inits on the next send instead of migrating. */
static const char* NanVar =
  "$y = 1\n"
  "$y = $y + sqrt(0 - 1)\n"
  "0\n";
static const char* Fixed =
  "$y = 0.5\n"
  "$y = $y * 1\n"
  "$y\n";

TEST(rollback) {
  Voice* v = must_load(Good, 48000.0);
  if (!v) return;
  tick_n(v, 1000, 0);
  CHECK(!can_rollback(v), "a first load can roll back");
  reload(v, Bad, 48000.0);
  CHECK(can_rollback(v), "a reload can't roll back");
  double blown = tick_n(v, 1000, 1000);
  CHECK(fabs(blown) > 8.0, "bad patch should blow up: %g", blown);
  CHECK(voice_rollback(v), "rollback refused");
  CHECK(!can_rollback(v), "a rollback can roll back again");
  double back = tick_n(v, 100, 2000);
  CHECK(fabs(back) <= 0.2 + 1e-9, "rolled-back voice is sane: %g", back);

  Voice* w = must_load(NanVar, 48000.0);
  if (!w) return;
  tick_n(w, 3, 0);
  reload(w, Fixed, 48000.0);
  double first = tick_n(w, 1, 0);
  CHECK(first == 0.5, "NaN var should re-init, got %g", first);

  /* scrub_state clears a NaN var in place without touching healthy ones. */
  reload(w, NanVar, 48000.0);
  tick_n(w, 3, 0);
  voice_scrub_state(w, 0);
  reload(w, Fixed, 48000.0);
  first = tick_n(w, 1, 0);
  CHECK(first == 0.5, "after a scrub, got %g", first);
}
