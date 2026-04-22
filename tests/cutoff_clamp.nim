## A negative-cutoff bpf used to feed `tan(PI * negCutoff / sr)` (a
## very negative angle), generating Inf/NaN that propagated through
## the filter's pool slots forever. The svf helper now clamps cutoff
## to (1.0, sr*0.499] up front, so the filter degrades to silent or
## quiet rather than producing NaN.

import std/[math]
import ../parser, ../voice

const Patch = """
let cut = -300
let filtered = noise() |> bpf(cut, 0.6)
filtered * 0.1
"""

let v = newVoice(48000.0)
v.load(parseProgram(Patch), 48000.0)

for i in 1 .. 10000:
  let s = v.tick(float64(i) / 48000.0)
  doAssert s.l == s.l and s.r == s.r,
    "negative cutoff produced NaN at sample " & $i
  doAssert abs(s.l) < 1e6 and abs(s.r) < 1e6,
    "negative cutoff produced Inf-ish at sample " & $i

echo "cutoff clamp ok"
