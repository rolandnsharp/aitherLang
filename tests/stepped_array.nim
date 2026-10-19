## A stepped state array: `$a = array(N, init, every = M)`. Reads see the
## elements as they were at the last step; stores land together every M
## samples, at the sample where pixel_index(M, 1) is 0, so with M = w·h
## they land on tv's frames. An element nobody stored keeps its value.

import std/strutils
import ../render

proc run(src: string; n = 64): seq[(float64, int)] =
  ## Each sample's left value and the pixel index (0 .. M-1) on the right.
  let a = renderPatchSrc(src, float64(n) / 48000.0)
  for i in 0 ..< a.left.len:
    result.add (a.left[i], int(a.right[i] + 0.5))

proc errorOf(src: string): string =
  try:
    discard renderPatchSrc(src, 0.001)
    ""
  except CatchableError as e: e.msg

proc near(x, y: float64): bool = abs(x - y) < 1e-9

# A counter stepped every 4 samples: within a step every read is the same,
# and each step adds one, however many samples stored in between.
block counter:
  let r = run("$c = array(1, every = 4)\n$c[0] = $c[0] + 1\n" &
              "[$c[0] * 0.01, pixel_index(4, 1)]\n")
  var steps = 0
  for i, (v, k) in r:
    if k == 0 and i > 0: inc steps
    if i > 0 and k == 0: doAssert near(v, 0.01 * float64(steps)), "step " & $steps & ": " & $v
    if k > 0 and i > 0: doAssert near(v, r[i - 1][0]), "a read moved within a step at " & $i

# Stepped with the frame: the value read all through a frame is the one
# stored at the last pixel of the frame before (7 on a 4x2 raster).
block frames:
  let r = run("let k = pixel_index(4, 2)\n$a = array(1, every = 8)\n$a[0] = k\n" &
              "[$a[0] * 0.1, pixel_index(8, 1)]\n")
  var seenFrame = false
  for (v, k) in r:
    if k == 0: seenFrame = true
    if seenFrame: doAssert near(v, 0.7), "a frame read " & $v

# An element nobody stores keeps its value through every step.
block untouched:
  for (v, _) in run("$a = array(2, 5, every = 3)\n$a[0] = $a[0] + 1\n" &
                    "[$a[1] * 0.1, 0]\n"):
    doAssert near(v, 0.5)

# every = 1 steps every sample: a store is read on the next one.
block everySample:
  let r = run("$c = array(1, every = 1)\n$c[0] = $c[0] + 1\n[$c[0] * 0.001, 0]\n")
  for i in 2 ..< r.len: doAssert near(r[i][0] - r[i - 1][0], 0.001)

# Inside each, stores are held to the end of each, then to the step.
block withEach:
  let r = run("$a = array(3, every = 3)\neach(3, i => $a[i] = $a[i] + i)\n" &
              "[$a[2] * 0.01, pixel_index(3, 1)]\n")
  var steps = 0
  for i, (v, k) in r:
    if k == 0 and i > 0:
      inc steps
      doAssert near(v, 0.02 * float64(steps)), "each step " & $steps & ": " & $v

# The errors say what to write instead.
doAssert "not let" in errorOf("let a = array(4, every = 2)\na[0]\n")
doAssert "whole number" in errorOf("$a = array(4, every = 1.5)\n$a[0]\n")
doAssert "every = N" in errorOf("$a = array(4, size = 2)\n$a[0]\n")
doAssert "by position" in errorOf("phasor(440, every = 2)\n")      # a builtin
doAssert "no parameter 'every'" in errorOf("osc(sin, 440, every = 2)\n")   # a stdlib def

echo "stepped_array ok"
