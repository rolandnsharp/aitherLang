/* Defense 2: when audio output is NaN/Inf the engine resets the voice's
 * pool and the next tick recovers cleanly. This test exercises the
 * reset-pool helper that the audio callback calls on detection. */

#include "check.h"

/* A patch with a stateful primitive so the lpf reads from the pool. */
static const char* Patch = "sin(TAU * phasor(220)) |> lpf(800, 0.5)\n";

TEST(nan_recovery) {
  Voice* v = must_load(Patch, 48000.0);
  if (!v) return;
  double l, r;
  /* Warm up briefly so the pool holds non-trivial state. */
  for (int i = 1; i <= 8; i++) tick_at(v, (double)i / 48000.0, &l, &r);

  /* Poison the pool by hand — simulates an unstable filter / resonator
   * that produced NaN at some point in its accumulator. (In production
   * the source is the patch itself; here we inject directly because a
   * clean patch wouldn't generate NaN deterministically.) */
  double* pool = ((VoiceHeader*)v->cur.state)->pool;
  for (int k = 0; k < 16; k++) pool[k] = NAN;

  tick_at(v, 9.0 / 48000.0, &l, &r);
  CHECK(l != l, "expected NaN tick after pool poisoning, got %g", l);

  /* Defense 2 in the audio callback: zero the whole pool, then continue. */
  voice_reset_pool(v);
  tick_at(v, 10.0 / 48000.0, &l, &r);
  CHECK(l == l && r == r, "expected non-NaN after pool reset, got (%g, %g)", l, r);
}
