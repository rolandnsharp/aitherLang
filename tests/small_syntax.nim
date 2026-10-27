## Small syntax that cost round trips: `mod` (and `%` pointing at it), number exponents.

import std/[math, strutils]
import ../render, ../parser

proc one(src: string): float64 = renderPatchSrc(src, 0.001).left[0]

doAssert one("7 mod 4 + 10 mod 3") == 3.0 + 1.0
doAssert one("-1 mod 4") == 3.0                # floored
try:
  discard tokenize("7 % 4")
  doAssert false, "`%` should be an error"
except ParseError as e:
  doAssert "mod" in e.msg, e.msg
doAssert one("1e3 + 2.5e-1 + 1E2 + .5e1") == 1000.0 + 0.25 + 100.0 + 5.0
doAssert abs(one("1e-3") - 0.001) < 1e-18
# `2e` is not an exponent: the number 2, then the identifier e.
let toks = tokenize("2e")
doAssert toks[0].kind == tkNum and toks[0].num == 2.0 and
         toks[1].kind == tkIdent and toks[1].str == "e"
echo "small syntax ok"
