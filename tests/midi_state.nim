## Populate midiState from the test thread via the writer API, compile +
## tick a patch that reads from it, and verify the output reflects the
## set values.

import std/math
import ../parser, ../voice, ../midi

const Stdlib = staticRead("../stdlib.aither")

proc load(src: string): NativeVoice =
  let stdAst = parseProgram(Stdlib)
  let userAst = parseProgram(src)
  let program = Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)
  result = newVoice(48000.0)
  result.load(program, 48000.0)

midiResetAll()

# 1. midi_cc — CC 74 at value 64 maps to 64/127.
let v1 = load("midi_cc(74)")
midiCc(74, 64)
var (l, r) = v1.tick(0.0)
let expected1 = 64.0 / 127.0
doAssert abs(l - expected1) < 1e-9, "cc got " & $l
doAssert abs(r - expected1) < 1e-9

midiCc(74, 32)
(l, r) = v1.tick(0.0)
let expected2 = 32.0 / 127.0
doAssert abs(l - expected2) < 1e-9, "cc follow got " & $l

# 2. midi_note(n) while held, 0 after release.
let v2 = load("midi_note(60)")
midiNoteOn(60, 127)
(l, r) = v2.tick(0.0)
doAssert abs(l - 1.0) < 1e-9
midiNoteOff(60)
(l, r) = v2.tick(0.0)
doAssert l == 0.0

# 3. midi_freq() & midi_gate() pair
let v3 = load("midi_gate() * midi_freq()")
midiNoteOn(69, 127)       # A4 = 440
(l, r) = v3.tick(0.0)
doAssert abs(l - 440.0) < 1e-6, "gate*freq got " & $l
midiNoteOff(69)
(l, r) = v3.tick(0.0)
doAssert l == 0.0, "after-off got " & $l    # gate is 0

# 4. Unheld note reads 0.
midiResetAll()
let v4 = load("midi_note(42)")
(l, r) = v4.tick(0.0)
doAssert l == 0.0

echo "midi_state ok"
