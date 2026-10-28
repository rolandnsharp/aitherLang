## The text aither writes, which is C (cli_output.c), as the Nim tests
## see it until they are C too (notes/c-port-2026-10-28.md): the same
## formatters over engine_types' objects, handed to C as its snapshots.

import std/os
import engine_types, analysis, render   # render.c: the low split cli_output.c reports

{.compile: "cli_output.c".}

const CliH = currentSourcePath().parentDir / "cli_output.h"

type
  CStatsSnap* {.importc: "StatsSnap", header: CliH, bycopy.} = object
    rms_l*, rms_r*, peak_l*, peak_r*: float64
    clips*: clong
    env_ring*: array[EnvBins, float32]
    env_bin_idx*: cint
  CPartSnap* {.importc: "PartSnap", header: CliH, bycopy.} = object
    name*: cstring
    state*: cint
    gain*, rms*, peak*: float64
    unheard*: cint
  CVoiceSnap* {.importc: "VoiceSnap", header: CliH, bycopy.} = object
    name*: cstring
    state*: cint
    gain*: float64
    parts*: ptr UncheckedArray[CPartSnap]
    nparts*: cint
  CVoiceStatsSnap {.importc: "VoiceStatsSnap", header: CliH, bycopy.} = object
    name: cstring
    is_master, state: cint
    gain: float64
    stats: CStatsSnap
  CCheckSnap {.importc: "CheckSnap", header: CliH, bycopy.} = object
    found: cint
    voice: CVoiceStatsSnap
    master: CStatsSnap
    parts: ptr UncheckedArray[CPartSnap]
    nparts: cint
    rolled_back, stuck: cint
    nan_events, runaway_events: clong
    retrying: cint
  CPartLevel {.importc: "PartLevel", header: CliH, bycopy.} = object
    name: cstring
    rms, peak, low, gain: float64
    unheard: cint

{.push header: CliH.}
proc cli_fmt_db(v: cdouble; o: cstring) {.importc.}
proc cli_meter_line(name: cstring; s: ptr CStatsSnap): cstring {.importc.}
proc cli_scope(target: cstring; found: cint; s: ptr CVoiceStatsSnap; n: cint): cstring {.importc.}
proc cli_parts(voice: cstring; found: cint; p: ptr CPartSnap; n: cint): cstring {.importc.}
proc cli_check(q: ptr CCheckSnap): cstring {.importc.}
proc cli_midi_header(info: cstring; active: cint): cstring {.importc.}
proc cli_voice_list(info: cstring; active: cint; v: ptr CVoiceSnap; n: cint): cstring {.importc.}
proc cli_spectrum(s: ptr CSummary): cstring {.importc.}
proc cli_audit(path: cstring; seconds, sr: cdouble; s: ptr CSummary): cstring {.importc.}
proc cli_part_levels(p: ptr CPartLevel; n: cint): cstring {.importc.}
{.pop.}
proc c_free(p: pointer) {.importc: "free", header: "<stdlib.h>".}

proc taken(p: cstring): string =
  result = $p
  c_free(p)

template first[T](s: seq[T]): ptr T = (if s.len > 0: unsafeAddr s[0] else: nil)

proc toC(s: StatsSnapshot): CStatsSnap =
  CStatsSnap(rms_l: s.rmsL, rms_r: s.rmsR, peak_l: s.peakL, peak_r: s.peakR,
             clips: clong(s.clips), env_ring: s.envRing, env_bin_idx: cint(s.envBinIdx))

proc toC(parts: seq[PartInfo]): seq[CPartSnap] =
  for p in parts:
    result.add CPartSnap(name: cstring(p.name), state: cint(ord(p.state)), gain: p.gain,
                         rms: p.rms, peak: p.peak, unheard: cint(p.unheard))

proc toC(v: VoiceStats): CVoiceStatsSnap =
  CVoiceStatsSnap(name: cstring(v.name), is_master: cint(v.isMaster), state: cint(ord(v.state)),
                  gain: v.gain, stats: toC(v.stats))

proc toC(s: SpectrumSummary): CSummary =
  result = CSummary(sr: s.sr, npeaks: cint(min(32, s.peaks.len)), centroid_hz: s.centroidHz,
                    rms_db: s.rmsDb, peak_db: s.peakDb, zero_crossing_rate: s.zeroCrossingRate,
                    fundamental_hz: s.fundamentalHz)
  for i in 0 ..< int(result.npeaks):
    result.peaks[i] = CPeak(freq_hz: s.peaks[i].freqHz, mag_db: s.peaks[i].magDb)

proc fmtDb*(v: float64): string =
  ## Linear to dB, one decimal, or "  -inf".
  var buf: array[16, char]
  cli_fmt_db(v, cast[cstring](addr buf[0]))
  $cast[cstring](addr buf[0])

proc formatMeterLine*(name: string; s: StatsSnapshot): string =
  var c = toC(s)
  taken(cli_meter_line(name, addr c))

proc formatScope*(target: string; q: ScopeQuery): string =
  var c: seq[CVoiceStatsSnap]
  for v in q.snapshots: c.add toC(v)
  taken(cli_scope(target, cint(q.found), first(c), cint(c.len)))

proc formatParts*(q: PartsQuery): string =
  let c = toC(q.parts)
  taken(cli_parts(q.voiceName, cint(q.found), first(c), cint(c.len)))

proc formatCheck*(q: CheckQuery): string =
  let parts = toC(q.parts)
  var c = CCheckSnap(found: cint(q.found), voice: toC(q.voice), master: toC(q.master),
                     parts: cast[ptr UncheckedArray[CPartSnap]](first(parts)),
                     nparts: cint(parts.len), rolled_back: cint(q.rolledBack), stuck: cint(q.stuck),
                     nan_events: clong(q.nanEvents), runaway_events: clong(q.runawayEvents),
                     retrying: cint(q.retrying))
  taken(cli_check(addr c))

proc formatMidiHeader*(m: MidiStatus): string =
  taken(cli_midi_header(m.portInfo, cint(m.active)))

proc formatVoiceList*(midi: MidiStatus; voices: seq[VoiceInfo]): string =
  var parts = newSeq[seq[CPartSnap]](voices.len)
  var c: seq[CVoiceSnap]
  for i, v in voices:
    parts[i] = toC(v.parts)
    c.add CVoiceSnap(name: cstring(v.name), state: cint(ord(v.state)), gain: v.gain,
                     parts: cast[ptr UncheckedArray[CPartSnap]](first(parts[i])),
                     nparts: cint(parts[i].len))
  taken(cli_voice_list(midi.portInfo, cint(midi.active), first(c), cint(c.len)))

proc formatSpectrum*(s: SpectrumSummary): string =
  var c = toC(s)
  taken(cli_spectrum(addr c))

proc formatAudit*(path: string; seconds, sr: float64; s: SpectrumSummary): string =
  var c = toC(s)
  taken(cli_audit(path, seconds, sr, addr c))

proc formatPartLevels*(parts: seq[PartLevel]): string =
  var c: seq[CPartLevel]
  for p in parts:
    c.add CPartLevel(name: cstring(p.name), rms: p.rms, peak: p.peak, low: p.low, gain: p.gain,
                     unheard: cint(p.unheard))
  taken(cli_part_levels(first(c), cint(c.len)))
