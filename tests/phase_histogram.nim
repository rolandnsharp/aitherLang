## Phase histogram — Test 1 of the pair-valued aether experiment.
##
## Renders a patch that strikes a source voice at a fixed rate
## (impulse(strikeRate)) and contains exactly one receiver voice ringing
## at `readFreq`. For each strike time T, finds the first positive-going
## zero crossing of the receiver's output after T, computes the offset
## (T_zero - T) mod period in radians, and histograms.
##
## Run via:
##   nim r --hints:off --warnings:off tests/phase_histogram.nim \
##         <patch> <seconds> [strikeRate=2.0] [readFreq=277.18]
##
## Reports per-strike phase, histogram (16 bins), circular variance,
## and equivalent angular standard deviation in radians (and as π
## fractions). For the test:
##   PASS: stddev < π/4 (≈ 0.785) — phase is peaked
##   FAIL: stddev ~ π — phase is uniformly distributed

import std/[os, strutils, math, strformat]
import ../render

proc reps(c: char; n: int): string =
  result = newString(max(0, n))
  for i in 0 ..< result.len: result[i] = c

let args = commandLineParams()
if args.len < 2:
  quit "usage: phase_histogram <patch> <seconds> [strikeRate=2.0] [readFreq=277.18]"

let patch = args[0]
let seconds = parseFloat(args[1])
let strikeRate = if args.len >= 3: parseFloat(args[2]) else: 2.0
let readFreq   = if args.len >= 4: parseFloat(args[3]) else: 277.18

let sr = 48000.0
let period = 1.0 / readFreq
let searchWindowSamples = int(sr * period * 2.0)  # search up to 2 periods

echo &"phase_histogram: {patch}  total={seconds}s  strikeRate={strikeRate} Hz  readFreq={readFreq} Hz"

var (l, r) = renderPatch(patch, seconds, int(sr))
let mono = monoMix(l, r)

# Strikes occur at t = k / strikeRate for k = 0, 1, 2, ...
# Skip k=0 (patch hasn't established a baseline yet) and any strike whose
# search window would run past the buffer.
var phases: seq[float64] = @[]
let nStrikesNominal = int(seconds * strikeRate)
for k in 1 ..< nStrikesNominal:
  let strikeSample = int(float64(k) / strikeRate * sr)
  if strikeSample + searchWindowSamples >= mono.len: break
  # Find first positive-going zero crossing in the window [strike, strike + 2*period]
  var found = false
  for i in (strikeSample + 1) ..< (strikeSample + searchWindowSamples):
    if mono[i-1] <= 0.0 and mono[i] > 0.0:
      # Linear interpolation for sub-sample accuracy
      let denom = mono[i] - mono[i-1]
      let frac = if denom != 0.0: -mono[i-1] / denom else: 0.0
      let zcTime = (float64(i-1) + frac) / sr
      let strikeTime = float64(strikeSample) / sr
      let dt = zcTime - strikeTime
      var phase = (dt - floor(dt / period) * period) / period * 2.0 * PI
      if phase < 0: phase += 2.0 * PI
      phases.add phase
      found = true
      break
  if not found: discard

if phases.len < 2:
  echo &"  ERROR: only {phases.len} phases extracted (no zero crossings found)"
  echo &"  receiver may be silent — check render and strike rate"
  quit(1)

# Circular statistics: R = |mean(e^{i*phase})| in [0, 1].
# R near 1 → phases concentrated; R near 0 → phases uniform.
var sumCos = 0.0
var sumSin = 0.0
for p in phases:
  sumCos += cos(p)
  sumSin += sin(p)
let n = float64(phases.len)
let R = sqrt(sumCos*sumCos + sumSin*sumSin) / n
let circVar = 1.0 - R
# Approximate angular variance for von Mises: σ² ≈ -2 ln(R)
let stddevRad = if R > 0.001: sqrt(-2.0 * ln(R)) else: PI

let meanAngle = arctan2(sumSin, sumCos)
let meanAngleNorm = if meanAngle < 0: meanAngle + 2.0 * PI else: meanAngle

const nBins = 16
var bins: array[nBins, int]
for p in phases:
  let bin = min(nBins-1, int(p / (2.0 * PI) * float64(nBins)))
  bins[bin] += 1
var maxBin = 0
for b in 0 ..< nBins:
  if bins[b] > maxBin: maxBin = bins[b]
let scale = if maxBin > 60: 60.0 / float64(maxBin) else: 1.0

echo &"  N strikes analysed = {phases.len}"
echo &"  mean angle         = {meanAngleNorm:>6.3f} rad   ({meanAngleNorm/PI:>5.3f} π)"
echo &"  circular variance  = {circVar:>6.4f}        (0 = peaked, 1 = uniform)"
echo &"  effective σ        = {stddevRad:>6.4f} rad   ({stddevRad/PI:>5.3f} π)"
let pi4 = PI / 4.0
let pi8 = PI / 8.0
let verdict =
  if stddevRad < pi8: "PASS (σ < π/8 — strongly peaked)"
  elif stddevRad < pi4: "PASS (σ < π/4 — peaked)"
  elif stddevRad < PI * 0.7: "PARTIAL (intermediate)"
  else: "FAIL (σ ~ π — uniform)"
echo &"  verdict            = {verdict}"
echo "  histogram (16 bins, * scaled):"
for b in 0 ..< nBins:
  let lo = float64(b) / float64(nBins) * 2.0 * PI
  let hi = float64(b+1) / float64(nBins) * 2.0 * PI
  let ascBars = int(float64(bins[b]) * scale)
  echo &"  [{lo:>5.3f}, {hi:>5.3f}) [{bins[b]:>3}]: {reps('*', ascBars)}"
