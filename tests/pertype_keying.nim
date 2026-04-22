## Verify per-helper-type state keying preserves state across structural
## edits. Patch A has [phasor, lpf, delay]. Patch B inserts a new wave()
## call at the top. Under the old flat-counter scheme, adding any stateful
## call shifts every subsequent call's state slot → lpf and delay would
## reset. Under per-type keying only wave() gets new state; phasor, lpf,
## delay continue from their old state.

import std/[math, strutils]
import ../parser, ../voice, ../codegen

const PatchA = """
let sig = phasor(110) * 2 - 1
let filtered = sig |> lpf(800, 0.5)
let echoed = filtered |> delay(0.2, 0.5)
echoed * 0.3
"""

# B inserts a wave() at the top. Historically this would shift lpf and
# delay's state slots. With per-type keying, only the wave region is
# new; phasor/lpf/delay migrate.
const PatchB = """
let m = wave(2, [200, 400, 600, 800])
let sig = phasor(110 + m * 0.0001) * 2 - 1
let filtered = sig |> lpf(800, 0.5)
let echoed = filtered |> delay(0.2, 0.5)
echoed * 0.3
"""

# ---- structural layout checks ------------------------------------------
# Regions produced by codegen should group by helper type.

let (_, _, _, regionsA) = generate(parseProgram(PatchA), "", 48000.0)
let (_, _, _, regionsB) = generate(parseProgram(PatchB), "", 48000.0)

proc dump(rs: seq[Region]): string =
  var parts: seq[string]
  for r in rs:
    parts.add r.typeName & "#" & $r.perTypeIdx & "@" & $r.offset & "[+" & $r.size & "]"
  parts.join(" ")

echo "A regions: ", dump(regionsA)
echo "B regions: ", dump(regionsB)

proc find(rs: seq[Region]; t: string; idx: int): Region =
  for r in rs:
    if r.typeName == t and r.perTypeIdx == idx: return r
  raise newException(ValueError, "no region " & t & "#" & $idx)

# lpf and delay regions (idx 0) must have the same size in A and B, so
# they are eligible for migration.
doAssert find(regionsA, "lpf", 0).size == find(regionsB, "lpf", 0).size
doAssert find(regionsA, "delay", 0).size == find(regionsB, "delay", 0).size
doAssert find(regionsA, "phasor", 0).size == find(regionsB, "phasor", 0).size

# Audible test -----------------------------------------------------------
let v = newVoice(48000.0)
v.load(parseProgram(PatchA), 48000.0)

# Run long enough to fully load the delay buffer (>0.2s).
var lastA: tuple[l, r: float64] = (0.0, 0.0)
for i in 1 .. 24000:
  lastA = v.tick(float64(i) / 48000.0)
echo "lastA = ", lastA.l

# Peek at the phasor slot before reload — it must survive.
let oldPhasorOff = find(regionsA, "phasor", 0).offset
let oldDelayOff = find(regionsA, "delay", 0).offset
let pool = cast[ptr UncheckedArray[float64]](v.state)
let phasorBefore = pool[oldPhasorOff]
let delayBufSample = pool[oldDelayOff + 100]  # arbitrary slot in the delay buffer
echo "phasor before = ", phasorBefore, "  delay-buf@100 = ", delayBufSample
doAssert phasorBefore != 0.0,   "phasor slot should be non-zero after ticks"
doAssert delayBufSample != 0.0, "delay buffer should contain audio"

v.load(parseProgram(PatchB), 48000.0)

let newPhasorOff = find(regionsB, "phasor", 0).offset
let newDelayOff = find(regionsB, "delay", 0).offset
let pool2 = cast[ptr UncheckedArray[float64]](v.state)
echo "phasor after reload = ", pool2[newPhasorOff]
echo "delay-buf@100 after reload = ", pool2[newDelayOff + 100]

doAssert abs(pool2[newPhasorOff] - phasorBefore) < 1e-9,
  "phasor state should migrate: got " & $pool2[newPhasorOff] &
  " want " & $phasorBefore
doAssert abs(pool2[newDelayOff + 100] - delayBufSample) < 1e-9,
  "delay buffer should migrate"

# First tick after reload shouldn't be catastrophically different — the
# delay buffer still contains the pre-reload audio, the phasor still has
# its phase. A small jump from the newly-initialised wave() is expected.
let firstB = v.tick(24001.0 / 48000.0)
echo "firstB = ", firstB.l
doAssert abs(firstB.l - lastA.l) < 0.2,
  "expected smooth continuation; got jump from " & $lastA.l &
  " to " & $firstB.l

echo "per-type keying ok"
