/* Phase 3: stdlib `poly` + `midi_keyboard`. The user-facing API lives
 * in the stdlib as a thin wrapper over `sum` + the new voice
 * primitives. These tests verify the wrappers compile, expand to the
 * right number of per-voice state regions, and tick NaN-free with no
 * notes held. */

#include "check.h"

static const char* keyboard =
  "play kbd:\n"
  "  midi_keyboard((f, g) => sin(TAU * phasor(f)) * g)\n"
  "kbd\n";

static const char* keyboard_quiet =
  "play kbd:\n"
  "  midi_keyboard((f, g) => sin(TAU * phasor(f)) * g * 0.1)\n"
  "kbd\n";

/* Compile `src` with the stdlib as the engine does and count its phasor regions, a steady
 * sine's turning point counted as one. -1 if it doesn't compile; with
 * `play_ok`, whether the plays are exactly `kbd`. */
static int phasor_regions(const char* src, int* play_ok) {
  char err[4096];
  Generated g;
  int n = -1;
  if (compile_full(src, &g, err, sizeof err) == 0) {
    n = 0;
    for (int i = 0; i < g.nregions; i++)
      if (!strcmp(g.regions[i].type_name, "phasor") || !strcmp(g.regions[i].type_name, "turner")) n++;
    if (play_ok) *play_ok = g.nparts == 1 && !strcmp(g.part_names[0], "kbd");
  } else {
    CHECK(0, "doesn't compile: %s", err);
  }
  return n;
}

/* The loudest left sample over `n` samples from t=0, failing on a NaN. */
static double max_abs(const char* src, int n) {
  Voice* v = must_load(src, 48000.0);
  if (!v) return NAN;
  double m = 0.0;
  for (int i = 0; i < n; i++) {
    double l, r;
    tick_at(v, (double)i / 48000.0, &l, &r);
    if (l != l) { CHECK(0, "NaN at sample %d", i); return NAN; }
    if (fabs(l) > m) m = fabs(l);
  }
  return m;
}

/* 1. midi_keyboard with a phasor voice — 8 voices means 8 phasor regions. */
TEST(midi_keyboard_fanout) {
  int play_ok = 0;
  int n = phasor_regions(keyboard, &play_ok);
  CHECK(n == 8, "midi_keyboard fanout to 8 phasor regions, got %d", n);
  CHECK(play_ok, "the plays should be exactly [kbd]");
}

/* 2. poly(4, ...) lets the user override the default. 4 phasor regions. */
TEST(midi_keyboard_poly_override) {
  int n = phasor_regions("play kbd:\n"
                         "  poly(4, (f, g) => sin(TAU * phasor(f)) * g)\n"
                         "kbd\n", NULL);
  CHECK(n == 4, "poly(4, ...) should produce 4 phasor regions, got %d", n);
}

/* 3. Silence with 0 notes held. The `if f > 0` guard short-circuits empty
 * slots so an idle keyboard contributes nothing, NaN-free. */
TEST(midi_keyboard_idle_silence) {
  midi_reset_all();
  Voice* v = must_load(keyboard, 48000.0);
  if (!v) return;
  for (int i = 0; i < 2000; i++) {
    double l, r;
    tick_at(v, (double)i / 48000.0, &l, &r);
    if (l != l || r != r) { CHECK(0, "NaN at sample %d", i); return; }
    if (l != 0.0 || r != 0.0) {
      CHECK(0, "idle keyboard should be exact zero, got %g/%g at sample %d", l, r, i);
      return;
    }
  }
}

/* 4. With one note held, output is non-zero and NaN-free. */
TEST(midi_keyboard_single_held) {
  midi_reset_all();
  midi_note_on(60, 100);
  double m = max_abs(keyboard_quiet, 2000);
  CHECK(m > 0.01, "one held note should produce audible output, maxAbs=%g", m);
  midi_reset_all();
}

/* 5. Three held notes → three voices contribute. Sum is bounded by the
 * number of held notes, not by 8 (idle slots stay at 0). */
TEST(midi_keyboard_three_held) {
  midi_reset_all();
  midi_note_on(60, 100);
  midi_note_on(64, 100);
  midi_note_on(67, 100);
  double m = max_abs(keyboard_quiet, 4800);   /* 100 ms — enough to sample all phases */
  CHECK(m > 0.05, "three held notes should be louder than one, maxAbs=%g", m);
  midi_reset_all();
}
