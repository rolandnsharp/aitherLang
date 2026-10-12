## haas and reverb take a stereo pair as well as a mono signal.

import std/[math]
import ../render

proc both(src: string; n: int): tuple[l, r: seq[float64]] =
  let s = renderPatchSrc(src, float64(n) / 48000.0)
  (s.left, s.right)

# haas on mono: the left side is the signal, the right is it 1 ms later.
let (ml, mr) = both("let x = if t < dt then 1 else 0\nhaas(x, 1)", 100)
doAssert ml[0] == 1.0 and mr[48] == 1.0 and mr[0] == 0.0, $ml[0 .. 2] & $mr[46 .. 50]
# haas on a pair: each side keeps its own signal, one delayed.
let (pl, pr) = both("let s = [if t < dt then 1 else 0, if t < dt then 0.5 else 0]\nhaas(s, -1)", 100)
doAssert pl[48] == 1.0 and pl[0] == 0.0 and pr[0] == 0.5, $pl[46 .. 50] & $pr[0 .. 2]
# reverb on a pair runs one reverb per side: a left-only impulse leaves
# the right side silent.
let (rl, rr) = both("let s = [if t < dt then 1 else 0, 0]\ns |> reverb(1.0, 0.5)", 4800)
var el, er = 0.0
for i in 0 ..< 4800:
  el += rl[i] * rl[i]; er += rr[i] * rr[i]
doAssert el > 0.1 and er == 0.0, $el & " " & $er
# reverb on mono still works and is identical on both sides.
let (ol, orr) = both("let x = if t < dt then 1 else 0\nx |> reverb(1.0, 0.5)", 4800)
doAssert ol == orr and ol.len == 4800
echo "stereo haas/reverb ok"
