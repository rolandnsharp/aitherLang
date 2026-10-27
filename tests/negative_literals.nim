## `-` on a number is a negative number: `[0, -0.5, 1]` is a table of
## literals, so `wave` and constant tables take negative values (they
## used to fail with "wave array must be numeric literals only").

import ../ccompile, ../voice


proc loadWith(src: string): NativeVoice =
  let program = src
  result = newVoice(48000.0)
  result.load(program, 48000.0)

block waveTable:
  let v = loadWith("wave(1, [-0.5, 0.5, 0.25])")
  doAssert abs(v.tick(0.0).l - (-0.5)) < 1e-9

block constTable:
  let v = loadWith("let tbl = [0.5, -0.25, -1]\ntbl[2] + tbl[1]")
  doAssert abs(v.tick(0.0).l - (-1.25)) < 1e-9

block stillAnExpression:
  let v = loadWith("let x = 3\n-x * 2 + -(1 + 1) + 2 - -1")
  doAssert abs(v.tick(0.0).l - (-5.0)) < 1e-9, $v.tick(0.0).l

echo "negative_literals: OK"
