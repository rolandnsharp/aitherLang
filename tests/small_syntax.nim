## Small syntax that cost round trips: `mod` (and `%` pointing at it), number exponents.

import std/[math, strutils]
import ../render, ../ccompile

proc one(src: string): float64 = renderPatchSrc(src, 0.001).left[0]

doAssert one("7 mod 4 + 10 mod 3") == 3.0 + 1.0
doAssert one("-1 mod 4") == 3.0                # floored
try:
  parse("7 % 4")
  doAssert false, "`%` should be an error"
except ParseError as e:
  doAssert "mod" in e.msg, e.msg
doAssert one("1e3 + 2.5e-1 + 1E2 + .5e1") == 1000.0 + 0.25 + 100.0 + 5.0
doAssert abs(one("1e-3") - 0.001) < 1e-18
# `2e` is not an exponent: it is the number 2, then the identifier e,
# two expressions side by side.
try:
  parse("2e")
  doAssert false, "`2e` should not parse as a number"
except ParseError as e:
  doAssert "second top-level expression" in e.msg, e.msg
echo "small syntax ok"
