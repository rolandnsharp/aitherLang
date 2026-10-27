## A hot-swap that blows up can be rolled back: the previous code comes
## back with its state as of the swap, and file-level `$vars` that went
## NaN re-init instead of migrating (they used to poison every resend).

import std/math
import ../ccompile, ../voice

proc tickN(v: NativeVoice; n: int; t0 = 0): tuple[l, r: float64] =
  for i in 1 .. n: result = v.tick(float64(t0 + i) / 48000.0)

const Good = """
$n = 0
$n = $n + 1
sin(TAU * phasor(220)) * 0.2
"""
const Bad = """
$n = 0
$n = $n + 1
$x = 0.01
$x = $x * 1.01
sin(TAU * phasor(220)) * 0.2 + $x
"""

let v = newVoice(48000.0)
v.load((Good), 48000.0)
discard v.tickN(1000)
doAssert not v.canRollback
v.load((Bad), 48000.0)
doAssert v.canRollback
let blown = v.tickN(1000, 1000)
doAssert abs(blown.l) > 8.0, "bad patch should blow up: " & $blown.l
doAssert v.rollback()
doAssert not v.canRollback
let back = v.tickN(100, 2000)
doAssert abs(back.l) <= 0.2 + 1e-9, "rolled-back voice is sane: " & $back.l

# A NaN'd file-level var re-inits on the next send instead of migrating.
const NanVar = """
$y = 1
$y = $y + sqrt(0 - 1)
0
"""
const Fixed = """
$y = 0.5
$y = $y * 1
$y
"""
let w = newVoice(48000.0)
w.load((NanVar), 48000.0)
discard w.tickN(3)
w.load((Fixed), 48000.0)
let first = w.tickN(1)
doAssert first.l == 0.5, "NaN var should re-init, got " & $first.l

# scrubState clears a NaN var in place without touching healthy ones.
w.load((NanVar), 48000.0)
discard w.tickN(3)
w.scrubState()
w.load((Fixed), 48000.0)
doAssert w.tickN(1).l == 0.5

echo "rollback ok"
