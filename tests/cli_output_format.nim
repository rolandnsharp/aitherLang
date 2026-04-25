## Pin the byte format of every CLI line that callers grep, parse, or
## mirror. The Phase 0 refactor moved these formatters out of engine
## into cli_output; if a future cleanup drifts the spacing or rounding,
## this test fails loudly instead of silently breaking external scripts.

import std/[strutils]
import ../engine_types, ../cli_output

# Helper: build a StatsSnapshot with chosen RMS/peak/clips values.
# envRing left at zeros so the sparkline section is constant noise·.
proc snap(rmsL, rmsR, peakL, peakR: float64; clips = 0): StatsSnapshot =
  StatsSnapshot(
    rmsL: rmsL, rmsR: rmsR,
    peakL: peakL, peakR: peakR,
    clips: clips,
    envBinIdx: 0)

# --- 1. fmtDb rounds to 1 decimal, returns "  -inf" for ~0.
block fmtDbCases:
  doAssert fmtDb(1.0) == "0.0", fmtDb(1.0)
  doAssert fmtDb(0.5) == "-6.0", fmtDb(0.5)
  doAssert fmtDb(0.0) == "  -inf", fmtDb(0.0)
  doAssert fmtDb(1e-10) == "  -inf", fmtDb(1e-10)

# --- 2. formatMeterLine: name + rms + peak + clips + 20-char env spark.
block meterLine:
  let line = formatMeterLine("foo", snap(0.5, 0.5, 0.7, 0.7, clips = 3))
  doAssert line.startsWith("foo  rms=-6.0dB peak=-3.1dB clips=3 env="),
    "meter line preamble: " & line
  # Sparkline is EnvBins glyphs (the · is multi-byte so byte len > 20).
  let spark = line.split("env=")[1]
  doAssert spark.len >= EnvBins,
    "spark expected ≥EnvBins bytes, got " & $spark.len & ": " & spark

# --- 3. formatScope with not-found returns the historic message.
block scopeNotFound:
  let q = ScopeQuery(found: false)
  doAssert formatScope("nope", q) == "not found: nope"

# --- 4. formatScope master block — header is literal "master".
block scopeMaster:
  let q = ScopeQuery(found: true,
    snapshots: @[VoiceStats(name: "master", isMaster: true,
                            stats: snap(0.1, 0.1, 0.2, 0.2))])
  let s = formatScope("master", q)
  doAssert s.startsWith("master\n"), "master block must lead with 'master\\n'"
  doAssert "RMS   L" in s, "scope must include 'RMS   L' aligned column"
  doAssert "peak  L" in s, "scope must include 'peak  L' aligned column"
  doAssert "clips=" in s

# --- 5. formatScope per-voice block has state + gain in header.
block scopeVoice:
  let q = ScopeQuery(found: true,
    snapshots: @[VoiceStats(name: "bass", isMaster: false,
                            state: vsPlaying, gain: 0.75,
                            stats: snap(0.3, 0.3, 0.5, 0.5, clips = 1))])
  let s = formatScope("bass", q)
  doAssert s.startsWith("bass  playing  gain=0.75\n"),
    "voice header form: " & s

# --- 6. formatParts: not-found, no-parts, normal cases.
block partsCases:
  doAssert formatParts(PartsQuery(found: false, voiceName: "x")) ==
    "not found: x"
  doAssert formatParts(PartsQuery(found: true, voiceName: "y", parts: @[])) ==
    "(no parts)"
  let q = PartsQuery(found: true, voiceName: "drums", parts: @[
    PartInfo(name: "kick", state: psPlaying, gain: 1.0),
    PartInfo(name: "snare", state: psSilent, gain: 0.0)])
  let txt = formatParts(q)
  doAssert txt == "drums\n  kick  [playing]  gain=1.00\n  snare  [silent]  gain=0.00",
    "parts format: " & txt

# --- 7. formatMidiHeader: empty input → empty string; populated → tagged.
block midiHeader:
  doAssert formatMidiHeader(MidiStatus(portInfo: "", active: false)) == ""
  doAssert formatMidiHeader(MidiStatus(portInfo: "Minilab3 (28:0)", active: true)) ==
    "MIDI: Minilab3 (28:0) [active]"
  doAssert formatMidiHeader(MidiStatus(portInfo: "Minilab3 (28:0)", active: false)) ==
    "MIDI: Minilab3 (28:0) [DROPPED]"

# --- 8. formatVoiceList: empty → "(no voices)"; populated → state + gain
#     per voice + indented per-part lines (with " (muted)" for gain≤0).
block voiceList:
  doAssert formatVoiceList(MidiStatus(), @[]) == "(no voices)"
  let voices = @[
    VoiceInfo(name: "bass", state: vsPlaying, gain: 1.0,
      parts: @[PartInfo(name: "low", state: psPlaying, gain: 1.0),
               PartInfo(name: "high", state: psSilent, gain: 0.0)])]
  let txt = formatVoiceList(MidiStatus(), voices)
  doAssert txt == "bass [playing] gain=1.00\n" &
                  "  low      gain=1.00\n" &
                  "  high     gain=0.00 (muted)",
    "voice list:\n" & txt
  # MIDI header prepends when present.
  let withMidi = formatVoiceList(
    MidiStatus(portInfo: "Minilab (28:0)", active: true), voices)
  doAssert withMidi.startsWith("MIDI: Minilab (28:0) [active]\n"),
    "midi header should lead: " & withMidi

echo "cli_output_format ok"
