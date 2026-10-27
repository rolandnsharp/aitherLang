## The rising-edge idiom on MIDI: `g > 0.5 and prev(g) < 0.5` fires
## once per note-on, the second and third too. `and` used to compile to
## C's `&&`, which skipped prev(g) while g was low, so its memory froze
## and the second note-on was missed. (prev itself is in
## tests/lang/memory.aither.)

import ../voice, ../midi

proc loadWith(src: string): NativeVoice =
  result = newVoice(48000.0)
  result.load(src, 48000.0)

# --- 6. The bell_verify.aither idiom — let-bound midi_gate() then
# prev(g) for the strike. End-to-end check that the rising-edge fires
# exactly once on note-on, with NO other firings around it.
block midiBellPattern:
  const P = """
let g      = midi_gate()
let strike = if g > 0.5 and prev(g) < 0.5 then 1.0 else 0.0
strike
"""
  midiResetAll()
  let v = loadWith(P)
  var fires = 0
  for i in 0..<20:
    if i == 5: midiNoteOn(60, 100)
    if i == 12: midiNoteOff(60)
    let s = v.tick(float64(i) / 48000.0)
    if s.l > 0.5: inc fires
  doAssert fires == 1,
    "midi note-on should produce exactly one strike, got " & $fires

# --- 7. Inline form — prev(midi_gate()) without the let intermediate.
# Should also fire exactly once.
block midiBellInline:
  const P = """
let s = if midi_gate() > 0.5 and prev(midi_gate()) < 0.5 then 1.0 else 0.0
s
"""
  midiResetAll()
  let v = loadWith(P)
  var fires = 0
  for i in 0..<20:
    if i == 5: midiNoteOn(60, 100)
    if i == 12: midiNoteOff(60)
    let s = v.tick(float64(i) / 48000.0)
    if s.l > 0.5: inc fires
  doAssert fires == 1,
    "inline-prev midi note-on should produce one strike, got " & $fires

# --- 8. THE BUG that motivated this fix: two consecutive note-on events.
# With C `&&` short-circuit, prev's slot froze while g was low, so the
# second note-on saw a stale `prev(g) = 1.0` and didn't fire. With
# eager bitwise `&`, both note-ons fire.
block twoNoteOns:
  const P = """
let g      = midi_gate()
let strike = if g > 0.5 and prev(g) < 0.5 then 1.0 else 0.0
strike
"""
  midiResetAll()
  let v = loadWith(P)
  var fires = 0
  for i in 0..<30:
    if i == 5:  midiNoteOn(60, 100)
    if i == 10: midiNoteOff(60)
    if i == 15: midiNoteOn(60, 100)
    if i == 20: midiNoteOff(60)
    let s = v.tick(float64(i) / 48000.0)
    if s.l > 0.5: inc fires
  doAssert fires == 2,
    "two consecutive note-ons should produce two strikes, got " & $fires

echo "prev_edge ok"
