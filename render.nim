## Offline patch render. Compiles an aither patch via parser/codegen,
## ticks a voice for N samples, returns the buffer in memory. No disk
## I/O, no engine, no audio thread.
##
## Patches that read MIDI primitives (`midi_freq`, `midi_gate`, etc.)
## see zero events here and produce silence — there's no MIDI thread
## simulating note-ons. Audit a live patch via `./aither spectrum`
## (which reads the engine's actual buffer) when MIDI matters.

import std/[os]
import parser, voice

const Stdlib = staticRead("stdlib.aither")

type RenderError* = object of CatchableError

proc renderProgram(prog: Node; seconds: float64; sr: int):
    tuple[left, right: seq[float64]] =
  let n = max(0, int(seconds * float64(sr)))
  result.left.setLen(n)
  result.right.setLen(n)
  let v = newVoice(float64(sr))
  v.load(prog, float64(sr))
  for i in 0 ..< n:
    let s = v.tick(float64(i) / float64(sr))
    if s.l != s.l or s.r != s.r:
      raise newException(RenderError,
        "patch produced NaN at sample " & $i & " of " & $n)
    result.left[i] = s.l
    result.right[i] = s.r

proc buildProgram(src, srcTag: string): Node =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(src)
  setSource(userAst, srcTag)
  Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

proc renderPatchSrc*(src: string; seconds: float64;
                     sr: int = 48000):
    tuple[left, right: seq[float64]] =
  ## Render an inline patch source string. `seconds` may be 0 (returns
  ## empty buffers — handy for "does it parse?"). NaN at any sample
  ## raises RenderError so downstream analysis can't silently ingest
  ## garbage.
  renderProgram(buildProgram(src, "<inline>"), seconds, sr)

proc renderPatch*(srcPath: string; seconds: float64;
                  sr: int = 48000):
    tuple[left, right: seq[float64]] =
  ## Render a patch from disk. Raises IOError if the file doesn't exist;
  ## ParseError / ValueError propagate from parser / codegen.
  if not fileExists(srcPath):
    raise newException(IOError, "patch not found: " & srcPath)
  let src = readFile(srcPath)
  renderProgram(buildProgram(src, srcPath), seconds, sr)

proc monoMix*(left, right: openArray[float64]): seq[float64] =
  ## (L + R) * 0.5 in fresh storage. Spectral analysis usually wants
  ## the master mono mix, not per-channel spectra.
  let n = min(left.len, right.len)
  result.setLen(n)
  for i in 0 ..< n:
    result[i] = (left[i] + right[i]) * 0.5
