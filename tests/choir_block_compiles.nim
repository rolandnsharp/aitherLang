## Step 2 regression test — the exact choir block pattern from the
## live-jam handoff must compile.
##
## Root cause: `isFnName` was consulted when deciding whether a user-def
## call's arg was a function-valued ident, but it didn't check whether
## that ident was shadowed by a local value. A play block with
##   let swell = (1 + sin(TAU * 0.06)) * 0.5
##   ease(swell)
## hit it because stdlib has `def swell(gate, attack, release)`. The
## local `swell` (a number) was treated as a function ref, ease's param
## `x` was bound as `inner.fns["x"] = swell` instead of as a scalar,
## and codegen later raised "unknown identifier: x (stdlib:37)" when
## emitting ease's body `let c = clamp(x, 0, 1)`.

import ../parser, ../voice

const ChoirPatch = """
let tempo = 120.0
let celloNotes = [110.0, 146.83, 164.81, 220.0]

play cello:
  let f = wave(tempo / 16, celloNotes)
  sin(TAU * phasor(f)) * 0.3

play choir:
  let cFreq = wave(tempo / 32, celloNotes) * 2.0
  let vib   = 1 + sin(TAU * 0.5) * 0.012
  let saws  = osc(saw, cFreq * vib) + osc(saw, cFreq * vib * 1.005) * 0.5
  let air   = noise() * 0.04
  let raw   = (saws + air) * 0.3
  let f1    = (raw |> bpf(800, 0.5)) * 1.0
  let f2    = (raw |> bpf(1400, 0.55)) * 0.7
  let f3    = (raw |> bpf(2800, 0.6)) * 0.4
  let swell = (1 + sin(TAU * 0.06)) * 0.5
  let env   = ease(swell) * 0.6 + 0.4
  let s = (f1 + f2 + f3) * env |> gain(0.15)
  pan(s, sin(TAU * 0.07) * 0.6)

cello + choir
"""

const Stdlib = staticRead("../stdlib.aither")
let stdAst = parseProgram(Stdlib)
setSource(stdAst, "stdlib")
let userAst = parseProgram(ChoirPatch)
setSource(userAst, "patches/choir_test.aither")
let program = Node(kind: nkBlock,
                   kids: stdAst.kids & userAst.kids, line: 1)

let v = newVoice(48000.0)
v.load(program, 48000.0, "patches/choir_test.aither")

# Also cover the minimal reduction of the bug — a bare `let <stdlib-def-name>`
# locally used as a scalar arg to another def must NOT be mistaken for a
# function-valued arg.
const Minimal = """
play t:
  let swell = 0.5
  ease(swell)
"""
let userAst2 = parseProgram(Minimal)
setSource(userAst2, "patches/min_shadow.aither")
let stdAst2 = parseProgram(Stdlib)
setSource(stdAst2, "stdlib")
let program2 = Node(kind: nkBlock,
                    kids: stdAst2.kids & userAst2.kids, line: 1)
let v2 = newVoice(48000.0)
v2.load(program2, 48000.0, "patches/min_shadow.aither")

# Sanity: pass a real def reference through osc — osc(saw, 220) — still
# works. `saw` is a stdlib shape helper, not a local, so the fn-ref path
# must stay live for legitimate higher-order calls.
const HigherOrder = """
play t:
  osc(saw, 220)
"""
let userAst3 = parseProgram(HigherOrder)
setSource(userAst3, "patches/ho.aither")
let stdAst3 = parseProgram(Stdlib)
setSource(stdAst3, "stdlib")
let program3 = Node(kind: nkBlock,
                    kids: stdAst3.kids & userAst3.kids, line: 1)
let v3 = newVoice(48000.0)
v3.load(program3, 48000.0, "patches/ho.aither")

# Run a few ticks of the choir patch just to confirm the compile is
# audible-clean (no NaN / Inf out of the gate).
for i in 0 ..< 256:
  let s = v.tick(float64(i) / 48000.0)
  doAssert s.l == s.l and s.r == s.r, "NaN at sample " & $i
  doAssert s.l > -10.0 and s.l < 10.0, "wild out at sample " & $i

echo "choir_block_compiles ok"
