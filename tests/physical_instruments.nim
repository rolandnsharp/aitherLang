## Physical-instrument library: pluck_string, bowed_string, struck_bar,
## tuning_fork. Each pins a property that justifies the physics
## paradigm — additive can't fake these without elaborate envelope
## tricks that miss the character.
##
## Invariants:
##   1. All four compile and tick NaN-free for 1 second @ 220 Hz.
##   2. pluck_string is actually pitched at the requested freq —
##      autocorrelation peak lands near lag = sr / freq.
##   3. struck_bar's mode-dependent damping is real: amplitude shortly
##      after the strike is much higher than at 1.5 s.
##   4. bowed_string's continuous excitation produces sustained output —
##      RMS after a 0.5 s settle is non-trivial.

import std/[math]
import ../parser, ../voice, ../codegen

const Stdlib = staticRead("../stdlib.aither")

proc prog(src: string): Node =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(src)
  setSource(userAst, "patches/test.aither")
  Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

proc renderMono(src: string; nSamples: int): seq[float64] =
  let v = newVoice(48000.0)
  v.load(prog(src), 48000.0)
  result.setLen(nSamples)
  for i in 0..<nSamples:
    let s = v.tick(float64(i) / 48000.0)
    doAssert s.l == s.l, "NaN at sample " & $i & " in:\n" & src
    result[i] = s.l

# --- 1. NaN-free for 1 second on a strike-at-fundamental excitation.
# Smoke test that every def survives an audio-rate run with the kind of
# input it's documented to accept.
block ticksClean:
  for src in [
      "tuning_fork(impulse(2) * 50, 220) * 0.05",
      "pluck_string(noise() * impulse(2) * 0.5, 220, 0.7)",
      "bowed_string(noise() * 0.3, 220) * 0.1",
      "struck_bar(impulse(2) * 30, 220) * 0.02",
    ]:
    let buf = renderMono(src, 48000)
    var energy = 0.0
    for v in buf: energy += v * v
    doAssert energy > 0.0, "no signal from: " & src

# --- 2. pluck_string is pitched at the requested freq.
# Autocorrelation over a settled window: the first prominent peak after
# lag 0 should sit near `sr / freq`. Karplus-Strong's exact pitch drifts
# slightly from the ideal because of LP-filter group delay and the
# one-sample loop offset, so the test allows a ±20-sample window around
# the ideal lag (about ±10% of the period).
block pluckIsPitched:
  const Freq = 220.0
  const SR = 48000.0
  let buf = renderMono(
    "pluck_string(noise() * impulse(2) * 0.5, " & $Freq & ", 0.7)",
    int(1.0 * SR))
  # Skip first 0.4 s — initial noise burst still dominates before the
  # LP-feedback loop has carved out the periodic shape.
  let start = int(0.4 * SR)
  let segLen = buf.len - start - 300
  let idealLag = int(SR / Freq)         # 218 samples
  var bestLag = 0
  var bestCorr = -1e9
  for lag in (idealLag - 20)..(idealLag + 20):
    var c = 0.0
    for i in 0..<segLen:
      c += buf[start + i] * buf[start + i + lag]
    if c > bestCorr:
      bestCorr = c
      bestLag = lag
  doAssert abs(bestLag - idealLag) <= 18,
    "pluck_string at " & $Freq & " Hz: autocorrelation peak at lag " &
    $bestLag & ", expected near " & $idealLag
  # And the autocorrelation at the best lag should be clearly positive
  # (a periodic signal correlates with itself at the period).
  doAssert bestCorr > 0.0,
    "pluck_string: autocorr at expected lag is non-positive (" &
    $bestCorr & ") — string is not periodic"

# --- 3. struck_bar: bright transient mellows to fundamental ring.
# Strike at sample 0 via a `t < dt` pulse (one-time impulse, no period
# alignment to worry about). Compare RMS in an early window (0..150 ms,
# all modes ringing) against a late window centred on 1.5 s. The late
# window should be markedly quieter — the high modes have died and only
# the lightly-damped fundamental remains.
block barModeDecay:
  const SR = 48000.0
  let buf = renderMono(
    "struck_bar(if t < dt then 50 else 0, 220) * 0.05",
    int(1.7 * SR))
  proc rms(buf: seq[float64]; lo, hi: int): float64 =
    var e = 0.0
    for i in lo..<hi: e += buf[i] * buf[i]
    sqrt(e / float64(hi - lo))
  let earlyRms = rms(buf, 0, int(0.15 * SR))
  let lateRms  = rms(buf, int(1.45 * SR), int(1.55 * SR))
  doAssert earlyRms > 0.0, "struck_bar produced no early signal"
  doAssert earlyRms > lateRms * 3.0,
    "struck_bar: mode-dependent decay should make early window much " &
    "louder than late; earlyRms=" & $earlyRms & " lateRms=" & $lateRms

# --- 4. bowed_string sustains under continuous excitation.
# RMS in a window 0.5..1.0 s should be comparable to a window 0.0..0.5 s
# (steady-state, not a decaying transient). Pin both: non-zero and
# within a 3x band of each other.
block bowSustain:
  const SR = 48000.0
  let buf = renderMono(
    "bowed_string(noise() * 0.3, 220) * 0.1",
    int(1.0 * SR))
  proc rms(buf: seq[float64]; lo, hi: int): float64 =
    var e = 0.0
    for i in lo..<hi: e += buf[i] * buf[i]
    sqrt(e / float64(hi - lo))
  let earlyRms = rms(buf, 0, int(0.5 * SR))
  let lateRms  = rms(buf, int(0.5 * SR), int(1.0 * SR))
  doAssert lateRms > 0.001,
    "bowed_string: late window silent (" & $lateRms & "), no sustain"
  doAssert lateRms > 0.3 * earlyRms,
    "bowed_string: late RMS (" & $lateRms & ") collapsed vs early (" &
    $earlyRms & ") — should be steady-state under continuous bow"
  doAssert lateRms < 3.0 * earlyRms,
    "bowed_string: late RMS (" & $lateRms & ") way bigger than early (" &
    $earlyRms & ") — system is blowing up, not steady"

# --- 5. tuning_fork's pitch is exact. Single-mode HO has no detune
# from filter delays etc. — easier to pin tightly.
block tuningForkPitch:
  const Freq = 220.0
  const SR = 48000.0
  let buf = renderMono(
    "tuning_fork(if t < dt then 50 else 0, " & $Freq & ") * 0.1",
    int(0.4 * SR))
  let start = int(0.05 * SR)            # past the initial jolt
  let segLen = buf.len - start - 300
  let idealLag = int(SR / Freq)
  var bestLag = 0
  var bestCorr = -1e9
  for lag in (idealLag - 10)..(idealLag + 10):
    var c = 0.0
    for i in 0..<segLen:
      c += buf[start + i] * buf[start + i + lag]
    if c > bestCorr:
      bestCorr = c
      bestLag = lag
  doAssert abs(bestLag - idealLag) <= 3,
    "tuning_fork at " & $Freq & " Hz: autocorr peak at lag " & $bestLag &
    ", expected " & $idealLag

echo "physical_instruments ok"
