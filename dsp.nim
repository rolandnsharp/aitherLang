## Native DSP primitives: compiled Nim, called from a patch's generated
## C. Each function claims slots from its voice's pool (a flat float64
## array) at runtime — no variants, no allocation.
##
## `claim(n)` advances the voice's `idx` by n; the generated C points
## `idx` at each call site's region before the call, so the same call
## site gets the same slots every sample = phase continuity, and hot
## reload carries a region across by its identity (voice.commit).

import std/[math, tables]

# A voice's pool is as big as its patch needs: the compiler adds up every
# delay, reverb, tape and state array (codegen's finaliseLayout), and the
# voice is allocated with that many slots after its struct, plus this
# shared fallback region at the tail.
const PoolOverflowSlots* = 128

type
  DspState* = object
    pool*:      ptr UncheckedArray[float64]   # this voice's, right after its struct
    poolSlots*: int          # its length, the overflow tail included
    idx*:       int
    overflow*:  bool         # set when claim had to clamp; callback may log
    sr*:        float64

template claim(s: var DspState; n: int = 1): int =
  # Bounds-check so a patch requesting more state than the pool holds
  # degrades to shared garbage-state rather than segfaulting the engine.
  if s.idx + n > s.poolSlots - PoolOverflowSlots:
    s.overflow = true
    s.poolSlots - PoolOverflowSlots
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
# The cutoff is in Hz: a = 1 - exp(-2 pi fc / sr) puts the -3 dB point at
# fc (within 3% up to 4 kHz at 48 kHz). Until 2026-10-12 it was
# a = cutoff / sr, so lp1(x, 4000) cut at 665 Hz; patches were converted.

proc onePoleCoef(cutoff, sr: float64): float64 {.inline.} =
  1.0 - exp(-TAU * max(cutoff, 0.0) / sr)

# Each stateful native with a coefficient comes in two halves: `coef`
# (pure, from the parameters and the rate) and the `_c` step that takes
# the coefficient ready-made. When a patch's parameters can't change
# from sample to sample, codegen computes the coefficient once
# (hoist_init) and calls the `_c` step; otherwise the plain native
# does both every sample. The plain native is literally one calling
# the other, so the two paths produce the same bits.

proc nLp1Coef*(cutoff, sr: float64): float64 {.cdecl, exportc: "n_lp1_coef".} =
  onePoleCoef(cutoff, sr)

proc nLp1C*(s: var DspState; signal, a: float64): float64 {.cdecl, exportc: "n_lp1_c".} =
  let i = s.claim()
  s.pool[i] += a * (signal - s.pool[i])
  s.pool[i]

proc nHp1C*(s: var DspState; signal, a: float64): float64 {.cdecl, exportc: "n_hp1_c".} =
  let i = s.claim()
  s.pool[i] += a * (signal - s.pool[i])
  signal - s.pool[i]

proc nLp1*(s: var DspState; signal, cutoff: float64): float64 {.cdecl, exportc: "n_lp1".} =
  nLp1C(s, signal, onePoleCoef(cutoff, s.sr))

proc nHp1*(s: var DspState; signal, cutoff: float64): float64 {.cdecl, exportc: "n_hp1".} =
  nHp1C(s, signal, onePoleCoef(cutoff, s.sr))

# ------------------------------------------------ SVF (Cytomic trapezoidal)

type FilterMode* = enum fmLow, fmHigh, fmBand, fmNotch

# The SVF's coefficients: k, a1, a2, a3 from the cutoff, resonance and
# rate. Clamp cutoff to (1 Hz, just under Nyquist). A negative or zero
# cutoff would push tan() into the wrong quadrant and produce NaN/Inf
# that the filter would then accumulate into its pool slots forever;
# clamping here turns "WOOP and silence" into a quiet/audible
# degradation.
proc svfCoef(cutoffArg, res, sr: float64): array[4, float64] {.inline.} =
  let cutoff = clamp(cutoffArg, 1.0, sr * 0.499)
  let g = tan(PI * cutoff / sr)
  let k = 2.0 * (1.0 - res)
  let a1 = 1.0 / (1.0 + g * (g + k))
  let a2 = g * a1
  let a3 = g * a2
  [k, a1, a2, a3]

