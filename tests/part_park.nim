## A parked part is silent and its body is skipped: its state freezes
## where it was and resumes on unpark. (The 2026-10-10 crackle was the
## engine underrunning on 21 muted parts that still computed.)

import std/strutils
import ../ccompile, ../voice

const Patch = """
play count:
  $n = 0
  $n = $n + 1
  $n
play other:
  $m = 0
  $m = $m + 1
  $m * 1000
count + other
"""
let v = newVoice(48000.0)
v.load((Patch), 48000.0)
doAssert v.partNames == @["count", "other"]
for i in 0 ..< 5: discard v.tick(float64(i) / 48000.0)
doAssert v.tick(0.0).l == 6006.0

# Park `count`: it contributes 0 and stops counting; `other` goes on.
v.partParked[0] = 1.0
for i in 0 ..< 10: discard v.tick(0.0)
let parked = v.tick(0.0)
doAssert parked.l == 17000.0, $parked.l
doAssert v.partOut[0] == 0.0 and v.partOut[1] == 0.0

# Unpark: count resumes from 6, not from 0 and not from 17.
v.partParked[0] = 0.0
let resumed = v.tick(0.0)
doAssert resumed.l == 7.0 + 18000.0, $resumed.l

# A hot reload keeps the park by name.
v.partParked[1] = 1.0
v.load((Patch.replace("* 1000", "* 100")), 48000.0)
doAssert v.partParked[0] == 0.0 and v.partParked[1] == 1.0, $v.partParked[1]
let after = v.tick(0.0)
doAssert after.l == 8.0, $after.l
echo "part park ok"
