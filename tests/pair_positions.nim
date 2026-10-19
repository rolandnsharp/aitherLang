## A def parameter handed to a pair builtin is a pair only where that
## builtin takes one: rotate's angle, cscale's factor, analytic's signal
## and phasor_pair's frequency are numbers, so `def turn(th): rotate(p, th)`
## takes a number, as written. (It was inferred a pair: "rotate takes a
## pair and an angle, got 4 numbers".)

import ../render

proc same(a, b: string) =
  let x = renderPatchSrc(a, 0.2)
  let y = renderPatchSrc(b, 0.2)
  doAssert x.left == y.left and x.right == y.right, "differ:\n  " & a & "\n  " & b

same("def turn(th): rotate([0.5, 0], th)\nturn(TAU * t)\n", "rotate([0.5, 0], TAU * t)\n")
same("def grow(k): cscale(k, [0.3, 0.1])\ngrow(t)\n", "cscale(t, [0.3, 0.1])\n")
same("def turn(p, th): rotate(p, th)\nturn([0.5, 0.2], t)\n", "rotate([0.5, 0.2], t)\n")   # p still a pair
same("def card(th): rotate([0.5, 0], th) - rotate([0.25, 0], 2 * th)\ncard(t) * 0.5\n",
     "(rotate([0.5, 0], t) - rotate([0.25, 0], 2 * t)) * 0.5\n")
echo "pair_positions ok"
