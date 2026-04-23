## midi_trig(n) fires exactly one sample per note-on. Rapid re-triggers
## fire again; sustained notes do not re-fire.

import ../parser, ../voice, ../midi

const Stdlib = staticRead("../stdlib.aither")

proc load(src: string): NativeVoice =
  let stdAst = parseProgram(Stdlib)
  let userAst = parseProgram(src)
  let program = Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)
  result = newVoice(48000.0)
  result.load(program, 48000.0)

midiResetAll()
let v = load("midi_trig(36)")

# Pre-trig: 0.
var (l, r) = v.tick(0.0)
doAssert l == 0.0, "pre-trig got " & $l

# First note-on: 1.0 on the next tick.
midiNoteOn(36, 100)
(l, r) = v.tick(0.0)
doAssert l == 1.0, "first trig got " & $l

# Next sample without new note-on: back to 0.
(l, r) = v.tick(0.0)
doAssert l == 0.0, "post-trig got " & $l

# Note-off does not fire.
midiNoteOff(36)
(l, r) = v.tick(0.0)
doAssert l == 0.0

# Re-trigger fires again.
midiNoteOn(36, 100)
(l, r) = v.tick(0.0)
doAssert l == 1.0, "re-trig got " & $l

# Rapid double-trigger between samples still registers as exactly one
# edge (the counter advanced; we only look at "did it advance since last
# sample"). This is intentional v1 behaviour — coalescing fast re-triggers
# is acceptable at 48 kHz sample resolution.
midiNoteOn(36, 100)
midiNoteOn(36, 100)
(l, r) = v.tick(0.0)
doAssert l == 1.0

echo "midi_trig_consume ok"
