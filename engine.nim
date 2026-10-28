## The engine, which is C (engine.c), as Nim sees it until the CLI is C
## too (notes/c-port-2026-10-28.md): runEngine for aither.nim, the
## gestures for the tests, and the hooks engine.c calls for what is
## still Nim (the text, `try`'s render, MIDI).

import std/[os, net, strutils, math]
import voice, midi, render, ccompile
import engine_types, cli_output, analysis
export engine_types

{.compile: "engine.c".}
{.compile: "realtime.c".}

const EngineH = currentSourcePath().parentDir / "engine.h"

type
  CStatsSnap {.importc: "StatsSnap", header: EngineH, bycopy.} = object
    rms_l, rms_r, peak_l, peak_r: float64
    clips: clong
    env_ring: array[EnvBins, float32]
    env_bin_idx: cint
  CPartSnap {.importc: "PartSnap", header: EngineH, bycopy.} = object
    name: cstring
    state: cint
    gain, rms, peak: float64
    unheard: cint
  CVoiceSnap {.importc: "VoiceSnap", header: EngineH, bycopy.} = object
    name: cstring
    state: cint
    gain: float64
    parts: ptr UncheckedArray[CPartSnap]
    nparts: cint
  CVoiceStatsSnap {.importc: "VoiceStatsSnap", header: EngineH, bycopy.} = object
    name: cstring
    is_master, state: cint
    gain: float64
    stats: CStatsSnap
  CCheckSnap {.importc: "CheckSnap", header: EngineH, bycopy.} = object
    found: cint
    voice: CVoiceStatsSnap
    master: CStatsSnap
    parts: ptr UncheckedArray[CPartSnap]
    nparts: cint
    rolled_back, stuck: cint
    nan_events, runaway_events: clong
    retrying: cint

proc engine_init(stdlibSrc: cstring) {.importc, header: EngineH.}
proc engine_run(files: ptr cstring; n: cint; seconds: cdouble) {.importc, header: EngineH.}
proc engine_load(path: cstring; fade: cdouble): cstring {.importc, header: EngineH.}
proc engine_stop(name: cstring; fade: cdouble): cstring {.importc, header: EngineH.}
proc engine_silence(name: cstring; on, park: cint; fade: cdouble): cstring {.importc, header: EngineH.}
proc engine_solo(name: cstring; fade: cdouble): cstring {.importc, header: EngineH.}
proc engine_mark_stopped(name: cstring): cint {.importc, header: EngineH.}
proc engine_active_count(): cint {.importc, header: EngineH.}
proc engine_voices(o: ptr ptr UncheckedArray[CVoiceSnap]): cint {.importc, header: EngineH.}
proc engine_free_voices(v: ptr UncheckedArray[CVoiceSnap]; n: cint) {.importc, header: EngineH.}
proc c_malloc(n: csize_t): pointer {.importc: "malloc", header: "<stdlib.h>".}
proc c_free(p: pointer) {.importc: "free", header: "<stdlib.h>".}

engine_init(Stdlib)        # a const: the C keeps the pointer

proc toC(s: string): cstring =
  ## Text for the C side, which frees it.
  result = cast[cstring](c_malloc(csize_t(s.len + 1)))
  if s.len > 0: copyMem(result, unsafeAddr s[0], s.len)
  result[s.len] = '\0'

proc taken(p: cstring): string =
  ## An error from the C side: "" for none, and the C's copy freed.
  if p == nil: return ""
  result = $p
  c_free(p)

# ---------------------------------------------------- the C snapshots, as Nim

proc toNim(s: CStatsSnap): StatsSnapshot =
  StatsSnapshot(rmsL: s.rms_l, rmsR: s.rms_r, peakL: s.peak_l, peakR: s.peak_r,
                clips: int(s.clips), envRing: s.env_ring, envBinIdx: int(s.env_bin_idx))

