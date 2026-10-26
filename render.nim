## Offline patch render. Compiles an aither patch via parser/codegen,
## ticks a voice for N samples, returns the buffer in memory. No disk
## I/O for the patch itself, no engine, no audio thread.
##
## Patches that read MIDI primitives (`midi_freq`, `midi_gate`, etc.)
## see zero events here and produce silence — there's no MIDI thread
## simulating note-ons. Audit a live patch via `./aither spectrum`
## (which reads the engine's actual buffer) when MIDI matters.
##
## `input()` reads a recording when one is given (raw float pairs, the
## engine's own format), so a patch that processes audio input can be
## iterated on offline against the same material every time.

import std/[os, math, strutils, sets]
import parser, voice, dsp, engine_types, imports, ccompile

const Stdlib* = staticRead("stdlib.aither")

type
  RenderError* = object of CatchableError
  Stereo* = tuple[left, right: seq[float64]]

  Rendered* = object
    audio*: Stereo
    parts*: seq[PartLevel]
    seconds*: float64            # length of the measured window
    skipped*: float64            # rendered first but not measured


# A render this long (lead-in included) is built with cc -O2 first: the
# seconds of compiling pay for themselves against TCC's slow code.
const OptimizeOverSeconds = 4.0

proc optimize*(v: NativeVoice) =
  if getEnv("AITHER_OPT") == "0" or findExe("cc").len == 0: return
  try:
    let (lib, fn) = compileOptimized(v.csrc, v.stateSize)
    discard v.upgrade(v.gen, lib, fn)
  except CatchableError:
    discard                       # stay on TCC; same output, just slower

# 4th-order Butterworth lowpass (two biquads) for the below-200 Hz
# share: steep enough that a 330 Hz tone reads ~2%, a 100 Hz one ~100%.
type Biquad = object
  b0, b1, b2, a1, a2, z1, z2: float64

proc lowpass(fc, q: float64; sr: int): Biquad =
  let w = 2.0 * PI * fc / float64(sr)
  let alpha = sin(w) / (2.0 * q)
  let a0 = 1.0 + alpha
  result.b0 = (1.0 - cos(w)) / 2.0 / a0
  result.b1 = (1.0 - cos(w)) / a0
  result.b2 = result.b0
  result.a1 = -2.0 * cos(w) / a0
  result.a2 = (1.0 - alpha) / a0

proc step(f: var Biquad; x: float64): float64 {.inline.} =
  result = f.b0 * x + f.z1
  f.z1 = f.b1 * x - f.a1 * result + f.z2
  f.z2 = f.b2 * x - f.a2 * result

proc renderVoice*(v: NativeVoice; seconds: float64; sr: int;
                  input: Stereo = (@[], @[]); skip = 0.0;
                  t0 = 0.0; feedInput = true): Rendered =
  ## Tick a loaded voice from engine time `t0`: `skip` seconds of lead-in
  ## (run, not kept), then `seconds` kept and measured per part. NaN at
  ## any sample raises RenderError naming the time it happened. Inside
  ## the engine pass `feedInput = false`: `input()` then reads the live
  ## capture, and the render leaves it alone.
  let nSkip = max(0, int(skip * float64(sr)))
  let n = max(0, int(seconds * float64(sr)))
  if float64(nSkip + n) / float64(sr) >= OptimizeOverSeconds: v.optimize()
  result.seconds = float64(n) / float64(sr)
  result.skipped = float64(nSkip) / float64(sr)
  result.audio.left.setLen(n)
  result.audio.right.setLen(n)
  let np = v.partNames.len
  var sumSq, lowSq, peak = newSeq[float64](np)
  var lp1, lp2 = newSeq[Biquad](np)
  for p in 0 ..< np:
    lp1[p] = lowpass(LowSplitHz, 0.5412, sr)
    lp2[p] = lowpass(LowSplitHz, 1.3066, sr)
  for i in 0 ..< nSkip + n:
    # Feed the recording one frame at a time; silence past its end.
    if not feedInput: discard
    elif i < input.left.len:
      inputL = input.left[i]
      inputR = input.right[i]
    else:
      inputL = 0.0
      inputR = 0.0
    let s = v.tick(t0 + float64(i) / float64(sr))
    if s.l != s.l or s.r != s.r:
      if feedInput:
        inputL = 0.0; inputR = 0.0
      raise newException(RenderError,
        "patch produced NaN at " & formatFloat(float64(i) / float64(sr), ffDecimal, 3) &
        "s (sample " & $i & " of " & $(nSkip + n) & ")")
    if i < nSkip: continue
    result.audio.left[i - nSkip] = s.l
    result.audio.right[i - nSkip] = s.r
    for p in 0 ..< np:
      let m = (v.partOut[2*p] + v.partOut[2*p + 1]) * 0.5
      let low = lp2[p].step(lp1[p].step(m))
      sumSq[p] += m * m
      lowSq[p] += low * low
      peak[p] = max(peak[p], max(abs(v.partOut[2*p]), abs(v.partOut[2*p + 1])))
  if feedInput:
    inputL = 0.0
    inputR = 0.0
  for p in 0 ..< np:
    result.parts.add PartLevel(name: v.partNames[p], gain: v.partGains[p],
      unheard: v.partNames[p] in v.unheard,
      rms: (if n > 0: sqrt(sumSq[p] / float64(n)) else: 0.0), peak: peak[p],
      low: (if sumSq[p] > 0.0: min(1.0, lowSq[p] / sumSq[p]) else: 0.0))

