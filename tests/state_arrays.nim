## State arrays: `$f = array(N)` (elements that persist, in the pool),
## `each(N, i => $f[i] = ...)` (one simultaneous step: every body reads
## the arrays as they were before the step), `let a = [p, q, r]` with
## runtime values and `let a = array(N)` (this sample's scratch), and
## `len`. Spatial physics (tubes, strings, membranes) is a few lines
## instead of generated code: tube24 below is checked against the
## 219-line version `notes/voice-eval/gen_tube24.py` wrote.

import std/[math, strutils, os]
import ../render, ../parser, ../voice, ../codegen

proc mono(src: string; n: int): seq[float64] =
  renderPatchSrc(src, float64(n) / 48000.0).left

proc errOf(src: string): string =
  try:
    discard renderPatchSrc(src, 0.001)
    ""
  except CatchableError as e: e.msg

# ---- local arrays: runtime values, cycling index, len -------------------
let la = mono("let a = [t * 0 + 7, 2, 3]\n" &
              "let b = array(4, 0.5)\n" &
              "b[2] = 1\n" &
              "a[1] + a[4] + a[-1] + b[0] + b[2] + len(a) * 100 + len(b) * 1000", 2)
# a[1]=2, a[4] cycles to a[1]=2, a[-1]=3, b[0]=0.5, b[2]=1, len 3 and 4.
doAssert la[0] == 2 + 2 + 3 + 0.5 + 1 + 300 + 4000, $la[0]

# A local array inside a def and a play, written with each.
let lp = mono("def tri3(x):\n  let a = array(3)\n  each(3, i => a[i] = x * (i + 1))\n  a[0] + a[1] + a[2]\n" &
              "play p:\n  let q = [1, 2, 3, 4 + t * 0]\n  q[3] = 10\n  tri3(2) + q[3]\n", 2)
doAssert lp[0] == 12 + 10, $lp[0]

# ---- state arrays in a def: persist across samples, 0 past the ends ----
let cnt = mono("def cnt():\n  $c = array(3, 1)\n  $c[0] = $c[0] + 1\n  $c[0] + $c[5] + $c[-1] + len($c) * 100\n" &
               "cnt()", 5)
for i in 0 ..< 5:
  doAssert cnt[i] == float64(i + 2) + 300, "tick " & $i & ": " & $cnt[i]

# Two call sites of the same def have their own arrays.
let two = mono("def cnt():\n  $c = array(2)\n  $c[1] = $c[1] + 1\n  $c[1]\ncnt() * 10 + cnt()", 3)
doAssert two[2] == 33, $two[2]

# ---- each is one simultaneous step --------------------------------------
# A shift register stepped in ascending order: with ordinary sequential
# stores the impulse would race to the end in one sample; one step per
# sample is the point.
let shift = mono("$f = array(4)\n" &
                 "let out = $f[3]\n" &
                 "each(3, i => $f[i + 1] = $f[i])\n" &
                 "$f[0] = if t < dt then 1 else 0\n" &
                 "out", 8)
doAssert shift[4] == 1.0 and shift.sum == 1.0, $shift
# Stores outside an each land at once, and a store inside a sum body works.
let seqs = mono("$g = array(3)\n$g[0] = 5\n$g[1] = $g[0] * 2\n$g[1] + sum(3, n => $g[n - 1] = n)", 2)
doAssert seqs[0] == 10.0, $seqs
# Two arrays in one each, like a waveguide's forward and backward waves.
let swap = mono("$a = array(2, 1)\n$b = array(2, 2)\n" &
                "each(2, i =>\n  $a[i] = $b[i]\n  $b[i] = $a[i])\n" &
                "$a[0] * 10 + $b[1]", 3)
# The final expression runs after the step: 1,2 -> 2,1 -> 1,2. A
# sequential each would read the new $a[i] and give 22 every sample.
doAssert swap[0] == 21 and swap[1] == 12 and swap[2] == 21, $swap

# ---- file-level state arrays: read from plays, kept across hot reloads --
const Top = """
$h = array(3)
let drive = if t < dt then 1 else 0
play a:
  each(2, i => $h[i + 1] = $h[i])
  $h[0] = drive
  $h[2]
play b:
  $h[1] * 0.5
a + b
"""
let topOut = mono(Top, 6)
# The step runs before the reads: $h[1] shows at sample 1 (b, x0.5),
# $h[2] at sample 2 (a).
doAssert topOut[1] == 0.5 and topOut[2] == 1.0 and topOut.sum == 1.5, $topOut

