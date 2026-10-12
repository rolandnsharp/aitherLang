## The GUIDE's "Spatial physics with arrays" string: stable, pitched,
## and it rings. Keeps the documented example honest.

import std/[math, strutils]
import ../render

const Snippet = """
def string(pluck, len, damp):
  $y = array(len)                    # displacement
  $v = array(len)                    # velocity
  each(len, i =>
    let force = ($y[i - 1] - 2 * $y[i] + $y[i + 1]) * 2e8
    let v = ($v[i] + force * dt) * damp
    $v[i] = v
    $y[i] = $y[i] + v * dt)
  $v[2] = $v[2] + pluck               # strike near one end
  $y[len - 3]                         # listen near the other

play guitar:
  string(impulse(1) * 3000, 48, 0.99998) * 0.3

guitar
"""
# impulse(1) first fires at t = 1 s; listen from there.
let sig = renderPatchSrc(Snippet, 2.0).left
var peak = 0.0
var late = 0.0
for i, x in sig:
  doAssert x == x, "NaN at " & $i
  peak = max(peak, abs(x))
  if i >= 48000 + 36000: late = max(late, abs(x))
echo "string peak ", formatFloat(peak, ffDecimal, 3), " late peak ", formatFloat(late, ffDecimal, 4)
doAssert peak > 0.01 and peak < 1.0, $peak
doAssert late > peak * 0.05, "the string should still ring 0.75 s after the pluck"
# Period by autocorrelation: c/dx = sqrt(2e8) = 14142 /s over 49 gaps ->
# ~144 Hz, a period of ~330 samples.
let n = sig.len
var best = 0
var bestR = -1e300
for lag in 200 .. 500:
  var r = 0.0
  for i in 52800 ..< n - lag: r += sig[i] * sig[i + lag]
  if r > bestR: bestR = r; best = lag
echo "string period ", best, " samples (", formatFloat(48000.0 / float64(best), ffDecimal, 1), " Hz)"
doAssert best > 300 and best < 360, $best
echo "guide string ok"
