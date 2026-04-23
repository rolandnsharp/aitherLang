## CC state lives in the engine, not the voice, so a hot reload of a
## patch that uses `midi_cc(n)` must see the knob at its current position
## on the first tick after reload — no "zeroed knobs on edit" surprise.

import ../parser, ../voice, ../midi

const Stdlib = staticRead("../stdlib.aither")

proc program(src: string): Node =
  let stdAst = parseProgram(Stdlib)
  let userAst = parseProgram(src)
  Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

midiResetAll()
midiCc(74, 90)

let v = newVoice(48000.0)
v.load(program("midi_cc(74)"), 48000.0)
var (l, r) = v.tick(0.0)
let expected = 90.0 / 127.0
doAssert abs(l - expected) < 1e-9, "pre-reload got " & $l

# Hot-reload: same voice, new patch source, same CC primitive.
v.load(program("midi_cc(74) + 0.1"), 48000.0)
(l, r) = v.tick(0.0)
doAssert abs(l - (expected + 0.1)) < 1e-9, "post-reload got " & $l

# And reloading to a patch that doesn't use CC at all, then back to one
# that does, still reflects the current CC value.
v.load(program("0.0"), 48000.0)
v.load(program("midi_cc(74)"), 48000.0)
(l, r) = v.tick(0.0)
doAssert abs(l - expected) < 1e-9, "after-idle-reload got " & $l

echo "midi_hotreload ok"
