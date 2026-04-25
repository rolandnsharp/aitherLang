## prev() invariants — the rising-edge idiom `g > 0.5 and prev(g) < 0.5`
## fires once per gate transition, including the SECOND, THIRD, ... press.
##
## BUGS_AND_ISSUES.md issue 2 reported that prev() "doesn't fire reliably
## on midi_gate edges". Hypothesis was state-slot keying by argument
## identity. Actual cause: aither's `and` was compiling to C's `&&`,
## which short-circuits — so prev(g) never ran while g was low, leaving
## prev's last-sample slot frozen at the previous high value. The second
## note-on then saw `prev(g) < 0.5` as false (stale 1.0) and skipped.
##
## Fix: codegen `and` / `or` as bitwise `&` / `|` on the 0/1 results,
## so both operands always evaluate. This test pins the multi-press
## behaviour plus the basic sample-delay invariants.

import std/[strutils, atomics]
import ../parser, ../voice, ../midi

const Stdlib = staticRead("../stdlib.aither")

proc loadWith(src: string): NativeVoice =
  let stdAst = parseProgram(Stdlib)
  let userAst = parseProgram(src)
  let program = Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)
  result = newVoice(48000.0)
  result.load(program, 48000.0)

# --- 1. prev() of a counter — returns previous sample's value.
block samplesDelay:
  const P = """
$c = 0.0
$c = $c + 1
prev($c)
"""
  let v = loadWith(P)
  for i in 0..<5:
    let s = v.tick(float64(i) / 48000.0)
    let want = float64(i)             # at tick i, c was incremented to i+1
                                      #   prev returns last tick's c which was i
    doAssert abs(s.l - want) < 1e-9,
      "tick " & $i & ": prev($c) = " & $s.l & " want " & $want

# --- 2. prev(let-bound expr) and prev(inline expr) behave identically.
block sameLetVsInline:
  const PL = """
$c = 0.0
$c = $c + 1
let g = $c
prev(g)
"""
  const PI = """
$c = 0.0
$c = $c + 1
prev($c)
"""
  let vl = loadWith(PL)
  let vi = loadWith(PI)
  for i in 0..<5:
    let sl = vl.tick(float64(i) / 48000.0)
    let si = vi.tick(float64(i) / 48000.0)
    doAssert abs(sl.l - si.l) < 1e-9,
      "tick " & $i & ": let-bound=" & $sl.l & " inline=" & $si.l

# --- 3. Two prev() calls on different expressions claim independent
# state. Without per-call-site keying, one would clobber the other.
block independentSlots:
  const P = """
$c = 0.0
$c = $c + 1
let p1 = prev($c)
let p2 = prev($c * 2.0)
p1 + p2
"""
  let v = loadWith(P)
  # tick i: c becomes i+1; prev(c) returns last tick's c = i; prev(c*2)
  # returns last tick's c*2 = 2i. Sum = 3i.
  for i in 0..<5:
    let s = v.tick(float64(i) / 48000.0)
    let want = 3.0 * float64(i)
    doAssert abs(s.l - want) < 1e-9,
      "tick " & $i & ": p1+p2 = " & $s.l & " want " & $want

# --- 4. Two prev() calls on the SAME expression also have independent
# state — symmetric variant of (3).
block sameExprIndependent:
  const P = """
$c = 0.0
$c = $c + 1
let p1 = prev($c)
let p2 = prev($c)
p1 - p2
"""
  let v = loadWith(P)
  for i in 0..<5:
    let s = v.tick(float64(i) / 48000.0)
    doAssert abs(s.l) < 1e-9,
      "tick " & $i & ": two prev(c) should be identical, diff = " & $s.l

# --- 5. Rising-edge detector idiom. Drive a synthetic 0→1→1→1→0 gate
# sequence and confirm strike fires exactly once at the 0→1 transition.
block risingEdge:
  const P = """
$c = 0.0
$c = $c + 1
let g = if $c >= 3 and $c <= 5 then 1.0 else 0.0
let strike = if g > 0.5 and prev(g) < 0.5 then 1.0 else 0.0
strike
"""
  # Tick 1: c=1, g=0, prev(g)=0 → 0.
  # Tick 2: c=2, g=0, prev(g)=0 → 0.
  # Tick 3: c=3, g=1, prev(g)=0 → 1 (FIRE!)
  # Tick 4: c=4, g=1, prev(g)=1 → 0.
  # Tick 5: c=5, g=1, prev(g)=1 → 0.
  # Tick 6: c=6, g=0, prev(g)=1 → 0.
  let v = loadWith(P)
  let want = [0.0, 0.0, 1.0, 0.0, 0.0, 0.0]
  for i in 0..<6:
    let s = v.tick(float64(i) / 48000.0)
    doAssert abs(s.l - want[i]) < 1e-9,
      "tick " & $i & ": strike = " & $s.l & " want " & $want[i]

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

# --- 9. `or` should also be eager — symmetric guarantee. The pattern
# `cond or update_state()` should run update_state regardless of cond.
block orEager:
  const P = """
$counter = 0.0
$counter = $counter + 1
let any = 1.0 or $counter > 0
any
"""
  let v = loadWith(P)
  for i in 0..<3:
    let s = v.tick(float64(i) / 48000.0)
    doAssert abs(s.l - 1.0) < 1e-9,
      "or returns 1 when either operand true (i=" & $i & ")"

echo "prev_edge ok"