proc nSvfCoef*(cutoff, res, sr: float64;
               o: ptr UncheckedArray[float64]) {.cdecl, exportc: "n_svf_coef".} =
  let k = svfCoef(cutoff, res, sr)
  for j in 0 .. 3: o[j] = k[j]

proc svfC(s: var DspState; signal, k, a1, a2, a3: float64;
          mode: FilterMode): float64 {.inline.} =
  let i = s.claim(2)
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

proc svf(s: var DspState; signal, cutoffArg, res: float64;
         mode: FilterMode): float64 {.inline.} =
  let k = svfCoef(cutoffArg, res, s.sr)
  svfC(s, signal, k[0], k[1], k[2], k[3], mode)

proc nLpf*(s: var DspState; signal, cutoff, res: float64): float64 {.cdecl, exportc: "n_lpf".} =
  svf(s, signal, cutoff, res, fmLow)
proc nHpf*(s: var DspState; signal, cutoff, res: float64): float64 {.cdecl, exportc: "n_hpf".} =
  svf(s, signal, cutoff, res, fmHigh)
proc nBpf*(s: var DspState; signal, cutoff, res: float64): float64 {.cdecl, exportc: "n_bpf".} =
  svf(s, signal, cutoff, res, fmBand)
proc nNotch*(s: var DspState; signal, cutoff, res: float64): float64 {.cdecl, exportc: "n_notch".} =
  svf(s, signal, cutoff, res, fmNotch)

# The hoisted steps: coefficients from n_svf_coef, computed once.
proc nLpfC*(s: var DspState; signal: float64;
            c: ptr UncheckedArray[float64]): float64 {.cdecl, exportc: "n_lpf_c".} =
  svfC(s, signal, c[0], c[1], c[2], c[3], fmLow)
proc nHpfC*(s: var DspState; signal: float64;
            c: ptr UncheckedArray[float64]): float64 {.cdecl, exportc: "n_hpf_c".} =
  svfC(s, signal, c[0], c[1], c[2], c[3], fmHigh)
proc nBpfC*(s: var DspState; signal: float64;
            c: ptr UncheckedArray[float64]): float64 {.cdecl, exportc: "n_bpf_c".} =
  svfC(s, signal, c[0], c[1], c[2], c[3], fmBand)
proc nNotchC*(s: var DspState; signal: float64;
              c: ptr UncheckedArray[float64]): float64 {.cdecl, exportc: "n_notch_c".} =
  svfC(s, signal, c[0], c[1], c[2], c[3], fmNotch)

# ------------------------------------------------------------------- delays
# Buffer lives inline in the pool. Layout: [cursor, sample0, sample1, ...].
#
# `time` is a real number of samples, not a whole one: the read point
# sits between two taps and the two are mixed linearly (a fractional
# delay). Physical models need this: a tube or string whose length is a
# whole number of samples is tuned to the sample rate, not to the
# player, and an 8-section tract came out 27% too short before this.
# Linear interpolation has a mild highpass-ish droop at half-sample
# positions (about -2 dB at 10 kHz, nothing below 2 kHz); if that ever
# matters an allpass interpolator can take its place.
#
# `delay` writes before it reads, so a time of 0 is this sample's input
# (it used to return the oldest sample in the buffer). The longest delay
# is one sample short of `max_time`. `fbdelay` reads before it writes
# (the feedback needs last pass's output) and so starts at one sample.

proc tapInterp(s: var DspState; base, bufLen, cursor: int;
               d: float64): float64 {.inline.} =
  # Sample `d` (>= 0, <= bufLen - 1) samples behind `cursor`.
  let d0 = int(d)
  let frac = d - float64(d0)
  let i0 = (cursor - d0 + bufLen) mod bufLen
  if frac <= 0.0: return s.pool[base + 1 + i0]
  let i1 = (i0 - 1 + bufLen) mod bufLen         # one sample older
  s.pool[base + 1 + i0] * (1.0 - frac) + s.pool[base + 1 + i1] * frac

