## `play scope:` and `play beam:` draw the oscilloscope and are never
## heard, so `check`, `parts` and the audit/try part table keep them out
## of the level comparison instead of flagging every real part QUIET.

import std/strutils
import ../engine_types, ../cli_output

let scopePart = PartInfo(name: "scope", state: psPlaying, gain: 1.0, rms: 0.9, peak: 1.0)
let beamPart  = PartInfo(name: "beam",  state: psPlaying, gain: 1.0, rms: 1.0, peak: 1.0)
let didge     = PartInfo(name: "didge", state: psPlaying, gain: 1.0, rms: 0.05, peak: 0.2)
let sticks    = PartInfo(name: "sticks", state: psPlaying, gain: 1.0, rms: 0.04, peak: 0.3)
let faint     = PartInfo(name: "faint", state: psPlaying, gain: 1.0, rms: 0.0001, peak: 0.001)

let q = CheckQuery(found: true,
  voice: VoiceStats(name: "set", state: vsPlaying, gain: 1.0),
  parts: @[scopePart, beamPart, didge, sticks])
let check = formatCheck(q)
doAssert "quiet parts" notin check, check
# A really quiet part is still flagged, against the loudest heard part.
let q2 = CheckQuery(found: true,
  voice: VoiceStats(name: "set", state: vsPlaying, gain: 1.0),
  parts: @[scopePart, didge, faint])
doAssert "quiet parts" in formatCheck(q2) and "faint" in formatCheck(q2), formatCheck(q2)

let table = formatPartLevels(@[
  PartLevel(name: "scope", rms: 0.9, peak: 1.0, low: 0.0, gain: 1.0),
  PartLevel(name: "beam", rms: 1.0, peak: 1.0, low: 0.0, gain: 1.0),
  PartLevel(name: "didge", rms: 0.05, peak: 0.2, low: 0.2, gain: 1.0),
  PartLevel(name: "sticks", rms: 0.04, peak: 0.3, low: 0.1, gain: 1.0)])
doAssert "QUIET" notin table, table
doAssert "scope: drawn, not heard" in table, table

let parts = formatParts(PartsQuery(voiceName: "set", found: true,
                                   parts: @[scopePart, didge]))
doAssert "drawn, not heard" in parts, parts
echo "scope meters ok"
