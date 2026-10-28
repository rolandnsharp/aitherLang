/* Defense 1: when the old pool's region for a (type, perTypeIdx) match
 * contains NaN/Inf, hot-reload migration leaves the new region zeroed
 * rather than copying the poison forward. Without this, fixing the
 * broken patch wouldn't recover — the migrated NaN state stays NaN
 * forever.
 *
 * Patch A feeds NaN into an lpf, polluting the lpf's pool slots. Patch
 * B is structurally compatible (same lpf at perTypeIdx 0) but reads a
 * clean signal. After reload, lpf state must be zeroed (skipped), so
 * the next tick produces a finite sample. */

#include "check.h"

static const char* PatchA = "sqrt(-1) |> lpf(800, 0.5)\n";
static const char* PatchB = "sin(TAU * phasor(220)) |> lpf(800, 0.5)\n";

TEST(migration_skip_poisoned) {
  Voice* v = must_load(PatchA, 48000.0);
  if (!v) return;
  double l, r;
  for (int i = 1; i <= 4; i++) tick_at(v, (double)i / 48000.0, &l, &r);

  /* Sanity: PatchA's lpf state should now contain NaN. */
  double* pool = ((VoiceHeader*)v->cur.state)->pool;
  int found_nan = 0;
  for (int i = 0; i < v->cur.nregions; i++) {
    VRegion* g = &v->cur.regions[i];
    if (strcmp(g->type_name, "lpf") == 0)
      for (long k = 0; k < g->size; k++) found_nan |= pool[g->offset + k] != pool[g->offset + k];
  }
  CHECK(found_nan, "test setup failed: lpf pool not poisoned");

  reload(v, PatchB, 48000.0);
  tick_at(v, 0.0, &l, &r);
  CHECK(l == l && r == r, "expected clean output after migrating away from poisoned pool, got %g, %g",
        l, r);

  /* And the lpf region in the new state should be zero (migration
   * skipped). After one tick the lpf has accumulated a tiny non-NaN
   * value, so we only check it isn't NaN. */
  double* pool2 = ((VoiceHeader*)v->cur.state)->pool;
  for (int i = 0; i < v->cur.nregions; i++) {
    VRegion* g = &v->cur.regions[i];
    if (strcmp(g->type_name, "lpf") == 0)
      for (long k = 0; k < g->size; k++)
        CHECK(pool2[g->offset + k] == pool2[g->offset + k], "lpf pool still poisoned after reload");
  }
}
