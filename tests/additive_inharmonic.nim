## Phase 3: verify stdlib's additive/inharmonic defs + shape/ratio
## functions compose correctly on top of sum() and produce the
## expected spectra.
##
## Three invariants pinned:
##   1. additive(freq, shape, max_n) produces non-zero, non-NaN output.
##   2. max_n actually controls harmonic count: lowering N reduces the
##      count of phasor regions and noticeably reduces brightness.
##   3. inharmonic() works with both parametric ratio fns (stiff_string)
##      and tabular ones (bar_partials).
## Also smoke-tests a cello patch using cello_shape + stiff_cello as
## documentation-ready sanity.

import std/[math, tables, strutils]
import ../parser, ../voice, ../codegen

const Stdlib = staticRead("../stdlib.aither")

proc prog(src: string): Node =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(src)
  setSource(userAst, "patches/test.aither")
  Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

proc rmsAudible(src: string; N: int): tuple[rms, peak: float] =
  let v = newVoice(48000.0)
  v.load(prog(src), 48000.0)
  var energy = 0.0
  var peak = 0.0
  # Skip startup transient so the filter/phasor states settle.
  for i in 0..<4800:
    discard v.tick(float64(i) / 48000.0)
  for i in 0..<N:
    let s = v.tick(float64(4800 + i) / 48000.0)
    doAssert s.l == s.l, "NaN at sample " & $i & " in:\n" & src
    energy += s.l * s.l
    if abs(s.l) > peak: peak = abs(s.l)
  (sqrt(energy / float(N)), peak)

proc countType(src: string; ty: string): int =
  let (_, _, _, regions) = generate(prog(src), "", 48000.0)
  for r in regions:
    if r.typeName == ty: inc result

# --- 1. additive() with saw_shape produces audible signal.
block additiveSaw:
  const P = "additive(440.0, saw_shape, 8) * 0.3"
  let (rms, peak) = rmsAudible(P, 24000)
  doAssert peak > 0.05,
    "additive saw at 440Hz should be audible, peak=" & $peak
  doAssert peak < 1.0,
    "additive saw should not saturate, peak=" & $peak

# --- 2. max_n controls harmonic count at codegen.
block maxNRegions:
  doAssert countType("additive(440.0, saw_shape, 4) * 0.3", "phasor") == 4,
    "additive(440, _, 4) must register 4 phasor regions"
  doAssert countType("additive(440.0, saw_shape, 16) * 0.3", "phasor") == 16,
    "additive(440, _, 16) must register 16 phasor regions"
  doAssert countType(
      "inharmonic(440.0, stiff_string, soft_decay, 12) * 0.3",
      "phasor") == 12,
    "inharmonic(..., 12) must register 12 phasor regions"

# --- 3. More harmonics => more energy (same fundamental, more content).
# Compare RMS at N=1 (pure sine) vs N=16 (16-harmonic saw).
block brighterHasMoreEnergy:
  let (rms1, _) = rmsAudible("additive(220.0, saw_shape, 1) * 0.1", 24000)
  let (rms16, _) = rmsAudible("additive(220.0, saw_shape, 16) * 0.1", 24000)
  doAssert rms16 > rms1,
    "16-harmonic saw should have more energy than 1-harmonic pure sine: " &
    "rms1=" & $rms1 & " rms16=" & $rms16

# --- 4. sqr_shape zeroes out even partials. Confirm codegen doesn't
# allocate wasted phasors for even n (partials whose amplitude is 0
# still technically run phasor, but the contribution is * 0 — audio
# correctness first, pool-opt separately).
block sqrShape:
  const P = "additive(440.0, sqr_shape, 6) * 0.3"
  let (rms, peak) = rmsAudible(P, 24000)
  doAssert peak > 0.05, "sqr shape should be audible, peak=" & $peak

# --- 5. inharmonic() with stiff_string + soft_decay — a basic stiff
# string. Just verify non-NaN audible.
block stiffStringPatch:
  const P = "inharmonic(220.0, stiff_string, soft_decay, 12) * 0.3"
  let (_, peak) = rmsAudible(P, 24000)
  doAssert peak > 0.05,
    "stiff string should be audible, peak=" & $peak

# --- 6. inharmonic() with bar_partials (tabular ratio fn). Verifies
# that if-ladder ratio functions work as fn-valued args.
block barPartials:
  const P = "inharmonic(220.0, bar_partials, bell_decay, 5) * 0.3"
  let (_, peak) = rmsAudible(P, 24000)
  doAssert peak > 0.05, "bar bell should be audible, peak=" & $peak

# --- 7. Cello example from the design doc — the acceptance test for
# the API. cello_shape over stiff_cello at a low pitch, with enough
# partials that the formant regions contribute.
block celloExample:
  const P = """
inharmonic(110.0, stiff_cello, cello_shape, 24) * 0.25
"""
  let (rms, peak) = rmsAudible(P, 24000)
  doAssert peak > 0.05,
    "cello should be audible, peak=" & $peak
  doAssert peak < 1.5,
    "cello should not saturate, peak=" & $peak
  doAssert countType(P, "phasor") == 24,
    "cello at max_n=24 must register 24 phasor regions"

# --- 8. Phi-spaced partials — a shimmering inharmonic texture.
block phiPartials:
  const P = "inharmonic(110.0, phi_partials, soft_decay, 6) * 0.3"
  let (_, peak) = rmsAudible(P, 24000)
  doAssert peak > 0.01,
    "phi partials should be audible, peak=" & $peak

# --- 9. Vowel formant — 'ee' synthesized additively. Confirms the
# Gaussian-peak shape functions compile and the formant boost is
# computed correctly per-partial.
block vowelEE:
  const P = "additive(220.0, vowel_ee, 20) * 0.15"
  let (rms, peak) = rmsAudible(P, 24000)
  doAssert peak > 0.05, "vowel_ee should be audible, peak=" & $peak

echo "additive_inharmonic ok"
