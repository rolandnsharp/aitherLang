/* CC state lives in the engine, not the voice, so a hot reload of a
 * patch that uses `midi_cc(n)` must see the knob at its current position
 * on the first tick after reload — no "zeroed knobs on edit" surprise. */

#include "check.h"

TEST(midi_hotreload) {
  midi_reset_all();
  midi_cc(74, 90);

  Voice* v = must_load("midi_cc(74)", 48000.0);
  if (!v) return;
  double l, r, expected = 90.0 / 127.0;
  tick_at(v, 0.0, &l, &r);
  CHECK(fabs(l - expected) < 1e-9, "pre-reload got %.17g", l);

  /* Hot reload: same voice, new patch source, same CC primitive. */
  reload(v, "midi_cc(74) + 0.1", 48000.0);
  tick_at(v, 0.0, &l, &r);
  CHECK(fabs(l - (expected + 0.1)) < 1e-9, "post-reload got %.17g", l);

  /* And reloading to a patch that doesn't use CC at all, then back to one
   * that does, still reflects the current CC value. */
  reload(v, "0.0", 48000.0);
  reload(v, "midi_cc(74)", 48000.0);
  tick_at(v, 0.0, &l, &r);
  CHECK(fabs(l - expected) < 1e-9, "after-idle-reload got %.17g", l);
}
