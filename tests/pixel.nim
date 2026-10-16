## pixel(w, h) and pixel_index(w, h) follow the engine clock: the
## engine's first sample is pixel 0 and the index steps one a sample,
## wrapping at w*h, so a patch draws on the grid tv counts. If it slipped
## by a sample the picture would shear. Run through tick_block, the
## engine's path, which sets t from its own second/fraction clock.

import ../parser, ../voice

const Stdlib = staticRead("../stdlib.aither")

proc voiceOf(src: string; sr: float64): NativeVoice =
  let prog = Node(kind: nkBlock, line: 1,
                  kids: parseProgram(Stdlib).kids & parseProgram(src).kids)
  result = newVoice(sr)
  result.load(prog, sr)

# n samples from the engine's start, in blocks of 7 so block edges fall
# everywhere.
proc run(v: NativeVoice; n: int): (seq[float64], seq[float64]) =
  let hdr = cast[ptr VoiceHeader](v.state)
  hdr.timeSec = 0.0
  hdr.timeFrac = 0.0
  hdr.fadeGain = 1.0
  hdr.fadeDelta = 0.0
  hdr.inBuf = nil
  var l = newSeq[float64](n)
  var r = newSeq[float64](n)
  var i = 0
  while i < n:
    let k = min(7, n - i)
    v.tickBlock(k, l[i].addr, r[i].addr)
    i += k
  (l, r)

# The index: 0, 1, 2 ... wrapping at w*h.
block steps:
  let (idx, _) = run(voiceOf("[pixel_index(5, 3), 0]", 48000.0), 40)
  for i in 0 ..< 40:
    doAssert idx[i] == float64(i mod 15), "sample " & $i & ": " & $idx[i]

# The frame: 0 for the first w*h samples, then 1, ...
block frames:
  let (fr, _) = run(voiceOf("[frame_number(5, 3), 0]", 48000.0), 40)
  for i in 0 ..< 40:
    doAssert fr[i] == float64(i div 15), "sample " & $i & ": " & $fr[i]

# The centre of each pixel: x across -1..1, y up, square pixels.
block centres:
  let (x, y) = run(voiceOf("pixel(4, 2)", 48000.0), 16)
  for i in 0 ..< 16:
    let col = i mod 4
    let row = (i div 4) mod 2
    doAssert abs(x[i] - (float64(2 * col + 1) / 4 - 1)) < 1e-12, "x at " & $i & ": " & $x[i]
    doAssert abs(y[i] - (float64(2 - 2 * row - 1) / 4)) < 1e-12, "y at " & $i & ": " & $y[i]

# At the 1.152 MHz a 160x120 tv takes, over more than a second, so the
# clock's fraction wraps: not one sample slips.
block fast:
  let n = 1_200_000
  let (idx, _) = run(voiceOf("[pixel_index(160, 120), 0]", 1152000.0), n)
  for i in 0 ..< n:
    if idx[i] != float64(i mod 19200):
      doAssert false, "slipped at sample " & $i & ": " & $idx[i]

echo "pixel ok"
