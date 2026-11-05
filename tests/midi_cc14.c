/* Controllers 0..31 pair with 32..63 into 14 bits (SPEC.md, "MIDI
 * input"): the coarse half alone reads as it always did, m / 127, so a
 * 7-bit knob still reaches 1; with the fine half it reads
 * (coarse * 128 + fine) / 16383; the fine half reads on its own too. */

#include "check.h"

TEST(midi_cc14) {
  midi_reset_all();
  midi_cc(20, 127);
  CHECK(n_midi_cc(20) == 1.0, "a 7-bit knob at the top reads 1: %.17g", n_midi_cc(20));
  midi_cc(20, 64);
  CHECK(fabs(n_midi_cc(20) - 64.0 / 127.0) < 1e-15, "coarse alone is m / 127: %.17g", n_midi_cc(20));
  midi_cc(52, 0);
  CHECK(fabs(n_midi_cc(20) - 8192.0 / 16383.0) < 1e-15, "64 then fine 0: %.17g", n_midi_cc(20));
  midi_cc(52, 1);
  CHECK(fabs(n_midi_cc(20) - 8193.0 / 16383.0) < 1e-15, "one step of 16383: %.17g", n_midi_cc(20));
  CHECK(fabs(n_midi_cc(52) - 1.0 / 127.0) < 1e-15, "the fine half reads on its own: %.17g", n_midi_cc(52));
  midi_cc(20, 10);
  CHECK(fabs(n_midi_cc(20) - 10.0 / 127.0) < 1e-15, "a new coarse value starts the pair again: %.17g", n_midi_cc(20));
  midi_cc(20, 127);
  midi_cc(52, 127);
  CHECK(n_midi_cc(20) == 1.0, "both halves full is 1: %.17g", n_midi_cc(20));
  midi_cc(64, 127);
  CHECK(n_midi_cc(64) == 1.0 && n_midi_cc(32) == 0.0, "controllers past 63 are 7-bit and pair with nothing");

  /* A patch reads the pair through midi_cc, whatever writes it. */
  midi_cc(21, 100);
  midi_cc(53, 37);
  Voice* v = must_load("midi_cc(21)", 48000.0);
  if (!v) return;
  double l, r;
  tick_at(v, 0.0, &l, &r);
  CHECK(fabs(l - (100 * 128 + 37) / 16383.0) < 1e-12, "the patch hears the 14 bits: %.17g", l);
  midi_reset_all();
}