proc renderProgram(prog: Node; seconds: float64; sr: int;
                   input: Stereo; skip = 0.0): Rendered =
  let v = newVoice(float64(sr))
  v.load(prog, float64(sr))
  renderVoice(v, seconds, sr, input, skip)

proc renderSource(src, tag: string; seconds: float64; sr: int;
                  input: Stereo; skip = 0.0): Rendered =
  ## A patch's source through the compiler in C, rendered.
  let v = newVoice(float64(sr))
  v.loadSource(Stdlib, src, tag, float64(sr))
  renderVoice(v, seconds, sr, input, skip)

proc buildProgram*(src, srcTag: string): Node =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  var userAst = parseProgram(src)
  setSource(userAst, srcTag)
  # Imports resolve against the patch's directory (the current directory
  # for inline source).
  var files: seq[string]
  var seen = initHashSet[string]()
  let fromPath = if srcTag == "<inline>": "" else: srcTag
  userAst = expandImports(userAst, fromPath, files, seen)
  Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

proc renderPatchSrc*(src: string; seconds: float64;
                     sr: int = 48000; input: Stereo = (@[], @[])): Stereo =
  ## Render an inline patch source string. `seconds` may be 0 (returns
  ## empty buffers — handy for "does it parse?"). NaN at any sample
  ## raises RenderError so downstream analysis can't silently ingest
  ## garbage.
  renderSource(src, "<inline>", seconds, sr, input).audio

proc patchCallsInput*(srcPath: string): bool =
  ## Does the patch (with its imports) call `input()`? `audit` reads
  ## stdin only then.
  compileSource(Stdlib, readFile(srcPath), srcPath, "", 48000.0).callsInput

proc renderPatchFull*(srcPath: string; seconds: float64; sr: int = 48000;
                      input: Stereo = (@[], @[]); skip = 0.0): Rendered =
  ## Render a patch from disk, with per-part levels. `skip` seconds run
  ## first and aren't kept, to reach a later section. Raises IOError if
  ## the file doesn't exist; ParseError / ValueError propagate from
  ## parser / codegen.
  if not fileExists(srcPath):
    raise newException(IOError, "patch not found: " & srcPath)
  let src = readFile(srcPath)
  renderSource(src, srcPath, seconds, sr, input, skip)

proc renderPatch*(srcPath: string; seconds: float64;
                  sr: int = 48000; input: Stereo = (@[], @[])): Stereo =
  renderPatchFull(srcPath, seconds, sr, input).audio

proc monoMix*(left, right: openArray[float64]): seq[float64] =
  ## (L + R) * 0.5 in fresh storage. Spectral analysis usually wants
  ## the master mono mix, not per-channel spectra.
  let n = min(left.len, right.len)
  result.setLen(n)
  for i in 0 ..< n:
    result[i] = (left[i] + right[i]) * 0.5
