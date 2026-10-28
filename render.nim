## Offline patch render, which is C (render.c), as the Nim tests see it
## until they are C too (notes/c-port-2026-10-28.md). Compiles an aither
## patch, ticks a voice for N samples, returns the buffer in memory. No
## disk I/O for the patch itself, no engine, no audio thread.
##
## Patches that read MIDI primitives (`midi_freq`, `midi_gate`, etc.)
## see zero events here and produce silence — there's no MIDI thread
## simulating note-ons. Audit a live patch via `./aither spectrum`
## (which reads the engine's actual buffer) when MIDI matters.
##
## `input()` reads a recording when one is given (raw float pairs, the
## engine's own format), so a patch that processes audio input can be
## iterated on offline against the same material every time.

import std/os
import voice, engine_types, ccompile
export ccompile.Stdlib


type
  RenderError* = object of CatchableError
  Stereo* = tuple[left, right: seq[float64]]

  Rendered* = object
    audio*: Stereo
    parts*: seq[PartLevel]
    seconds*: float64            # length of the measured window
    skipped*: float64            # rendered first but not measured


const RenderH = currentSourcePath().parentDir / "render.h"
{.compile: "render.c".}

type
  CPartLevel {.importc: "PartLevel", header: RenderH, bycopy.} = object
    name: cstring
    rms, peak, low, gain: float64
    unheard: cint
  CRendered {.importc: "Rendered", header: RenderH, bycopy.} = object
    left, right: ptr UncheckedArray[float64]
    n: clong
    parts: ptr UncheckedArray[CPartLevel]
    nparts: cint
    seconds, skipped: float64

proc render_optimize(v: NativeVoice) {.importc, header: RenderH.}
proc cRenderVoice(v: NativeVoice; seconds: cdouble; sr: cint; inL, inR: ptr float64; nIn: clong;
                  skip, t0: cdouble; feedInput: cint; o: ptr CRendered; err: cstring;
                  errlen: cint): cint {.importc: "render_voice", header: RenderH.}
proc rendered_free(r: ptr CRendered) {.importc, header: RenderH.}

proc optimize*(v: NativeVoice) = render_optimize(v)
  ## The voice's cc -O2 build now, unless AITHER_OPT=0 or there's no cc.

proc renderVoice*(v: NativeVoice; seconds: float64; sr: int;
                  input: Stereo = (@[], @[]); skip = 0.0;
                  t0 = 0.0; feedInput = true): Rendered =
  ## Tick a loaded voice from engine time `t0`: `skip` seconds of lead-in
  ## (run, not kept), then `seconds` kept and measured per part (render.c).
  ## NaN at any sample raises RenderError naming the time it happened.
  ## Inside the engine pass `feedInput = false`: `input()` then reads the
  ## live capture, and the render leaves it alone.
  var c: CRendered
  var err = newString(512)
  let n = min(input.left.len, input.right.len)
  let inL = if n > 0: unsafeAddr input.left[0] else: nil
  let inR = if n > 0: unsafeAddr input.right[0] else: nil
  if cRenderVoice(v, seconds, cint(sr), inL, inR, clong(n), skip, t0, cint(feedInput), addr c,
                  err.cstring, 512) != 0:
    raise newException(RenderError, $err.cstring)
  result.seconds = c.seconds
  result.skipped = c.skipped
  result.audio.left.setLen(int(c.n))
  result.audio.right.setLen(int(c.n))
  for i in 0 ..< int(c.n):
    result.audio.left[i] = c.left[i]
    result.audio.right[i] = c.right[i]
  for p in 0 ..< int(c.nparts):
    let x = c.parts[p]
    result.parts.add PartLevel(name: $x.name, rms: x.rms, peak: x.peak, low: x.low,
                               gain: x.gain, unheard: x.unheard != 0)
  rendered_free(addr c)

proc renderSource(src, tag: string; seconds: float64; sr: int;
                  input: Stereo; skip = 0.0): Rendered =
  ## A patch's source through the compiler in C, rendered.
  let v = newVoice(float64(sr))
  v.loadSource(Stdlib, src, tag, float64(sr))
  renderVoice(v, seconds, sr, input, skip)

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
  callsInput(readFile(srcPath), srcPath)

proc renderPatchFull*(srcPath: string; seconds: float64; sr: int = 48000;
                      input: Stereo = (@[], @[]); skip = 0.0): Rendered =
  ## Render a patch from disk, with per-part levels. `skip` seconds run
  ## first and aren't kept, to reach a later section. Raises IOError if
  ## the file doesn't exist; ParseError / ImportError / ValueError
  ## propagate from the compiler.
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