proc toNim(p: ptr UncheckedArray[CPartSnap]; n: cint): seq[PartInfo] =
  for i in 0 ..< int(n):
    result.add PartInfo(name: $p[i].name, state: PartState(p[i].state), gain: p[i].gain,
                        rms: p[i].rms, peak: p[i].peak, unheard: p[i].unheard != 0)

proc toNim(v: CVoiceStatsSnap): VoiceStats =
  VoiceStats(name: $v.name, isMaster: v.is_master != 0, state: VoiceState(v.state),
             gain: v.gain, stats: toNim(v.stats))

proc toNim(v: ptr UncheckedArray[CVoiceSnap]; n: cint): seq[VoiceInfo] =
  for i in 0 ..< int(n):
    result.add VoiceInfo(name: $v[i].name, state: VoiceState(v[i].state), gain: v[i].gain,
                         parts: toNim(v[i].parts, v[i].nparts))

proc midiStatus*(): MidiStatus =
  MidiStatus(portInfo: lastConnectInfo, active: midiSubscriptionActive())

# ------------------------------------------------------------------ the hooks

proc fmtMeter(name: cstring; s: ptr CStatsSnap): cstring {.exportc: "aither_fmt_meter", cdecl.} =
  toC(formatMeterLine($name, toNim(s[])))

proc fmtList(v: ptr UncheckedArray[CVoiceSnap]; n: cint): cstring {.exportc: "aither_fmt_list", cdecl.} =
  toC(formatVoiceList(midiStatus(), toNim(v, n)))

proc fmtLevels(target: cstring; found: cint; s: ptr UncheckedArray[CVoiceStatsSnap];
               n: cint): cstring {.exportc: "aither_fmt_levels", cdecl.} =
  var q = ScopeQuery(found: found != 0)
  for i in 0 ..< int(n): q.snapshots.add toNim(s[i])
  toC(formatScope($target, q))

proc fmtParts(voice: cstring; found: cint; p: ptr UncheckedArray[CPartSnap];
              n: cint): cstring {.exportc: "aither_fmt_parts", cdecl.} =
  toC(formatParts(PartsQuery(voiceName: $voice, found: found != 0, parts: toNim(p, n))))

proc fmtCheck(q: ptr CCheckSnap): cstring {.exportc: "aither_fmt_check", cdecl.} =
  toC(formatCheck(CheckQuery(
    found: q.found != 0, voice: toNim(q.voice), master: toNim(q.master),
    parts: toNim(q.parts, q.nparts), rolledBack: q.rolled_back != 0, stuck: q.stuck != 0,
    nanEvents: int(q.nan_events), runawayEvents: int(q.runaway_events),
    retrying: q.retrying != 0)))

proc fmtSpectrum(mono: ptr UncheckedArray[float64]; n: clong;
                 sr: cdouble): cstring {.exportc: "aither_fmt_spectrum", cdecl.} =
  toC(formatSpectrum(analyze(toOpenArray(mono, 0, int(n) - 1), sr)))

proc tryReport(scratch: NativeVoice; seconds, skip, t0: cdouble; sr: cint;
               head: cstring): cstring {.exportc: "aither_try_report", cdecl.} =
  ## `try`'s render of the scratch voice the engine prepared, as the reply.
  let head = $head
  var r: Rendered
  try:
    r = renderVoice(scratch, seconds, int(sr), skip = skip, t0 = t0, feedInput = false)
  except CatchableError as e:
    return toC("ERR " & head & "\n  WOULD BREAK: " & e.msg &
               "\n  (sent live, this rolls back to the playing version)")
  var peak = 0.0
  for i in 0 ..< r.audio.left.len:
    peak = max(peak, max(abs(r.audio.left[i]), abs(r.audio.right[i])))
  var lines = @[head]
  if peak > 3.0:                     # engine.c's ROLLBACK_LIMIT
    # The spectrum of a blow-up is noise; the part table shows which
    # play it came from.
    lines.add "  WOULD BREAK: peak +" & $int(20.0 * log10(peak)) &
              " dBFS (sent live, anything past +9.5 dBFS rolls back)"
  else:
    let mono = monoMix(r.audio.left, r.audio.right)
    lines.add formatSpectrum(analyze(mono, float64(sr)))
  let parts = formatPartLevels(r.parts)
  if parts.len > 0: lines.add parts
  toC("OK\n" & lines.join("\n"))

