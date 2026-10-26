## State regions are owned by the def they sit in (and, for `$state`,
## by the state's name). Replacing one physical model by another of the
## same shape starts the new one clean; editing a def's body keeps its
## state; the renamed-statement fallback also respects the owner.
## (2026-10-11: `stick` ($x,$dx x3) swapped for `bar` ($bv,$bx x3)
## inherited the old displacement and `try` showed +24 dBFS.)

import std/[strutils]
import ../parser, ../voice, ../codegen

const Stdlib = staticRead("../stdlib.aither")

proc prog(src: string): Node =
  Node(kind: nkBlock, kids: parseProgram(Stdlib).kids & parseProgram(src).kids)

const Stick = """
def stick(force):
  sum(3, n =>
    $x = 0.0
    $dx = 0.0
    $dx = $dx + (force * 1000 - 50 * $dx - 400000 * n * $x) * dt
    $x = $x + $dx * dt
    $x)
"""
const Bar = """
def bar(force):
  sum(3, n =>
    $bv = 0.0
    $bx = 0.0
    $bv = $bv + (force * 4000 - 20 * $bv - 900000 * n * $bx) * dt
    $bx = $bx + $bv * dt
    $bv)
"""
const Bar2 = Bar.replace("4000", "3000")           # an edit inside bar
const Drive = "let trig = if t < dt then 1 else 0\n"

proc vars(rs: seq[Region]; owner: string): seq[Region] =
  for r in rs:
    if r.typeName == "var" and r.owner.startsWith(owner): result.add r

proc slots(v: NativeVoice; rs: seq[Region]): seq[float64] =
  let pool = poolOf(v.state)
  for r in rs:
    for k in 0 ..< r.size: result.add pool[r.offset + k]

# Owners are recorded: bar's state is `bar/$bv`, `bar/$bx`.
let (_, _, _, rb) = generate(prog(Bar & Drive & "play hit:\n  bar(trig)\n"), "", 48000.0)
var owners: seq[string]
for r in rb:
  if r.typeName == "var": owners.add r.owner
doAssert "bar/$bv" in owners and "bar/$bx" in owners, $owners
doAssert vars(rb, "bar").len == 6, $vars(rb, "bar").len

# 1. stick -> bar in the same play: bar starts from zero.
let v = newVoice(48000.0)
v.load(prog(Stick & Drive & "play hit:\n  stick(trig)\n"), 48000.0)
for i in 0 ..< 2000: discard v.tick(float64(i) / 48000.0)
let (_, _, _, rs) = generate(prog(Stick & Drive & "play hit:\n  stick(trig)\n"), "", 48000.0)
let stickState = v.slots(vars(rs, "stick"))
var moving = 0
for x in stickState:
  if abs(x) > 1e-6 and x != 1.0: inc moving
doAssert moving >= 3, "stick should be ringing: " & $stickState

v.load(prog(Bar & Drive & "play hit:\n  bar(trig)\n"), 48000.0)
let barState = v.slots(vars(rb, "bar"))
for x in barState:
  doAssert x == 0.0, "bar inherited stick's state: " & $barState
echo "stick -> bar: bar starts clean"

# 2. An edit inside bar keeps bar's state (same owner, same shape).
for i in 0 ..< 2000: discard v.tick(float64(i) / 48000.0)
let barBefore = v.slots(vars(rb, "bar"))
v.load(prog(Bar2 & Drive & "play hit:\n  bar(trig)\n"), 48000.0)
let (_, _, _, rb2) = generate(prog(Bar2 & Drive & "play hit:\n  bar(trig)\n"), "", 48000.0)
doAssert v.slots(vars(rb2, "bar")) == barBefore, "bar's state should survive an edit"
echo "bar edit: state kept"

# 3. Renamed statement, same def: the fallback carries bar's state over.
v.load(prog(Bar2 & Drive & "play hit2:\n  bar(trig)\n"), 48000.0)
let (_, _, _, rb3) = generate(prog(Bar2 & Drive & "play hit2:\n  bar(trig)\n"), "", 48000.0)
doAssert v.slots(vars(rb3, "bar")) == barBefore, "renamed play should keep bar's state"
echo "renamed play, same def: state kept"

# 4. Renamed statement AND a different def of the same shape: clean.
v.load(prog(Stick & Drive & "play hit3:\n  stick(trig)\n"), 48000.0)
let (_, _, _, rs3) = generate(prog(Stick & Drive & "play hit3:\n  stick(trig)\n"), "", 48000.0)
for x in v.slots(vars(rs3, "stick")):
  doAssert x == 0.0, "stick inherited bar's state via the fallback"
echo "renamed play, other def: starts clean"
echo "def identity migration ok"
