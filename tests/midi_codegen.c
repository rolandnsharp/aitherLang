/* Patches using all 5 MIDI primitives compile and produce the expected
 * extern references. No runtime check here — just confirm that codegen
 * recognizes the names and emits the expected calls. */

#include "check.h"

static const char* Src =
  "play synth:\n"
  "  let env = midi_gate()\n"
  "  let cut = 200 + midi_cc(74) * 4000\n"
  "  env * cut * midi_freq() * 0.0001\n"
  "\n"
  "play kick:\n"
  "  midi_trig(36) * midi_note(36) * 0.5\n"
  "\n"
  "(synth + kick)\n";

TEST(midi_codegen) {
  char err[4096];
  Generated g;
  int rc = compile_full(Src, &g, err, sizeof err);
  CHECK(rc == 0, "doesn't compile: %s", err);
  if (rc != 0) return;
  CHECK(strstr(g.csrc, "n_midi_cc"), "n_midi_cc missing in emitted C");
  CHECK(strstr(g.csrc, "n_midi_note"), "n_midi_note missing in emitted C");
  CHECK(strstr(g.csrc, "n_midi_freq()"), "n_midi_freq() missing in emitted C");
  CHECK(strstr(g.csrc, "n_midi_gate()"), "n_midi_gate() missing in emitted C");
  CHECK(strstr(g.csrc, "n_midi_trig"), "n_midi_trig missing in emitted C");
  CHECK(strstr(g.csrc, "extern double n_midi_cc(int)"), "no extern double n_midi_cc(int)");
  CHECK(g.nparts == 2 && strcmp(g.part_names[0], "synth") == 0 &&
          strcmp(g.part_names[1], "kick") == 0,
        "parts aren't synth, kick (%d of them)", g.nparts);

  /* Arg-count errors surface through codegen, not as silent no-ops. */
  rc = compile_full("play x: midi_cc()\nx", &g, err, sizeof err);
  CHECK(rc == -2, "midi_cc with no args should fail codegen (got %d: %s)", rc, rc ? err : "");
}
