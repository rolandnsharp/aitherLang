## Parameter sweep on aether_muqabala — render with one parameter
## substituted to a variant value, report aggregate stddev numbers.
## Scans the cybernetic regime structure: which parameter regions
## produce chaotic motion vs static attractors vs runaway.
##
## Run via:
##   nim r --hints:off --warnings:off --threads:on tests/sweep_muqabala.nim

import std/[strutils, math, strformat, os]
import ../render, ../analysis

const PatchPath = "patches/aether_muqabala.aither"

proc mean(xs: openArray[float64]): float64 =
  if xs.len == 0: return 0.0
  var s = 0.0
  for x in xs: s += x
  s / float64(xs.len)

proc stddev(xs: openArray[float64]): float64 =
  if xs.len < 2: return 0.0
  let m = mean(xs)
  var s = 0.0
  for x in xs: s += (x - m) * (x - m)
  sqrt(s / float64(xs.len - 1))

proc auditWindowed(src: string; seconds: float64; windowSec: float64):
    tuple[rmsMean, rmsStd, centroidMean, centroidStd,
          peakHzMean, peakHzStd, peakDb: float64; nan: bool] =
  let sr = 48000
  var l, r: seq[float64]
  try:
    (l, r) = renderPatchSrc(src, seconds, sr)
  except RenderError:
    result.nan = true
    return
  let mono = monoMix(l, r)
  let nWin = int(seconds / windowSec)
  let winSamples = int(windowSec * float64(sr))
  var rmsSeries, centSeries, peakHzSeries, peakDbSeries: seq[float64] = @[]
  for i in 0 ..< nWin:
    let start = i * winSamples
    let stop = min(start + winSamples, mono.len)
    if stop - start < 1024: break
    let chunk = mono[start ..< stop]
    let summary = analyze(chunk, float64(sr))
    rmsSeries.add summary.rmsDb
    centSeries.add summary.centroidHz
    if summary.peaks.len > 0:
      peakHzSeries.add summary.peaks[0].freqHz
      peakDbSeries.add summary.peaks[0].magDb
  result.rmsMean = mean(rmsSeries)
  result.rmsStd = stddev(rmsSeries)
  result.centroidMean = mean(centSeries)
  result.centroidStd = stddev(centSeries)
  result.peakHzMean = mean(peakHzSeries)
  result.peakHzStd = stddev(peakHzSeries)
  result.peakDb = mean(peakDbSeries)

let basePatch = readFile(PatchPath)

proc swap(orig: string; param: string; value: string): string =
  ## Replace the first `let <param> = ...` occurrence with `let <param> = value`.
  ## Naive but works for our patch's straightforward let lines.
  let lines = orig.split('\n')
  var dst = newSeq[string](lines.len)
  var done = false
  for i, line in lines:
    if not done and line.startsWith("let " & param & " "):
      dst[i] = "let " & param & " = " & value
      done = true
    else:
      dst[i] = line
  dst.join("\n")

proc oneRow(label: string; src: string) =
  let r = auditWindowed(src, 8.0, 1.0)
  if r.nan:
    echo &"  {label:<32}  NaN"
  else:
    echo &"  {label:<32}  RMS μ={r.rmsMean:>6.2f} σ={r.rmsStd:>5.2f} | " &
         &"cent μ={r.centroidMean:>6.0f} σ={r.centroidStd:>6.1f} | " &
         &"peak μ={r.peakHzMean:>5.0f} σ={r.peakHzStd:>5.1f}"

echo "PARAMETER SWEEP — aether_muqabala (8s renders, 1s windows)"
echo ""
echo "baseline (defaults):"
oneRow("baseline", basePatch)

echo ""
echo "carrierToAether sweep (the falsification + loop-gain knob):"
for v in @["0.0", "0.2", "0.5", "0.8", "1.5", "3.0", "5.0", "10.0"]:
  oneRow(&"carrierToAether={v}", swap(basePatch, "carrierToAether", v))

echo ""
echo "carrierCoupling sweep (aether → carrier additive bias):"
for v in @["0.0", "0.1", "0.3", "0.6", "1.0", "2.0"]:
  oneRow(&"carrierCoupling={v}", swap(basePatch, "carrierCoupling", v))

echo ""
echo "regStrength sweep (regulator pull on the noise floor):"
for v in @["0.0", "0.1", "0.4", "0.8", "1.5", "3.0"]:
  oneRow(&"regStrength={v}", swap(basePatch, "regStrength", v))

echo ""
echo "aetherDecay sweep (memory of the medium):"
for v in @["0.85", "0.95", "0.99", "0.995", "0.999"]:
  oneRow(&"aetherDecay={v}", swap(basePatch, "aetherDecay", v))

echo ""
echo "fmDepthHz sweep (state → carrier-frequency wandering):"
for v in @["0.0", "20.0", "60.0", "150.0", "400.0"]:
  oneRow(&"fmDepthHz={v}", swap(basePatch, "fmDepthHz", v))

echo ""
echo "foldModDepth sweep (state → wavefolder-amount):"
for v in @["0.0", "0.5", "1.5", "4.0", "10.0"]:
  oneRow(&"foldModDepth={v}", swap(basePatch, "foldModDepth", v))

echo ""
echo "COMBO sweep: high carrierToAether × low regStrength (suspected chaos):"
for cToA in @["1.0", "1.5", "2.0", "3.0"]:
  for reg in @["0.0", "0.05", "0.1", "0.2"]:
    let p1 = swap(basePatch, "carrierToAether", cToA)
    let p2 = swap(p1, "regStrength", reg)
    oneRow(&"cToA={cToA},reg={reg}", p2)
