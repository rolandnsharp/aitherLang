## Patches using all 5 MIDI primitives compile and produce the expected
## extern references. No runtime check here — just confirm that codegen
## recognizes the names and emits the expected calls.

import std/strutils
import ../parser, ../codegen

const Src = """
play synth:
  let env = midi_gate()
  let cut = 200 + midi_cc(74) * 4000
  env * cut * midi_freq() * 0.0001

play kick:
  midi_trig(36) * midi_note(36) * 0.5

(synth + kick)
"""

let ast = parseProgram(Src)
let (csrc, _, parts, _) = generate(ast)

doAssert "n_midi_cc" in csrc, "n_midi_cc missing in emitted C"
doAssert "n_midi_note" in csrc
doAssert "n_midi_freq()" in csrc
doAssert "n_midi_gate()" in csrc
doAssert "n_midi_trig" in csrc
doAssert "extern double n_midi_cc(int)" in csrc
doAssert parts == @["synth", "kick"]

# Arg-count errors surface through codegen, not as silent no-ops.
var bad = false
try:
  discard generate(parseProgram("play x: midi_cc()\nx"))
except ValueError:
  bad = true
doAssert bad, "midi_cc with no args should fail codegen"

echo "midi_codegen ok"
