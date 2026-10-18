## tcc and cc -O2 compute the same bits.
##
##   make exact
##
## Every voice starts on TCC's code and is swapped for a cc -O2 build of
## the same C when that's ready. The swap keeps the state, so it's only
## seamless if both builds do the same arithmetic: otherwise every voice
## jumps to a slightly different trajectory the moment the fast code
## lands, and two voices meant to stay locked drift apart. This renders
## every tracked patch for a second both ways, from fresh state and with
## the corpus's MIDI (knobs at half, a C-E-G chord), and compares every
## sample's bits.

import std/[os, osproc, strutils, algorithm, sequtils]
import ../render, ../voice, ../midi

proc renderWith(path: string; optimized: bool): Stereo =
  midiResetAll()
  for cc in 0 .. 127: midiCc(cc, 64)
  for key in [60, 64, 67]: midiNoteOn(key, 100)
  let v = newVoice(48000.0)
  v.load(buildProgram(readFile(path), path), 48000.0, path)
  if optimized:
    v.optimize()
    doAssert v.optimized, path & ": the cc -O2 build didn't land"
  renderVoice(v, 1.0, 48000).audio

let files = execProcess("git ls-files '*.aither'").splitLines()
  .filterIt(it.len > 0 and "/lib/" notin it).sorted()
var differ, failed = 0
for f in files:
  var a, b: Stereo
  try:
    a = renderWith(f, false)
    b = renderWith(f, true)
  except CatchableError as e:
    inc failed
    echo "ERROR  ", f, ": ", e.msg.splitLines()[0]
    continue
  var first = -1
  var worst = 0.0
  for i in 0 ..< a.left.len:
    for (x, y) in [(a.left[i], b.left[i]), (a.right[i], b.right[i])]:
      if cast[uint64](x) != cast[uint64](y):
        if first < 0: first = i
        worst = max(worst, abs(x - y))
  if first >= 0:
    inc differ
    echo "DIFFER ", f, ": from sample ", first, ", largest gap ",
         formatFloat(worst, ffScientific, 2)

echo files.len, " patches: ", files.len - differ - failed, " bit-identical, ",
     differ, " differ, ", failed, " failed to render"
quit(if differ + failed == 0: 0 else: 1)
