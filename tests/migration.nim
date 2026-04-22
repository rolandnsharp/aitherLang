## Item 1 test: top-level `var` values migrate by name across hot reload.
## Patch A accumulates `phase` for 100 ticks (→ 1.0). Reload same patch;
## first tick should see the migrated phase, not a fresh 0.

import std/[os, tables]
import ../parser, ../voice

const PatchA = """
var phase = 0
phase = phase + 0.01
phase
"""

const PatchB = """
var phase = 0
var freq = 440
phase = phase + 0.01
phase + freq * 0
"""

let progA = parseProgram(PatchA)
let v = newVoice(48000.0)
v.load(progA, 48000.0)

var l, r: float64
for i in 1 .. 100:
  discard v.tick(float64(i) / 48000.0)
# After 100 ticks, phase ≈ 1.0.
let afterA = v.tick(0.0)
doAssert abs(afterA.l - 1.01) < 1e-9,
  "pre-reload phase was " & $afterA.l
echo "before reload: phase=", afterA.l

# Reload the same program. Without migration, phase would reset to 0
# (and the first tick would return 0.01). With migration, phase is
# preserved (first tick returns ≈1.02).
v.load(progA, 48000.0)
let afterReload = v.tick(0.0)
doAssert abs(afterReload.l - 1.02) < 1e-9,
  "expected migrated phase ≈1.02, got " & $afterReload.l
echo "after reload:  phase=", afterReload.l

# Now load a program that keeps `phase` but adds a new `freq`. phase must
# stay migrated; freq must initialize to 440.
let progB = parseProgram(PatchB)
v.load(progB, 48000.0)
let afterB = v.tick(0.0)
# phase was ≈1.02, increments to ≈1.03; freq*0 = 0.
doAssert abs(afterB.l - 1.03) < 1e-9,
  "expected ≈1.03 after adding freq var, got " & $afterB.l
# Inspect the freq slot directly via the varAddr sibling logic — easier to
# assert against the compiled struct by running a tick that reads it.
# Patch that returns freq:
const PatchC = """
var phase = 0
var freq = 440
freq
"""
v.load(parseProgram(PatchC), 48000.0)
let afterC = v.tick(0.0)
doAssert abs(afterC.l - 440.0) < 1e-9,
  "freq should migrate from prior compilation, got " & $afterC.l
echo "migration ok"
