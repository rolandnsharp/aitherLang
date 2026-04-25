## Step 3b regression test — a play-local `let` or `$state` whose name
## matches a play block must error at codegen time with a clear message,
## not crash later with a misleading type mismatch.
##
## Live-jam pain: writing
##   play bass: ...
##   play bagpipes:
##     let bass = osc(saw, 55.0)   # meant a local; actually shadows play bass
##     bass |> lpf(...)            # crashes: lpf receives a stereo value
## wasted minutes tracking down a "stereo received" error that really
## meant "you accidentally reused the play name."

import std/[strutils]
import ../parser, ../codegen

const Stdlib = staticRead("../stdlib.aither")

proc prog(src: string): Node =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(src)
  setSource(userAst, "patches/shadow.aither")
  Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

proc mustFail(src: string; wantSubstr: string) =
  let ast = prog(src)
  try:
    discard generate(ast, "patches/shadow.aither")
    doAssert false, "expected compile error for: " & src
  except CatchableError as e:
    doAssert wantSubstr.toLowerAscii() in e.msg.toLowerAscii(),
      "error should mention '" & wantSubstr & "', got: " & e.msg

# 3b.1: let inside a play shadowing ANOTHER play's name
mustFail("""
play bass:
  sin(TAU * phasor(55)) * 0.3
play bagpipes:
  let bass = osc(saw, 55.0)
  bass
bass + bagpipes
""", "play block")

# 3b.2: same, with forward reference (the shadowing play appears before
# the shadowed play in source). The pre-scan must still catch it.
mustFail("""
play bagpipes:
  let bass = osc(saw, 55.0)
  bass
play bass:
  sin(TAU * phasor(55)) * 0.3
bass + bagpipes
""", "play block")

# 3b.3: $state inside a play shadowing a play name
mustFail("""
play bass:
  sin(TAU * phasor(55)) * 0.3
play other:
  $bass = 0.0
  $bass = $bass + 0.01
  $bass
bass + other
""", "play block")

# 3b.4: non-shadowing let is still fine — `let localThing = ...` inside a
# play block alongside a `play other:` whose name doesn't collide.
let ok = """
play bass:
  sin(TAU * phasor(55)) * 0.3
play lead:
  let note = 440.0
  sin(TAU * phasor(note)) * 0.3
bass + lead
"""
discard generate(prog(ok), "patches/ok.aither")

echo "shadow_play_name ok"
