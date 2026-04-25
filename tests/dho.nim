## dho(force, freq, damp) — universal second-order oscillator. Three
## invariants pin the math: (1) struck DHO rings at the requested
## frequency and decays at the predicted timescale, (2) under a
## constant force at critical damping, x approaches force/k, (3)
## critical damping with a step input rises monotonically with no
## overshoot.

import std/[math]
import ../parser, ../voice

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

# --- 1. Impulse response: pitch + decay envelope.
# A struck DHO at 440 Hz with damp=0.01 should ring near 440 Hz and its
# envelope should decay with time constant tau = 1/(damp*omega) ≈ 36 ms.
# Use a one-shot strike at t=0 (`if t < dt`). Force is acceleration —
# peak amplitude ≈ force/(sr*omega), so a 440 Hz impulse needs force on
# the order of 1e8 to ring at unit-ish amplitude.
block impulseRingdown:
  const Freq = 440.0
  const SR = 48000.0
  const Damp = 0.01
  let omega = 2.0 * PI * Freq
  let tauExpected = 1.0 / (Damp * omega)        # ≈ 0.0362 s
  let buf = renderMono(
    "dho(if t < dt then 100000000 else 0, " & $Freq & ", " & $Damp & ")",
    int(0.4 * SR))

  # Sanity: there must actually be signal to autocorrelate.
  var sigEnergy = 0.0
  for v in buf: sigEnergy += v * v
  doAssert sigEnergy > 0.0, "dho impulse produced no signal"

  # Pitch via autocorrelation in a settled window (skip first 5 ms of
  # the strike transient, leave 300 samples of slack at the tail).
  let start = int(0.005 * SR)
  let segLen = buf.len - start - 300
  let idealLag = int(SR / Freq)                 # 109 samples
  var bestLag = 0
  var bestCorr = -1e9
  for lag in (idealLag - 6)..(idealLag + 6):
    var c = 0.0
    for i in 0..<segLen:
      c += buf[start + i] * buf[start + i + lag]
    if c > bestCorr:
      bestCorr = c
      bestLag = lag
  doAssert abs(bestLag - idealLag) <= 3,
    "dho impulse: autocorr peak at lag " & $bestLag &
    ", expected " & $idealLag
  doAssert bestCorr > 0.0,
    "dho impulse: autocorr non-positive — not periodic"

  # Decay envelope. Compare RMS in two windows (lateWindow centred at
  # ~2 tau later than earlyWindow). Ratio should match exp(-2) ≈ 0.135
  # within a generous band — semi-implicit Euler at omega*dt ≈ 0.058
  # has small but measurable damping/frequency shift.
  proc rms(b: seq[float64]; lo, hi: int): float64 =
    var e = 0.0
    for i in lo..<hi: e += b[i] * b[i]
    sqrt(e / float64(hi - lo))
  let winLen = int(0.005 * SR)                  # 5 ms windows
  let earlyLo = int(0.010 * SR)
  let lateLo  = int(0.010 * SR + 2.0 * tauExpected * SR)
  let earlyRms = rms(buf, earlyLo, earlyLo + winLen)
  let lateRms  = rms(buf, lateLo,  lateLo  + winLen)
  doAssert earlyRms > 1e-4, "dho impulse: no audible ringdown"
  let ratio = lateRms / earlyRms
  let expected = exp(-2.0)                      # ≈ 0.135
  doAssert ratio > expected * 0.5 and ratio < expected * 1.6,
    "dho impulse: decay ratio " & $ratio &
    " not near exp(-2) ≈ 0.135 (early=" & $earlyRms &
    " late=" & $lateRms & ")"

# --- 2. Steady-state under constant force at critical damping equals
# force/k where k = omega^2. Critical damping means x rises smoothly to
# the equilibrium without oscillation; after several time constants the
# integrator should sit within a few percent of force/k.
block steadyStateUnderForce:
  const SR = 48000.0
  const Freq = 10.0                             # tame integration, fast settle
  const Force = 100.0
  let omega = 2.0 * PI * Freq
  let k = omega * omega
  let xExpected = Force / k                     # ≈ 0.02533
  # Settling time at critical damping: well within 1 second for f=10 Hz.
  let buf = renderMono(
    "dho(" & $Force & ", " & $Freq & ", 1.0)",
    int(1.0 * SR))
  let final = buf[^1]
  doAssert abs(final - xExpected) < xExpected * 0.05,
    "dho steady-state: got " & $final & ", expected " & $xExpected

# --- 3. Critical damping under a step input is monotonically rising
# (no overshoot). Sample-by-sample non-decreasing within a tiny epsilon
# (numerical noise, semi-implicit integration).
block criticalDampingNoOvershoot:
  const SR = 48000.0
  const Freq = 10.0
  const Force = 100.0
  let omega = 2.0 * PI * Freq
  let k = omega * omega
  let xExpected = Force / k
  let buf = renderMono(
    "dho(" & $Force & ", " & $Freq & ", 1.0)",
    int(0.5 * SR))
  var prev = buf[0]
  var maxVal = buf[0]
  var minDelta = 0.0
  for v in buf:
    if v > maxVal: maxVal = v
    let d = v - prev
    if d < minDelta: minDelta = d
    prev = v
  doAssert maxVal <= xExpected * 1.001,
    "dho critical: overshoot — peak " & $maxVal &
    " > steady-state " & $xExpected
  doAssert minDelta > -1e-9,
    "dho critical: non-monotonic step (worst dip = " & $minDelta & ")"

# --- 4. dho_v exposes velocity: dx of the same equation. Under a
# step force at low damping, dx oscillates around 0 with the same
# period as x. Verify it's not constant zero and ticks NaN-free.
block velocityVariant:
  const SR = 48000.0
  let buf = renderMono(
    "dho_v(if t < dt then 100000000 else 0, 440, 0.01)",
    int(0.1 * SR))
  var maxAbs = 0.0
  for v in buf:
    if abs(v) > maxAbs: maxAbs = abs(v)
  doAssert maxAbs > 100.0,
    "dho_v: velocity output too small (" & $maxAbs & ")"

echo "dho ok"
