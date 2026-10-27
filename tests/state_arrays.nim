## State arrays across a hot reload, and the tube written with arrays
## checked against the generated one. (What arrays compute is in
## tests/lang/arrays.aither.)

import std/[strutils]
import ../render, ../ccompile, ../voice

proc mono(src: string; n: int): seq[float64] =
  renderPatchSrc(src, float64(n) / 48000.0).left

# Hot reload: same shape keeps the elements; a new size starts fresh.
proc sarr(rs: seq[Region]; name: string): Region =
  for r in rs:
    if r.typeName == "sarr" and r.owner == "/$" & name: return r
  raise newException(ValueError, "no state array " & name)
let v = newVoice(48000.0)
v.load(("$h = array(3)\n$h[1] = $h[1] + 1\n$h[1]"), 48000.0)
for i in 0 ..< 10: discard v.tick(float64(i) / 48000.0)
v.load(("$h = array(3)\n$h[1] = $h[1] + 1\n$h[1] * 2"), 48000.0)
doAssert v.tick(0.0).l == 22.0, "state array should survive a reload"
let regs = compile(("$h = array(3)\n$h[1]"), "", 48000.0).regions
doAssert sarr(regs, "h").size == 4, $sarr(regs, "h").size
v.load(("$h = array(5)\n$h[1] = $h[1] + 1\n$h[1]"), 48000.0)
doAssert v.tick(0.0).l == 1.0, "a resized state array should start fresh"

# ---- the tube: 24 one-sample sections, written with arrays ---------------
# Same physics as gen_tube24.py's def (one forward and one backward wave
# per section, Kelly-Lochbaum scattering at each junction, area junctions
# at the glottis and the lips), compared sample by sample. The generated
# version rounds its interpolation weights to 4 decimals, hence the
# tolerance. The array version is the library file patches/lib/tube.aither.
const TubeGen = staticRead("../notes/voice-eval/tube24.aither")
# The array version lives in patches/lib/tube.aither (import "lib/tube.aither").
const TubeArr = staticRead("../patches/lib/tube.aither").replace("def tube24(", "def tube24a(")
const Drive = """
let ph = phasor(110)
let flow = pow(max(0, sin(TAU * ph)), 2) * 0.3
let hiss = noise() * 0.02
"""
const Ah = "flow, hiss, 5, 0.3, 7.5, 0.2, 3.6, 3.53, 3.36, 3.13, 2.92, 2.72, 2.56, 2.49"
let genOut = mono(TubeGen & Drive & "tube24(" & Ah & ")\n", 9600)
let arrOut = mono(TubeArr & Drive & "tube24a(" & Ah & ")\n", 9600)
# The noise is per call site; feed both the same by zeroing it... both use
# one noise() at file level, so they match sample for sample.
var maxDiff = 0.0
var peak = 0.0
for i in 0 ..< genOut.len:
  maxDiff = max(maxDiff, abs(genOut[i] - arrOut[i]))
  peak = max(peak, abs(genOut[i]))
echo "tube24 generated vs arrays: peak ", formatFloat(peak, ffDecimal, 4),
     ", max diff ", formatFloat(maxDiff, ffScientific, 2)
doAssert peak > 0.01, "the tube should sound"
doAssert maxDiff < peak * 0.02, "array tube differs from the generated one: " & $maxDiff
echo "state arrays ok"
