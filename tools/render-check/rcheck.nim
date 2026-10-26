## Every patch rendered briefly through the Nim compiler (the reference)
## and the C one, sample for sample: the same three windows as the corpus,
## shorter. Prints each patch whose bits differ, and where.
import std/[os, strutils]
import ../../render, ../../midi, ../../voice

proc chord(keys: openArray[int]; vel: int) =
  for k in keys: midiNoteOn(k, vel)

proc windows(path, src: string; c: bool): seq[Stereo] =
  midiResetAll()
  for cc in 0 .. 127: midiCc(cc, 64)
  chord([60, 64, 67], 100)
  let v = newVoice(48000.0)
  if c: v.loadSource(Stdlib, src, path, 48000.0) else: v.load(buildProgram(src, path), 48000.0)
  result.add renderVoice(v, 0.15, 48000).audio
  for k in [60, 64, 67]: midiNoteOff(k)
  chord([65, 69, 72], 50)
  result.add renderVoice(v, 0.15, 48000, t0 = 0.15).audio
  midiResetAll()
  for cc in 0 .. 127: midiCc(cc, 64)
  chord([60, 64, 67], 100)
  let late = newVoice(48000.0)
  if c: late.loadSource(Stdlib, src, path, 48000.0) else: late.load(buildProgram(src, path), 48000.0)
  result.add renderVoice(late, 0.15, 48000, t0 = 150.0).audio

var bad = 0
for path in commandLineParams():
  let src = readFile(path)
  var a, b: seq[Stereo]
  var ea, eb = ""
  try: a = windows(path, src, false) except CatchableError as e: ea = e.msg.splitLines()[0]
  try: b = windows(path, src, true) except CatchableError as e: eb = e.msg.splitLines()[0]
  if ea.len > 0 or eb.len > 0:
    if ea != eb:
      echo "differ ", path, ": nim ", (if ea.len > 0: ea else: "renders"), " | c ",
           (if eb.len > 0: eb else: "renders")
      inc bad
    continue
  block cmp:
    for w in 0 ..< a.len:
      for i in 0 ..< a[w].left.len:
        if cast[uint64](a[w].left[i]) != cast[uint64](b[w].left[i]) or
           cast[uint64](a[w].right[i]) != cast[uint64](b[w].right[i]):
          echo "differ ", path, ": window ", w, " sample ", i, " nim ", a[w].left[i], " ",
               a[w].right[i], " c ", b[w].left[i], " ", b[w].right[i]
          inc bad
          break cmp
quit(if bad > 0: 1 else: 0)
