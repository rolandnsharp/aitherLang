## Temporal audit — windowed RMS / centroid / peak-frequency analysis
## of a patch's render. Used to characterise cybernetic patches whose
## spectrum/RMS evolves over time (a property the default audit's
## single-FFT-over-the-whole-buffer flattens out).
##
## Run via:
##   nim r --hints:off --warnings:off tests/temporal_audit.nim \
##         <patch> <seconds> [windowSec]
##
## Reports per-window RMS dB, centroid Hz, dominant peak (Hz, dB),
## and aggregate stats: mean and stddev of centroid + RMS across
## windows. A static patch has near-zero stddev; a cybernetic patch
## has large stddev.

import std/[os, strutils, math, strformat]
import ../render, ../analysis

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

let args = commandLineParams()
if args.len < 2:
  quit "usage: temporal_audit <patch> <seconds> [windowSec=1.0]"

let patch = args[0]
let seconds = parseFloat(args[1])
let windowSec =
  if args.len >= 3: parseFloat(args[2]) else: 1.0

let sr = 48000
let nWin = int(seconds / windowSec)
if nWin < 2:
  quit "need at least 2 windows: increase seconds or shrink windowSec"

echo &"temporal_audit: {patch}  total={seconds}s  window={windowSec}s  N={nWin}"
echo "  win   t(s)     RMS(dB)   centroid(Hz)   fund(Hz)   peak(Hz @ dB)"

var (l, r) = renderPatch(patch, seconds, sr)
let mono = monoMix(l, r)

let winSamples = int(windowSec * float64(sr))
var rmsSeries: seq[float64] = @[]
var centroidSeries: seq[float64] = @[]
var fundSeries: seq[float64] = @[]
var peakHzSeries: seq[float64] = @[]
var peakDbSeries: seq[float64] = @[]

for i in 0 ..< nWin:
  let start = i * winSamples
  let stop = min(start + winSamples, mono.len)
  if stop - start < 1024: break
  let chunk = mono[start ..< stop]
  let summary = analyze(chunk, float64(sr))
  let pkHz = if summary.peaks.len > 0: summary.peaks[0].freqHz else: 0.0
  let pkDb = if summary.peaks.len > 0: summary.peaks[0].magDb else: -240.0
  rmsSeries.add summary.rmsDb
  centroidSeries.add summary.centroidHz
  fundSeries.add summary.fundamentalHz
  peakHzSeries.add pkHz
  peakDbSeries.add pkDb
  let t = float64(i) * windowSec
  echo &"  {i:>3}  {t:>5.2f}    {summary.rmsDb:>7.2f}    {summary.centroidHz:>9.1f}    {summary.fundamentalHz:>7.1f}    {pkHz:>7.1f} @ {pkDb:>6.2f}"

echo ""
echo &"aggregate over {nWin} windows:"
echo &"  RMS dB           mean={mean(rmsSeries):>7.2f}   stddev={stddev(rmsSeries):>6.2f}"
echo &"  centroid Hz      mean={mean(centroidSeries):>7.1f}   stddev={stddev(centroidSeries):>6.1f}"
echo &"  fundamental Hz   mean={mean(fundSeries):>7.1f}   stddev={stddev(fundSeries):>6.1f}"
echo &"  dominant peak Hz mean={mean(peakHzSeries):>7.1f}   stddev={stddev(peakHzSeries):>6.1f}"
echo &"  peak dB          mean={mean(peakDbSeries):>7.2f}   stddev={stddev(peakDbSeries):>6.2f}"
