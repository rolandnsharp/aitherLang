/* midi_trig(n) fires exactly one sample per note-on. Rapid re-triggers
 * fire again; sustained notes do not re-fire. */

#include "check.h"

TEST(midi_trig_consume) {
  double l, r;
  midi_reset_all();
  Voice* v = must_load("midi_trig(36)", 48000.0);
  if (!v) return;

  /* Pre-trig: 0. */
  tick_at(v, 0.0, &l, &r);
  CHECK(l == 0.0, "pre-trig got %.17g", l);

  /* First note-on: 1.0 on the next tick. */
  midi_note_on(36, 100);
  tick_at(v, 0.0, &l, &r);
  CHECK(l == 1.0, "first trig got %.17g", l);

  /* Next sample without new note-on: back to 0. */
  tick_at(v, 0.0, &l, &r);
  CHECK(l == 0.0, "post-trig got %.17g", l);

  /* Note-off does not fire. */
  midi_note_off(36);
  tick_at(v, 0.0, &l, &r);
  CHECK(l == 0.0, "note-off fired: %.17g", l);

  /* Re-trigger fires again. */
  midi_note_on(36, 100);
  tick_at(v, 0.0, &l, &r);
  CHECK(l == 1.0, "re-trig got %.17g", l);

  /* Rapid double-trigger between samples still registers as exactly one
   * edge (the counter advanced; we only look at "did it advance since last
   * sample"). This is intentional v1 behaviour — coalescing fast re-triggers
   * is acceptable at 48 kHz sample resolution. */
  midi_note_on(36, 100);
  midi_note_on(36, 100);
  tick_at(v, 0.0, &l, &r);
  CHECK(l == 1.0, "double trig got %.17g", l);
}
