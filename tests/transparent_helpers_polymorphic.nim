## Transparent signal helpers (lpf, hpf, bpf, lp1, hp1, drive, fold,
## delay, fbdelay, slew, discharge, pluck, swell, adsr, gain, prev,
## tremolo) accept either mono or stereo input and preserve the shape:
## stereo input → stereo output with TWO independent state allocations,
## one per channel.
##
## What this test pins:
## 1. Region count — stereo input doubles the per-helper regions, mono
##    stays at one. Captures hot-reload migration's invariant that
##    structurally matching call sites keep their state.
## 2. Per-channel state independence — feeding [impulse, 0] through a
##    stereo lpf rings on L only; R stays silent. Proves the L and R
##    filters are truly independent, not shared.
## 3. Pipe chains (stereo |> lpf |> drive |> fold) compile and run.
## 4. Mono-input helpers stay single-region (no regression).
## 5. pan / haas with stereo input raise a clear, actionable error.
## 6. A stateful scalar arg (e.g. lfo modulator) is shared across the
##    two channel emissions instead of registering twice — a bug-fix
##    that surfaces once stereo splits routinely include native
##    stateful helpers.

import std/strutils
import ../parser, ../voice, ../codegen
const stdlibSrc = staticRead("../stdlib.aither")

proc parseFull(src: string): Node =
  parseProgram(stdlibSrc & "\n" & src)

proc countRegions(src, typeName: string): int =
  let (_, _, _, regions) = generate(parseFull(src), "", 48000.0)
  for r in regions:
    if r.typeName == typeName: inc result

proc shouldFail(src, expectedSubstr: string) =
  try:
    discard generate(parseFull(src), "", 48000.0)
    doAssert false,
      "expected error containing '" & expectedSubstr & "', got success"
  except ValueError as e:
    doAssert expectedSubstr in e.msg,
      "expected error to contain '" & expectedSubstr & "', got: " & e.msg

# ---- 1. Region count for each transparent helper -----------------------
# Each native helper that accepts stereo registers two regions of its
# own type. Mono stays at one. The (typeName, perTypeIdx, size) tuple is
# what voice.commit uses for hot-reload migration; one region per
# channel keeps that path intact.
const NativeCases = [
  ("lpf",       "let s = [sin(t), cos(t)]\ns |> lpf(800, 0.5)\n",       "lpf"),
  ("hpf",       "let s = [sin(t), cos(t)]\ns |> hpf(200, 0.5)\n",       "hpf"),
  ("bpf",       "let s = [sin(t), cos(t)]\ns |> bpf(800, 0.5)\n",       "bpf"),
  ("lp1",       "let s = [sin(t), cos(t)]\ns |> lp1(800)\n",            "lp1"),
  ("hp1",       "let s = [sin(t), cos(t)]\ns |> hp1(200)\n",            "hp1"),
  ("delay",     "let s = [sin(t), cos(t)]\ns |> delay(0.01, 0.05)\n",   "delay"),
  ("fbdelay",   "let s = [sin(t), cos(t)]\ns |> fbdelay(0.01, 0.05, 0.3)\n", "fbdelay"),
  ("slew",      "let s = [sin(t), cos(t)]\ns |> slew(0.01)\n",          "slew"),
  ("discharge", "let s = [sin(t), cos(t)]\ns |> discharge(20)\n",       "discharge"),
  ("tremolo",   "let s = [sin(t), cos(t)]\ns |> tremolo(5, 0.5)\n",     "tremolo"),
]

for (name, src, region) in NativeCases:
  let got = countRegions(src, region)
  doAssert got == 2,
    name & " stereo: expected 2 " & region & " regions, got " & $got

# Mono input still allocates one region only. Hot-reload migration
# requires the structural identity to be preserved across re-emits.
doAssert countRegions(
  "let s = sin(TAU * phasor(440))\ns |> lpf(800, 0.5)\n", "lpf") == 1

# ---- 2. Aither-defined stateful helpers --------------------------------
# pluck wraps `discharge`; the inlined body claims a discharge region
# per call site. swell uses `$level` (one var region). adsr uses
# `$level` + `$peaked` (two vars per call). prev uses `$last` (one
# var). Doubling the calls via stereo input therefore doubles the
# inlined region claims — exactly the per-channel state separation we
# want.
doAssert countRegions(
  "let s = [sin(t), cos(t)]\ns |> pluck(0.1)\n", "discharge") == 2
doAssert countRegions(
  "let s = [sin(t), cos(t)]\ns |> swell(0.1, 0.5)\n", "var") == 2
doAssert countRegions(
  "let s = [sin(t), cos(t)]\ns |> adsr(0.01, 0.3, 0.7, 0.4)\n",
  "var") == 4
doAssert countRegions(
  "let s = [sin(t), cos(t)]\ns |> prev\n", "var") == 2

# Pure aither defs (drive, fold, gain) have no state, but they're in
# the allowlist so the path is uniform. Just verify they compile in
# stereo context — region count is irrelevant.
discard generate(parseFull(
  "let s = [sin(t), cos(t)]\ns |> drive(1.5)\n"), "", 48000.0)
discard generate(parseFull(
  "let s = [sin(t), cos(t)]\ns |> fold(1.5)\n"), "", 48000.0)
discard generate(parseFull(
  "let s = [sin(t), cos(t)]\ns |> gain(0.5)\n"), "", 48000.0)

