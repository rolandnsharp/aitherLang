/* Populate the MIDI state from the test thread via the writer API,
 * compile + tick a patch that reads from it, and verify the output
 * reflects the set values. */

#include "check.h"

TEST(midi_state) {
  double l, r;
  midi_reset_all();

  /* 1. midi_cc — CC 74 at value 64 maps to 64/127. */
  Voice* v1 = must_load("midi_cc(74)", 48000.0);
  if (!v1) return;
  midi_cc(74, 64);
  tick_at(v1, 0.0, &l, &r);
  CHECK(fabs(l - 64.0 / 127.0) < 1e-9, "cc got %.17g", l);
  CHECK(fabs(r - 64.0 / 127.0) < 1e-9, "cc right got %.17g", r);

  midi_cc(74, 32);
  tick_at(v1, 0.0, &l, &r);
  CHECK(fabs(l - 32.0 / 127.0) < 1e-9, "cc follow got %.17g", l);

  /* 2. midi_note(n) while held, 0 after release. */
  Voice* v2 = must_load("midi_note(60)", 48000.0);
  if (!v2) return;
  midi_note_on(60, 127);
  tick_at(v2, 0.0, &l, &r);
  CHECK(fabs(l - 1.0) < 1e-9, "held note got %.17g", l);
  midi_note_off(60);
  tick_at(v2, 0.0, &l, &r);
  CHECK(l == 0.0, "released note got %.17g", l);

  /* 3. midi_freq() & midi_gate() pair. */
  Voice* v3 = must_load("midi_gate() * midi_freq()", 48000.0);
  if (!v3) return;
  midi_note_on(69, 40);       /* A4 = 440; the gate is 1 however softly it's played */
  tick_at(v3, 0.0, &l, &r);
  CHECK(fabs(l - 440.0) < 1e-6, "gate*freq got %.17g", l);
  midi_note_off(69);
  tick_at(v3, 0.0, &l, &r);
  CHECK(l == 0.0, "after-off got %.17g", l);   /* gate is 0 */

  /* 4. Unheld note reads 0. */
  midi_reset_all();
  Voice* v4 = must_load("midi_note(42)", 48000.0);
  if (!v4) return;
  tick_at(v4, 0.0, &l, &r);
  CHECK(l == 0.0, "unheld note got %.17g", l);
}
