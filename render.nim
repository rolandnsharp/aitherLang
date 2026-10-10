## Offline patch render. Compiles an aither patch via parser/codegen,
## ticks a voice for N samples, returns the buffer in memory. No disk
## I/O for the patch itself, no engine, no audio thread.
##
## Patches that read MIDI primitives (`midi_freq`, `midi_gate`, etc.)
## see zero events here and produce silence — there's no MIDI thread
## simulating note-ons. Audit a live patch via `./aither spectrum`
## (which reads the engine's actual buffer) when MIDI matters.
##
## `input()` reads a recording when one is given (`readWav`), so a
## patch that processes the engine's audio input can be iterated on
## offline against the same material every time.

import std/[os, streams]
import parser, voice, dsp

const Stdlib = staticRead("stdlib.aither")

type
  RenderError* = object of CatchableError
  Stereo* = tuple[left, right: seq[float64]]

proc renderProgram(prog: Node; seconds: float64; sr: int;
                   input: Stereo): Stereo =
  let n = max(0, int(seconds * float64(sr)))
  result.left.setLen(n)
  result.right.setLen(n)
  let v = newVoice(float64(sr))
  v.load(prog, float64(sr))
  for i in 0 ..< n:
    # Feed the recording one frame at a time; silence past its end.
    if i < input.left.len:
      inputL = input.left[i]
      inputR = input.right[i]
    else:
      inputL = 0.0
      inputR = 0.0
    let s = v.tick(float64(i) / float64(sr))
    if s.l != s.l or s.r != s.r:
      inputL = 0.0; inputR = 0.0
      raise newException(RenderError,
        "patch produced NaN at sample " & $i & " of " & $n)
    result.left[i] = s.l
    result.right[i] = s.r
  inputL = 0.0
  inputR = 0.0

proc buildProgram(src, srcTag: string): Node =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(src)
  setSource(userAst, srcTag)
  Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

proc renderPatchSrc*(src: string; seconds: float64;
                     sr: int = 48000; input: Stereo = (@[], @[])): Stereo =
  ## Render an inline patch source string. `seconds` may be 0 (returns
  ## empty buffers — handy for "does it parse?"). NaN at any sample
  ## raises RenderError so downstream analysis can't silently ingest
  ## garbage.
  renderProgram(buildProgram(src, "<inline>"), seconds, sr, input)

proc renderPatch*(srcPath: string; seconds: float64;
                  sr: int = 48000; input: Stereo = (@[], @[])): Stereo =
  ## Render a patch from disk. Raises IOError if the file doesn't exist;
  ## ParseError / ValueError propagate from parser / codegen.
  if not fileExists(srcPath):
    raise newException(IOError, "patch not found: " & srcPath)
  let src = readFile(srcPath)
  renderProgram(buildProgram(src, srcPath), seconds, sr, input)

proc monoMix*(left, right: openArray[float64]): seq[float64] =
  ## (L + R) * 0.5 in fresh storage. Spectral analysis usually wants
  ## the master mono mix, not per-channel spectra.
  let n = min(left.len, right.len)
  result.setLen(n)
  for i in 0 ..< n:
    result[i] = (left[i] + right[i]) * 0.5

proc readWav*(path: string; sr: int = 48000): Stereo =
  ## Read a WAV file (16- or 24-bit PCM, or 32-bit float; any channel
  ## count, the first two are kept and a mono file fills both sides).
  ## The engine runs at one rate, so a file at another rate is refused
  ## rather than resampled: `sox in.wav -r 48000 out.wav` first.
  let f = newFileStream(path, fmRead)
  if f == nil:
    raise newException(IOError, "input not found: " & path)
  defer: f.close()
  if f.readStr(4) != "RIFF" or (discard f.readUint32(); f.readStr(4) != "WAVE"):
    raise newException(RenderError, path & " is not a WAV file")
  var fmtTag, channels, bits: int
  var rate: int
  var haveFmt = false
  while not f.atEnd:
    let id = f.readStr(4)
    if id.len < 4: break
    let size = int(f.readUint32())
    if id == "fmt ":
      fmtTag = int(f.readUint16())
      channels = int(f.readUint16())
      rate = int(f.readUint32())
      discard f.readUint32()              # byte rate
      discard f.readUint16()              # block align
      bits = int(f.readUint16())
      if size > 16: f.setPosition(f.getPosition() + size - 16)
      haveFmt = true
    elif id == "data":
      if not haveFmt:
        raise newException(RenderError, path & ": data before fmt chunk")
      if rate != sr:
        raise newException(RenderError,
          path & " is " & $rate & " Hz; the engine runs at " & $sr &
          " Hz. Resample first: sox " & path & " -r " & $sr & " out.wav")
      if fmtTag == 0xFFFE: fmtTag = (if bits == 32: 3 else: 1)  # extensible
      let bytesPer = bits div 8
      if channels < 1 or bytesPer < 2 or bytesPer > 4 or
         (fmtTag == 1 and bits == 32) or (fmtTag == 3 and bits != 32) or
         fmtTag notin [1, 3]:
        raise newException(RenderError,
          path & ": unsupported format (need 16/24-bit PCM or 32-bit float)")
      let frames = size div (bytesPer * channels)
      result.left.setLen(frames)
      result.right.setLen(frames)
      var buf = newString(bytesPer * channels)
      for i in 0 ..< frames:
        if f.readData(addr buf[0], buf.len) != buf.len: break
        for ch in 0 ..< min(channels, 2):
          let o = ch * bytesPer
          var v: float64
          if fmtTag == 3:
            var x: float32
            copyMem(addr x, addr buf[o], 4)
            v = float64(x)
          elif bits == 16:
            let u = int16(uint16(buf[o].ord) or (uint16(buf[o + 1].ord) shl 8))
            v = float64(u) / 32768.0
          else:                           # 24-bit, sign-extended
            var u = int32(uint32(buf[o].ord) or (uint32(buf[o + 1].ord) shl 8) or
                          (uint32(buf[o + 2].ord) shl 16))
            if (u and 0x800000) != 0: u = u - 0x1000000
            v = float64(u) / 8388608.0
          if ch == 0: result.left[i] = v
          else: result.right[i] = v
        if channels == 1: result.right[i] = result.left[i]
      return
    else:
      f.setPosition(f.getPosition() + size + (size and 1))
  raise newException(RenderError, path & ": no data chunk")
