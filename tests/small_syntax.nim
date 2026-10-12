## Small syntax that cost round trips: `%` as mod, number exponents.

import std/[math]
import ../render

proc one(src: string): float64 = renderPatchSrc(src, 0.001).left[0]

doAssert one("7 % 4 + 10 % 3") == 3.0 + 1.0
doAssert one("-1 % 4") == 3.0                  # same as mod: floored
doAssert one("1e3 + 2.5e-1 + 1E2 + .5e1") == 1000.0 + 0.25 + 100.0 + 5.0
doAssert abs(one("1e-3") - 0.001) < 1e-18
# `2e` is not an exponent: the number 2, then the identifier e.
doAssert one("let e = 1\n2e") == 1.0
echo "small syntax ok"
