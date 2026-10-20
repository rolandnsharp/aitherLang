## A patch's picture compiles to GLSL (`aither shader`, for tools/vis):
## the sum of its `draw`s. Pairs are vec2, a `def rec` is a bounded loop,
## a draw in a play sees that play's lets, only what the draws use is
## compiled, and what needs memory is an error that names it. The sound
## never sees a draw.

import std/strutils
import ../shader, ../render

proc errorOf(src: string): string =
  try:
    discard toGlsl(src)
    ""
  except CatchableError as e: e.msg

for f in ["fields/waves.aither", "fields/julia.aither", "fields/swirl.aither",
          "fields/bloom.aither", "patches/pulse.aither", "patches/unfold.aither",
          "patches/rain.aither", "patches/orbits.aither"]:
  doAssert "void main()" in toGlsl(readFile(f)), f

let j = toGlsl("def rec esc(z, c, n): if n == 0 or magnitude(z) > 2 then n else esc(cmul(z, z) + c, c, n - 1)\n" &
               "draw esc(pixel(), [0.3, 0.5], 64)\n")
doAssert "for (int it = 0; it < " & $RecLimit in j and "continue;" in j and "break;" in j
doAssert "vec2 z_" in j                                   # z took the pair
doAssert "o = vec4(vec2(0.5), 0.0, 1.0)" in toGlsl("draw 0.5\n")   # a number fills both sides
doAssert "tab_notes" in toGlsl("let notes = [1, 2, 3]\ndraw notes[t]\n")
doAssert "float[](" in toGlsl("let f = 2 + t\nlet a = [f, f * 2, f * 3]\ndraw a[t]\n")   # computed entries
doAssert "+ vec2(0.25)" in toGlsl("draw [1, 0]\ndraw 0.25\n")      # draws add

# One patch: a play's draw sees its lets; the sound's filter isn't
# compiled because no draw uses it; osc takes a shape by name.
let song = "let beat = phasor(2)\n" &
           "play kick:\n  let env = exp(-beat * 14)\n  let s = sin(TAU * phasor(50)) * env |> lpf(300, 0.5)\n" &
           "  draw [env, 0]\n  s\n" &
           "play lead:\n  draw osc(saw, 3) * 0.1\n  osc(saw, 440) * 0.1\n"
let g = toGlsl(song)
doAssert "fract((2.0) * (uT - uStart))" in g and "a_saw" in g and "lpf" notin g
doAssert "only the sound" in errorOf("play a:\n  let f = 1 |> lpf(300, 0.5)\n  draw f\n  f\n")
doAssert "the sound's state" in errorOf("$x = 0\ndraw $x\n")

# A picture's $state: a field a call site, read here, at a point, or by side.
let tr = toGlsl("def trail(p, new):\n  $g = [0, 0]\n  $g = $g[p * 0.9] * 0.9 + new\n  $g[0] + $g\n" &
                "draw trail(pixel(), 0.1)\ndraw trail(pixel(), 0.2)\n")
doAssert "// fields: 2" in tr and "a_field(uF0, " in tr and "oF1 = " in tr and "fv_0.x" in tr
doAssert "starts from" in errorOf("def f(p):\n  $g = p\n  $g\ndraw f(pixel())\n")
doAssert "at most 3" in errorOf("def f(p):\n  $a = 0\n  $b = 0\n  $c = 0\n  $d = 0\n  $a\ndraw f(pixel())\n")
doAssert "only reads" in errorOf("def f(p):\n  $g = [0, 0]\n  $g[p] = 1\n  $g\ndraw f(pixel())\n")
doAssert "never changes" in errorOf("let f = 2 + t\ndraw phasor(f)\n")      # it would drift
doAssert "(uT - uStart)" in toGlsl("let fs = [2, 3]\nlet f = fs[1] * 2\ndef tick(g): phasor(g)\ndraw tick(f)\n")
doAssert "uStart" in toGlsl("let pos = t - start_t\ndraw pos\n")
doAssert "last() is gone" in errorOf("draw last(pixel())\n")
doAssert "nothing to draw" in errorOf("sin(1)\n")
doAssert "can't nest" in errorOf("def rec f(n): if n == 0 then 0 else 1 + f(n - 1)\ndraw f(3)\n")

# The sound is the same with or without its draws.
let a = renderPatchSrc("play k:\n  let e = exp(-phasor(2) * 9)\n  draw [e, 0]\n  sin(TAU * phasor(60)) * e\n", 0.5)
let b = renderPatchSrc("play k:\n  let e = exp(-phasor(2) * 9)\n  sin(TAU * phasor(60)) * e\n", 0.5)
doAssert a.left == b.left and a.right == b.right
echo "shader_fields ok"
