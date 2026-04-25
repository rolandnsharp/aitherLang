## Phase 2: polyphonic held-notes table.
##
## The engine tracks up to MaxPolyphony (16) simultaneously-held notes.
## midi_voice_freq(n) / midi_voice_gate(n) expose the nth slot, 1-based,
## so the slot index pairs cleanly with sum's iteration counter.
##
## Allocator policy (re-trigger same note → fill empty → reuse released
## → steal oldest) is documented in midi.nim's allocateSlot. The tests
## below pin the user-facing properties: voice indices stay stable for
## still-held notes, mono `midi_freq()` keeps tracking the most recent
## note-on, and going past 16 notes evicts the oldest.

import std/math
import ../parser, ../voice, ../midi

const Stdlib = staticRead("../stdlib.aither")

proc load(src: string): NativeVoice =
  let stdAst = parseProgram(Stdlib)
  let userAst = parseProgram(src)
  let program = Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)
  result = newVoice(48000.0)
  result.load(program, 48000.0)

proc hzOf(note: int): float64 =
  440.0 * pow(2.0, (float64(note) - 69.0) / 12.0)

# 1. Single-note allocation: voice 1 reads the held pitch and velocity.
midiResetAll()
let v1 = load("[midi_voice_freq(1), midi_voice_gate(1)]")
var (l, r) = v1.tick(0.0)
doAssert l == 0.0 and r == 0.0,
  "fresh engine: voice 1 should read 0/0, got " & $l & "/" & $r

midiNoteOn(60, 100)              # C4 ≈ 261.63 Hz
(l, r) = v1.tick(0.0)
doAssert abs(l - hzOf(60)) < 1e-3, "voice 1 freq for C4: " & $l
doAssert abs(r - 100.0/127.0) < 1e-9, "voice 1 gate for C4: " & $r

midiNoteOff(60)
(l, r) = v1.tick(0.0)
doAssert abs(l - hzOf(60)) < 1e-3,
  "voice 1 freq stays after release (synth release tail still reads it): " & $l
doAssert r == 0.0, "voice 1 gate goes to 0 on release: " & $r

# 2. Three-note chord — each held note maps to its own slot in held-order.
midiResetAll()
let v2 = load("""
[midi_voice_freq(1) + midi_voice_freq(2) * 0 + midi_voice_freq(3) * 0,
 midi_voice_freq(2)]
""")
midiNoteOn(60, 100)              # C
midiNoteOn(64, 100)              # E
midiNoteOn(67, 100)              # G
(l, r) = v2.tick(0.0)
doAssert abs(l - hzOf(60)) < 1e-3, "voice 1 = C: " & $l
doAssert abs(r - hzOf(64)) < 1e-3, "voice 2 = E: " & $r

# Voice 3 separately: read just slot 3.
let v2b = load("midi_voice_freq(3)")
(l, r) = v2b.tick(0.0)
doAssert abs(l - hzOf(67)) < 1e-3, "voice 3 = G: " & $l

# 3. Releasing the middle note: voices 1 and 3 unchanged. Voice 2 keeps
# its pitch but reports gate=0 — release semantics, not slot collapse.
midiNoteOff(64)
let v3a = load("midi_voice_freq(1)")
let v3b = load("midi_voice_freq(2)")
let v3c = load("midi_voice_freq(3)")
let v3d = load("midi_voice_gate(2)")
(l, _) = v3a.tick(0.0); doAssert abs(l - hzOf(60)) < 1e-3, "v1 stays: " & $l
(l, _) = v3b.tick(0.0); doAssert abs(l - hzOf(64)) < 1e-3,
  "v2 keeps freq during release: " & $l
(l, _) = v3c.tick(0.0); doAssert abs(l - hzOf(67)) < 1e-3, "v3 stays: " & $l
(l, _) = v3d.tick(0.0); doAssert l == 0.0, "v2 gate=0 after release: " & $l

# 4. New note after a release reuses the freed slot.
midiNoteOn(72, 100)              # C5 — should land in slot 2 (released first)
(l, _) = v3b.tick(0.0)
doAssert abs(l - hzOf(72)) < 1e-3, "C5 takes the freed slot 2: " & $l

# 5. midi_freq() / midi_gate() track the most recent note-on, unchanged.
let v5 = load("[midi_freq(), midi_gate()]")
midiResetAll()
midiNoteOn(60, 100)
midiNoteOn(64, 90)
midiNoteOn(67, 80)
(l, r) = v5.tick(0.0)
doAssert abs(l - hzOf(67)) < 1e-3, "midi_freq() = most recent: " & $l
doAssert abs(r - 80.0/127.0) < 1e-9, "midi_gate() = most recent vel: " & $r
midiNoteOff(67)
(l, r) = v5.tick(0.0)
doAssert r == 0.0, "midi_gate goes to 0 when most-recent releases: " & $r

# 6. Voice stealing: 17 distinct held notes evict the oldest.
midiResetAll()
for i in 0 ..< MaxPolyphony:
  midiNoteOn(60 + i, 100)        # held in held-order
# All 16 slots full. v1 starts as the oldest = note 60.
let v6 = load("midi_voice_freq(1)")
(l, _) = v6.tick(0.0)
doAssert abs(l - hzOf(60)) < 1e-3,
  "before eviction, slot 1 = oldest = C4: " & $l
midiNoteOn(80, 100)              # 17th note → evict slot holding lowest onAt
# The slot that previously held note 60 now holds note 80.
(l, _) = v6.tick(0.0)
doAssert abs(l - hzOf(80)) < 1e-3,
  "after 17th note, oldest slot replaced: " & $l

# 7. Re-pressing a held note re-triggers in place (same slot, refreshed velocity).
midiResetAll()
midiNoteOn(60, 100)
midiNoteOn(64, 100)
let v7f = load("midi_voice_freq(1)")
let v7g = load("midi_voice_gate(1)")
(l, _) = v7f.tick(0.0); doAssert abs(l - hzOf(60)) < 1e-3
(l, _) = v7g.tick(0.0); doAssert abs(l - 100.0/127.0) < 1e-9
midiNoteOn(60, 60)               # re-press at lower velocity → in-place update
(l, _) = v7f.tick(0.0); doAssert abs(l - hzOf(60)) < 1e-3,
  "still slot 1 after re-press: " & $l
(l, _) = v7g.tick(0.0); doAssert abs(l - 60.0/127.0) < 1e-9,
  "velocity refreshed in slot 1: " & $l

# 8. Out-of-range voice index returns 0 cleanly (no crash).
let v8 = load("[midi_voice_freq(0), midi_voice_freq(99)]")
(l, r) = v8.tick(0.0)
doAssert l == 0.0 and r == 0.0,
  "voice index out of [1..MaxPolyphony] returns 0: " & $l & "/" & $r

echo "midi_polyphony ok"
