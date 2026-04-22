## End-to-end: parse stdlib + kick.aither, generate C, compile via TCC,
## run tick() 48000 times, verify we get an audible impulse-driven tone.

import std/[math, os]
import ../parser, ../dsp, ../tcc, ../codegen

proc errHandler(opaque: pointer; msg: cstring) {.cdecl.} =
  stderr.writeLine "TCC: ", $msg

const Stdlib = staticRead("../stdlib.aither")

let patch = if paramCount() >= 1: paramStr(1) else: "patches/kick.aither"
let userSrc = readFile(patch)
let stdAst = parseProgram(Stdlib)
let userAst = parseProgram(userSrc)
let program = Node(kind: nkBlock, kids: stdAst.kids & userAst.kids, line: 1)

let (csrc, varNames) = generate(program)
echo "---- generated C ----"
echo csrc
echo "---- vars: ", varNames

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

doAssert s.compileString(csrc) == 0, "compile failed"
doAssert s.relocate() == 0

type
  VState = object
    state: DspState
    t, dt, start_t: float64
    vars: array[32, float64]      # plenty of room
    inited: array[32, uint8]
  TickFn = proc (s: ptr VState; outL, outR: ptr float64) {.cdecl.}

let fn = cast[TickFn](s.getSymbol("tick"))
doAssert fn != nil

var st = create(VState)
st.state.sr = 48000.0
st.dt = 1.0 / 48000.0

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
doAssert peak > 0.05, "kick should produce audible signal"
doAssert peak < 1.0, "kick should stay below clip"
dealloc(st)
s.delete()
echo "kick codegen ok"
