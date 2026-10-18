## resonator(sig, freq, decay) rings at freq Hz, like every frequency in
## aither. It squared freq as if it were radians per second until
## 2026-10-18, so resonator(x, 440, ...) rang at 70 Hz and the GUIDE's
## plucked string and bell were 2 pi too low.

import ../render, ../analysis

proc fundamental(src: string): float64 =
  let a = renderPatchSrc(src, 2.0)
  analyze(monoMix(a.left, a.right), 48000.0).fundamentalHz

for hz in [110.0, 440.0, 1000.0]:
  let f = fundamental("impulse(2) |> resonator(" & $hz & ", 0.5)")
  doAssert abs(f - hz) / hz < 0.01, "resonator at " & $hz & " Hz rings at " & $f

echo "resonator_hz ok"
