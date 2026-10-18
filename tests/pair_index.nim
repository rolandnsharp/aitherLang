## A pair indexes like a two-entry table: p[i] truncates i and wraps at
## 2, so p[0] is the left side, p[1] the right, p[-1] the right and p[2]
## the left, whether the index is a constant or worked out as it runs.
## Before 2026-10-18 only p[0] and p[1] compiled, so a def that indexed
## its parameter took a three-entry table but not a two-entry one.

import ../render

proc at(src: string; seconds: float64): float64 =
  ## The left channel `seconds` into a one-second render.
  let a = renderPatchSrc(src, 1.0)
  a.left[int(seconds * 48000)]

proc near(x, y: float64): bool = abs(x - y) < 1e-9

let half = "int(phasor(1) * 2)"             # 0 for the first half second, then 1
doAssert near(at("let p = [0.1, 0.3]\np[-1]\n", 0.1), 0.3), "p[-1] is the right side"
doAssert near(at("let p = [0.1, 0.3]\np[2]\n", 0.1), 0.1), "p[2] wraps to the left"
doAssert near(at("let p = [0.1, 0.3]\np[3]\n", 0.1), 0.3), "p[3] wraps to the right"
for src in ["let p = [0.1, 0.3]\np[" & half & "]\n",
            "play a:\n  [0.1, 0.3]\na[" & half & "]\n",
            "def pick(tbl, i):\n  tbl[i]\nlet t = [0.1, 0.3]\npick(t, " & half & ")\n",
            "def pick(tbl, i):\n  tbl[i]\npick([0.1, 0.3], " & half & ")\n"]:
  doAssert near(at(src, 0.25), 0.1) and near(at(src, 0.75), 0.3), "runtime index: " & src
# A three-entry table through the same def is unchanged.
doAssert near(at("def pick(tbl, i):\n  tbl[i]\nlet t = [0.1, 0.2, 0.3]\npick(t, 2)\n", 0.5), 0.3)
echo "pair_index ok"
