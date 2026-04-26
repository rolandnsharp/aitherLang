## Geometric/spectral operations on float pairs.
##
## Two-arg builtins return scalars (magnitude, phase). Four pair-returning
## builtins (cmul, cdiv, cscale, rotate) and one stateful one (analytic)
## piggy-back on aither's existing 2-element-pair plumbing — composers
## destructure with `out[0]` (real) / `out[1]` (imag), the same way they
## index a stereo-returning def.
##
## The mandatory invariant: `cmul((0, 1), (0, 1)) == (-1, 0)`. If that
## test ever fails, the implementation has slid into split-complex
## (where p² = +1) and the whole motivation evaporates.

import std/[math, strutils]
import ../parser, ../voice

const Stdlib = staticRead("../stdlib.aither")

proc prog(src: string): Node =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(src)
  setSource(userAst, "patches/test.aither")
  Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

proc evalLR(src: string): tuple[l, r: float64] =
  let v = newVoice(48000.0)
  v.load(prog(src), 48000.0)
  v.tick(0.0)

proc evalScalar(src: string): float64 = evalLR(src).l

# --- 1. The i² = -1 invariant. cmul((0, 1), (0, 1)) must be (-1, 0).
# A wrong sign here means split-complex (p² = +1), a sibling algebra
# with completely different group structure — frequency shifting and
# Mandelbrot iteration both break under it.
block iSquaredMinusOne:
  let p = evalLR("""
let z = cmul(0, 1, 0, 1)
[z[0], z[1]]
""")
  doAssert abs(p.l - (-1.0)) < 1e-12,
    "cmul((0,1),(0,1)) real should be -1.0, got " & $p.l
  doAssert abs(p.r) < 1e-12,
    "cmul((0,1),(0,1)) imag should be 0.0, got " & $p.r

# --- 2. 1 is the multiplicative identity: cmul((1, 0), (a, b)) == (a, b).
block oneIsIdentity:
  let p = evalLR("""
let z = cmul(1, 0, 0.7, -0.4)
[z[0], z[1]]
""")
  doAssert abs(p.l - 0.7) < 1e-12, "real " & $p.l
  doAssert abs(p.r - (-0.4)) < 1e-12, "imag " & $p.r

# --- 3. magnitude(rotate(1, 0, omega)) ≈ 1 for any omega. Rotation
# preserves magnitude, full stop.
block rotationPreservesMagnitude:
  for k in 0 ..< 8:
    let omega = float64(k) * (PI / 4.0)
    let src = """
let r = rotate(1, 0, """ & $omega & """)
magnitude(r[0], r[1])
"""
    let m = evalScalar(src)
    doAssert abs(m - 1.0) < 1e-12,
      "magnitude after rotate by " & $omega & " = " & $m

# --- 4. phase(rotate(1, 0, omega)) ≈ omega (mod 2π).
block phaseAfterRotate:
  for omega in [0.0, 0.1, 1.0, -0.7, 1.234]:
    let src = """
let r = rotate(1, 0, """ & $omega & """)
phase(r[0], r[1])
"""
    let p = evalScalar(src)
    # atan2 returns -π..π; normalise omega the same way.
    var want = omega
    while want > PI: want -= 2.0 * PI
    while want < -PI: want += 2.0 * PI
    doAssert abs(p - want) < 1e-9,
      "phase(rotate(1,0," & $omega & ")) = " & $p & " want " & $want

# --- 5. cdiv: (a*b) / b == a for non-zero b.
block divInvertsMul:
  # (3, 2) * (1.5, -0.4) = (3*1.5 - 2*(-0.4), 3*(-0.4) + 2*1.5) = (5.3, 1.8)
  # (5.3, 1.8) / (1.5, -0.4) should give (3, 2) back.
  let p = evalLR("""
let prod = cmul(3, 2, 1.5, -0.4)
let back = cdiv(prod[0], prod[1], 1.5, -0.4)
[back[0], back[1]]
""")
  doAssert abs(p.l - 3.0) < 1e-9, "cdiv recovered re " & $p.l
  doAssert abs(p.r - 2.0) < 1e-9, "cdiv recovered im " & $p.r

# --- 6. cdiv by (0, 0) yields (0, 0) (aither's div-by-zero convention,
# extended to the pair).
block cdivByZero:
  let p = evalLR("""
let z = cdiv(1, 1, 0, 0)
[z[0], z[1]]
""")
  doAssert p.l == 0.0 and p.r == 0.0,
    "cdiv by zero should give (0, 0), got (" & $p.l & ", " & $p.r & ")"

