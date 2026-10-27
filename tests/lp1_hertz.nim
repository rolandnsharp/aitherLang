## lp1 / hp1 take hertz: the -3 dB point of lp1(x, fc) is at fc. They used
## to take a coefficient (a = cutoff / sr), which put lp1(x, 4000) at
## 665 Hz; every patch was converted to the hertz it already produced.
## hp1 is the input minus lp1 (so lp1 + hp1 = the input, exactly); that
## shape lands within 0.6 dB of -3 dB at fc up to 1 kHz.

import std/math
import ../ccompile, ../voice


proc loadWith(src: string): NativeVoice =
  let program = src
  result = newVoice(48000.0)
  result.load(program, 48000.0)

proc gainAt(src: string; f: float64): float64 =
  ## steady-state gain of a sine at f through the patch (its `x`)
  let v = loadWith("let x = sin(TAU * phasor(" & $f & "))\n" & src)
  var inE, outE = 0.0
  for i in 0..<96000:
    let s = v.tick(float64(i) / 48000.0).l
    if i >= 48000:
      let x = sin(TAU * f * float64(i + 1) / 48000.0)
      inE += x * x; outE += s * s
  sqrt(outE / inE)

block lowpassMinus3dB:
  for fc in [100.0, 1000.0, 4000.0]:
    let g = 20 * log10(gainAt("lp1(x, " & $fc & ")", fc))
    doAssert abs(g + 3.0) < 0.35, "lp1 at " & $fc & " Hz: " & $g & " dB, want -3"

block highpassMinus3dB:
  for fc in [100.0, 1000.0]:
    let g = 20 * log10(gainAt("hp1(x, " & $fc & ")", fc))
    doAssert abs(g + 3.0) < 0.7, "hp1 at " & $fc & " Hz: " & $g & " dB, want -3"

block passAndStop:
  doAssert gainAt("lp1(x, 1000)", 50) > 0.99      # far below: passes
  doAssert gainAt("lp1(x, 100)", 5000) < 0.03     # far above: 6 dB/octave down

echo "lp1_hertz: OK"