proc nDelay*(s: var DspState; signal, time, maxTime: float64): float64 {.cdecl, exportc: "n_delay".} =
  let bufLen = max(1, int(maxTime * s.sr))
  let base = s.claim(1 + bufLen)
  let cursor = int(s.pool[base]) mod bufLen
  s.pool[base + 1 + cursor] = signal
  let d = clamp(time * s.sr, 0.0, float64(bufLen - 1))
  result = tapInterp(s, base, bufLen, cursor, d)
  s.pool[base] = float64((cursor + 1) mod bufLen)

proc nFbdelay*(s: var DspState; signal, time, maxTime, fb: float64): float64 {.cdecl, exportc: "n_fbdelay".} =
  let bufLen = max(1, int(maxTime * s.sr))
  let base = s.claim(1 + bufLen)
  let cursor = int(s.pool[base]) mod bufLen
  let d = clamp(time * s.sr, min(1.0, float64(bufLen - 1)), float64(bufLen - 1))
  result = tapInterp(s, base, bufLen, cursor, d)
  s.pool[base + 1 + cursor] = signal + result * fb
  s.pool[base] = float64((cursor + 1) mod bufLen)

# -------------------------------------------------------------------- tape
# A piece of tape `max_time` long, with a write head and a read head whose
# positions are signals, in seconds, wrapping round it. Layout: the
# samples, nothing else (the heads are where the patch says they are).
#
# It reads before it writes, as a deck's play head sits behind its record
# head: a read at the write position hears what was there, one trip round
# the tape ago, which is what a loop recording over itself must play. The
# read is between samples (linear, as `delay`), so a slow head bends the
# pitch; the write lands on the nearest sample. While `on` > 0 the write
# is `signal` plus what was there times `keep` (the erase head: 0 records
# over, 1 adds, between fades old passes). `delay` is a tape whose write
# head stays at now.

proc nTape*(s: var DspState; signal, on, w, r, maxTime, keep: float64): float64 {.cdecl, exportc: "n_tape".} =
  let n = max(1, int(maxTime * s.sr))
  let base = s.claim(n)
  var at = floorMod(r * s.sr, float64(n))
  # A head on a sample reads that sample: `t * sr` lands a hair either
  # side of a whole number, and a loop (r = w) must not hear a millionth
  # of the sample just written beside it.
  let near = floor(at + 0.5)
  if abs(at - near) < 1e-6: at = floorMod(near, float64(n))
  let i = min(int(at), n - 1)
  let frac = at - float64(i)
  result = s.pool[base + i] * (1.0 - frac) + s.pool[base + (i + 1) mod n] * frac
  if on > 0.0:
    let k = int(floorMod(floor(w * s.sr + 0.5), float64(n)))
    s.pool[base + k] = signal + s.pool[base + k] * keep

# ------------------------------------------------------------------ reverb
# Schroeder: 4 parallel comb filters into 2 series allpass.

const ReverbCombLens = [1557, 1617, 1491, 1422]
const ReverbApLens = [225, 556]

# The slots one reverb claims: each comb's cursor, damp state and
# buffer, each allpass's cursor and buffer.
proc reverbSlots(): int =
  for L in ReverbCombLens: result += 2 + L
  for L in ReverbApLens: result += 1 + L

# Each comb's feedback gain from the decay time: one pow per comb.
proc reverbCoef(rt60, sr: float64): array[4, float64] {.inline.} =
  for ci in 0 .. 3:
    result[ci] = pow(10.0, -3.0 * float64(ReverbCombLens[ci]) / (rt60 * sr))

proc nReverbCoef*(rt60, sr: float64;
                  o: ptr UncheckedArray[float64]) {.cdecl, exportc: "n_reverb_coef".} =
  let g = reverbCoef(rt60, sr)
  for j in 0 .. 3: o[j] = g[j]

proc reverbC(s: var DspState; signal, wet: float64;
             gains: array[4, float64]): float64 {.inline.} =
  const
    combLens = ReverbCombLens
    apLens   = ReverbApLens
    apFb     = 0.5
    damp     = 0.3
  let base = s.claim(reverbSlots())
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
    let g = gains[ci]
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

