## resetPool used to zero the entire DSP pool. That clobbered state
## storage too — meaning user-authored auto-fade-in patterns (e.g.
## `$startTime = -1; $startTime = if $startTime < 0 then t else $startTime`)
## would re-trigger from scratch every time NaN tripped a reset. Now
## resetPool walks the regions and skips the "var" type, so user-named
## state survives. (Region typeName is still "var" for layout-stability
## across the pre-/post-sigil migration boundary; the sigil is a parser
## change, not a codegen one.)

import std/[math]
import ../parser, ../voice

const Patch = """
def step():
  $counter = 0.0
  $counter = $counter + 1.0
  $counter
let c = step()
c
"""

let v = newVoice(48000.0)
v.load(parseProgram(Patch), 48000.0)

var last = 0.0
for i in 1 .. 100:
  last = v.tick(float64(i) / 48000.0).l
doAssert last == 100.0, "expected counter=100 after 100 ticks, got " & $last

# Locate the (single) var region and confirm its value slot really holds 100.
var varRegionOff = -1
var varRegionSize = 0
for r in v.regions:
  if r.typeName == "var":
    varRegionOff = r.offset
    varRegionSize = r.size
doAssert varRegionOff >= 0, "test setup failed: no var region"
let pool = cast[ptr UncheckedArray[float64]](v.state)
doAssert pool[varRegionOff] == 1.0,     "var inited-flag should be set"
doAssert pool[varRegionOff + 1] == 100.0, "var value slot should be 100"

# Reset the pool. The var region must NOT be zeroed.
v.resetPool()
doAssert pool[varRegionOff] == 1.0,     "var inited-flag survived?"
doAssert pool[varRegionOff + 1] == 100.0, "var value survived reset?"

let after = v.tick(101.0 / 48000.0).l
doAssert after == 101.0,
  "counter should continue past reset, got " & $after

echo "reset preserves var ok"
