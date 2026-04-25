## Phase 3: stdlib `poly` + `midi_keyboard`. The user-facing API lives
## in stdlib.aither as a thin wrapper over `sum` + the new voice
## primitives. These tests verify the wrappers compile, expand to the
## right number of per-voice state regions, and tick NaN-free with no
## notes held.

import std/math
import ../parser, ../voice, ../codegen, ../midi

const Stdlib = staticRead("../stdlib.aither")

proc program(src: string): Node =
  let stdAst = parseProgram(Stdlib)
  let userAst = parseProgram(src)
  Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

# 1. midi_keyboard with a phasor voice — 8 voices means 8 phasor regions.
block fanout:
  let p = program("""
play kbd:
  midi_keyboard((f, g) => sin(TAU * phasor(f)) * g)
kbd
""")
  let (_, _, parts, regions) = generate(p, "", 48000.0)
  var phasorCount = 0
  for r in regions:
    if r.typeName == "phasor": inc phasorCount
  doAssert phasorCount == 8,
    "midi_keyboard fanout to 8 phasor regions, got " & $phasorCount
  doAssert parts == @["kbd"]

# 2. poly(4, ...) lets the user override the default. 4 phasor regions.
block polyOverride:
  let p = program("""
play kbd:
  poly(4, (f, g) => sin(TAU * phasor(f)) * g)
kbd
""")
  let (_, _, _, regions) = generate(p, "", 48000.0)
  var phasorCount = 0
  for r in regions:
    if r.typeName == "phasor": inc phasorCount
  doAssert phasorCount == 4,
    "poly(4, ...) should produce 4 phasor regions, got " & $phasorCount

# 3. Silence with 0 notes held. The `if f > 0` guard short-circuits empty
# slots so an idle keyboard contributes nothing, NaN-free.
block idleSilence:
  midiResetAll()
  let v = newVoice(48000.0)
  v.load(program("""
play kbd:
  midi_keyboard((f, g) => sin(TAU * phasor(f)) * g)
kbd
"""), 48000.0)
  for i in 0..<2000:
    let s = v.tick(float64(i) / 48000.0)
    doAssert s.l == s.l and s.r == s.r, "no NaN at sample " & $i
    doAssert s.l == 0.0 and s.r == 0.0,
      "idle keyboard should be exact zero, got " & $s.l & "/" & $s.r & " at sample " & $i

# 4. With one note held, output is non-zero and NaN-free.
block singleHeld:
  midiResetAll()
  midiNoteOn(60, 100)
  let v = newVoice(48000.0)
  v.load(program("""
play kbd:
  midi_keyboard((f, g) => sin(TAU * phasor(f)) * g * 0.1)
kbd
"""), 48000.0)
  var maxAbs = 0.0
  for i in 0..<2000:
    let s = v.tick(float64(i) / 48000.0)
    doAssert s.l == s.l, "no NaN at sample " & $i
    if abs(s.l) > maxAbs: maxAbs = abs(s.l)
  doAssert maxAbs > 0.01,
    "one held note should produce audible output, maxAbs=" & $maxAbs
  midiResetAll()

# 5. Three held notes → three voices contribute. Sum is bounded by the
# number of held notes, not by 8 (idle slots stay at 0).
block threeHeld:
  midiResetAll()
  midiNoteOn(60, 100)
  midiNoteOn(64, 100)
  midiNoteOn(67, 100)
  let v = newVoice(48000.0)
  v.load(program("""
play kbd:
  midi_keyboard((f, g) => sin(TAU * phasor(f)) * g * 0.1)
kbd
"""), 48000.0)
  var maxAbs = 0.0
  for i in 0..<4800:                  # 100 ms — enough to sample all phases
    let s = v.tick(float64(i) / 48000.0)
    doAssert s.l == s.l, "no NaN at sample " & $i
    if abs(s.l) > maxAbs: maxAbs = abs(s.l)
  doAssert maxAbs > 0.05,
    "three held notes should be louder than one, maxAbs=" & $maxAbs
  midiResetAll()

echo "midi_keyboard_compiles ok"
