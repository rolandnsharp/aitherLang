## The background -O2 build: same C as the TCC fast path, compiled by cc
## and linked in memory by TCC. It must drop into the loaded voice (same
## state layout), sound the same, and be refused once the voice has been
## reloaded. Also prints the speedup, since that's the point of it.

import std/[monotimes, times, os]
import ../parser, ../voice, ../codegen

const Stdlib = staticRead("../stdlib.aither")
const SR = 48000

proc program(src, name: string): Node =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(src)
  setSource(userAst, name)
  Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

proc render(v: NativeVoice; fromSample, n: int): seq[float64] =
  for i in fromSample ..< fromSample + n:
    result.add v.tick(float64(i) / float64(SR)).l

let path = if paramCount() >= 1: paramStr(1) else: "patches/bush_doof.aither"
let src = readFile(path)

# Two identical voices, run in lockstep; then upgrade one mid-stream.
let a = newVoice(float64(SR))
a.load(program(src, path), float64(SR))
let b = newVoice(float64(SR))
b.load(program(src, path), float64(SR))

let warm = SR div 2
discard a.render(0, warm)
discard b.render(0, warm)

let (lib, fn) = compileOptimized(b.csrc, b.stateSize)
doAssert b.upgrade(b.gen, lib, fn), "upgrade refused for the current generation"
doAssert b.optimized

let n = SR * 3
var t0 = getMonoTime()
let slow = a.render(warm, n)
let tccTime = getMonoTime() - t0
t0 = getMonoTime()
let fast = b.render(warm, n)
let optTime = getMonoTime() - t0

# cc may fuse multiply-adds, so allow rounding-level drift, nothing audible.
var worst = 0.0
for i in 0 ..< n: worst = max(worst, abs(slow[i] - fast[i]))
doAssert worst < 1e-6, "optimized output drifted by " & $worst

# A stale build must not replace newer code.
b.load(program(src, path), float64(SR))
doAssert not b.upgrade(b.gen - 1, lib, fn), "stale upgrade was accepted"
doAssert not b.optimized

let budget = 3.0                       # seconds of audio rendered
echo "ok ", path, ": max drift ", worst
echo "  tcc: ", tccTime.inMilliseconds, " ms (",
     int(tccTime.inMilliseconds.float / (budget * 10)), "% of one core)"
echo "  -O2: ", optTime.inMilliseconds, " ms (",
     int(optTime.inMilliseconds.float / (budget * 10)), "% of one core)"
