/* Phase 2: polyphonic held-notes table.
 *
 * The engine tracks up to MIDI_MAX_POLYPHONY (16) simultaneously-held
 * notes. midi_voice_freq/gate/vel/key(n) expose the nth slot, 1-based,
 * so the slot index pairs cleanly with sum's iteration counter.
 *
 * Allocator policy (re-trigger same note → fill empty → reuse released
 * → steal oldest) is documented in midi.c's allocate_slot. The tests
 * below pin the user-facing properties: voice indices stay stable for
 * still-held notes, mono `midi_freq()` keeps tracking the most recent
 * note-on, and going past 16 notes evicts the oldest. */

#include "check.h"

static double hz_of(int note) { return 440.0 * pow(2.0, (note - 69.0) / 12.0); }

/* A voice's first sample, both sides; a voice that didn't compile has
 * already failed the test and reads NaN. */
static void tick0(Voice* v, double* l, double* r) {
  if (!v) { *l = *r = NAN; return; }
  tick_at(v, 0.0, l, r);
}

TEST(midi_polyphony) {
  double l, r;
  const double sr = 48000.0;

  /* 1. Single-note allocation: voice 1 reads the held pitch and velocity. */
  midi_reset_all();
  Voice* v1 = must_load("[midi_voice_freq(1), midi_voice_gate(1)]", sr);
  tick0(v1, &l, &r);
  CHECK(l == 0.0 && r == 0.0, "fresh engine: voice 1 should read 0/0, got %g/%g", l, r);

  midi_note_on(60, 100);           /* C4 ≈ 261.63 Hz */
  tick0(v1, &l, &r);
  CHECK(fabs(l - hz_of(60)) < 1e-3, "voice 1 freq for C4: %.17g", l);
  CHECK(r == 1.0, "voice 1 gate for C4 is 1 while held, whatever the velocity: %g", r);
  Voice* v1v = must_load("[midi_voice_vel(1), midi_voice_key(1)]", sr);
  tick0(v1v, &l, &r);
  CHECK(fabs(l - 100.0 / 127.0) < 1e-9, "voice 1 vel for C4: %.17g", l);
  CHECK(r == 60.0, "voice 1 key for C4: %g", r);

  midi_note_off(60);
  tick0(v1, &l, &r);
  CHECK(fabs(l - hz_of(60)) < 1e-3,
        "voice 1 freq stays after release (synth release tail still reads it): %.17g", l);
  CHECK(r == 0.0, "voice 1 gate goes to 0 on release: %g", r);
  tick0(v1v, &l, &r);
  CHECK(fabs(l - 100.0 / 127.0) < 1e-9 && r == 60.0,
        "vel and key stay after release, so a release tail scaled by vel rings on: %g %g", l, r);

  /* 2. Three-note chord — each held note maps to its own slot in held-order. */
  midi_reset_all();
  Voice* v2 = must_load("[midi_voice_freq(1) + midi_voice_freq(2) * 0 + midi_voice_freq(3) * 0,\n"
                        " midi_voice_freq(2)]\n", sr);
  midi_note_on(60, 100);           /* C */
  midi_note_on(64, 100);           /* E */
  midi_note_on(67, 100);           /* G */
  tick0(v2, &l, &r);
  CHECK(fabs(l - hz_of(60)) < 1e-3, "voice 1 = C: %.17g", l);
  CHECK(fabs(r - hz_of(64)) < 1e-3, "voice 2 = E: %.17g", r);

  /* Voice 3 separately: read just slot 3. */
  Voice* v2b = must_load("midi_voice_freq(3)", sr);
  tick0(v2b, &l, &r);
  CHECK(fabs(l - hz_of(67)) < 1e-3, "voice 3 = G: %.17g", l);

  /* 3. Releasing the middle note: voices 1 and 3 unchanged. Voice 2 keeps
   * its pitch but reports gate=0 — release semantics, not slot collapse. */
  midi_note_off(64);
  Voice* v3a = must_load("midi_voice_freq(1)", sr);
  Voice* v3b = must_load("midi_voice_freq(2)", sr);
  Voice* v3c = must_load("midi_voice_freq(3)", sr);
  Voice* v3d = must_load("midi_voice_gate(2)", sr);
  tick0(v3a, &l, &r); CHECK(fabs(l - hz_of(60)) < 1e-3, "v1 stays: %.17g", l);
  tick0(v3b, &l, &r); CHECK(fabs(l - hz_of(64)) < 1e-3, "v2 keeps freq during release: %.17g", l);
  tick0(v3c, &l, &r); CHECK(fabs(l - hz_of(67)) < 1e-3, "v3 stays: %.17g", l);
  tick0(v3d, &l, &r); CHECK(l == 0.0, "v2 gate=0 after release: %g", l);

  /* 4. New note after a release reuses the freed slot. */
  midi_note_on(72, 100);           /* C5 — should land in slot 2 (released first) */
  tick0(v3b, &l, &r);
  CHECK(fabs(l - hz_of(72)) < 1e-3, "C5 takes the freed slot 2: %.17g", l);

  /* 5. midi_freq() / midi_gate() / midi_vel() track the most recent note-on. */
  Voice* v5 = must_load("[midi_freq(), midi_gate()]", sr);
  Voice* v5v = must_load("midi_vel()", sr);
  midi_reset_all();
  midi_note_on(60, 100);
  midi_note_on(64, 90);
  midi_note_on(67, 80);
  tick0(v5, &l, &r);
  CHECK(fabs(l - hz_of(67)) < 1e-3, "midi_freq() = most recent: %.17g", l);
  CHECK(r == 1.0, "midi_gate() is 1 while the most recent note is held: %g", r);
  tick0(v5v, &l, &r);
  CHECK(fabs(l - 80.0 / 127.0) < 1e-9, "midi_vel() = most recent velocity: %.17g", l);
  midi_note_off(67);
  tick0(v5, &l, &r);
  CHECK(r == 0.0, "midi_gate goes to 0 when most-recent releases: %g", r);
  tick0(v5v, &l, &r);
  CHECK(fabs(l - 80.0 / 127.0) < 1e-9, "midi_vel() stays after the note-off: %.17g", l);

  /* 6. Voice stealing: 17 distinct held notes evict the oldest. */
  midi_reset_all();
  for (int i = 0; i < MIDI_MAX_POLYPHONY; i++)
    midi_note_on(60 + i, 100);     /* held in held-order */
  /* All 16 slots full. v1 starts as the oldest = note 60. */
  Voice* v6 = must_load("midi_voice_freq(1)", sr);
  tick0(v6, &l, &r);
  CHECK(fabs(l - hz_of(60)) < 1e-3, "before eviction, slot 1 = oldest = C4: %.17g", l);
  midi_note_on(80, 100);           /* 17th note → evict the slot held longest */
  /* The slot that previously held note 60 now holds note 80. */
  tick0(v6, &l, &r);
  CHECK(fabs(l - hz_of(80)) < 1e-3, "after 17th note, oldest slot replaced: %.17g", l);

  /* 7. Re-pressing a held note re-triggers in place (same slot, refreshed velocity). */
  midi_reset_all();
  midi_note_on(60, 100);
  midi_note_on(64, 100);
  Voice* v7f = must_load("midi_voice_freq(1)", sr);
  Voice* v7g = must_load("midi_voice_vel(1)", sr);
  tick0(v7f, &l, &r); CHECK(fabs(l - hz_of(60)) < 1e-3, "slot 1 = C4: %.17g", l);
  tick0(v7g, &l, &r); CHECK(fabs(l - 100.0 / 127.0) < 1e-9, "slot 1 vel: %.17g", l);
  midi_note_on(60, 60);            /* re-press at lower velocity → in-place update */
  tick0(v7f, &l, &r); CHECK(fabs(l - hz_of(60)) < 1e-3, "still slot 1 after re-press: %.17g", l);
  tick0(v7g, &l, &r); CHECK(fabs(l - 60.0 / 127.0) < 1e-9, "velocity refreshed in slot 1: %.17g", l);
  /* 7b. Soft notes are held notes: the gate is 1 at velocity 1 too. */
  midi_reset_all();
  midi_note_on(60, 1);
  Voice* v7s = must_load("midi_voice_gate(1)", sr);
  tick0(v7s, &l, &r); CHECK(l == 1.0, "velocity 1 still gates: %g", l);

  /* 8. Out-of-range voice index returns 0 cleanly (no crash). */
  Voice* v8 = must_load("[midi_voice_freq(0), midi_voice_freq(99)]", sr);
  tick0(v8, &l, &r);
  CHECK(l == 0.0 && r == 0.0, "voice index out of [1..MIDI_MAX_POLYPHONY] returns 0: %g/%g", l, r);
}
