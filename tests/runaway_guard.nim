## Runaway guard: the engine mutes a voice whose output passes ±8
## (+18 dBFS). This pins the threshold from both sides: a genuinely
## unstable patch must trip it, and loud-but-healthy shipped patches
## (bush_doof runs close to 0 dBFS) must never trip it.

import ../parser, ../voice, ../codegen, ../engine_types

const Stdlib = staticRead("../stdlib.aither")
const SR = 48000

proc load(src, name: string): NativeVoice =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(src)
  setSource(userAst, name)
  let prog = Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)
  result = newVoice(float64(SR))
  result.load(prog, float64(SR))

# Returns the sample index where the guard trips, or -1.
proc firstRunaway(v: NativeVoice; seconds: float64): int =
  for i in 0 ..< int(seconds * float64(SR)):
    let s = v.tick(float64(i) / float64(SR))
    if isRunaway(s.l, s.r): return i
  -1

# Unstable: grows 0.1% per sample, passes 8 after ~2100 samples.
const Unstable = """
$x = 0.01
$x = $x * 1.001
$x
"""
let hit = firstRunaway(load(Unstable, "unstable"), 1.0)
doAssert hit >= 0, "unstable patch never tripped the runaway guard"

doAssert not isRunaway(1.0, -1.0), "full scale is not a runaway"
doAssert not isRunaway(RunawayLimit, 0.0), "limit itself is allowed"
doAssert isRunaway(0.0, -RunawayLimit * 1.01)
doAssert isRunaway(Inf, 0.0)

const Healthy = [
  ("patches/bush_doof.aither", staticRead("../patches/bush_doof.aither")),
  ("patches/qigong_drift.aither", staticRead("../patches/qigong_drift.aither")),
]
for (name, src) in Healthy:
  let at = firstRunaway(load(src, name), 3.0)
  doAssert at < 0, name & " tripped the runaway guard at sample " & $at

echo "runaway guard ok (unstable tripped at sample ", hit, ")"
