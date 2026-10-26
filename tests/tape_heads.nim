## tape(sig, on, w, r, max_time, keep): a piece of tape with a write head
## and a read head at positions given as signals. A delay, a loop, half
## speed, reverse and sound on sound are choices of the heads; the pool
## holds a tape far longer than the old fixed 4 MB.

import std/[math, strutils]
import ../render

const Sr = 48000.0
proc both(src: string; n: int): Stereo = renderPatchSrc(src, float64(n) / Sr)

# A delay: the write head at now, the read head 3 samples behind.
let d = both("let x = if t < dt then 1 else 0\ntape(x, 1, t, t - 3 / sr, 0.01)", 8).left
doAssert abs(d[3] - 1.0) < 1e-9 and abs(d.sum - 1.0) < 1e-9, $d

# A loop: half a second recorded, then played round and round, exactly.
# Left is what went in, right is the tape, which reads before it writes:
# silence the first time round, then what was recorded half a second ago.
let lp = both("let x = noise()\n[x, tape(x, if t < 0.5 then 1 else 0, t, t, 0.5)]", 72000)
for i in 0 ..< 24000: doAssert lp.right[i] == 0.0, "fresh tape at " & $i
for i in 24000 ..< 72000:
  doAssert abs(lp.right[i] - lp.left[i mod 24000]) < 1e-9, "loop at " & $i

# Half speed: a 1 kHz tone read at half the rate it was written is 500 Hz.
let hs = both("tape(sin(TAU * 1000 * t), 1, t, t * 0.5, 1)", 48000).left
var crossings = 0
for i in 24001 ..< 48000:
  if (hs[i - 1] < 0.0) != (hs[i] < 0.0): inc crossings
doAssert abs(crossings - 500) <= 2, "half speed: " & $crossings & " crossings in 0.5 s"

# Reverse: a second of the sample count written, then read backwards.
let rv = both("tape(t * sr, if t < 1 then 1 else 0, t, 2 - t, 1)", 96000).left
for i in 48010 ..< 95990:
  doAssert abs(rv[i] - (96000.0 - float64(i))) < 1e-6, "reverse at " & $i & ": " & $rv[i]

# keep: sound on sound. An impulse goes round a 100-sample tape (0.0021 s
# at 48 kHz), each
# pass written over itself at half: 1, 0.5, 0.25.
let ov = both("let x = if t < dt then 1 else 0\ntape(x, 1, t, t, 0.0021, keep = 0.5)", 320).left
doAssert abs(ov[100] - 1.0) < 1e-9 and abs(ov[200] - 0.5) < 1e-9 and
         abs(ov[300] - 0.25) < 1e-9, $ov[100] & " " & $ov[200] & " " & $ov[300]

# A pair: two tapes on the same heads, each side its own.
let pr = both("let x = if t < dt then 1 else 0\ntape([x, x * 2], 1, t, t - 5 / sr, 0.01)", 8)
doAssert abs(pr.left[5] - 1.0) < 1e-9 and abs(pr.right[5] - 2.0) < 1e-9, $pr.left & $pr.right

# Thirty seconds of stereo: 2.88 million slots, seven times the old pool.
let long = both("let x = if t < dt then 1 else 0\ntape([x, x], 1, t, t - 10 / sr, 30)", 16)
doAssert abs(long.left[10] - 1.0) < 1e-9 and abs(long.right[10] - 1.0) < 1e-9, $long.left

echo "tape heads ok"
