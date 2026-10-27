## A two-element array is a table when a table is wanted and a stereo
## pair when a pair is wanted; the receiving operation decides, not the
## length. Before this, `wave(1, [0.25, 0.75])` failed with "stateful
## call 'wave' receives a stereo value", so a two-step sequence was
## impossible to write — while three steps worked.
##
## Also covers two smaller bugs found alongside:
##   - a 3-entry table in arithmetic (`t3 * 0.1`) reached the C compiler
##     as an undeclared symbol instead of a compile error

import std/[strutils]
import ../ccompile, ../voice

proc peakOf(src: string): float64 =
  let prog = (src)
  let v = newVoice(48000.0)
  v.load(prog, 48000.0)
  for i in 0 ..< 4800:
    let s = v.tick(float64(i) / 48000.0)
    doAssert s.l == s.l, "NaN at sample " & $i
    result = max(result, abs(s.l))

proc errorOf(src: string): string =
  try:
    discard compile((src), "patches/x.aither")
    doAssert false, "expected a compile error"
  except CatchableError as e:
    return e.msg

# 1. Two-step wave, literal in place.
doAssert peakOf("""
play a:
  sin(TAU * phasor(220 * pow(2, wave(1, [0.25, 0.75])))) * 0.2
a
""") > 0.0, "two-step wave literal"

# 2. Two-step wave via a top-level let.
doAssert peakOf("""
let steps = [0.25, 0.75]
play a:
  sin(TAU * phasor(220 * pow(2, wave(1, steps)))) * 0.2
a
""") > 0.0, "two-step wave, top-level let"

# 3. Two-step wave via a play-local let.
doAssert peakOf("""
play a:
  let steps = [0.25, 0.75]
  sin(TAU * phasor(220 * pow(2, wave(1, steps)))) * 0.2
a
""") > 0.0, "two-step wave, play-local let"

# 4. The same two-element let is still a pair in arithmetic.
doAssert peakOf("""
let gains = [0.2, 0.1]
play a:
  sin(TAU * phasor(220)) * gains
a
""") > 0.0, "two-element let as a pair"

# 5. A pair given to a stateful call runs it once a side (it used to be
# an error naming a hard-coded `bass`).
doAssert peakOf("""
play a:
  let wide = [220, 221]
  sin(TAU * phasor(wide)) * 0.2
a
""") > 0.0, "phasor of a pair"

# 6. A 3-entry table in arithmetic is a compile error, not a C error.
let msg3 = errorOf("""
let t3 = [1.0, 2.0, 3.0]
play a:
  sin(TAU * phasor(220)) * (t3 * 0.1)
a
""")
doAssert "table" in msg3 and "undeclared" notin msg3,
  "expected a table error, got: " & msg3

echo "two_entry_tables ok"
