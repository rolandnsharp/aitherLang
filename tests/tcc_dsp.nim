## Validates that generated C can call into dsp.nim's n_* primitives
## via tcc_add_symbol. Layout contract: VoiceState begins with the exact
## DspState fields (pool, idx, overflow, sr), so a VoiceState* casts to
## a DspState* safely.

import std/math
import ../tcc, ../dsp

proc errHandler(opaque: pointer; msg: cstring) {.cdecl.} =
  stderr.writeLine "TCC: ", $msg

const CSource = """
#include <math.h>

typedef struct DspState DspState;
extern double n_lpf(DspState* s, double sig, double cut, double res);

typedef struct {
  /* --- DspState prefix (must match dsp.nim layout) --- */
  double pool[524288];
  long   idx;
  char   overflow;
  /* 7 bytes padding */
  double sr;
  /* --- aither voice fields --- */
  double t, dt, start_t;
} VoiceState;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define TAU (2.0 * M_PI)

double tick(VoiceState* s) {
  s->idx = 0;
  double x = sin(TAU * 2000.0 * s->t);     /* 2 kHz tone */
  return n_lpf((DspState*)s, x, 200.0, 0.2);  /* LPF at 200 Hz → heavy atten */
}
"""

let s = tccNew()
s.setErrorFunc(nil, errHandler)
doAssert s.setOutputType(OutputMemory) == 0
doAssert s.addLibrary("m") == 0
doAssert s.addSymbol("n_lpf", cast[pointer](nLpf)) == 0
doAssert s.compileString(CSource) == 0
doAssert s.relocate() == 0

type
  VState = object
    state: DspState
    t, dt, start_t: float64
  TickFn = proc (s: ptr VState): float64 {.cdecl.}

echo "sizeof DspState  = ", sizeof(DspState)
echo "sizeof VState    = ", sizeof(VState)

let fn = cast[TickFn](s.getSymbol("tick"))
doAssert fn != nil

var st = create(VState)  # heap — 4 MB, too big for the stack
st.state.sr = 48000.0
st.dt = 1.0 / 48000.0

# Run enough samples to let the filter settle, then measure rms.
var sumSq = 0.0
let N = 48000
for i in 0 ..< N:
  st.t = float64(i) * st.dt
  let v = fn(st)
  if i >= N div 2:
    sumSq += v * v
let rms = sqrt(sumSq / float64(N div 2))
# Raw 2 kHz tone has rms ≈ 0.707; after 200 Hz LPF it should be heavily
# attenuated (well below 0.1).
echo "filtered rms = ", rms
doAssert rms < 0.1, "expected heavy attenuation, got " & $rms

dealloc(st)
s.delete()
echo "dsp step ok"
