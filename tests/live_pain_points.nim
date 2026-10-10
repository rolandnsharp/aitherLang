## Language papercuts that cost round trips during the 2026-10-10 set.

import std/[strutils, math]
import ../render

proc errOf(src: string): string =
  try:
    discard renderPatchSrc(src, 0.01)
    ""
  except CatchableError as e: e.msg

# A let used above its line: a plain error naming both lines, not TCC's
# "'l_b' undeclared".
let e = errOf("let a = b + 1\nlet b = 2\na * 0\n")
doAssert "used before its `let` on line 2" in e, e

# min/max take any number of arguments.
let (l, _) = renderPatchSrc("max(0.1, 0.4, 0.2) + min(1, 0.5, 0.9)", 0.01)
doAssert abs(l[^1] - 0.9) < 1e-12, $l[^1]

# A `$state` init can read the lets above it.
let (l2, _) = renderPatchSrc("let tempo = 120\n$beat = tempo / 60\n$beat", 0.01)
doAssert l2[^1] == 2.0, $l2[^1]

# A file of play blocks plays all of them instead of compiling to silence.
let (l3, r3) = renderPatchSrc("play a:\n  0.25\n\nplay b:\n  [0.5, 0.125]\n", 0.01)
doAssert l3[^1] == 0.75 and r3[^1] == 0.375, $l3[^1] & " " & $r3[^1]

# A def param named like a file-level stereo let shadows it: adsr's
# sustain `s` vs `let s = [L, R]` (2026-10-10 regression).
let (l4, r4) = renderPatchSrc(
  "let s = [0.9, 0.0]\nlet e = s |> adsr(0.001, 0.3, 0.7, 0.4)\ne\n", 0.01)
doAssert l4[^1] > 0.9 and r4[^1] == 0.0,
  "stereo through adsr (left gate open, right closed): " & $l4[^1] & " " & $r4[^1]

echo "live pain points ok"
