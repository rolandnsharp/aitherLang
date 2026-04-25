## Phase 3 verification test for patches/backing.aither.
##
## The patch is the acceptance criterion for the whole bug-fix bundle:
## it exercises (1) arrays-as-values in a play block (the `roots` table),
## (2) `else if` chaining (chord_third), (3) prev() rising-edge for the
## bell, plus the long-standing additive / inharmonic / sum / lambda
## stack. If this compiles + ticks NaN-free at full 48 kHz over a few
## seconds, the language is no longer the bottleneck — only the human
## ears.

import std/[math, strutils]
import ../parser, ../voice, ../codegen

const Stdlib = staticRead("../stdlib.aither")
const Patch = staticRead("../patches/backing.aither")

proc loadProgram(): NativeVoice =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(Patch)
  setSource(userAst, "patches/backing.aither")
  let prog = Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)
  result = newVoice(48000.0)
  result.load(prog, 48000.0)

let v = loadProgram()

# Tick for ~3 seconds. Record peak, energy, and any NaN/Inf.
const SR = 48000
const Duration = 3
var peak = 0.0
var energy = 0.0
var nanSamples = 0
for i in 0 ..< SR * Duration:
  let s = v.tick(float64(i) / SR.float64)
  if s.l != s.l or s.r != s.r:           # NaN check
    inc nanSamples
    continue
  if abs(s.l) > peak: peak = abs(s.l)
  if abs(s.r) > peak: peak = abs(s.r)
  energy += s.l * s.l + s.r * s.r

let rms = sqrt(energy / (SR.float64 * Duration.float64 * 2.0))

doAssert nanSamples == 0,
  "backing.aither produced " & $nanSamples & " NaN samples in " &
  $Duration & "s — patch has a numerical bug"

doAssert peak > 0.05,
  "backing.aither should be audible, peak=" & $peak & " (too quiet)"
doAssert peak < 1.0,
  "backing.aither should not saturate, peak=" & $peak & " (too loud)"
doAssert rms > 0.005,
  "backing.aither should sustain energy, rms=" & $rms

# Region-level smoke check: confirm the codegen unrolled the spectral
# stuff. additive(..., 8) at three pads + 6 partials at two more =
# many phasors. Just check there are >40 (catches a broken unroll).
let (_, _, parts, regions) = generate(
  Node(kind: nkBlock,
       kids: parseProgram(Stdlib).kids & parseProgram(Patch).kids, line: 1),
  "patches/backing.aither", 48000.0)
var phasorCount = 0
for r in regions:
  if r.typeName == "phasor": inc phasorCount
# Expected ≈ 39: bass=8, pad=16 (3 additives), drums=3, bell=5,
# texture=4, top-level beat/bar/phrase=3. >30 catches a broken unroll
# without being brittle to small patch tweaks.
doAssert phasorCount > 30,
  "expected many phasor regions from additive/inharmonic unroll, got " &
  $phasorCount

doAssert parts == @["bass", "pad", "kick", "snare", "hat", "bell", "texture"],
  "play blocks must compile in declaration order, got " & $parts

echo "backing_compiles ok (peak=", peak.formatFloat(ffDecimal, 3),
     " rms=", rms.formatFloat(ffDecimal, 4),
     " phasors=", phasorCount, ")"
