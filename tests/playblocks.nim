## Item 3 test: play blocks compile, partNames populate, per-part gain
## works, stereo bodies produce independent L/R channels, and hot-reload
## preserves part gains by name.

import std/[math]
import ../parser, ../voice

# Three plays: scalar, scalar-but-different, and explicit stereo.
# Final expression references them both as a sum (stereo broadcast) and
# via indexing (check channel picks work).
const PatchA = """
play a:
  sin(TAU * phasor(440)) * 0.3
play b:
  [sin(TAU * phasor(220)) * 0.3, sin(TAU * phasor(221)) * 0.3]
a + b
"""

let prog = parseProgram(PatchA)
let v = newVoice(48000.0)
v.load(prog, 48000.0)

doAssert v.partNames == @["a", "b"], "partNames was " & $v.partNames
doAssert v.partGains.len == 2
for g in v.partGains: doAssert g == 1.0

# Play block b is stereo — expect L != R over a short run (220 Hz vs 221 Hz).
var deltaSq = 0.0
for i in 0 ..< 4800:
  let s = v.tick(float64(i) / 48000.0)
  deltaSq += (s.l - s.r) * (s.l - s.r)
doAssert deltaSq > 0.0, "stereo body should produce L != R over time"

# Muting play a should drop the overall level significantly.
for i in 0 ..< 2: v.partGains[i] = 1.0
var fullPeak = 0.0
for i in 0 ..< 4800:
  let s = v.tick(float64(i) / 48000.0)
  fullPeak = max(fullPeak, max(abs(s.l), abs(s.r)))

v.partGains[0] = 0.0  # silence play a
var mutedPeak = 0.0
for i in 0 ..< 4800:
  let s = v.tick(float64(i + 10000) / 48000.0)
  mutedPeak = max(mutedPeak, max(abs(s.l), abs(s.r)))
doAssert mutedPeak < fullPeak,
  "muting play a should reduce peak (full=" & $fullPeak &
  " muted=" & $mutedPeak & ")"

# Hot reload — change the patch slightly but keep both part names.
# Part a's gain (set to 0.0 above) should persist.
const PatchB = """
play a:
  sin(TAU * phasor(441)) * 0.3
play b:
  [sin(TAU * phasor(219)) * 0.3, sin(TAU * phasor(222)) * 0.3]
a + b
"""
v.load(parseProgram(PatchB), 48000.0)
doAssert v.partGains[0] == 0.0,
  "hot reload should preserve part 'a' gain of 0.0, got " & $v.partGains[0]
doAssert v.partGains[1] == 1.0,
  "part 'b' gain should stay 1.0, got " & $v.partGains[1]
echo "playblocks ok"
