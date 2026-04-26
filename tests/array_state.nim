## Mutable arrays in $state. `array_make(N)` carves a per-call-site
## region in the voice pool of (length, capacity, data...) shape;
## `array_get/set/len/push/pop/resize` are bounds-checked operations on
## the handle. Out-of-bounds reads return 0 (no crash) — important for
## live-coding ergonomics.

import std/[math]
import ../parser, ../voice, ../codegen

const Stdlib = staticRead("../stdlib.aither")

proc prog(src: string): Node =
  let stdAst = parseProgram(Stdlib)
  setSource(stdAst, "stdlib")
  let userAst = parseProgram(src)
  setSource(userAst, "patches/test.aither")
  Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

proc evalAt(src: string; nTicks: int = 1): float64 =
  let v = newVoice(48000.0)
  v.load(prog(src), 48000.0)
  var last = 0.0
  for i in 0 ..< nTicks:
    last = v.tick(float64(i) / 48000.0).l
  last

# --- 1. make / get / set / len round-trip.
# Default state of an N-element array is length=N, capacity=N, all zeros.
# Setting and reading back any element works.
block roundTrip:
  let v = evalAt("""
let arr = array_make(8)
array_set(arr, 3, 42.0)
array_get(arr, 3)
""")
  doAssert abs(v - 42.0) < 1e-12, "round-trip set/get failed: " & $v

block lenIsCap:
  let v = evalAt("""
let arr = array_make(8)
array_len(arr)
""")
  doAssert abs(v - 8.0) < 1e-12, "array_len after make(8) should be 8: " & $v

# --- 2. Out-of-bounds reads return 0 silently. Live-coding contract:
# never crash on a mistyped index.
block outOfBoundsRead:
  let high = evalAt("""
let arr = array_make(4)
array_set(arr, 0, 1.0)
array_get(arr, 99)
""")
  doAssert high == 0.0, "OOB read should be 0, got " & $high
  let neg = evalAt("""
let arr = array_make(4)
array_set(arr, 0, 1.0)
array_get(arr, -1)
""")
  doAssert neg == 0.0, "negative index should be 0, got " & $neg

# --- 3. Out-of-bounds writes are no-ops; data unaffected.
block outOfBoundsWrite:
  let v = evalAt("""
let arr = array_make(4)
array_set(arr, 0, 7.0)
array_set(arr, 99, 9.0)
array_get(arr, 0)
""")
  doAssert abs(v - 7.0) < 1e-12, "OOB write must not corrupt other slots"

# --- 4. Push past cap is a no-op. Length stays at cap.
block pushPastCap:
  let v = evalAt("""
let arr = array_make(4)
array_push(arr, 1.0)
array_push(arr, 2.0)
array_len(arr)
""")
  doAssert abs(v - 4.0) < 1e-12,
    "push past cap shouldn't grow length, got " & $v

# --- 5. Pop returns the last element and shrinks length.
block popShrinks:
  let popped = evalAt("""
let arr = array_make(4)
array_set(arr, 3, 99.0)
array_pop(arr)
""")
  doAssert abs(popped - 99.0) < 1e-12, "pop should return last, got " & $popped
  # And length is now 3.
  let lenAfter = evalAt("""
let arr = array_make(4)
array_set(arr, 3, 99.0)
array_pop(arr)
array_len(arr)
""")
  doAssert abs(lenAfter - 3.0) < 1e-12,
    "len after pop should be 3, got " & $lenAfter

# --- 6. Resize shrinks then re-grows; growth pads with zeros.
block resizePadsZero:
  # Set slot 2 to 5; resize down to 1 (drops slot 2); resize back to 4
  # (re-zeros slot 2). Slot 2 should read as 0.
  let v = evalAt("""
let arr = array_make(4)
array_set(arr, 2, 5.0)
array_resize(arr, 1)
array_resize(arr, 4)
array_get(arr, 2)
""")
  doAssert v == 0.0,
    "resize re-grow must re-zero previously-popped slots, got " & $v

# --- 7. State persistence across ticks. A counter-array that increments
# slot 0 every tick should accumulate.
block persistsAcrossTicks:
  const SR = 48000.0
  let v = newVoice(SR)
  v.load(prog("""
let arr = array_make(4)
array_set(arr, 0, array_get(arr, 0) + 1)
array_get(arr, 0)
"""), SR)
  for i in 0 ..< 5:
    discard v.tick(float64(i) / SR)
  let final = v.tick(5.0 / SR)
  doAssert abs(final.l - 6.0) < 1e-12,
    "after 6 ticks slot 0 should be 6, got " & $final.l

# --- 8. Pool budget: 1000 sequential push-pop pairs do not blow the
# pool. The array stays at length=cap (push past cap is no-op), pops
# unwind, never allocates beyond the static region size.
block tightPushPopLoop:
  const SR = 48000.0
  let v = newVoice(SR)
  v.load(prog("""
let arr = array_make(8)
let _ = array_pop(arr)
let _2 = array_push(arr, 1.0)
array_len(arr)
"""), SR)
  for i in 0 ..< 1000:
    discard v.tick(float64(i) / SR)
  let final = v.tick(1000.0 / SR)
  doAssert abs(final.l - 8.0) < 1e-12,
    "len after 1000 ticks of push/pop should be 8, got " & $final.l

# --- 9. Voice isolation: a fresh voice doesn't see another voice's data.
block voiceIsolation:
  let pa = """
let arr = array_make(4)
array_set(arr, 0, 99.0)
array_get(arr, 0)
"""
  let v1 = newVoice(48000.0)
  v1.load(prog(pa), 48000.0)
  discard v1.tick(0.0)
  # Allocate an independent voice on the same patch — pool is separate.
  let v2 = newVoice(48000.0)
  v2.load(prog("""
let arr = array_make(4)
array_get(arr, 0)
"""), 48000.0)
  let r2 = v2.tick(0.0)
  doAssert r2.l == 0.0,
    "fresh voice's array should start at 0, got " & $r2.l

# --- 10. Capacity bounds: array_make refuses N <= 0 and N > 4096.
block capacityBounds:
  for bad in [-1, 0, 4097]:
    var msg = ""
    try:
      discard evalAt("array_make(" & $bad & ")")
    except CatchableError as e:
      msg = e.msg
    doAssert msg.len > 0,
      "array_make(" & $bad & ") should error"

# --- 11. Region accounting: array_make claims a single typed region
# whose size is cap+2 (length + capacity + data slots). Two arrays
# each get their own region.
block regionLayout:
  const P = """
let a = array_make(8)
let b = array_make(4)
array_get(a, 0) + array_get(b, 0)
"""
  let (_, _, _, regions) = generate(parseProgram(P), "", 48000.0)
  var arrayRegions: seq[int]
  for r in regions:
    if r.typeName == "array":
      arrayRegions.add r.size
  doAssert arrayRegions.len == 2,
    "expected 2 array regions, got " & $arrayRegions.len
  doAssert arrayRegions == @[10, 6],
    "expected sizes [10, 6] (cap+2 each), got " & $arrayRegions

echo "array_state ok"
