## End-to-end: parse stdlib + kick.aither, generate C, compile via TCC,
## run tick() 48000 times, verify we get an audible impulse-driven tone.

import std/[math, os, monotimes]
import ../parser, ../dsp, ../tcc, ../codegen

proc errHandler(opaque: pointer; msg: cstring) {.cdecl.} =
  stderr.writeLine "TCC: ", $msg

const Stdlib = staticRead("../stdlib.aither")

let patch = if paramCount() >= 1: paramStr(1) else: "patches/kick.aither"
let userSrc = readFile(patch)
let stdAst = parseProgram(Stdlib)
let userAst = parseProgram(userSrc)
let program = Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

let (csrc, varNames, partNames, _) = generate(program)
echo "---- generated C ----"
echo csrc
echo "---- vars: ", varNames
echo "---- parts: ", partNames

let s = tccNew()
s.setErrorFunc(nil, errHandler)
doAssert s.setOutputType(OutputMemory) == 0
doAssert s.addLibrary("m") == 0
# Register dsp.nim symbols
doAssert s.addSymbol("n_lpf",      cast[pointer](nLpf)) == 0
doAssert s.addSymbol("n_hpf",      cast[pointer](nHpf)) == 0
doAssert s.addSymbol("n_bpf",      cast[pointer](nBpf)) == 0
doAssert s.addSymbol("n_notch",    cast[pointer](nNotch)) == 0
doAssert s.addSymbol("n_lp1",      cast[pointer](nLp1)) == 0
doAssert s.addSymbol("n_hp1",      cast[pointer](nHp1)) == 0
doAssert s.addSymbol("n_delay",    cast[pointer](nDelay)) == 0
doAssert s.addSymbol("n_fbdelay",  cast[pointer](nFbdelay)) == 0
doAssert s.addSymbol("n_reverb",   cast[pointer](nReverb)) == 0
doAssert s.addSymbol("n_impulse",  cast[pointer](nImpulse)) == 0
doAssert s.addSymbol("n_resonator",cast[pointer](nResonator)) == 0
doAssert s.addSymbol("n_discharge",cast[pointer](nDischarge)) == 0
doAssert s.addSymbol("n_tremolo",  cast[pointer](nTremolo)) == 0
doAssert s.addSymbol("n_slew",     cast[pointer](nSlew)) == 0
doAssert s.addSymbol("n_wave",     cast[pointer](nWave)) == 0
doAssert s.addSymbol("shape_saw",  cast[pointer](shapeSaw)) == 0
doAssert s.addSymbol("shape_tri",  cast[pointer](shapeTri)) == 0
doAssert s.addSymbol("shape_sqr",  cast[pointer](shapeSqr)) == 0
# tick() (the one-sample entry) reads input() from the dsp globals.
doAssert s.addSymbol("n_input_l",  cast[pointer](nInputL)) == 0
doAssert s.addSymbol("n_input_r",  cast[pointer](nInputR)) == 0
# Hoisted coefficient forms (hoist_init).
doAssert s.addSymbol("n_lp1_coef", cast[pointer](nLp1Coef)) == 0
doAssert s.addSymbol("n_lp1_c",    cast[pointer](nLp1C)) == 0
doAssert s.addSymbol("n_hp1_c",    cast[pointer](nHp1C)) == 0
doAssert s.addSymbol("n_svf_coef", cast[pointer](nSvfCoef)) == 0
doAssert s.addSymbol("n_lpf_c",    cast[pointer](nLpfC)) == 0
doAssert s.addSymbol("n_hpf_c",    cast[pointer](nHpfC)) == 0
doAssert s.addSymbol("n_bpf_c",    cast[pointer](nBpfC)) == 0
doAssert s.addSymbol("n_notch_c",  cast[pointer](nNotchC)) == 0
doAssert s.addSymbol("n_reverb_coef", cast[pointer](nReverbCoef)) == 0
doAssert s.addSymbol("n_reverb_c", cast[pointer](nReverbC)) == 0
# `aither profile`'s clock: tick1 reads it only while profiling.
proc nowSec(): float64 {.cdecl.} = float64(getMonoTime().ticks) * 1e-9
doAssert s.addSymbol("n_now",      cast[pointer](nowSec)) == 0

doAssert s.compileString(csrc) == 0, "compile failed"
doAssert s.relocate() == 0

type
  VState = object
    state: DspState
    t, dt: float64
    partGainsPtr: pointer
    partOutPtr: pointer
    vars: array[32, float64]      # plenty of room
    inited: array[32, uint8]
  TickFn = proc (s: ptr VState; outL, outR: ptr float64) {.cdecl.}

let fn = cast[TickFn](s.getSymbol("tick"))
doAssert fn != nil

var st = create(VState)
# The pool the patch asks for (voice_pool_slots), beside the state.
let slots = cast[proc (): clong {.cdecl.}](s.getSymbol("voice_pool_slots"))()
var pool = newSeq[float64](slots)
st.state.pool = cast[ptr UncheckedArray[float64]](addr pool[0])
st.state.poolSlots = int(slots)
st.state.sr = 48000.0
st.dt = 1.0 / 48000.0
var partGains: array[16, float64]
for i in 0 ..< partGains.len: partGains[i] = 1.0
st.partGainsPtr = partGains[0].addr

var peak = 0.0
var energy = 0.0
for i in 0 ..< 48000:
  st.t = float64(i) * st.dt
  var l, r: float64
  fn(st, addr l, addr r)
  if abs(l) > peak: peak = abs(l)
  energy += l * l
  if i < 5 or (i mod 8000 == 0):
    echo "sample ", i, ": l=", l, " r=", r
echo "peak=", peak, " rms=", sqrt(energy / 48000.0)
doAssert peak > 0.0, "patch produced no signal"
doAssert peak < 2.0, "patch saturated wildly"
dealloc(st)
s.delete()
echo "kick codegen ok"
