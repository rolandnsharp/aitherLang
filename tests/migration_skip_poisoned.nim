## Defense 1: when the old pool's region for a (type, perTypeIdx) match
## contains NaN/Inf, hot-reload migration leaves the new region zeroed
## rather than copying the poison forward. Without this, fixing the
## broken patch wouldn't recover — the migrated NaN state stays NaN
## forever.
##
## Patch A feeds NaN into an lpf, polluting the lpf's pool slots. Patch
## B is structurally compatible (same lpf at perTypeIdx 0) but reads a
## clean signal. After reload, lpf state must be zeroed (skipped), so
## the next tick produces a finite sample.

import std/[math]
import ../parser, ../voice

const PatchA = """
sqrt(-1) |> lpf(800, 0.5)
"""

const PatchB = """
sin(TAU * phasor(220)) |> lpf(800, 0.5)
"""

let v = newVoice(48000.0)
v.load(parseProgram(PatchA), 48000.0)
for i in 1 .. 4:
  discard v.tick(float64(i) / 48000.0)

# Sanity: PatchA's lpf state should now contain NaN.
let pool = cast[ptr UncheckedArray[float64]](v.state)
var foundNaN = false
for r in v.regions:
  if r.typeName == "lpf":
    for k in 0 ..< r.size:
      if pool[r.offset + k] != pool[r.offset + k]: foundNaN = true
doAssert foundNaN, "test setup failed: lpf pool not poisoned"

v.load(parseProgram(PatchB), 48000.0)
let s = v.tick(0.0)
doAssert s.l == s.l and s.r == s.r,
  "expected clean output after migrating away from poisoned pool, got " &
  $s.l & ", " & $s.r

# And the lpf region in the new state should be zero (migration skipped).
let pool2 = cast[ptr UncheckedArray[float64]](v.state)
for r in v.regions:
  if r.typeName == "lpf":
    # After one tick the lpf has accumulated a tiny non-NaN value, so we
    # only check it isn't NaN.
    for k in 0 ..< r.size:
      doAssert pool2[r.offset + k] == pool2[r.offset + k],
        "lpf pool still poisoned after reload"

echo "migration skip-poisoned ok"