proc midiCommand(argc: cint; argv: ptr UncheckedArray[cstring]): cstring
                {.exportc: "aither_midi_command", cdecl.} =
  let usage = "ERR usage: midi list|connect <spec>|disconnect"
  let r =
    case ($argv[0]).toLowerAscii()
    of "list":
      let body = midiListPorts()
      if body.len == 0: "OK (no MIDI ports)"
      else: "OK\n" & body.strip(leading = false)
    of "connect":
      if argc < 2: "ERR usage: midi connect <client:port>"
      elif midiConnect($argv[1]): "OK connected " & $argv[1]
      else: "ERR midi connect failed: " & $argv[1]
    of "disconnect":
      # We don't track single subscriptions, so the only offer is closing
      # and reopening the sequencer, which drops the thread too.
      midiShutdown()
      if midiOpen():
        midiStartThread()
        "OK midi reset"
      else: "ERR midi reopen failed"
    else: usage
  toC(r)

proc midiResubscribe(): cstring {.exportc: "aither_midi_resubscribe", cdecl.} =
  ## Resubscribe if the input thread had silently died; nil when nothing did.
  let s = midiResubscribeIfDropped()
  if s.len == 0: nil else: toC(s)

proc midiUp() {.exportc: "aither_midi_up", cdecl.} =
  if midiOpen():
    let who = midiAutoConnect()
    if who.len > 0:
      stderr.writeLine "[aither] connected to MIDI: ", who
    else:
      stderr.writeLine "[aither] MIDI ready, no input port auto-connected"
    midiStartThread()
  else:
    stderr.writeLine "[aither] MIDI unavailable (ALSA seq open failed)"

# ---------------------------------------------------------- the engine's API

proc runEngine*(files: seq[string]; seconds: float64) =
  ## The engine: the voices in `files` (and any sent later), mixed to
  ## stdout as float pairs. Never returns.
  var cs = newSeq[cstring](files.len + 1)
  for i, f in files: cs[i] = cstring(f)
  engine_run(addr cs[0], cint(files.len), seconds)

proc loadPatch*(filename: string; fadeIn: float64): string =
  taken(engine_load(filename, fadeIn))

proc stopVoice*(name: string; fade: float64): string = taken(engine_stop(name, fade))

proc setVoiceSilenced*(name: string; on, park: bool; fade: float64): string =
  ## `mute`/`park` a voice (on) or `unmute`/`unpark` it.
  taken(engine_silence(name, cint(on), cint(park), fade))

proc soloVoice*(name: string; fade: float64): string = taken(engine_solo(name, fade))

proc markStoppedForTest*(name: string): bool =
  ## Test-only: mark a voice inactive, as the audio thread does when its
  ## fade-out completes, to exercise the sweep without an audio thread.
  engine_mark_stopped(name) != 0

proc activeVoiceCount*(): int = int(engine_active_count())

proc voiceInfoList*(): seq[VoiceInfo] =
  ## Every voice and its parts: `list`.
  var v: ptr UncheckedArray[CVoiceSnap]
  let n = engine_voices(addr v)
  result = toNim(v, n)
  engine_free_voices(v, n)

# ----------------------------------------------------------------- the client

let SocketPath = getEnv("AITHER_SOCK", "/tmp/aither.sock")

proc queryCmd*(cmd: string): string =
  var sock = newSocket(AF_UNIX, SOCK_STREAM, IPPROTO_IP)
  try:
    sock.connectUnix(SocketPath)
    sock.send(cmd & "\n")
    result = sock.recv(65536).strip()
  except OSError:
    result = "error: engine not running? (" & SocketPath & ")"
  finally:
    sock.close()

proc sendCmd*(cmd: string) =
  echo queryCmd(cmd)
