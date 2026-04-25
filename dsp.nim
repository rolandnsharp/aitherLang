## Native DSP primitives. Compiled Nim, called from the bytecode VM
## via opCallNative. Each function claims slots from voice.dspPool
## (flat float64 array) at runtime — no variants, no allocation.
##
## State convention follows aitherNimScript: `claimDsp(n)` advances
## the per-voice dspIdx by n; same call order each tick = same slots
## = phase continuity. Hot reload preserves the pool and idx pattern
## as long as call structure is stable.

import std/math

const DspPoolSize* = 524288     # 4 MB per voice; covers seconds of fbdelay
const OverflowSlot = DspPoolSize - 128   # shared fallback region at pool tail

type
  DspState* = object
    pool*:     array[DspPoolSize, float64]
    idx*:      int
    overflow*: bool          # set when claim had to clamp; callback may log
    sr*:       float64

template claim(s: var DspState; n: int = 1): int =
  # Bounds-check so a patch requesting more state than the pool holds
  # degrades to shared garbage-state rather than segfaulting the engine.
  if s.idx + n > OverflowSlot:
    s.overflow = true
    OverflowSlot
  else:
    let r = s.idx
    s.idx += n
    r

# ---------------------------------------------------------------- shapes
# (saw, tri, sqr are builtin opcodes; here only as helpers if needed)

proc shapeSaw*(x: float64): float64 {.cdecl, exportc: "shape_saw".} =
  (x / TAU) mod 1.0 * 2.0 - 1.0

proc shapeTri*(x: float64): float64 {.cdecl, exportc: "shape_tri".} =
  abs((x / TAU) mod 1.0 * 4.0 - 2.0) - 1.0

proc shapeSqr*(x: float64): float64 {.cdecl, exportc: "shape_sqr".} =
  if (x / TAU) mod 1.0 < 0.5: 1.0 else: -1.0

# ---------------------------------------------------------------- oscillators

proc nWave*(s: var DspState; freq: float64;
            values: ptr UncheckedArray[float64]; n: cint): float64
           {.cdecl, exportc: "n_wave".} =
  if n <= 0: return 0.0
  let i = s.claim()
  s.pool[i] = (s.pool[i] + freq / s.sr) mod 1.0
  if s.pool[i] < 0.0: s.pool[i] += 1.0
  let idx = int(s.pool[i] * float64(n)) mod int(n)
  values[idx]

# ---------------------------------------------------------------- 1-pole filters

proc nLp1*(s: var DspState; signal, cutoff: float64): float64 {.cdecl, exportc: "n_lp1".} =
  let i = s.claim()
  let a = clamp(cutoff / s.sr, 0.0, 1.0)
  s.pool[i] += a * (signal - s.pool[i])
  s.pool[i]

proc nHp1*(s: var DspState; signal, cutoff: float64): float64 {.cdecl, exportc: "n_hp1".} =
  let i = s.claim()
  let a = clamp(cutoff / s.sr, 0.0, 1.0)
  s.pool[i] += a * (signal - s.pool[i])
  signal - s.pool[i]

# ------------------------------------------------ SVF (Cytomic trapezoidal)

type FilterMode* = enum fmLow, fmHigh, fmBand, fmNotch

proc svf(s: var DspState; signal, cutoffArg, res: float64;
         mode: FilterMode): float64 {.inline.} =
  # Clamp cutoff to (1 Hz, just under Nyquist). A negative or zero cutoff
  # would push tan() into the wrong quadrant and produce NaN/Inf that the
  # filter would then accumulate into its pool slots forever; clamping
  # here turns "WOOP and silence" into a quiet/audible degradation.
  let cutoff = clamp(cutoffArg, 1.0, s.sr * 0.499)
  let i = s.claim(2)
  let g = tan(PI * cutoff / s.sr)
  let k = 2.0 * (1.0 - res)
  let a1 = 1.0 / (1.0 + g * (g + k))
  let a2 = g * a1
  let a3 = g * a2
  let v3 = signal - s.pool[i + 1]
  let v1 = a1 * s.pool[i] + a2 * v3
  let v2 = s.pool[i + 1] + a2 * s.pool[i] + a3 * v3
  s.pool[i]     = 2.0 * v1 - s.pool[i]
  s.pool[i + 1] = 2.0 * v2 - s.pool[i + 1]
  case mode
  of fmLow:   v2
  of fmHigh:  signal - k * v1 - v2
  of fmBand:  v1
  of fmNotch: signal - k * v1

