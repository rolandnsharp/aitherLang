## Hoisting: a native whose coefficient arguments can't change from
## sample to sample gets them computed once (hoist_init) and steps with
## the `_c` form; a modulated one keeps the plain call. Either way the
## bits are the same: the same arithmetic, once instead of every sample.

import std/[os, strutils]
import ../parser, ../voice, ../codegen

const Patch = """
let cut = 1200
def warm(x, hz = 800): lp1(x, hz)
play a:
  let s = saw(TAU * phasor(55))
  warm(s) * 0.1 + lpf(s, cut, 0.4) * 0.1 + hp1(s, 2000) * 0.05
play b:
  let m = 800 + 400 * sin(TAU * t)
  lp1(saw(TAU * phasor(110 + 5 * sin(TAU * 0.5 * t))), m) * 0.1
play c:
  (a + b) |> reverb(2, 0.3)

c
"""

proc csrcOf(src: string): string = generate(parseProgram(src), "", 48000.0).csrc

proc render(src: string; n = 2400): seq[float64] =
  let v = newVoice(48000.0)
  v.load(parseProgram(src), 48000.0)
  for i in 0 ..< n:
    let s = v.tick(float64(i) / 48000.0)
    result.add s.l

putEnv("AITHER_HOIST", "1")
let onAll = csrcOf(Patch)
# Past the extern declarations: hoist_init, then the per-sample code.
let on = onAll[onAll.find("static void hoist_init") .. ^1]
# The constant ones are hoisted: lp1 via the def's default, the SVF with
# a let-bound cutoff, hp1 with a literal, reverb's decay, both constant
# phasors. The modulated lp1 and phasor in `b` are not.
doAssert on.count("n_lp1_coef(") == 2, on          # 800 and 2000
doAssert on.count("n_lp1_c(") == 1 and on.count("n_hp1_c(") == 1
doAssert on.count("n_svf_coef(") == 1 and on.count("n_lpf_c(") == 1
# `a + b` is a pair (parts are), so reverb is split per channel: two
# steps, one hoisted set of coefficients.
doAssert on.count("n_reverb_coef(") == 1 and on.count("n_reverb_c(") == 2, on
doAssert on.count("n_lp1((DspState") == 1, "the modulated lp1 stays plain"
doAssert on.count("hoist_init(s);") == 2, "both entry points init"
let aOn = render(Patch)

putEnv("AITHER_HOIST", "0")
let off = csrcOf(Patch)
doAssert off.count("n_lp1_coef(") == 1 and off.count("_c((DspState") == 0  # the extern only
let aOff = render(Patch)
putEnv("AITHER_HOIST", "1")

doAssert aOn.len == aOff.len
var peak = 0.0
for i in 0 ..< aOn.len:
  peak = max(peak, abs(aOn[i]))
  doAssert aOn[i] == aOff[i], "sample " & $i & ": " & $aOn[i] & " vs " & $aOff[i]
doAssert peak > 0.001, "the patch made no sound"

# Knobs: midi_cc is read once a block, so a name bound to one, a def
# parameter given one, and costly pure math over them are invariant.
# The per-partial exp of a knob-driven spectral morph moves to
# hoist_init, and the bits stay the same.
const Knobs = """
let morph = midi_cc(20)
let u = clamp(morph * 2, 0, 1)
def partial(n, u): exp(log(n) * (1 - u) + log(n * n) * u)
let cut = 400 + 3000 * midi_cc(21)
sum(4, n => sin(TAU * phasor(110 * partial(n, u))) / n) * 0.1 |> lp1(cut)
"""
putEnv("AITHER_HOIST", "1")
let kOn = csrcOf(Knobs)
let at = kOn.find("static void hoist_init(")
let hoistInit = kOn[at .. kOn.find("\n}", at)]
doAssert hoistInit.count("exp(") >= 4 and "n_midi_cc" in hoistInit,
  "the knob-driven exps are computed in hoist_init:\n" & hoistInit
let k1 = render(Knobs)
putEnv("AITHER_HOIST", "0")
let k0 = render(Knobs)
putEnv("AITHER_HOIST", "1")
for i in 0 ..< k1.len:
  doAssert k1[i] == k0[i], "knob sample " & $i & ": " & $k1[i] & " vs " & $k0[i]

echo "hoist ok"
