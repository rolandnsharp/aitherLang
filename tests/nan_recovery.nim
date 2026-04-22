## Defense 2: when audio output is NaN/Inf the engine resets the voice's
## pool and the next tick recovers cleanly. This test exercises the
## `resetPool` helper that the audio callback will call on detection.

import std/[math]
import ../parser, ../voice, ../dsp

# A patch with a stateful primitive so the lpf reads from the pool.
const Patch = """
sin(TAU * phasor(220)) |> lpf(800, 0.5)
"""

let v = newVoice(48000.0)
v.load(parseProgram(Patch), 48000.0)

# Warm up briefly so the pool holds non-trivial state.
for i in 1 .. 8:
  discard v.tick(float64(i) / 48000.0)

# Manually poison the pool — simulates an unstable filter / resonator
# that produced NaN at some point in its accumulator. (In production
# the source is the patch itself; here we inject directly because a
# clean patch wouldn't generate NaN deterministically.)
let pool = cast[ptr UncheckedArray[float64]](v.state)
for k in 0 ..< 16:
  pool[k] = NaN

let bad = v.tick(9.0 / 48000.0)
doAssert bad.l != bad.l, "expected NaN tick after pool poisoning, got " & $bad.l

# Defense 2 in the audio callback: zero the whole pool, then continue.
v.resetPool()
let good = v.tick(10.0 / 48000.0)
doAssert good.l == good.l and good.r == good.r,
  "expected non-NaN after pool reset, got (" & $good.l & ", " & $good.r & ")"

echo "nan recovery ok"
