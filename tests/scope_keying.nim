## State is keyed by the statement a stateful call sits in, so adding
## one to a play or let leaves every other statement's state alone.
## Before, keys were per-type counters over the whole file: a `prev`
## added in an early play shifted the `prev` slots of every later one,
## and physical models fed each other's velocities and blew up.

import ../ccompile, ../voice


proc prog(src: string): string =
  src

const A = """
play a:
  let x = prev(sin(TAU * phasor(3)))
  x * 0.1

play b:
  let y = prev(sin(TAU * phasor(5)))
  let z = prev(y)
  z * 0.1
"""

# Adds a prev() at the top of play a: under per-type counters, b's two
# prev regions would move to indices 2 and 3 and pick up a's state.
const B = """
play a:
  let w = prev(0.25)
  let x = prev(sin(TAU * phasor(3)))
  x * 0.1 + w * 0

play b:
  let y = prev(sin(TAU * phasor(5)))
  let z = prev(y)
  z * 0.1
"""

proc find(rs: seq[Region]; scope, t: string; i: int): Region =
  for r in rs:
    if r.scope == scope and r.typeName == t and r.scopeIdx == i: return r
  raise newException(ValueError, "no region " & scope & " " & t & "#" & $i)

let ra = compile(prog(A), "", 48000.0).regions
let gen = compile(prog(B), "", 48000.0)
let rb = gen.regions

let v = newVoice(48000.0)
v.load(prog(A), 48000.0)
for i in 1 .. 4800: discard v.tick(float64(i) / 48000.0)
let pool = poolOf(v.state)
var before: seq[seq[float64]]
for (sc, idx) in [("play b/let y", 0), ("play b/let z", 0), ("play a/let x", 0)]:
  let r = find(ra, sc, "var", idx)
  var s: seq[float64]
  for k in 0 ..< r.size: s.add pool[r.offset + k]
  before.add s

v.load(prog(B), 48000.0)
let pool2 = poolOf(v.state)
for n, (sc, idx) in [("play b/let y", 0), ("play b/let z", 0), ("play a/let x", 0)]:
  let r = find(rb, sc, "var", idx)
  for k in 0 ..< r.size:
    doAssert pool2[r.offset + k] == before[n][k],
      sc & " lost its state across the edit"
# The new call starts fresh.
let w = find(rb, "play a/let w", "var", 0)
for k in 0 ..< w.size: doAssert pool2[w.offset + k] == 0.0

echo "scope keying ok"