proc nReverb*(s: var DspState; signal, rt60, wet: float64): float64 {.cdecl, exportc: "n_reverb".} =
  reverbC(s, signal, wet, reverbCoef(rt60, s.sr))

proc nReverbC*(s: var DspState; signal, wet: float64;
               g: ptr UncheckedArray[float64]): float64 {.cdecl, exportc: "n_reverb_c".} =
  reverbC(s, signal, wet, [g[0], g[1], g[2], g[3]])

# ---------------------------------------------------------------- physics

proc nImpulse*(s: var DspState; freq: float64): float64 {.cdecl, exportc: "n_impulse".} =
  let i = s.claim()
  let prev = s.pool[i]
  s.pool[i] = (s.pool[i] + freq / s.sr) mod 1.0
  if s.pool[i] < 0.0: s.pool[i] += 1.0
  if s.pool[i] < prev: 1.0 else: 0.0

proc nResonator*(s: var DspState; input, freq, decay: float64): float64 {.cdecl, exportc: "n_resonator".} =
  ## A driven damped oscillator ringing at `freq` Hz; `decay` is the
  ## damping rate (larger dies faster). It took freq in radians per
  ## second until 2026-10-18, so resonator(x, 440, ...) rang at 70 Hz.
  let i = s.claim(2)
  let omega = 2.0 * PI * freq
  let omega2 = omega * omega
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

# ------------------------------------------------------------------ Hilbert
# Two cascaded 4-stage IIR allpass networks (Niemitalo's "improved"
# coefficient set). Their outputs differ by ≈ 90° across the audio band
# with matched group delay, so feeding `signal` to both gives an
# (analytic-real, analytic-imag) pair without the half-sample mismatch
# you'd get from sending signal directly through one branch.
#
# Each stage is a 2nd-order allpass with coefficient c (already a²):
#   y[n] = c*(x[n] + y[n-2]) - x[n-2]
# State per stage = (x[n-1], x[n-2], y[n-1], y[n-2]) = 4 slots.
# Shape: 4 stages × 4 slots = 16 slots per branch.
const HilbertCoefsA = [0.6923878, 0.9360654322959,
                       0.9882295226860, 0.9987488452737]
const HilbertCoefsB = [0.4021921162426, 0.8561710882487,
                       0.9722909545651, 0.9952884791278]

proc hilbertCascade(s: var DspState; base: int; signal: float64;
                    coefs: array[4, float64]): float64 {.inline.} =
  var x = signal
  for k in 0 ..< 4:
    let off = base + k * 4
    let xN2 = s.pool[off + 0]
    let xN1 = s.pool[off + 1]
    let yN2 = s.pool[off + 2]
    let yN1 = s.pool[off + 3]
    let y = coefs[k] * (x + yN2) - xN2
    s.pool[off + 0] = xN1
    s.pool[off + 1] = x
    s.pool[off + 2] = yN1
    s.pool[off + 3] = y
    x = y
  x

proc nHilbertA*(s: var DspState; signal: float64): float64
    {.cdecl, exportc: "n_hilbert_a".} =
  hilbertCascade(s, s.claim(16), signal, HilbertCoefsA)

proc nHilbertB*(s: var DspState; signal: float64): float64
    {.cdecl, exportc: "n_hilbert_b".} =
  hilbertCascade(s, s.claim(16), signal, HilbertCoefsB)

# Single-sideband frequency shifter. analytic(signal) rotated at hz Hz,
# real-part-only. Owns a 32-slot Hilbert pair plus a 1-slot phase
# accumulator. Positive hz shifts up, negative hz shifts down — both
# leave the original spectrum coherent (no mirror image), unlike a
# ring modulator. The lower bound where the Hilbert phase response
# starts to deviate is ≈ 30 Hz; above that the shift is clean.
proc nFreqShift*(s: var DspState; signal, hz: float64): float64
    {.cdecl, exportc: "n_freq_shift".} =
  let baseAB = s.claim(32)
  let realA = hilbertCascade(s, baseAB,        signal, HilbertCoefsA)
  let imagB = hilbertCascade(s, baseAB + 16,   signal, HilbertCoefsB)
  let phaseSlot = s.claim(1)
  s.pool[phaseSlot] = (s.pool[phaseSlot] + hz / s.sr) mod 1.0
  if s.pool[phaseSlot] < 0.0: s.pool[phaseSlot] += 1.0
  let phase = s.pool[phaseSlot] * TAU
  # SSB upshift form: y = m·cos(α) - H[m]·sin(α). The branchB cascade
  # of this allpass design produces -H[m] (standard Hilbert sign with
  # opposite quadrature), so the leading sign here is + rather than -.
  # Net effect: hz > 0 shifts the spectrum up, hz < 0 shifts down —
  # the user-intuitive direction.
  realA * cos(phase) + imagB * sin(phase)

