/* poly's voices finish (stdlib.aither, SPEC.md "Polyphony"): released,
 * and 80 dB under their own peak for 100 ms, a voice stops and plays 0
 * until its slot's next note-on; `tail = s` retires it s seconds after
 * release, `tail = -1` never. The threshold is the voice's own: one
 * sounding at 1e-6 rings on. Two patches holding one note keep their own
 * tails. */

#include "check.h"

static double run(Voice* v, double* t, double seconds) {
  /* Tick on from *t for `seconds`; the last sample's left side. */
  double l = 0, r = 0;
  long n = (long)(seconds * 48000.0);
  for (long i = 0; i < n; i++) {
    tick_at(v, *t, &l, &r);
    *t += 1.0 / 48000.0;
  }
  return l;
}

TEST(poly_retire) {
  midi_reset_all();
  Voice* rel = must_load("poly(4, (f, gate) => if gate > 0 then 0.1 else 0.000001)", 48000.0);
  Voice* fixed = must_load("poly(4, (f, gate) => 0.1, tail = 0.05)", 48000.0);
  Voice* never = must_load("poly(4, (f, gate) => 0.1, tail = -1)", 48000.0);
  Voice* faint = must_load("poly(4, (f, gate) => 0.000001 * (if gate > 0 then 1 else 0.5))", 48000.0);
  if (!rel || !fixed || !never || !faint) return;
  Voice* all[4] = {rel, fixed, never, faint};
  double t[4] = {0, 0, 0, 0}, l[4];

  midi_note_on(60, 100);
  for (int i = 0; i < 4; i++) l[i] = run(all[i], &t[i], 0.05);
  NEAR(l[0], 0.1, 1e-12, "held, the relative voice sounds");
  NEAR(l[3], 1e-6, 1e-15, "held, the faint voice sounds");

  midi_note_off(60);
  for (int i = 0; i < 4; i++) l[i] = run(all[i], &t[i], 0.03);
  NEAR(l[0], 1e-6, 1e-15, "30 ms after release: 80 dB down, not yet 100 ms quiet");
  NEAR(l[1], 0.1, 1e-12, "tail = 0.05: still sounding at 30 ms");
  for (int i = 0; i < 4; i++) l[i] = run(all[i], &t[i], 0.12);
  CHECK(l[0] == 0.0, "150 ms after release the relative voice has finished: %g", l[0]);
  CHECK(l[1] == 0.0, "tail = 0.05 has finished: %g", l[1]);
  NEAR(l[2], 0.1, 1e-12, "tail = -1 never finishes");
  NEAR(l[3], 5e-7, 1e-15, "the faint voice at half its peak rings on: its own level, not an absolute one");

  /* A new note in the slot wakes it. */
  midi_note_on(62, 100);
  l[0] = run(rel, &t[0], 0.01);
  NEAR(l[0], 0.1, 1e-12, "the next note-on wakes the voice");
  midi_reset_all();
}