proc nLpf*(s: var DspState; signal, cutoff, res: float64): float64 {.cdecl, exportc: "n_lpf".} =
  svf(s, signal, cutoff, res, fmLow)
proc nHpf*(s: var DspState; signal, cutoff, res: float64): float64 {.cdecl, exportc: "n_hpf".} =
  svf(s, signal, cutoff, res, fmHigh)
proc nBpf*(s: var DspState; signal, cutoff, res: float64): float64 {.cdecl, exportc: "n_bpf".} =
  svf(s, signal, cutoff, res, fmBand)
proc nNotch*(s: var DspState; signal, cutoff, res: float64): float64 {.cdecl, exportc: "n_notch".} =
  svf(s, signal, cutoff, res, fmNotch)

# ------------------------------------------------------------------- delays
# Buffer lives inline in the pool. Layout: [cursor, sample0, sample1, ...].

proc nDelay*(s: var DspState; signal, time, maxTime: float64): float64 {.cdecl, exportc: "n_delay".} =
  let bufLen = max(1, int(maxTime * s.sr))
  let base = s.claim(1 + bufLen)
  let cursor = int(s.pool[base]) mod bufLen
  let rd = (cursor - clamp(int(time * s.sr), 0, bufLen - 1) + bufLen) mod bufLen
  result = s.pool[base + 1 + rd]
  s.pool[base + 1 + cursor] = signal
  s.pool[base] = float64((cursor + 1) mod bufLen)

proc nFbdelay*(s: var DspState; signal, time, maxTime, fb: float64): float64 {.cdecl, exportc: "n_fbdelay".} =
  let bufLen = max(1, int(maxTime * s.sr))
  let base = s.claim(1 + bufLen)
  let cursor = int(s.pool[base]) mod bufLen
  let rd = (cursor - clamp(int(time * s.sr), 0, bufLen - 1) + bufLen) mod bufLen
  result = s.pool[base + 1 + rd]
  s.pool[base + 1 + cursor] = signal + result * fb
  s.pool[base] = float64((cursor + 1) mod bufLen)

# ------------------------------------------------------------------ reverb
# Schroeder: 4 parallel comb filters into 2 series allpass.

proc nReverb*(s: var DspState; signal, rt60, wet: float64): float64 {.cdecl, exportc: "n_reverb".} =
  const
    combLens = [1557, 1617, 1491, 1422]
    apLens   = [225, 556]
    apFb     = 0.5
    damp     = 0.3
  var total = 0
  for L in combLens: total += 2 + L     # cursor, damp state, buffer
  for L in apLens:   total += 1 + L     # cursor, buffer
  let base = s.claim(total)
  var off = base
  var combSum = 0.0
  for ci in 0 .. 3:
    let blen = combLens[ci]
    let curSlot  = off
    let dampSlot = off + 1
    let bufStart = off + 2
    off += 2 + blen
    let cur = int(s.pool[curSlot]) mod blen
    let output = s.pool[bufStart + cur]
    let filt = output * (1.0 - damp) + s.pool[dampSlot] * damp
    s.pool[dampSlot] = filt
    let g = pow(10.0, -3.0 * float64(blen) / (rt60 * s.sr))
    s.pool[bufStart + cur] = signal + filt * g
    s.pool[curSlot] = float64((cur + 1) mod blen)
    combSum += output
  var ap = combSum * 0.25
  for ai in 0 .. 1:
    let blen = apLens[ai]
    let curSlot  = off
    let bufStart = off + 1
    off += 1 + blen
    let cur = int(s.pool[curSlot]) mod blen
    let bufOut = s.pool[bufStart + cur]
    s.pool[bufStart + cur] = ap + bufOut * apFb
    ap = bufOut - ap
    s.pool[curSlot] = float64((cur + 1) mod blen)
  signal * (1.0 - wet) + ap * wet