# Hot reload: same shape keeps the elements; a new size starts fresh.
const Stdlib = staticRead("../stdlib.aither")
proc prog(src: string): Node =
  Node(kind: nkBlock, kids: parseProgram(Stdlib).kids & parseProgram(src).kids)
proc sarr(rs: seq[Region]; name: string): Region =
  for r in rs:
    if r.typeName == "sarr" and r.owner == "/$" & name: return r
  raise newException(ValueError, "no state array " & name)
let v = newVoice(48000.0)
v.load(prog("$h = array(3)\n$h[1] = $h[1] + 1\n$h[1]"), 48000.0)
for i in 0 ..< 10: discard v.tick(float64(i) / 48000.0)
v.load(prog("$h = array(3)\n$h[1] = $h[1] + 1\n$h[1] * 2"), 48000.0)
doAssert v.tick(0.0).l == 22.0, "state array should survive a reload"
let (_, _, _, regs) = generate(prog("$h = array(3)\n$h[1]"), "", 48000.0)
doAssert sarr(regs, "h").size == 4, $sarr(regs, "h").size
v.load(prog("$h = array(5)\n$h[1] = $h[1] + 1\n$h[1]"), 48000.0)
doAssert v.tick(0.0).l == 1.0, "a resized state array should start fresh"

# ---- errors say what to do ----------------------------------------------
doAssert "compile-time integer" in errOf("let n = t\n$f = array(n)\n$f[0]"), errOf("let n = t\n$f = array(n)\n$f[0]")
doAssert "declares an array" in errOf("array(3) + 1")
doAssert "constant table" in errOf("let c = [1, 2, 3]\nc[0] = 5\nc[0]")

# ---- the tube: 24 one-sample sections, written with arrays ---------------
# Same physics as gen_tube24.py's def (one forward and one backward wave
# per section, Kelly-Lochbaum scattering at each junction, area junctions
# at the glottis and the lips), compared sample by sample. The generated
# version rounds its interpolation weights to 4 decimals, hence the
# tolerance.
const TubeGen = staticRead("../notes/voice-eval/tube24.aither")
const TubeArr = """
def tube24a(src, noiz, place, deg, place2, deg2, p1, p2, p3, p4, p5, p6, p7, p8):
  # areas: 8 control points glottis -> lips, interpolated to 24 sections,
  # narrowed by two articulators (place 1..8, deg 0 open .. 1 closed)
  let p = [p1, p2, p3, p4, p5, p6, p7, p8]
  let s = array(24)
  each(24, j =>
    let x = j * 7 / 23 + 1
    let lo = floor(x)
    let fr = x - lo
    let base = p[lo - 1] * (1 - fr) + p[min(lo, 7)] * fr
    s[j] = max(0.02, base * (1 - deg * exp(-(x - place) * (x - place) * 3))
                          * (1 - deg2 * exp(-(x - place2) * (x - place2) * 3))))
  let k = array(23)
  let nz = array(23)
  each(23, j =>
    let x = j * 7 / 23 + 1
    k[j] = (s[j] - s[j + 1]) / (s[j] + s[j + 1])
    nz[j] = noiz * exp(-(x - place) * (x - place) * 6))
  $f = array(24)
  $b = array(24)
  let kG = (s[0] - 0.15) / (s[0] + 0.15)
  let g0 = src + kG * $b[0]
  let kL = (s[23] - 25) / (s[23] + 25)
  let lipR = lp1($f[23], 19580)
  let bwL = kL * lipR
  let out = $f[23] + bwL
  each(23, j =>
    $f[j + 1] = clamp(((1 + k[j]) * $f[j] - k[j] * $b[j + 1] + nz[j]) * 0.996, -50, 50)
    $b[j] = clamp((k[j] * $f[j] + (1 - k[j]) * $b[j + 1]) * 0.996, -50, 50))
  $f[0] = clamp(g0 * 0.996, -50, 50)
  $b[23] = clamp(bwL * 0.996, -50, 50)
  out - prev(out)
"""
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
