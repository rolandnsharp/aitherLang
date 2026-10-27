## tick_block is tick, n times, to the bit.
##
## The engine runs voices a block at a time (codegen's BlockTemplate):
## the clock, input(), the part bodies, the fade and part-gain ramps
## and the meters all advance inside C. Everything render, try and the
## tests do still goes one sample at a time through tick(). The two
## must agree exactly, or offline and live would drift apart.

import std/[math]
import ../ccompile, ../voice, ../dsp, ../engine_types

const Patch = """
let tempo = 2
play bass:
  saw(TAU * phasor(55 + 10 * sin(TAU * t))) |> lpf(400 + 300 * sin(TAU * 0.5 * t), 0.7)
play hit:
  noise() * discharge(impulse(tempo), 30) * 0.3
play thru:
  input() * 0.5
play seq:
  sin(TAU * phasor(wave(tempo, [220, 330, 440]))) * 0.2

(bass + hit) * 0.5 + thru + seq |> tanh
"""

const Sr = 48000.0
const N = 4800
const Block = 512

# One voice, sample by sample, with the engine's clock arithmetic.
proc single(): seq[float64] =
  let v = newVoice(Sr)
  v.load((Patch), Sr)
  var sec, frac = 0.0
  let dt = 1.0 / Sr
  for i in 0 ..< N:
    frac += dt
    if frac >= 1.0:
      sec += 1.0
      frac -= 1.0
    inputL = 0.1 * sin(float64(i) * 0.01)
    inputR = -inputL
    let s = v.tick(sec + frac)
    result.add s.l
    result.add s.r
  inputL = 0.0
  inputR = 0.0

# The same voice, in blocks, through tick_block.
proc blocks(): seq[float64] =
  let v = newVoice(Sr)
  v.load((Patch), Sr)
  let hdr = cast[ptr VoiceHeader](v.state)
  hdr.timeSec = 0.0
  hdr.timeFrac = 0.0
  hdr.fadeGain = 1.0
  hdr.fadeDelta = 0.0
  var inBuf = newSeq[float64](2 * Block)
  var outL = newSeq[float64](Block)
  var outR = newSeq[float64](Block)
  var i = 0
  while i < N:
    let n = min(Block, N - i)
    for k in 0 ..< n:
      inBuf[2*k] = 0.1 * sin(float64(i + k) * 0.01)
      inBuf[2*k + 1] = -inBuf[2*k]
    hdr.inBuf = inBuf[0].addr
    v.tickBlock(n, outL[0].addr, outR[0].addr)
    for k in 0 ..< n:
      result.add outL[k]
      result.add outR[k]
    i += n
  doAssert hdr.bad == 0

let a = single()
let b = blocks()
doAssert a.len == b.len
var peak = 0.0
for i in 0 ..< a.len:
  peak = max(peak, abs(a[i]))
  doAssert a[i] == b[i], "sample " & $(i div 2) & (if i mod 2 == 0: " L " else: " R ") &
    $a[i] & " vs " & $b[i]
doAssert peak > 0.01, "the patch made no sound"

echo "block_tick ok"
