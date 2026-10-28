/* Item 1 test: top-level `$state` values migrate by name across hot
 * reload. Patch A accumulates `$phase` for 100 ticks (-> 1.0). Reload
 * the same patch; the first tick should see the migrated phase, not a
 * fresh 0. */

#include "check.h"

static const char* PatchA =
  "$phase = 0\n"
  "$phase = $phase + 0.01\n"
  "$phase\n";

static const char* PatchB =
  "$phase = 0\n"
  "$freq = 440\n"
  "$phase = $phase + 0.01\n"
  "$phase + $freq * 0\n";

/* A patch that returns freq. */
static const char* PatchC =
  "$phase = 0\n"
  "$freq = 440\n"
  "$freq\n";

TEST(migration) {
  Voice* v = must_load(PatchA, 48000.0);
  if (!v) return;
  double l, r;
  for (int i = 1; i <= 100; i++) tick_at(v, (double)i / 48000.0, &l, &r);
  /* After 100 ticks, phase is 1.0. */
  tick_at(v, 0.0, &l, &r);
  NEAR(l, 1.01, 1e-9, "pre-reload phase");

  /* Reload the same program. Without migration, phase would reset to 0
   * (and the first tick would return 0.01). With migration, phase is
   * preserved (the first tick returns 1.02). */
  reload(v, PatchA, 48000.0);
  tick_at(v, 0.0, &l, &r);
  NEAR(l, 1.02, 1e-9, "migrated phase");

  /* A program that keeps `phase` but adds a new `freq`: phase stays
   * migrated (1.02 -> 1.03; freq * 0 = 0), freq initializes to 440. */
  reload(v, PatchB, 48000.0);
  tick_at(v, 0.0, &l, &r);
  NEAR(l, 1.03, 1e-9, "phase after adding the freq var");
  reload(v, PatchC, 48000.0);
  tick_at(v, 0.0, &l, &r);
  NEAR(l, 440.0, 1e-9, "freq migrated from the prior compilation");
}
