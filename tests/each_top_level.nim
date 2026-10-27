## A top-level `each(...)` is a statement, not the patch's output. It was
## taken as the final expression (value 0), so a file of play blocks with
## an `each` among its lets went silent while every part still metered.

import ../ccompile, ../voice


proc loadWith(src: string): NativeVoice =
  let program = src
  result = newVoice(48000.0)
  result.load(program, 48000.0)

block playsStillPlay:
  let v = loadWith("let w = array(3)\neach(3, r => w[r] = r * 0.25)\nplay a:\n  w[2]")
  let s = v.tick(0.0)
  doAssert abs(s.l - 0.5) < 1e-9, "a top-level each silenced the plays: " & $s.l

block finalExpressionStillWins:
  let v = loadWith("let w = array(2)\neach(2, r => w[r] = 1)\nw[0] + w[1]")
  doAssert abs(v.tick(0.0).l - 2.0) < 1e-9

echo "each_top_level: OK"
