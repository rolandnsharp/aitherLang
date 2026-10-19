## Any call can name its arguments: builtins by the names SPEC gives
## them (`reverb(sig, rt60 = 2, wet = 0.3)`), as defs always could. A
## named call renders bit for bit the same as its positional form.

import std/strutils
import ../render

proc same(a, b: string) =
  let x = renderPatchSrc(a, 0.5)
  let y = renderPatchSrc(b, 0.5)
  doAssert x.left == y.left and x.right == y.right, "differ:\n  " & a & "\n  " & b

proc errorOf(src: string): string =
  try:
    discard renderPatchSrc(src, 0.001)
    ""
  except CatchableError as e: e.msg

let hit = "let x = noise() * discharge(impulse(3), 40)\n"
same(hit & "reverb(x, 2, 0.3)\n", hit & "reverb(x, wet = 0.3, rt60 = 2)\n")
same(hit & "x |> reverb(2, 0.3)\n", hit & "x |> reverb(rt60 = 2, wet = 0.3)\n")   # the pipe fills sig
same(hit & "lpf(x, 800, 0.5)\n", hit & "lpf(x, res = 0.5, cut = 800)\n")
same(hit & "fbdelay(x, 0.25, 1, 0.6)\n", hit & "fbdelay(x, fb = 0.6, time = 0.25, max_time = 1)\n")
same("dho(impulse(2) * 40000000, 440, 0.01)\n", "dho(damp = 0.01, freq = 440, force = impulse(2) * 40000000)\n")
same("wave(2, [0.1, 0.2, 0.3])\n", "wave(arr = [0.1, 0.2, 0.3], freq = 2)\n")
same("clamp(phasor(3) * 2 - 1, -0.5, 0.5)\n", "clamp(phasor(freq = 3) * 2 - 1, h = 0.5, l = -0.5)\n")

doAssert "no parameter 'mix'" in errorOf(hit & "reverb(x, 2, mix = 0.3)\n")
doAssert "rt60, wet" in errorOf(hit & "reverb(x, 2, mix = 0.3)\n")      # says what it takes
doAssert "missing 'rt60'" in errorOf(hit & "reverb(x, wet = 0.3)\n")
doAssert "given twice" in errorOf(hit & "reverb(x, 2, 0.3, rt60 = 1)\n")
doAssert "before named" in errorOf(hit & "reverb(x, wet = 0.3, 2)\n")
doAssert "by position" in errorOf("sin(x = 1)\n")
echo "builtin_named ok"
