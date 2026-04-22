## Native voice: codegen → TCC → dlopen → function pointer. Replaces
## the old bytecode VM. Each voice owns a TCC state (kept alive while
## its compiled tick() can run) and a malloc'd VoiceState buffer.

import std/[strutils, tables]
import parser, tcc, dsp, codegen

type
  TickFn* = proc (s: pointer; outL, outR: ptr float64) {.cdecl.}
  SizeFn = proc (): cint {.cdecl.}

  # Mirrors the codegen's VoiceState prefix (through `start_t`). Writing
  # t/start_t from Nim is a plain struct-field store into the malloc'd
  # buffer — no allocation, no GC touch.
  VoiceHeader* {.byref.} = object
    pool*:          array[DspPoolSize, float64]
    idx*:           int
    overflow*:      bool
    sr*:            float64
    t*, dt*, start_t*: float64
    # Points into the Nim-owned partGains seq so generated C can read
    # per-part gains. Set at load(); the seq is sized once (on first load
    # per voice) so its data address stays stable.
    partGainsPtr*:  pointer

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

# TCC invokes this synchronously during compile/relocate; we stash the
# most recent message so compileProgram can report it on failure.
var lastTccError {.threadvar.}: string

proc errHandler(opaque: pointer; msg: cstring) {.cdecl.} =
  lastTccError = $msg
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

proc compileProgram(program: Node; patchPath: string):
    tuple[lib: TccState; tickFn: TickFn; size: int;
          varNames, partNames: seq[string]] =
  let (csrc, varNames, partNames) = generate(program, patchPath)
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
  lastTccError = ""
  registerNatives(lib)
  # Append a size-reporting helper so the engine allocates exactly
  # sizeof(VoiceState) — which varies by patch (extra fields per top-level var).
  let withSize = csrc & "\nint voice_state_size(void) { return sizeof(VoiceState); }\n"
  if lib.compileString(withSize) < 0:
    lib.delete()
    raise newException(ValueError,
      if lastTccError.len > 0: lastTccError else: "TCC compile failed")
  if lib.relocate() < 0:
    lib.delete()
    raise newException(ValueError, "TCC relocate failed")
  let fn = cast[TickFn](lib.getSymbol("tick"))
  let sizeFn = cast[SizeFn](lib.getSymbol("voice_state_size"))
  if fn == nil or sizeFn == nil:
    lib.delete()
    raise newException(ValueError, "TCC symbol lookup failed")
  (lib, fn, int(sizeFn()), varNames, partNames)

proc newVoice*(sr: float64): NativeVoice =
  NativeVoice()

# Layout constants mirror the codegen's emitted struct — see the NOTE in
# codegen.nim near the VoiceState emission. Migration here writes directly
# into the buffer by computed offsets; stay in lockstep with codegen.
const HeaderSize  = sizeof(VoiceHeader)   # bytes up to and including start_t
const VarSlotSize = sizeof(float64)

proc varAddr(state: pointer; i: int): ptr float64 {.inline.} =
  cast[ptr float64](cast[uint](state) + uint(HeaderSize) +
                    uint(i) * uint(VarSlotSize))

proc initedAddr(state: pointer; nVars, i: int): ptr uint8 {.inline.} =
  cast[ptr uint8](cast[uint](state) + uint(HeaderSize) +
                  uint(nVars) * uint(VarSlotSize) + uint(i))

proc load*(v: NativeVoice; program: Node; sr: float64;
           patchPath: string = "") =
  # Snapshot the previous compilation's var values by name. Anything that
  # migrates gets its inited bit set so the lazy-init guard skips it.
  var snapshot = initTable[string, float64]()
  if v.state != nil:
    for i, name in v.varNames:
      snapshot[name] = varAddr(v.state, i)[]

  let (lib, fn, size, varNames, partNames) = compileProgram(program, patchPath)
  let newState = alloc0(size)
  cast[ptr VoiceHeader](newState).sr = sr
  cast[ptr VoiceHeader](newState).dt = 1.0 / sr

  for i, name in varNames:
    if name in snapshot:
      varAddr(newState, i)[] = snapshot[name]
      initedAddr(newState, varNames.len, i)[] = 1'u8

  # Diff old partNames vs new; preserve gain by name, default to 1.0.
  var oldPartGains = initTable[string, float64]()
  var oldPartTargets = initTable[string, float64]()
  var oldPartDeltas = initTable[string, float64]()
  for i, n in v.partNames:
    oldPartGains[n] = v.partGains[i]
    oldPartTargets[n] = v.partFadeTargets[i]
    oldPartDeltas[n] = v.partFadeDeltas[i]
  v.partNames = partNames
  v.partGains.setLen(partNames.len)
  v.partFadeDeltas.setLen(partNames.len)
  v.partFadeTargets.setLen(partNames.len)
  for i, n in partNames:
    v.partGains[i] = oldPartGains.getOrDefault(n, 1.0)
    v.partFadeTargets[i] = oldPartTargets.getOrDefault(n, v.partGains[i])
    v.partFadeDeltas[i] = oldPartDeltas.getOrDefault(n, 0.0)
  # Publish gain-array pointer into the new state. The seq's data is
  # stable for the lifetime of this allocation (we set its length above
  # and never append past it). If there are no parts, point at a dummy
  # one-element stash to avoid a null deref from the generated code.
  if partNames.len > 0:
    cast[ptr VoiceHeader](newState).partGainsPtr = v.partGains[0].addr
  else:
    cast[ptr VoiceHeader](newState).partGainsPtr = nil

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