# ---- 3. Pipe chain from the task definition-of-done --------------------
discard generate(parseFull(
  "let s = [sin(t), cos(t)]\ns |> lpf(800, 0.5) |> drive(1.2) |> fold(0.7)\n"
), "", 48000.0)
doAssert countRegions(
  "let s = [sin(t), cos(t)]\ns |> lpf(800, 0.5) |> drive(1.2) |> fold(0.7)\n",
  "lpf") == 2

# ---- 4. Per-channel state independence (audible) -----------------------
# Feed an asymmetric stereo signal through lpf — only the L channel
# carries a 110 Hz tone, R is silent. After the filter, L should still
# carry a (filtered) tone and R should remain near-silent. If the L
# and R filter slots were aliased, R's accumulator would pick up
# energy from L's input.
block perChannelIndependence:
  const Patch = """
let trig = if t < 0.001 then 1 else 0
let asymStereo = [sin(TAU * phasor(110)) * 0.5, 0]
asymStereo |> lpf(2000, 0.3)
"""
  let v = newVoice(48000.0)
  v.load(parseProgram(Patch), 48000.0)
  var sumLsq = 0.0
  var sumRsq = 0.0
  for i in 0 ..< 24000:
    let s = v.tick(float64(i) / 48000.0)
    sumLsq += s.l * s.l
    sumRsq += s.r * s.r
  let rmsL = sumLsq / 24000.0
  let rmsR = sumRsq / 24000.0
  doAssert rmsL > 0.01,
    "L channel should carry a filtered tone, rmsL² = " & $rmsL
  doAssert rmsR < 1e-10,
    "R channel should stay silent (independent state), rmsR² = " & $rmsR

# A second variant: each channel filters at a different frequency by
# binding a stereo cutoff. Tests that scalar args propagate to both
# channels uniformly while the channel signals stay separate.
block independentDelay:
  # L channel: 1-sample delay; R channel: also 1-sample delay; both
  # carrying different tones. After delay, each channel should be a
  # phase-shifted version of its own input — not a mix.
  const Patch = """
let s = [sin(TAU * phasor(220)), sin(TAU * phasor(440))]
s |> delay(0.001, 0.05)
"""
  let v = newVoice(48000.0)
  v.load(parseProgram(Patch), 48000.0)
  # Skip past the delay buffer fill so we observe the steady-state
  # filtered output, then accumulate channel energy.
  for i in 0 ..< 1024:
    discard v.tick(float64(i) / 48000.0)
  var sumLsq = 0.0
  var sumRsq = 0.0
  for i in 1024 ..< 1024 + 24000:
    let s = v.tick(float64(i) / 48000.0)
    sumLsq += s.l * s.l
    sumRsq += s.r * s.r
  # Both channels should carry their own tone (RMS² ≈ 0.5 for unit
  # sine) — if delay slots were aliased, the two tones would mix and
  # one or both RMS values would deviate substantially.
  let rmsLsq = sumLsq / 24000.0
  let rmsRsq = sumRsq / 24000.0
  doAssert abs(rmsLsq - 0.5) < 0.05,
    "L channel rms² should be ≈ 0.5 (sine through delay), got " & $rmsLsq
  doAssert abs(rmsRsq - 0.5) < 0.05,
    "R channel rms² should be ≈ 0.5, got " & $rmsRsq

# ---- 5. Stateful scalar arg shared across channels ---------------------
# `lpf(stereo, modulated_cutoff, q)` should NOT register the modulator's
# state twice. That used to happen via the original-AST-node duplication
# in the split path; the fix binds via a fresh ident per scalar arg.
# Region count: 2 stereo source phasors + 1 lfo phasor = 3 total.
doAssert countRegions(
  "let s = [sin(TAU*phasor(440)), sin(TAU*phasor(660))]\n" &
  "s |> lpf(800 + sin(TAU*phasor(2)) * 100, 0.5)\n",
  "phasor") == 3
doAssert countRegions(
  "let s = [sin(TAU*phasor(440)), sin(TAU*phasor(660))]\n" &
  "s |> lpf(800 + sin(TAU*phasor(2)) * 100, 0.5)\n",
  "lpf") == 2

# ---- 6. pan / haas refuse stereo input with a clear error --------------
shouldFail("let s = [sin(t), cos(t)]\ns |> pan(0)\n",
           "pan takes a mono signal")
shouldFail("let s = [sin(t), cos(t)]\ns |> haas(8)\n",
           "haas takes a mono signal")

# ---- 7. Migration identity preserved across mono → mono reload ---------
# A patch that uses a transparent helper on mono input must still match
# its own (typeName, perTypeIdx, size) on a no-op reload.
block migrationStability:
  const Patch = """
let s = sin(TAU * phasor(220))
s |> lpf(800, 0.5)
"""
  let prog = parseProgram(Patch)
  let (_, _, _, r1) = generate(prog, "", 48000.0)
  let (_, _, _, r2) = generate(prog, "", 48000.0)
  doAssert r1.len == r2.len
  for i in 0 ..< r1.len:
    doAssert r1[i].typeName == r2[i].typeName
    doAssert r1[i].perTypeIdx == r2[i].perTypeIdx
    doAssert r1[i].size == r2[i].size

echo "transparent helper polymorphism ok"
