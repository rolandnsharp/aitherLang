## delay() reads between samples (linear interpolation) and a 0-sample
## delay is this sample's input, not the oldest sample in the buffer.
## fbdelay keeps read-before-write (its feedback needs the last output)
## and so starts at one sample. (2026-10-12: the 8-section tube came out
## 27% too short with whole-sample delays.)

import std/[math, strutils]
import ../render

proc mono(src: string; n: int): seq[float64] =
  renderPatchSrc(src, float64(n) / 48000.0).left

# An impulse at sample 0, delayed by 0 samples, comes out at sample 0.
let d0 = mono("let x = if t < dt then 1 else 0\ndelay(x, 0, 0.001)", 8)
doAssert d0[0] == 1.0, "0-sample delay should be the current input: " & $d0
doAssert d0[1 .. ^1].max == 0.0, $d0

# A whole-sample delay is an exact shift.
let d3 = mono("let x = if t < dt then 1 else 0\ndelay(x, 3 / sr, 0.001)", 8)
doAssert d3[3] == 1.0 and d3.sum == 1.0, $d3

# 2.5 samples: the impulse lands half on sample 2, half on sample 3.
let d25 = mono("let x = if t < dt then 1 else 0\ndelay(x, 2.5 / sr, 0.001)", 8)
doAssert abs(d25[2] - 0.5) < 1e-12 and abs(d25[3] - 0.5) < 1e-12, $d25
doAssert abs(d25.sum - 1.0) < 1e-12, $d25

# Interpolation keeps a slow signal continuous: a ramp delayed by a
# fractional amount is the ramp shifted by exactly that amount.
let rampDelayed = mono("delay(t * sr, 7.25 / sr, 0.001)", 20)
for i in 10 ..< 20:
  doAssert abs(rampDelayed[i] - (float64(i) - 7.25)) < 1e-9,
    "sample " & $i & ": " & $rampDelayed[i]

# The longest delay is just under max_time, and the whole buffer is
# usable: 47 samples of a 1 ms (48-sample) buffer.
let d47 = mono("let x = if t < dt then 1 else 0\ndelay(x, 47 / sr, 0.001)", 50)
doAssert d47[47] == 1.0 and d47.sum == 1.0, $d47

# fbdelay: a 0 time clamps to one sample, not the oldest sample.
let fb = mono("let x = if t < dt then 1 else 0\nfbdelay(x, 0, 0.001, 0.5)", 6)
doAssert fb[1] == 1.0 and fb[2] == 0.5 and fb[3] == 0.25, $fb
# ... and a fractional feedback delay still decays sanely.
let fb25 = mono("let x = if t < dt then 1 else 0\nfbdelay(x, 2.5 / sr, 0.001, 0.5)", 12)
doAssert abs(fb25[2] - 0.5) < 1e-12 and abs(fb25[3] - 0.5) < 1e-12, $fb25
doAssert fb25.max <= 0.5 + 1e-12, $fb25

# A recirculating loop tunes to its length, not to the sample grid: a
# 108.5-sample delay read through `$loop` (one more sample) echoes every
# 109.5 samples. Each pass spreads the pulse over one more sample
# (linear interpolation), but its centre stays exact.
let ks = mono("let strike = if t < 1 / sr then 1 else 0\n" &
              "$loop = 0.0\n$loop = strike + delay($loop, 108.5 / sr, 0.05) * 0.999\n$loop",
              400)
proc centroid(sig: seq[float64]; lo, hi: int): float64 =
  var num, den = 0.0
  for i in lo .. hi:
    num += float64(i) * sig[i]; den += sig[i]
  num / den
doAssert abs(ks[109] - 0.4995) < 1e-9 and abs(ks[110] - 0.4995) < 1e-9, $ks[105 .. 112]
doAssert abs(centroid(ks, 100, 120) - 109.5) < 1e-9, $centroid(ks, 100, 120)
doAssert abs(centroid(ks, 210, 230) - 219.0) < 1e-9, $centroid(ks, 210, 230)
doAssert abs(centroid(ks, 320, 340) - 328.5) < 1e-9, $centroid(ks, 320, 340)
echo "delay interpolation ok"
