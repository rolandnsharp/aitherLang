## Pure formatters for everything aither writes to stdout. Engine
## returns data structs (defined in engine_types.nim); this module
## turns them into bytes. No I/O here, no engine state.
##
## Single source of truth for line widths, dB rounding, table
## alignment, sparkline glyphs.

import std/[math, strutils]
import engine_types, analysis

const SparkBlocks = [" ", "▁", "▂", "▃", "▄",
                     "▅", "▆", "▇", "█"]

proc dBOf(v: float64): float64 {.inline.} =
  if v < 1e-9: -180.0 else: 20.0 * log10(v)

proc envBar(v: float32): string =
  let f = v.float64
  if f < 1e-5: return SparkBlocks[0]
  let db = dBOf(f)
  let idx = clamp(int((db + 60.0) / 60.0 * 8.0 + 0.5), 0, 8)
  SparkBlocks[idx]

proc fmtDb*(v: float64): string =
  ## Linear → dB string. Mirrors the historic engine fmtDb byte-for-byte.
  if v < 1e-9: return "  -inf"
  formatFloat(dBOf(v), ffDecimal, 1)

proc voiceStateWord(s: VoiceState): string =
  case s
  of vsStopped:    "stopped"
  of vsMuted:      "muted"
  of vsFadingOut:  "fading-out"
  of vsFadingIn:   "fading-in"
  of vsPlaying:    "playing"

proc partStateWord(s: PartState): string =
  case s
  of psSilent:     "silent"
  of psFadingOut:  "fading-out"
  of psFadingIn:   "fading-in"
  of psPlaying:    "playing"

proc formatMeterLine*(name: string; s: StatsSnapshot): string =
  ## Compact `name  rms=X dB peak=Y dB clips=N env=<bars>` summary
  ## tacked onto `send` responses. Sparkline uses · for blank bins.
  var spark = ""
  for k in 0 ..< EnvBins:
    let v = s.envRing[(s.envBinIdx + k) mod EnvBins]
    spark &= (if v.float64 < 1e-5: "·" else: envBar(v))
  let rms = max(s.rmsL, s.rmsR)
  let peak = max(s.peakL, s.peakR)
  name & "  rms=" & fmtDb(rms).strip() & "dB peak=" &
    fmtDb(peak).strip() & "dB clips=" & $s.clips & " env=" & spark

proc formatStatsBlock*(header: string; s: StatsSnapshot): string =
  ## Multi-line scope block with RMS/peak/clips/envelope. Used by the
  ## per-voice and master sections of `formatScope`.
  var spark = ""
  for k in 0 ..< EnvBins:
    spark &= envBar(s.envRing[(s.envBinIdx + k) mod EnvBins])
  header & "\n" &
  "  RMS   L " & fmtDb(s.rmsL) & " dB   R " & fmtDb(s.rmsR) & " dB\n" &
  "  peak  L " & fmtDb(s.peakL) & " dB   R " & fmtDb(s.peakR) &
       " dB   clips=" & $s.clips & "\n" &
  "  env   " & spark

proc voiceStatsBlock(v: VoiceStats): string =
  let header =
    if v.isMaster: "master"
    else:
      v.name & "  " & voiceStateWord(v.state) & "  gain=" &
      formatFloat(v.gain, ffDecimal, 2)
  formatStatsBlock(header, v.stats)

proc formatScope*(target: string; q: ScopeQuery): string =
  ## Renders the result of a `scope [target]` query. Empty/`*`/`master`
  ## paths and the named-voice path produce the historic byte sequence.
  if not q.found:
    return "not found: " & target
  var blocks: seq[string]
  for v in q.snapshots:
    blocks.add voiceStatsBlock(v)
  blocks.join("\n\n")

proc formatParts*(q: PartsQuery): string =
  if not q.found:
    return "not found: " & q.voiceName
  if q.parts.len == 0:
    return "(no parts)"
  var lines: seq[string]
  for p in q.parts:
    lines.add "  " & p.name & "  [" & partStateWord(p.state) & "]  gain=" &
              formatFloat(p.gain, ffDecimal, 2)
  q.voiceName & "\n" & lines.join("\n")

proc formatMidiHeader*(m: MidiStatus): string =
  ## Single line for the MIDI header in `list`. Empty string when
  ## nothing was ever connected (caller decides whether to skip).
  if m.portInfo.len == 0: return ""
  let state = if m.active: "active" else: "DROPPED"
  "MIDI: " & m.portInfo & " [" & state & "]"

proc trimDot(s: string): string =
  # `formatFloat(x, ffDecimal, 0)` returns "1234." with a trailing dot;
  # strip it so freq strings read as "1234 Hz" not "1234. Hz".
  if s.endsWith("."): s[0 ..< s.len - 1] else: s

proc fmtHz(v: float64): string =
  if v >= 1000.0: trimDot(formatFloat(v, ffDecimal, 0)) & " Hz"
  else: formatFloat(v, ffDecimal, 1) & " Hz"

proc formatSpectrum*(s: SpectrumSummary): string =
  ## Multi-line spectral summary. Used by `audit` and `spectrum`.
  ## Pure function on a SpectrumSummary; analysis.nim does the math.
  var lines: seq[string]
  lines.add "  RMS:        " & formatFloat(s.rmsDb, ffDecimal, 1) &
            " dB    Peak: " & formatFloat(s.peakDb, ffDecimal, 1) & " dB"
  let fund =
    if s.fundamentalHz <= 0.0: "(no clear pitch — non-tonal)"
    else: fmtHz(s.fundamentalHz)
  lines.add "  Fundamental: " & fund
  lines.add "  Centroid:   " & fmtHz(s.centroidHz)
  lines.add "  ZCR:        " &
            trimDot(formatFloat(s.zeroCrossingRate, ffDecimal, 0)) &
            " / sec"
  lines.add "  Top peaks:"
  for i, p in s.peaks:
    let idx = align($(i + 1), 2)
    let freq = align(fmtHz(p.freqHz), 12)
    let mag = formatFloat(p.magDb, ffDecimal, 1) & " dB"
    lines.add "    " & idx & ".  " & freq & "  " & mag
  lines.join("\n")

proc formatAudit*(path: string; seconds, sr: float64;
                  s: SpectrumSummary): string =
  ## `audit` adds a one-line header above the spectrum summary so the
  ## reader sees what was rendered. seconds + sr come from the renderer.
  "audit: " & path & " (" & formatFloat(seconds, ffDecimal, 1) &
    "s @ " & trimDot(formatFloat(sr, ffDecimal, 0)) & " Hz)\n" &
    formatSpectrum(s)

proc formatVoiceList*(midi: MidiStatus; voices: seq[VoiceInfo]): string =
  var lines: seq[string]
  let head = formatMidiHeader(midi)
  if head.len > 0: lines.add head
  for v in voices:
    lines.add v.name & " [" & voiceStateWord(v.state) & "] gain=" &
              formatFloat(v.gain, ffDecimal, 2)
    for p in v.parts:
      let suffix = if p.gain <= 0.0: " (muted)" else: ""
      lines.add "  " & alignLeft(p.name, 8) & " gain=" &
                formatFloat(p.gain, ffDecimal, 2) & suffix
  if lines.len == 0: "(no voices)" else: lines.join("\n")
