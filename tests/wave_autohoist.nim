## Step 3a regression test — a numeric-literal array bound via `let`
## inside a play block must be usable as the wave() data arg without
## a manual top-level hoist.
##
## Live-jam pain: writing
##   play bag:
##     let pipeNotes = [220.0, 246.94, 277.18, ...]
##     wave(tempo, pipeNotes)
## errored with "array value can't appear here". The workaround was
## to move the array above the play — annoying mid-set.
##
## Also verify the error path: if the array's elements aren't all
## numeric literals (i.e. would need runtime evaluation), the error
## must be clear that wave() arrays must be compile-time constants,
## NOT the misleading "array value can't appear here" that used to
## fire for any non-top-level array.

import std/[strutils]
import ../parser, ../voice, ../codegen

# 3a.1: play-local numeric-literal array, used via wave().
const Local = """
play pipes:
  let pipeNotes = [220.0, 246.94, 277.18, 293.66]
  let f = wave(1.0, pipeNotes)
  sin(TAU * phasor(f)) * 0.3
pipes
"""
block autoHoist:
  let prog = parseProgram(Local)
  let v = newVoice(48000.0)
  v.load(prog, 48000.0)
  # Run a handful of ticks; the wave should at minimum produce finite
  # non-silent output.
  var peak = 0.0
  for i in 0 ..< 2400:
    let s = v.tick(float64(i) / 48000.0)
    doAssert s.l == s.l, "NaN at sample " & $i
    peak = max(peak, abs(s.l))
  doAssert peak > 0.0, "auto-hoisted wave produced silence"

# 3a.2: two play blocks with the same local array name must not collide
# — each play's array should be independently visible from its own body.
# Length 3 so both arrays go through the auto-hoist path (length 2 keeps
# stereo-pair semantics).
const TwoPlays = """
play a:
  let notes = [110.0, 220.0, 330.0]
  let f = wave(1.0, notes)
  sin(TAU * phasor(f)) * 0.3
play b:
  let notes = [330.0, 440.0, 550.0]
  let f = wave(1.0, notes)
  sin(TAU * phasor(f)) * 0.3
a + b
"""
block noCollide:
  let prog = parseProgram(TwoPlays)
  let v = newVoice(48000.0)
  v.load(prog, 48000.0)
  var peak = 0.0
  for i in 0 ..< 2400:
    let s = v.tick(float64(i) / 48000.0)
    peak = max(peak, abs(s.l))
  doAssert peak > 0.0

# 3a.3: a local array whose entries are NOT numeric literals produces a
# clearer error than the generic "array value can't appear here".
# Length 3 so the error comes from the let-hoist guard (length 2 would
# route into the stereo-pair emission path and a different error).
const Dynamic = """
let tempo = 120.0
play t:
  let notes = [tempo, tempo * 2, tempo * 3]
  wave(1.0, notes)
"""
block dynError:
  let prog = parseProgram(Dynamic)
  try:
    discard generate(prog, "patches/dyn.aither")
    doAssert false, "expected error for dynamic array"
  except CatchableError as e:
    doAssert "numeric literal" in e.msg.toLowerAscii() or
             "numeric literals" in e.msg.toLowerAscii(),
      "error should mention numeric literals, got: " & e.msg

echo "wave_autohoist ok"
