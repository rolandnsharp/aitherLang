## MIDI subscription bookkeeping smoke test. Real ALSA-loop tests
## need hardware, but the no-connect paths and the listVoices header
## format can be verified without a sequencer.
##
## Pre-fix: the engine had no notion of "subscription dropped" — when
## the input thread died mid-session (issue 4c) the operator just
## reached for the keys, heard nothing, and had to guess whether the
## patch was wrong or MIDI was gone. This test pins the no-MIDI path
## (silent in `list`, no false "DROPPED" line) and the recovery API
## contract (returns "" when nothing to do).

import std/[os, strutils]
import ../engine, ../midi

# Before any connect, the resubscribe call should be a no-op.
doAssert midiResubscribeIfDropped() == "",
  "no-connect resubscribe should return empty"

# And subscriptionActive should be false (we never started a thread).
doAssert not midiSubscriptionActive(),
  "subscriptionActive should be false before any startThread"

# `list` output with no voices and no MIDI should be the literal
# "(no voices)" — adding the MIDI header in the no-connect case
# would clutter the output.
let beforeAny = listVoices()
doAssert "MIDI:" notin beforeAny,
  "listVoices should not show MIDI header when nothing was connected: " & beforeAny

# Load a voice → list output now mentions the voice but still no MIDI.
const Patch = """
play tone:
  sin(TAU * phasor(440)) * 0.05
tone
"""
const Path = "/tmp/aither_test_midi_resilience.aither"
writeFile(Path, Patch)
doAssert loadPatch(Path, 0.0).len == 0
let afterLoad = listVoices()
doAssert "MIDI:" notin afterLoad,
  "MIDI header still hidden when never connected: " & afterLoad
doAssert "aither_test_midi_resilience" in afterLoad,
  "loaded voice should appear: " & afterLoad

removeFile(Path)
echo "midi_resilience ok"
