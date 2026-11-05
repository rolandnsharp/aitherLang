/* glue.c: what aither_rt.c adds to the engine's own natives (dsp.c,
 * midi_state.c) for an exported patch: the public setters of aither_rt.h,
 * and the two hooks a patch's code calls that the engine gives it. */

#include "aither_rt.h"
#include "midi.h"

void aither_cc(int n, int value) { midi_cc(n, value); }

void aither_cc14(int n, int value) {
  if (n < 0 || n > 31) return;
  value = value < 0 ? 0 : value > 16383 ? 16383 : value;
  midi_cc(n, value >> 7);
  midi_cc(n + 32, value & 127);
}

void aither_note_on(int note, int velocity) {
  if (velocity > 0) midi_note_on(note, velocity);
  else midi_note_off(note);
}

void aither_note_off(int note) { midi_note_off(note); }
void aither_midi_reset(void) { midi_reset_all(); }

/* A recursion's guard: the engine stops one that runs past its block's
 * deadline. An exported patch has no deadline (its host owns the clock),
 * so a recursion runs to the end it was written with. */
long n_overrun(void* state, const char* where) {
  (void)state;
  (void)where;
  return 0;
}

/* `aither profile`'s clock: an exported patch isn't profiled. */
double n_now(void) { return 0.0; }
