## Regression test for the emitStereo double-emission bug that turned
## any stateful primitive in a final expression (or one combined with
## stereo) into a 2x-speed version of itself.
##
## Background. emitStereo walks a subtree looking for stereo values. In
## the "all-scalar" fallback branches of nkBinOp/nkUnary/nkIf/nkCall it
## re-emitted the whole node via emitExpr, registering duplicate state
## regions AND returning a compound C expression that the top-level
## harness then wrote into BOTH `*outL` and `*outR`. With side-effecting
## expressions (phasor, delay, native DSP) that meant the state-advance
## ran twice per sample — phasor(110) as a final expression sounded at
## 220 Hz, not 110 Hz.
##
## The fix bound scalar-valued results to a C temp via `pre` so both
## outL and outR reference the temp, and short-circuited refsStereo=false
## subtrees straight to a single emitExpr. This test pins the invariants.

import std/[math, strutils]
import ../parser, ../voice, ../codegen

proc zeroCrossings(src: string; windowStart, N: int; sr = 48000.0): int =
  let v = newVoice(sr)
  v.load(parseProgram(src), sr)
  for i in 0..<windowStart:
    discard v.tick(float64(i) / sr)
  var prev = 0.0
  result = 0
  for i in windowStart..<(windowStart+N):
    let s = v.tick(float64(i) / sr)
    if prev <= 0 and s.l > 0: inc result
    prev = s.l

proc countRegions(src: string; typeName: string): int =
  let (_, _, _, regions) = generate(parseProgram(src), "", 48000.0)
  for r in regions:
    if r.typeName == typeName: inc result

# --- 1. Bare stateful primitive as final expression.
# Before the fix, phasor(110) as a final expression ran at 220 Hz.
# After the fix, it must run at 110 Hz.
block bareFinal:
  let zc = zeroCrossings("phasor(110.0) * 2 - 1", 4800, 48000)
  doAssert abs(zc - 110) <= 2,
    "phasor(110) as final expression should sound at 110 Hz, got " &
    $zc & " Hz — emitStereo is duplicating the phasor across outL/outR"

# --- 2. Same via sin wrapper — catches the deeper-nested double-emit.
block sinWrapper:
  let zc = zeroCrossings("sin(TAU * phasor(110.0))", 4800, 48000)
  doAssert abs(zc - 110) <= 2,
    "sin(TAU*phasor(110)) should sound at 110 Hz, got " & $zc & " Hz"

# --- 3. Wrapped in a let (already correct even pre-fix) — sanity
# check we didn't regress the common pattern. Uses the `*2-1` bipolar
# map so zero-crossings are detectable.
block letWrapped:
  const P = "let sig = phasor(110.0) * 2 - 1\nsig"
  let zc = zeroCrossings(P, 4800, 48000)
  doAssert abs(zc - 110) <= 2,
    "let-wrapped phasor(110) should sound at 110 Hz, got " & $zc & " Hz"

# --- 4. Region count for a bare stateful primitive final expression.
# Each phasor call corresponds to exactly one state region; scalar
# subtrees that reach the output must register exactly once.
block regionCountBare:
  doAssert countRegions("phasor(110.0)", "phasor") == 1,
    "phasor(110) alone should register exactly 1 phasor region"
  doAssert countRegions("phasor(110.0) * 2 - 1", "phasor") == 1,
    "phasor(110)*2-1 should register exactly 1 phasor region, not N"
  doAssert countRegions("sin(TAU * phasor(110.0)) * 0.3", "phasor") == 1,
    "sin(TAU*phasor(110))*0.3 should register exactly 1 phasor region"

# --- 5. sum() with stateful lambda: N iterations → exactly N regions.
# Pre-fix: `sum(4, n => phasor(...)) * 0.3` produced 12 regions (or
# similar multiple). Post-fix: 4.
block sumRegionCount:
  doAssert countRegions(
      "sum(4, n => sin(TAU * phasor(100.0 * n)))", "phasor") == 4,
    "bare sum(4, phasor) should register exactly 4 phasor regions"
  doAssert countRegions(
      "sum(4, n => sin(TAU * phasor(100.0 * n))) * 0.3", "phasor") == 4,
    "sum(4, phasor) * 0.3 should still register exactly 4 phasor regions " &
    "— scalar binop context must not multiply region count"
  doAssert countRegions(
      "sum(16, n => sin(TAU * phasor(n * 440.0)) / n) * 0.3", "phasor") == 16,
    "Phase 3 stdlib-style 16-harmonic saw should register exactly 16 " &
    "phasor regions"

# --- 6. Stateful primitive in stereo combine. Before the fix, a
# scalar containing phasor, when combined with a stereo value, had its
# C text duplicated into both L and R channels of the combined stereo
# result — phasor ran twice per sample. After the fix, it must bind
# via a temp and run once.
block scalarPlusStereo:
  # Construct a stereo let whose value is (near-)zero so we can measure
  # the phasor frequency without the stereo channels polluting the
  # zero-crossing count. Nominal note: sin(0) = cos(0) = 0 at t=0, but
  # sin/cos diverge as t grows — so we use a tiny amplitude scaling to
  # ensure the phasor dominates.
  const P = """
let tiny = [sin(t) * 0.00001, cos(t) * 0.00001]
(phasor(110.0) * 2 - 1) + tiny
"""
  let zc = zeroCrossings(P, 4800, 48000)
  doAssert abs(zc - 110) <= 2,
    "scalar-plus-stereo: phasor must run once, got " & $zc & " Hz"

# --- 7. Play block with a stateful final expression.
# Play bodies also go through emitStereo for the final expression.
# This is the pattern the Phase 3 stdlib generates:
#   play saw: additive(midi_freq(), saw_shape, 16) * 0.3
# where additive expands to sum(...) — all stateful, all in final pos.
block playBlockStateful:
  const P = """
play tone:
  phasor(110.0) * 2 - 1
tone
"""
  let zc = zeroCrossings(P, 4800, 48000)
  doAssert abs(zc - 110) <= 2,
    "play-block scalar stateful final should sound at 110 Hz, got " & $zc

echo "stereo_once ok"
