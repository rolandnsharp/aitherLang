## Native DSP primitives: dsp.c, compiled into aither and called from a
## patch's generated C. This module is the bridge: the C functions under
## their Nim names, and the table of what a patch calls by name.

import std/[tables, os, strutils]

const DspH = currentSourcePath().parentDir / "dsp.h"   # found from any build dir
{.compile("dsp.c", "-ffp-contract=off").}

const PoolOverflowSlots* = 128   # dsp.h's POOL_OVERFLOW_SLOTS

type
  DspState* {.importc: "DspState", header: DspH, bycopy.} = object
    pool*      {.importc: "pool".}: ptr UncheckedArray[float64]
    poolSlots* {.importc: "pool_slots".}: int
    idx*       {.importc: "idx".}: int
    overflow*  {.importc: "overflow".}: bool
    sr*        {.importc: "sr".}: float64

{.push cdecl, header: DspH.}
proc shapeSaw*(x: float64): float64 {.importc: "shape_saw".}
proc shapeTri*(x: float64): float64 {.importc: "shape_tri".}
proc shapeSqr*(x: float64): float64 {.importc: "shape_sqr".}
proc nWave*(s: var DspState; freq: float64; values: ptr UncheckedArray[float64]; n: cint): float64 {.importc: "n_wave".}
proc nLp1Coef*(cutoff, sr: float64): float64 {.importc: "n_lp1_coef".}
proc nLp1C*(s: var DspState; signal, a: float64): float64 {.importc: "n_lp1_c".}
proc nHp1C*(s: var DspState; signal, a: float64): float64 {.importc: "n_hp1_c".}
proc nLp1*(s: var DspState; signal, cutoff: float64): float64 {.importc: "n_lp1".}
proc nHp1*(s: var DspState; signal, cutoff: float64): float64 {.importc: "n_hp1".}
proc nSvfCoef*(cutoff, res, sr: float64; o: ptr UncheckedArray[float64]) {.importc: "n_svf_coef".}
proc nLpf*(s: var DspState; signal, cutoff, res: float64): float64 {.importc: "n_lpf".}
proc nHpf*(s: var DspState; signal, cutoff, res: float64): float64 {.importc: "n_hpf".}
proc nBpf*(s: var DspState; signal, cutoff, res: float64): float64 {.importc: "n_bpf".}
proc nNotch*(s: var DspState; signal, cutoff, res: float64): float64 {.importc: "n_notch".}
proc nLpfC*(s: var DspState; signal: float64; c: ptr UncheckedArray[float64]): float64 {.importc: "n_lpf_c".}
proc nHpfC*(s: var DspState; signal: float64; c: ptr UncheckedArray[float64]): float64 {.importc: "n_hpf_c".}
proc nBpfC*(s: var DspState; signal: float64; c: ptr UncheckedArray[float64]): float64 {.importc: "n_bpf_c".}
proc nNotchC*(s: var DspState; signal: float64; c: ptr UncheckedArray[float64]): float64 {.importc: "n_notch_c".}
proc nDelay*(s: var DspState; signal, time, maxTime: float64): float64 {.importc: "n_delay".}
proc nFbdelay*(s: var DspState; signal, time, maxTime, fb: float64): float64 {.importc: "n_fbdelay".}
proc nTape*(s: var DspState; signal, on, w, r, maxTime, keep: float64): float64 {.importc: "n_tape".}
proc nReverbCoef*(rt60, sr: float64; o: ptr UncheckedArray[float64]) {.importc: "n_reverb_coef".}
proc nReverb*(s: var DspState; signal, rt60, wet: float64): float64 {.importc: "n_reverb".}
proc nReverbC*(s: var DspState; signal, wet: float64; g: ptr UncheckedArray[float64]): float64 {.importc: "n_reverb_c".}
proc dspReverbSlots*(): clong {.importc: "dsp_reverb_slots".}
proc nImpulse*(s: var DspState; freq: float64): float64 {.importc: "n_impulse".}
proc nDischarge*(s: var DspState; input, rate: float64): float64 {.importc: "n_discharge".}
proc nHilbertA*(s: var DspState; signal: float64): float64 {.importc: "n_hilbert_a".}
proc nHilbertB*(s: var DspState; signal: float64): float64 {.importc: "n_hilbert_b".}
proc nFreqShift*(s: var DspState; signal, hz: float64): float64 {.importc: "n_freq_shift".}
proc nInputL*(): float64 {.importc: "n_input_l".}
proc nInputR*(): float64 {.importc: "n_input_r".}
{.pop.}

# The current input frame, written by whoever drives tick(): the engine's
# audio callback (one capture frame per output frame) or render.nim (one
# frame of a recording per sample). Zero when nothing feeds it.
var inputL* {.importc: "dsp_input_l", header: DspH.}: float64
var inputR* {.importc: "dsp_input_r", header: DspH.}: float64

# ------------------------------------------------------------- the natives
# Every native a patch calls by name: dsp.c's dsp_natives, the one list,
# read here in its order (so the C compilers list them alike).

type
  NativeSpec* = object
    params*: seq[string]
    slots*: int        # the slots a call site claims (with a buffer: besides it)
    bufArg*: int       # the parameter sizing a buffer, or -1
    fn*: pointer
  NativeRow {.importc: "NativeSpec", header: DspH, bycopy.} = object
    name, params: cstring
    slots: clong
    buf_arg: cint
    fn: pointer

proc dspNatives(): ptr UncheckedArray[NativeRow] =
  {.emit: "`result` = (NativeSpec*)dsp_natives;".}

proc readNatives(): OrderedTable[string, NativeSpec] =
  let rows = dspNatives()
  var i = 0
  while rows[i].name != nil:
    let r = rows[i]
    result[$r.name] = NativeSpec(params: ($r.params).split(','), slots: int(r.slots),
                                 bufArg: int(r.buf_arg), fn: r.fn)
    inc i

let Natives* = readNatives()
