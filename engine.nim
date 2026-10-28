## The engine, which is C (engine.c), as the Nim tests see it until they
## are C too (notes/c-port-2026-10-28.md): loading and the gestures, and
## the voice list for formatting.

import std/os
import voice, midi, render, analysis, cli_output, ccompile
import engine_types
export engine_types

{.compile: "engine.c".}
{.compile: "realtime.c".}

const EngineH = currentSourcePath().parentDir / "engine.h"

proc engine_init(stdlibSrc: cstring) {.importc, header: EngineH.}
proc engine_load(path: cstring; fade: cdouble): cstring {.importc, header: EngineH.}
proc engine_stop(name: cstring; fade: cdouble): cstring {.importc, header: EngineH.}
proc engine_silence(name: cstring; on, park: cint; fade: cdouble): cstring {.importc, header: EngineH.}
proc engine_solo(name: cstring; fade: cdouble): cstring {.importc, header: EngineH.}
proc engine_mark_stopped(name: cstring): cint {.importc, header: EngineH.}
proc engine_active_count(): cint {.importc, header: EngineH.}
proc engine_voices(o: ptr ptr UncheckedArray[CVoiceSnap]): cint {.importc, header: EngineH.}
proc engine_free_voices(v: ptr UncheckedArray[CVoiceSnap]; n: cint) {.importc, header: EngineH.}
proc c_free(p: pointer) {.importc: "free", header: "<stdlib.h>".}

engine_init(Stdlib)        # a const: the C keeps the pointer

proc taken(p: cstring): string =
  ## An error from the C side: "" for none, and the C's copy freed.
  if p == nil: return ""
  result = $p
  c_free(p)

proc loadPatch*(filename: string; fadeIn: float64): string =
  taken(engine_load(filename, fadeIn))

proc stopVoice*(name: string; fade: float64): string = taken(engine_stop(name, fade))

proc setVoiceSilenced*(name: string; on, park: bool; fade: float64): string =
  ## `mute`/`park` a voice (on) or `unmute`/`unpark` it.
  taken(engine_silence(name, cint(on), cint(park), fade))

proc soloVoice*(name: string; fade: float64): string = taken(engine_solo(name, fade))

proc markStoppedForTest*(name: string): bool =
  ## Mark a voice inactive, as the audio thread does when its fade-out
  ## completes, to exercise the sweep without an audio thread.
  engine_mark_stopped(name) != 0

proc activeVoiceCount*(): int = int(engine_active_count())

proc voiceInfoList*(): seq[VoiceInfo] =
  ## Every voice and its parts: `list`.
  var v: ptr UncheckedArray[CVoiceSnap]
  let n = engine_voices(addr v)
  for i in 0 ..< int(n):
    var parts: seq[PartInfo]
    for k in 0 ..< int(v[i].nparts):
      let p = v[i].parts[k]
      parts.add PartInfo(name: $p.name, state: PartState(p.state), gain: p.gain, rms: p.rms,
                         peak: p.peak, unheard: p.unheard != 0)
    result.add VoiceInfo(name: $v[i].name, state: VoiceState(v[i].state), gain: v[i].gain,
                         parts: parts)
  engine_free_voices(v, n)

proc midiStatus*(): MidiStatus =
  MidiStatus(portInfo: lastConnectInfo(), active: midiSubscriptionActive())