# --- 7. cscale broadcasts a scalar over a pair.
block cscaleSimple:
  let p = evalLR("""
let z = cscale(2.5, 1, -3)
[z[0], z[1]]
""")
  doAssert abs(p.l - 2.5) < 1e-12, "cscale re " & $p.l
  doAssert abs(p.r - (-7.5)) < 1e-12, "cscale im " & $p.r

# --- 8. Pair calls passed to a scalar-only consumer error out. The
# canonical user mistake is `magnitude(cmul(...))` (single-arg) — the
# scalar emitter has no slot for a two-component value, and the brief
# explicitly forbids overloading `+`/`*` to mean complex algebra. The
# codegen must refuse it with a hint about the let-binding pattern.
# (Note: `pair + scalar` is *not* an error — that goes through the
# existing stereo-broadcast path and produces a component-wise pair,
# which is consistent with how aither already treats every pair value.)
block scalarMisuseErrors:
  var msg = ""
  try:
    discard evalScalar("magnitude(cmul(1, 0, 1, 0))")
  except CatchableError as e:
    msg = e.msg
  doAssert msg.len > 0,
    "magnitude(cmul(...)) with one arg should error, got success"
  doAssert "magnitude" in msg or "2 scalars" in msg or "pair" in msg.toLowerAscii(),
    "error should mention magnitude/scalars/pair, got: " & msg

# --- 9. analytic(signal) returns (signal-ish, hilbert(signal)).
# The pair's magnitude is the instantaneous envelope. For a steady sine
# at 1 kHz, the envelope should be roughly constant equal to the input
# amplitude after a few ms of settling.
block analyticEnvelope:
  const SR = 48000.0
  const Freq = 1000.0
  let v = newVoice(SR)
  v.load(prog("""
play out:
  let x = sin(TAU * phasor(""" & $Freq & """)) * 0.5
  let z = analytic(x)
  [z[0], z[1]]
out
"""), SR)
  let nSamples = int(0.1 * SR)
  var maxEnv = 0.0
  var minEnv = 1e9
  for i in 0 ..< nSamples:
    let s = v.tick(float64(i) / SR)
    if i > int(0.02 * SR):                # skip first 20 ms transient
      let env = sqrt(s.l * s.l + s.r * s.r)
      if env > maxEnv: maxEnv = env
      if env < minEnv: minEnv = env
  # The envelope should be ≈ 0.5 with small ripple. Allow generous bands
  # because the IIR allpass network has sub-dB response variation.
  doAssert maxEnv < 0.65 and minEnv > 0.35,
    "analytic envelope of 1 kHz sine: max=" & $maxEnv & " min=" & $minEnv

# --- 10. freq_shift moves a tone's frequency by `hz`.
# A 440 Hz tone shifted by +200 Hz must show its dominant peak near
# 640 Hz (not 440, not at 440 + integer harmonics).
block freqShiftPeak:
  const SR = 48000.0
  const F = 440.0
  const Shift = 200.0
  let v = newVoice(SR)
  v.load(prog("""
let x = sin(TAU * phasor(""" & $F & """)) * 0.5
freq_shift(x, """ & $Shift & """)
"""), SR)
  # Render 0.5s, skip first 50 ms (allpass transient).
  let nSamples = int(0.5 * SR)
  var buf = newSeq[float64](nSamples)
  for i in 0 ..< nSamples:
    buf[i] = v.tick(float64(i) / SR).l
  let skip = int(0.05 * SR)
  let segLen = nSamples - skip
  # Goertzel-ish narrowband detection at 440 Hz vs 640 Hz.
  proc bandPower(b: openArray[float64]; lo, hi: int; freq: float64): float64 =
    var re, im = 0.0
    for i in lo ..< hi:
      let phase = TAU * freq * float64(i) / SR
      re += b[i] * cos(phase)
      im += b[i] * sin(phase)
    re * re + im * im
  let pAt440 = bandPower(buf, skip, nSamples, 440.0)
  let pAt640 = bandPower(buf, skip, nSamples, 640.0)
  # Shifted band should dominate by at least 20 dB. With clean SSB the
  # leakage at the original pitch is the unwanted-sideband leak — for
  # the 4-stage Hilbert it's ~30-40 dB down.
  doAssert pAt640 > pAt440 * 100.0,
    "freq_shift: power at 640 Hz (" & $pAt640 & ") should dominate over 440 Hz (" & $pAt440 & ")"

echo "pair_ops ok"
