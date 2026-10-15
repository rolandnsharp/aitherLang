## `check`, `parts` and `audit` say when a part plays and isn't heard.
##
## Static: a part the final expression never reaches (directly, or
## through another part, or inside a call) is NOT IN THE FINAL
## EXPRESSION, known at compile time. No final expression: every part
## is heard. Dynamic: the loudest heard part is healthy but the voice's
## output is 30 dB or more below it: QUIET VOICE.

import std/[strutils, os]
import ../parser, ../codegen, ../render, ../engine_types, ../cli_output

proc unheardOf(src: string): seq[string] =
  generateFull(parseProgram(src), "", 48000.0).unheard

# Forgotten on the last line.
doAssert unheardOf("play drone:\n  0.1\nplay sticks:\n  0.2\n\ndrone\n") == @["sticks"]
# Reached through another part, and inside a call.
doAssert unheardOf("play drone:\n  0.1\nplay sticks:\n  0.2\nplay mix:\n  drone + sticks\n\nabs(mix)\n").len == 0
# No final expression: all parts play.
doAssert unheardOf("play drone:\n  0.1\nplay sticks:\n  0.2\n").len == 0
# A name that is a let, not a part, doesn't count as reaching a part.
doAssert unheardOf("play drone:\n  0.1\nlet sticks = 0.2\n\ndrone + sticks\n").len == 0

# audit's part table carries the flag.
block auditTable:
  let path = getTempDir() / "aither_unheard_" & $getCurrentProcessId() & ".aither"
  writeFile(path, "play drone:\n  0.1\nplay sticks:\n  0.2\n\ndrone\n")
  let r = renderPatchFull(path, 0.01)
  removeFile(path)
  var sticksHeard = true
  for p in r.parts:
    if p.name == "sticks": sticksHeard = not p.unheard
  doAssert not sticksHeard, "audit should flag sticks as unheard"

# The dynamic warning in check's report.
block quietVoice:
  let loud = PartInfo(name: "drone", state: psPlaying, gain: 1.0, rms: 0.3, peak: 0.4, unheard: false)
  let q = CheckQuery(found: true,
    voice: VoiceStats(name: "v", state: vsPlaying, gain: 1.0,
                      stats: StatsSnapshot(rmsL: 0.001, rmsR: 0.001, peakL: 0.001, peakR: 0.001)),
    parts: @[loud])
  let s = formatCheck(q)
  doAssert "QUIET VOICE" in s, s
  # A healthy voice says nothing.
  var ok = q
  ok.voice.stats = StatsSnapshot(rmsL: 0.3, rmsR: 0.3, peakL: 0.4, peakR: 0.4)
  doAssert "QUIET VOICE" notin formatCheck(ok)
  # The static note.
  var missing = q
  missing.parts.add PartInfo(name: "sticks", state: psPlaying, gain: 1.0, rms: 0.2, peak: 0.3, unheard: true)
  doAssert "NOT IN THE FINAL EXPRESSION (computed, never heard): sticks" in formatCheck(missing)
  doAssert "NOT IN THE FINAL EXPRESSION" in formatPartLevels(@[
    PartLevel(name: "sticks", rms: 0.2, peak: 0.3, gain: 1.0, unheard: true)])

echo "check_unheard ok"
