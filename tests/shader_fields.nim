## A field patch compiles to GLSL (`aither shader`, for tools/vis): pairs
## are vec2, a `def rec` is a bounded loop, and what a GPU can't run
## (state, play) is an error that says so.

import std/strutils
import ../shader

proc errorOf(src: string): string =
  try:
    discard toGlsl(src)
    ""
  except CatchableError as e: e.msg

for f in ["fields/waves.aither", "fields/julia.aither", "fields/swirl.aither"]:
  doAssert "void main()" in toGlsl(readFile(f)), f

let j = toGlsl("def rec esc(z, c, n): if n == 0 or magnitude(z) > 2 then n else esc(cmul(z, z) + c, c, n - 1)\n" &
               "esc(pixel(), [0.3, 0.5], 64)\n")
doAssert "for (int it = 0; it < " & $RecLimit in j and "continue;" in j and "break;" in j
doAssert "vec2 z_" in j                                   # z took the pair
doAssert "o = vec4(vec2(" in toGlsl("0.5\n")              # a number fills both sides
doAssert "tab_notes" in toGlsl("let notes = [1, 2, 3]\nnotes[t]\n")

doAssert "no state" in errorOf("$x = 0\n$x\n")
doAssert "play is for sound" in errorOf("play a:\n  1\n")
doAssert "can't nest" in errorOf("def rec f(n): if n == 0 then 0 else 1 + f(n - 1)\nf(3)\n")
doAssert "isn't something" in errorOf("reverb(1, 2, 0.3)\n")
echo "shader_fields ok"