# ---------------------------------------------------------------- physics

proc nImpulse*(s: var DspState; freq: float64): float64 {.cdecl, exportc: "n_impulse".} =
  let i = s.claim()
  let prev = s.pool[i]
  s.pool[i] = (s.pool[i] + freq / s.sr) mod 1.0
  if s.pool[i] < 0.0: s.pool[i] += 1.0
  if s.pool[i] < prev: 1.0 else: 0.0

proc nResonator*(s: var DspState; input, freq, decay: float64): float64 {.cdecl, exportc: "n_resonator".} =
  let i = s.claim(2)
  let omega2 = freq * freq
  let invSr = 1.0 / s.sr
  s.pool[i+1] += (-decay * s.pool[i+1] - omega2 * s.pool[i] + input * omega2) * invSr
  s.pool[i]   += s.pool[i+1] * invSr
  s.pool[i]

# Damped harmonic oscillator. The universal second-order linear
# system: ddx = -k*x - c*dx + force, with k = omega^2, c = 2*damp*omega,
# omega = TAU*freq. Sweep `damp` and you walk through every named
# synthesis paradigm — sustained sine (0), bell (~0.005), pluck (~0.2),
# critical (1.0), overdamped/silence (>1). `force` is added directly to
# acceleration, so the steady state under constant force is force/k.
# Semi-implicit (symplectic) Euler matches stdlib's tuning_fork.
proc nDho*(s: var DspState; force, freq, damp: float64): float64 {.cdecl, exportc: "n_dho".} =
  let i = s.claim(2)
  let omega = TAU * freq
  let k = omega * omega
  let c = 2.0 * damp * omega
  let invSr = 1.0 / s.sr
  s.pool[i+1] += (-c * s.pool[i+1] - k * s.pool[i] + force) * invSr
  s.pool[i]   += s.pool[i+1] * invSr
  s.pool[i]

# Velocity output of the same DHO equation. Separate call site, so it
# owns its own (x, dx) state — chain DHO physics by feeding one
# oscillator's velocity into another's force without sharing slots.
proc nDhoV*(s: var DspState; force, freq, damp: float64): float64 {.cdecl, exportc: "n_dho_v".} =
  let i = s.claim(2)
  let omega = TAU * freq
  let k = omega * omega
  let c = 2.0 * damp * omega
  let invSr = 1.0 / s.sr
  s.pool[i+1] += (-c * s.pool[i+1] - k * s.pool[i] + force) * invSr
  s.pool[i]   += s.pool[i+1] * invSr
  s.pool[i+1]

proc nDischarge*(s: var DspState; input, rate: float64): float64 {.cdecl, exportc: "n_discharge".} =
  let i = s.claim()
  s.pool[i] = max(input, s.pool[i] * (1.0 - rate / s.sr))
  s.pool[i]

# -------------------------------------------------------- modulation/effects

proc nTremolo*(s: var DspState; signal, rate, depth: float64): float64 {.cdecl, exportc: "n_tremolo".} =
  let i = s.claim()
  s.pool[i] = (s.pool[i] + rate / s.sr) mod 1.0
  if s.pool[i] < 0.0: s.pool[i] += 1.0
  let lfo = (sin(TAU * s.pool[i]) + 1.0) * 0.5
  signal * (1.0 - depth + lfo * depth)

proc nSlew*(s: var DspState; signal, time: float64): float64 {.cdecl, exportc: "n_slew".} =
  let i = s.claim()
  let a = if time > 0.0: min(1.0, (1.0 / s.sr) / time) else: 1.0
  s.pool[i] += (signal - s.pool[i]) * a
  s.pool[i]
