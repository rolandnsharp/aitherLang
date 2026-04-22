## Step 3: hand-written C for `sin(TAU * 440 * t) * 0.3`, compiled
## through TCC, run 48000 samples, verify they look like a sine wave.

import std/math
import ../tcc

proc errHandler(opaque: pointer; msg: cstring) {.cdecl.} =
  stderr.writeLine "TCC: ", $msg

type
  VoiceState {.byref.} = object
    t, sr, dt, start_t: float64

  TickFn = proc (s: ptr VoiceState): float64 {.cdecl.}

const CSource = """
#include <math.h>

typedef struct { double t, sr, dt, start_t; } VoiceState;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TAU (2.0 * M_PI)

double tick(VoiceState* s) {
  return sin(TAU * 440.0 * s->t) * 0.3;
}
"""

let s = tccNew()
s.setErrorFunc(nil, errHandler)
doAssert s.setOutputType(OutputMemory) == 0
doAssert s.addLibrary("m") == 0
doAssert s.compileString(CSource) == 0
doAssert s.relocate() == 0

let fn = cast[TickFn](s.getSymbol("tick"))
doAssert fn != nil

var state = VoiceState(sr: 48000.0, dt: 1.0 / 48000.0)

# Verify known samples
state.t = 0.0
doAssert abs(fn(addr state) - 0.0) < 1e-9
state.t = 1.0 / (4.0 * 440.0)     # quarter period → peak
let peak = fn(addr state)
doAssert abs(peak - 0.3) < 1e-6, "peak was " & $peak

# Run 48000 samples; count sign changes (should be ~880 for 440 Hz).
var prev = 0.0
var crossings = 0
for i in 0 ..< 48000:
  state.t = float64(i) * state.dt
  let v = fn(addr state)
  if (prev <= 0.0 and v > 0.0) or (prev > 0.0 and v <= 0.0):
    inc crossings
  prev = v
# 440 Hz over 1 s = 880 zero crossings (one per half-period)
doAssert crossings >= 878 and crossings <= 882,
         "expected ~880 crossings, got " & $crossings

echo "peak=", peak, " crossings=", crossings
echo "step 3 ok"
s.delete()
