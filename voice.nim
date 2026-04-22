## Native voice: codegen → TCC → dlopen → function pointer. Replaces
## the old bytecode VM. Each voice owns a TCC state (kept alive while
## its compiled tick() can run) and a malloc'd VoiceState buffer.

import std/[strutils]
import parser, tcc, dsp, codegen

type
  TickFn* = proc (s: pointer; outL, outR: ptr float64) {.cdecl.}
  SizeFn = proc (): cint {.cdecl.}

  # Mirrors the codegen's VoiceState prefix (through `start_t`). Writing
  # t/start_t from Nim is a plain struct-field store into the malloc'd
  # buffer — no allocation, no GC touch.
  VoiceHeader* {.byref.} = object
    pool*:     array[DspPoolSize, float64]
    idx*:      int
    overflow*: bool
    sr*:       float64
    t*, dt*, start_t*: float64

  NativeVoice* = ref object
    state*:     pointer          # malloc'd VoiceState buffer
    stateSize*: int
    tickFn*:    TickFn
    tccLib*:    TccState
    oldLibs*:   seq[TccState]    # kept alive across hot reloads (small leak)
    varNames*:  seq[string]
    startT*:    float64
    # Play-block support (unused in phase 1; present so engine.nim's
    # per-sample gain loop is a zero-iter no-op without special cases).
    partNames*:       seq[string]
    partGains*:       seq[float64]
    partFadeDeltas*:  seq[float64]
    partFadeTargets*: seq[float64]

proc errHandler(opaque: pointer; msg: cstring) {.cdecl.} =
  stderr.writeLine "TCC: ", $msg

proc registerNatives(s: TccState) =
  discard s.addSymbol("n_lpf",       cast[pointer](nLpf))
  discard s.addSymbol("n_hpf",       cast[pointer](nHpf))
  discard s.addSymbol("n_bpf",       cast[pointer](nBpf))
  discard s.addSymbol("n_notch",     cast[pointer](nNotch))
  discard s.addSymbol("n_lp1",       cast[pointer](nLp1))
  discard s.addSymbol("n_hp1",       cast[pointer](nHp1))
  discard s.addSymbol("n_delay",     cast[pointer](nDelay))
  discard s.addSymbol("n_fbdelay",   cast[pointer](nFbdelay))
  discard s.addSymbol("n_reverb",    cast[pointer](nReverb))
  discard s.addSymbol("n_impulse",   cast[pointer](nImpulse))
  discard s.addSymbol("n_resonator", cast[pointer](nResonator))
  discard s.addSymbol("n_discharge", cast[pointer](nDischarge))
  discard s.addSymbol("n_tremolo",   cast[pointer](nTremolo))
  discard s.addSymbol("n_slew",      cast[pointer](nSlew))
  discard s.addSymbol("n_wave",      cast[pointer](nWave))
  discard s.addSymbol("shape_saw",   cast[pointer](shapeSaw))
  discard s.addSymbol("shape_tri",   cast[pointer](shapeTri))
  discard s.addSymbol("shape_sqr",   cast[pointer](shapeSqr))

proc compileProgram(program: Node):
    tuple[lib: TccState; tickFn: TickFn; size: int; varNames: seq[string]] =
  let (csrc, varNames) = generate(program)
  let lib = tccNew()
  if cast[pointer](lib) == nil:
    raise newException(ValueError, "tcc_new failed")
  lib.setErrorFunc(nil, errHandler)
  if lib.setOutputType(OutputMemory) != 0:
    lib.delete()
    raise newException(ValueError, "tcc_set_output_type failed")
  if lib.addLibrary("m") != 0:
    lib.delete()
    raise newException(ValueError, "tcc_add_library m failed")
  registerNatives(lib)
  # Append a size-reporting helper so the engine allocates exactly
  # sizeof(VoiceState) — which varies by patch (extra fields per top-level var).
  let withSize = csrc & "\nint voice_state_size(void) { return sizeof(VoiceState); }\n"
  if lib.compileString(withSize) < 0:
    lib.delete()
    raise newException(ValueError, "TCC compile failed")
  if lib.relocate() < 0:
    lib.delete()
    raise newException(ValueError, "TCC relocate failed")
  let fn = cast[TickFn](lib.getSymbol("tick"))
  let sizeFn = cast[SizeFn](lib.getSymbol("voice_state_size"))
  if fn == nil or sizeFn == nil:
    lib.delete()
    raise newException(ValueError, "TCC symbol lookup failed")
  (lib, fn, int(sizeFn()), varNames)

proc newVoice*(sr: float64): NativeVoice =
  NativeVoice()

proc load*(v: NativeVoice; program: Node; sr: float64) =
  let (lib, fn, size, varNames) = compileProgram(program)
  # Fresh state buffer. (State migration by-name is a later step; for now
  # hot reload retriggers the voice fresh.)
  let newState = alloc0(size)
  cast[ptr VoiceHeader](newState).sr = sr
  cast[ptr VoiceHeader](newState).dt = 1.0 / sr
  if v.state != nil:
    dealloc(v.state)
    # Hold onto the old lib until the next swap — audio callback may have
    # been mid-call. This leaks one TCC state (~10 KB) per swap.
    v.oldLibs.add v.tccLib
  v.state = newState
  v.stateSize = size
  v.tickFn = fn
  v.tccLib = lib
  v.varNames = varNames

proc tick*(v: NativeVoice; t: float64): tuple[l, r: float64] {.inline.} =
  let hdr = cast[ptr VoiceHeader](v.state)
  hdr.t = t
  hdr.start_t = v.startT
  var l, r: float64
  v.tickFn(v.state, addr l, addr r)
  (l, r)
