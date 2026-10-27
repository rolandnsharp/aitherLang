## A `def rec` past its block's deadline gives up: the block is silent,
## every sample counted bad, `stuck` set and the def named for the log.
## (What recursion computes is in tests/lang/state.aither.)

import ../voice

block guard:
  let v = newVoice(48000.0)
  v.load("""
def rec forever(x): forever(x + 1)
forever(0)
""", 48000.0, "patches/stuck.aither")
  let hdr = cast[ptr VoiceHeader](v.state)
  hdr.fadeGain = 1.0
  hdr.inBuf = nil
  hdr.deadline = 1e-9            # long gone
  var l = newSeq[float64](16)
  var r = newSeq[float64](16)
  v.tickBlock(16, l[0].addr, r[0].addr)
  doAssert hdr.stuck == 1, "stuck " & $hdr.stuck
  doAssert hdr.bad == 16, "bad " & $hdr.bad
  for i in 0 ..< 16:
    doAssert l[i] == 0.0 and r[i] == 0.0, "sample " & $i & ": " & $l[i]
  doAssert $hdr.stuckAt == "forever (patches/stuck.aither:1)", $hdr.stuckAt

echo "recursion_guard ok"
