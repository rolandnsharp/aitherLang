## Native voice: codegen → TCC → dlopen → function pointer. Replaces
## the old bytecode VM. Each voice owns a TCC state (kept alive while
## its compiled tick() can run) and a malloc'd VoiceState buffer.

import std/[strutils, tables]
import parser, tcc, dsp, codegen, midi

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
    # Per-helper-type layout of this voice's pool (see codegen.Region).
    # Used at hot-reload time to copy matching (type, perTypeIdx) regions
    # from the old state into the new one.
    regions*:         seq[Region]

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
  discard s.addSymbol("n_midi_cc",         cast[pointer](nMidiCc))
  discard s.addSymbol("n_midi_note",       cast[pointer](nMidiNote))
  discard s.addSymbol("n_midi_freq",       cast[pointer](nMidiFreq))
  discard s.addSymbol("n_midi_gate",       cast[pointer](nMidiGate))
  discard s.addSymbol("n_midi_trig",       cast[pointer](nMidiTrig))
  discard s.addSymbol("n_midi_voice_freq", cast[pointer](nMidiVoiceFreq))
  discard s.addSymbol("n_midi_voice_gate", cast[pointer](nMidiVoiceGate))

proc compileProgram(program: Node; patchPath: string; sr: float64):
    tuple[lib: TccState; tickFn: TickFn; size: int;
          varNames, partNames: seq[string]; regions: seq[Region]] =
  let (csrc, varNames, partNames, regions) = generate(program, patchPath, sr)
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
  (lib, fn, int(sizeFn()), varNames, partNames, regions)

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

type
  # The output of the heavy phase (compile + alloc). Doesn't touch the
  # live voice, so it can run outside the audio mutex.
  Prepared* = ref object
    lib*: TccState
    tickFn*: TickFn
    state*: pointer
    stateSize*: int
    varNames*, partNames*: seq[string]
    regions*: seq[Region]

proc prepare*(program: Node; sr: float64; patchPath: string = ""): Prepared =
  ## Compile + allocate the new state buffer. Slow (TCC compile is 5-20 ms
  ## on big patches, state zero-init is another ms for 4 MB). Caller must
  ## hand the result to `commit` to actually swap it in.
  let (lib, fn, size, varNames, partNames, regions) =
    compileProgram(program, patchPath, sr)
  let newState = alloc0(size)
  cast[ptr VoiceHeader](newState).sr = sr
  cast[ptr VoiceHeader](newState).dt = 1.0 / sr
  Prepared(lib: lib, tickFn: fn, state: newState, stateSize: size,
           varNames: varNames, partNames: partNames, regions: regions)

# Defense 1: a region is "poisoned" if any of its slots is NaN, ±Inf,
# or absurdly large (a near-blow-up that hasn't tipped to Inf yet).
# Carrying poison forward across hot reload would mean fixing the patch
# doesn't help — the migrated state stays sick.
proc isPoisonedRegion(pool: ptr UncheckedArray[float64];
                      offset, size: int): bool {.inline.} =
  for k in 0 ..< size:
    let v = pool[offset + k]
    if v != v or v > 1e6 or v < -1e6: return true
  false

# Defense 2: zero the stateful-primitive regions of the DSP pool.
# Called from the audio callback when a tick produces NaN/Inf — the
# next tick reads from clean filter / delay / reverb state. Regions
# whose typeName is "var" are skipped: those hold user-named state
# (counters, fade-in start times, etc.) that performance patches rely
# on, and resetting them would re-trigger user-authored fades on every
# NaN event. Top-level vars sit after the pool in the struct and
# aren't touched either.
proc resetPool*(v: NativeVoice) =
  if v.state == nil: return
  let pool = cast[ptr UncheckedArray[float64]](v.state)
  for r in v.regions:
    if r.typeName == "var": continue
    zeroMem(addr pool[r.offset], r.size * sizeof(float64))

