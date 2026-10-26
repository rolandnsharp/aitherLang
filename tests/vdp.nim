## vdp: a self-sustaining oscillator grows from its start to a circle of
## radius 1 at its frequency, and two detuned ones on a spring lock.

import std/math
import ../render

const Sr = 48000.0
proc left(src: string; secs: float64): seq[float64] = renderPatchSrc(src, secs).left

proc crossings(x: seq[float64]; a, b: int): int =
  for i in a + 1 ..< b:
    if (x[i - 1] < 0.0) != (x[i] < 0.0): inc result

# Alone: a sine at 220 Hz, peak about 1 once grown, from 0.01.
let one = left("vdp(220)[0]", 2.0)
doAssert abs(one[0] - 0.01) < 0.001, $one[0]
var peak = 0.0
for i in 48000 ..< 96000: peak = max(peak, abs(one[i]))
doAssert abs(peak - 1.0) < 0.02, "radius " & $peak
doAssert abs(crossings(one, 48000, 96000) - 440) <= 2, $crossings(one, 48000, 96000)

# Two at 220 and 223 Hz: apart they beat, a 3 Hz swell; on a spring of
# 0.1 they lock, and the sum's loudness holds still.
proc sumOf(k: string): seq[float64] =
  left("$pa = 0.0\n$pb = 0.0\nlet a = vdp(220, push = " & k & " * ($pb - $pa))\n" &
       "let b = vdp(223, push = " & k & " * ($pa - $pb))\n$pa = a[0]\n$pb = b[0]\na[0] + b[0]", 6.0)
proc swing(x: seq[float64]): float64 =
  # how much the sum's peak level varies over the last 2 s, in 50 ms windows
  var lo = 1e9
  var hi = 0.0
  var i = 4 * 48000
  while i + 2400 <= x.len:
    var p = 0.0
    for j in i ..< i + 2400: p = max(p, abs(x[j]))
    lo = min(lo, p); hi = max(hi, p)
    i += 2400
  hi - lo
let apart = swing(sumOf("0"))
let locked = swing(sumOf("0.1"))
doAssert apart > 1.0, "apart they should beat: " & $apart
doAssert locked < 0.05, "on a spring they should lock: " & $locked
echo "vdp ok (beat swing ", apart, ", locked ", locked, ")"
