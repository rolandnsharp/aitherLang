/* The rising-edge idiom on MIDI: `g > 0.5 and prev(g) < 0.5` fires
 * once per note-on, the second and third too. `and` used to compile to
 * C's `&&`, which skipped prev(g) while g was low, so its memory froze
 * and the second note-on was missed. (prev itself is in
 * tests/lang/memory.aither.) */

#include "check.h"

/* Strikes over `n` samples, with note 60 on at each `ons` index and off
 * at each `offs` index (-1 ends a list). */
static int strikes(const char* src, int n, const int* ons, const int* offs) {
  midi_reset_all();
  Voice* v = must_load(src, 48000.0);
  if (!v) return -1;
  int fires = 0;
  for (int i = 0; i < n; i++) {
    for (const int* o = ons; *o >= 0; o++) if (*o == i) midi_note_on(60, 100);
    for (const int* o = offs; *o >= 0; o++) if (*o == i) midi_note_off(60);
    double l, r;
    tick_at(v, (double)i / 48000.0, &l, &r);
    if (l > 0.5) fires++;
  }
  return fires;
}

static const char* bell =
  "let g      = midi_gate()\n"
  "let strike = if g > 0.5 and prev(g) < 0.5 then 1.0 else 0.0\n"
  "strike\n";

/* 6. The bell idiom — let-bound midi_gate() then prev(g) for the strike.
 * End-to-end check that the rising edge fires exactly once on note-on,
 * with NO other firings around it. */
TEST(prev_edge_midi_bell_pattern) {
  int fires = strikes(bell, 20, (int[]){5, -1}, (int[]){12, -1});
  CHECK(fires == 1, "midi note-on should produce exactly one strike, got %d", fires);
}

/* 7. Inline form — prev(midi_gate()) without the let intermediate.
 * Should also fire exactly once. */
TEST(prev_edge_midi_bell_inline) {
  int fires = strikes("let s = if midi_gate() > 0.5 and prev(midi_gate()) < 0.5 then 1.0 else 0.0\n"
                      "s\n",
                      20, (int[]){5, -1}, (int[]){12, -1});
  CHECK(fires == 1, "inline-prev midi note-on should produce one strike, got %d", fires);
}

/* 8. THE BUG that motivated this fix: two consecutive note-on events.
 * With C `&&` short-circuit, prev's slot froze while g was low, so the
 * second note-on saw a stale `prev(g) = 1.0` and didn't fire. With
 * eager bitwise `&`, both note-ons fire. */
TEST(prev_edge_two_note_ons) {
  int fires = strikes(bell, 30, (int[]){5, 15, -1}, (int[]){10, 20, -1});
  CHECK(fires == 2, "two consecutive note-ons should produce two strikes, got %d", fires);
}