proc commit*(v: NativeVoice; p: Prepared) =
  ## Apply a prepared compile. Must be called under the audio mutex:
  ## touches v.state / v.tickFn which the audio thread reads each sample.
  ## This is the fast phase — just a handful of memcpys and pointer
  ## stores, a few microseconds, not milliseconds.
  if v.state != nil:
    for i, name in v.varNames:
      let old = varAddr(v.state, i)[]
      # copy into the new struct at the new field's offset
      for j, nName in p.varNames:
        if nName == name:
          varAddr(p.state, j)[] = old
          initedAddr(p.state, p.varNames.len, j)[] = 1'u8
          break

    # Per-helper-type region migration. For each region in the new
    # layout, find the old region with the same (typeName, perTypeIdx)
    # and a matching size, then copy those pool slots across. Mismatched
    # sizes (e.g. delay with a changed max_time) and unmatched regions
    # leave the new region zeroed — we just lose that one helper's
    # history rather than shifting everything else.
    let oldPool = cast[ptr UncheckedArray[float64]](v.state)
    let newPool = cast[ptr UncheckedArray[float64]](p.state)
    for newR in p.regions:
      for oldR in v.regions:
        if oldR.typeName == newR.typeName and
           oldR.perTypeIdx == newR.perTypeIdx and
           oldR.size == newR.size:
          if not isPoisonedRegion(oldPool, oldR.offset, oldR.size):
            copyMem(addr newPool[newR.offset],
                    addr oldPool[oldR.offset],
                    newR.size * sizeof(float64))
          # else: leave the new region zeroed; the patch gets a clean
          # slate for that helper while everything else still migrates.
          break

  # Diff old partNames vs new; preserve gain/fade state by name.
  var oldGains = initTable[string, float64]()
  var oldTargets = initTable[string, float64]()
  var oldDeltas = initTable[string, float64]()
  for i, n in v.partNames:
    oldGains[n] = v.partGains[i]
    oldTargets[n] = v.partFadeTargets[i]
    oldDeltas[n] = v.partFadeDeltas[i]
  v.partNames = p.partNames
  v.partGains.setLen(p.partNames.len)
  v.partFadeDeltas.setLen(p.partNames.len)
  v.partFadeTargets.setLen(p.partNames.len)
  for i, n in p.partNames:
    v.partGains[i] = oldGains.getOrDefault(n, 1.0)
    v.partFadeTargets[i] = oldTargets.getOrDefault(n, v.partGains[i])
    v.partFadeDeltas[i] = oldDeltas.getOrDefault(n, 0.0)
  if p.partNames.len > 0:
    cast[ptr VoiceHeader](p.state).partGainsPtr = v.partGains[0].addr
  else:
    cast[ptr VoiceHeader](p.state).partGainsPtr = nil

  # Swap. The audio callback is guaranteed to see a consistent pair
  # (tickFn + matching state) because we hold the mutex.
  if v.state != nil:
    dealloc(v.state)
    v.oldLibs.add v.tccLib   # keep old code alive — small leak per reload
  v.state = p.state
  v.stateSize = p.stateSize
  v.tickFn = p.tickFn
  v.tccLib = p.lib
  v.varNames = p.varNames
  v.regions = p.regions

proc load*(v: NativeVoice; program: Node; sr: float64;
           patchPath: string = "") =
  ## Convenience wrapper: prepare + commit. Call this only when NOT on
  ## the audio critical path (e.g. from tests). Engine.nim uses the
  ## split form so the heavy compile happens outside the mutex.
  v.commit(prepare(program, sr, patchPath))

proc tick*(v: NativeVoice; t: float64): tuple[l, r: float64] {.inline.} =
  let hdr = cast[ptr VoiceHeader](v.state)
  hdr.t = t
  hdr.start_t = v.startT
  var l, r: float64
  v.tickFn(v.state, addr l, addr r)
  (l, r)