# ------------------------------------------------------------------ input
# The current input frame, written by whoever drives tick(): the engine's
# audio callback (one capture frame per output frame) or render.nim (one
# frame of a recording per sample). Zero when nothing feeds it, so a
# patch that reads input() still runs silently without a capture device.
var inputL*, inputR*: float64

proc nInputL*(): float64 {.cdecl, exportc: "n_input_l".} = inputL
proc nInputR*(): float64 {.cdecl, exportc: "n_input_r".} = inputR

# ------------------------------------------------------------- the natives
# Every native a patch calls by name, in one place: its parameters as
# SPEC names them (`name=v` has a default), the slots one call site
# claims, and its code, `n_<name>` in C. codegen and voice take
# everything else from here. A buffer's length comes from a parameter,
# a literal in seconds: `extra + max(1, int(seconds * sr))` slots.

type
  NativeSpec* = object
    params*: seq[string]
    slots*: int        # the slots a call site claims (with a buffer: besides it)
    bufArg*: int       # the parameter sizing a buffer, or -1
    fn*: pointer

proc fixed(params: seq[string]; slots: int; fn: pointer): NativeSpec =
  NativeSpec(params: params, slots: slots, bufArg: -1, fn: fn)

proc buffered(params: seq[string]; bufArg, extra: int; fn: pointer): NativeSpec =
  NativeSpec(params: params, slots: extra, bufArg: bufArg, fn: fn)

let Natives* = {
  "lp1":       fixed(@["sig", "cut"], 1, cast[pointer](nLp1)),
  "hp1":       fixed(@["sig", "cut"], 1, cast[pointer](nHp1)),
  "lpf":       fixed(@["sig", "cut", "res"], 2, cast[pointer](nLpf)),
  "hpf":       fixed(@["sig", "cut", "res"], 2, cast[pointer](nHpf)),
  "bpf":       fixed(@["sig", "cut", "res"], 2, cast[pointer](nBpf)),
  "notch":     fixed(@["sig", "cut", "res"], 2, cast[pointer](nNotch)),
  "delay":     buffered(@["sig", "time", "max_time"], 2, 1, cast[pointer](nDelay)),
  "fbdelay":   buffered(@["sig", "time", "max_time", "fb"], 2, 1, cast[pointer](nFbdelay)),
  "tape":      buffered(@["sig", "on", "w", "r", "max_time", "keep=0"], 4, 0, cast[pointer](nTape)),
  "reverb":    fixed(@["sig", "rt60", "wet"], reverbSlots(), cast[pointer](nReverb)),
  "impulse":   fixed(@["freq"], 1, cast[pointer](nImpulse)),
  "resonator": fixed(@["sig", "freq", "decay"], 2, cast[pointer](nResonator)),
  "dho":       fixed(@["force", "freq", "damp"], 2, cast[pointer](nDho)),
  "dho_v":     fixed(@["force", "freq", "damp"], 2, cast[pointer](nDhoV)),
  "discharge": fixed(@["sig", "rate"], 1, cast[pointer](nDischarge)),
  "tremolo":   fixed(@["sig", "rate", "depth"], 1, cast[pointer](nTremolo)),
  "slew":      fixed(@["sig", "time"], 1, cast[pointer](nSlew)),
  "wave":      fixed(@["freq", "arr"], 1, cast[pointer](nWave)),   # its table is emitted (codegen)
}.toTable
