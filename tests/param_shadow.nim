## A def's parameter shadows a top-level def of the same name. stdlib's
## `inharmonic(freq, ratio, amp, max_n)` calls its parameter `ratio(n)`;
## a patch with its own `def ratio(n, k)` used to fail to compile
## ("ratio: missing 'k' (stdlib:186)") because the pass that fills in
## defaults checked every call against the top-level def, whatever was
## in scope. Found writing patches/spectra.aither.

import std/[strutils]
import ../render

proc level(src: string): float64 =
  let a = renderPatchSrc(src, 0.05)
  for x in a.left: result = max(result, abs(x))

doAssert level("def ratio(n, k): n * k\nsin(TAU * phasor(220)) * 0.1") > 0.05,
  "a def named ratio alongside stdlib's inharmonic"
doAssert level("def ratio(n, k): n * k\ninharmonic(220, stiff_string, soft_decay, 6) * 0.1 + ratio(1, 0)") > 0.05,
  "inharmonic's ratio parameter and a top-level ratio def, both called"

echo "param_shadow ok"
