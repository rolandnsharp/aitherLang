## Voice slot leak regression. Pre-fix: stopped voices stayed in the
## slots array even after their fade-out completed, so a session that
## stopped 16 voices without re-sending hit "voice limit reached" on
## the 17th send despite no voice being audible. Fix: loadPatch sweeps
## inactive slots out before checking the MaxVoices limit.

import std/[os, strutils]
import ../engine

const Patch = """
play tone:
  sin(TAU * phasor(440)) * 0.05
tone
"""

# Load 12 voices with distinct names. Use the basename = filename
# convention loadPatch enforces (one .aither = one voice slot).
var paths: seq[string] = @[]
for i in 0 ..< 12:
  let p = "/tmp/aither_test_slot_" & $i & ".aither"
  writeFile(p, Patch)
  paths.add p
  doAssert loadPatch(p, 0.0).len == 0,
    "load #" & $i & " should succeed"

doAssert activeVoiceCount() == 12,
  "12 voices should be active, got " & $activeVoiceCount()

# Stop all 12 and force their callback-side fade-out to complete via
# the test helper (no audio thread runs in this test harness).
for i in 0 ..< 12:
  let name = "aither_test_slot_" & $i
  doAssert stopVoice(name, 0.0).len == 0
  doAssert markStoppedForTest(name)

doAssert activeVoiceCount() == 0,
  "after force-stopping all 12, active count should be 0, got " &
  $activeVoiceCount()

# Now load a NEW voice. The sweep should recycle the stopped slots so
# this succeeds. Pre-fix it returned "voice limit reached" because
# slotCount stayed at 12 forever.
let newPath = "/tmp/aither_test_slot_new.aither"
writeFile(newPath, Patch)
let result = loadPatch(newPath, 0.0)
doAssert result.len == 0,
  "new voice after stopping 12 should succeed, got: " & result

let listing = listVoices()
doAssert "aither_test_slot_new" in listing,
  "new voice should appear in list: " & listing
doAssert "aither_test_slot_0" notin listing,
  "stopped voices should be swept from list: " & listing

for p in paths: removeFile(p)
removeFile(newPath)
echo "slot_sweep ok"
