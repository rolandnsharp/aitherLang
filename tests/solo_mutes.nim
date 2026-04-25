## solo X must mute the other voices, not stop them. The pre-fix
## behaviour faded other voices' fadeGain to 0 and then marked them
## inactive ([stopped]), which `unmute` couldn't revive — the operator
## had to re-`send` and lose running state. After the fix, `solo X`
## sets `muted = true` on the others (instant, recoverable) and
## clears `muted` on the targeted voice.
##
## Test path: write two trivial patches to /tmp, load each via
## engine.loadPatch, call soloVoice, parse listVoices output.

import std/[os, strutils]
import ../engine, ../midi, ../cli_output

const PatchA = """
play tone:
  sin(TAU * phasor(440)) * 0.1
tone
"""

const PatchB = """
play tone:
  sin(TAU * phasor(220)) * 0.1
tone
"""

let pathA = "/tmp/aither_test_solo_a.aither"
let pathB = "/tmp/aither_test_solo_b.aither"
writeFile(pathA, PatchA)
writeFile(pathB, PatchB)

# Load both. fadeIn=0 → fadeGain starts at 1.0.
doAssert engine.loadPatch(pathA, 0.0).len == 0
doAssert engine.loadPatch(pathB, 0.0).len == 0

# Sanity: both should be playing.
let beforeSolo = formatVoiceList(midiStatus(), voiceInfoList())
doAssert "aither_test_solo_a [playing]" in beforeSolo,
  "before solo, voice A should be [playing]:\n" & beforeSolo
doAssert "aither_test_solo_b [playing]" in beforeSolo,
  "before solo, voice B should be [playing]:\n" & beforeSolo

# Solo A.
doAssert engine.soloVoice("aither_test_solo_a", 0.0).len == 0

let afterSolo = formatVoiceList(midiStatus(), voiceInfoList())
doAssert "aither_test_solo_a [playing]" in afterSolo,
  "after solo, voice A should be [playing]:\n" & afterSolo
doAssert "aither_test_solo_b [muted]" in afterSolo,
  "after solo, voice B should be [muted] not [stopped]:\n" & afterSolo
doAssert "[stopped]" notin afterSolo,
  "no voice should be [stopped] after solo:\n" & afterSolo

# Unmute B — should return to [playing], proving the muted state is
# recoverable. The pre-fix behaviour left B [stopped] and unmute had
# no effect.
doAssert engine.setMute("aither_test_solo_b", false).len == 0

let afterUnmute = formatVoiceList(midiStatus(), voiceInfoList())
doAssert "aither_test_solo_b [playing]" in afterUnmute,
  "after unmute, voice B should be back to [playing]:\n" & afterUnmute

# Solo a different voice — A should now go to muted.
doAssert engine.soloVoice("aither_test_solo_b", 0.0).len == 0
let afterSolo2 = formatVoiceList(midiStatus(), voiceInfoList())
doAssert "aither_test_solo_a [muted]" in afterSolo2,
  "after solo B, A should be [muted]:\n" & afterSolo2
doAssert "aither_test_solo_b [playing]" in afterSolo2,
  "after solo B, B should be [playing]:\n" & afterSolo2

removeFile(pathA)
removeFile(pathB)
echo "solo_mutes ok"
