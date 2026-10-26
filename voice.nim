## Native voice: codegen → TCC → dlopen → function pointer. Replaces
## the old bytecode VM. Each voice owns a TCC state (kept alive while
## its compiled tick() can run) and a malloc'd VoiceState buffer.

import std/[strutils, tables, sets, os, osproc, locks, monotimes]
import parser, tcc, dsp, codegen, midi, engine_types, ccompile

type
  TickFn* = proc (s: pointer; outL, outR: ptr float64) {.cdecl.}
  TickBlockFn* = proc (s: pointer; n: clong; outL, outR: ptr float64) {.cdecl.}
  SizeFn = proc (): cint {.cdecl.}

  # Mirrors the codegen's VoiceState prefix. Writing t from Nim is a
  # plain struct-field store into the malloc'd
  # buffer — no allocation, no GC touch.
  VoiceHeader* {.byref.} = object
    pool*:          ptr UncheckedArray[float64]   # right after the struct
    poolSlots*:     int
    idx*:           int
    overflow*:      bool
    sr*:            float64
    t*, dt*: float64
    # Points into the Nim-owned partGains seq so generated C can read
    # per-part gains. Set at load(); the seq is sized once (on first load
    # per voice) so its data address stays stable.
    partGainsPtr*:  pointer
    # Points at NativeVoice.partOut: tick() writes each part's L/R there,
    # and the engine meters them.
    partOutPtr*:    pointer
    # Points at NativeVoice.partParked: 1.0 where a part is parked, and
    # tick() skips that part's body (its state stays as it was).
    partParkPtr*:   pointer
    # Block mode (tick_block, see codegen's BlockTemplate): the per-part
    # ramps and meters C keeps, the block input, and the engine's clock
    # and fade, read and written per block. Same order as the C struct.
    partFadeDeltaPtr*, partFadeTargetPtr*, partParkPendingPtr*: pointer
    partPeakPtr*, partSqPtr*: pointer
    inBuf*:         pointer
    inL*, inR*:     float64
    fadeGain*, fadeDelta*: float64
    timeSec*, timeFrac*:   float64
    peakDecay*, rmsAlpha*: float64
    peakRaw*:       float64
    bad*:           int
    nParts*:        int
    # Recursion's guard (notes/tail-recursion-2026-10-16.md): the engine
    # sets a deadline per block (monotonic seconds, 0 for none); a
    # recursion still running past it gives up, sets `stuck` and leaves
    # its def and line in `stuckAt`.
    deadline*:      float64
    stuck*:         int
    stuckAt*:       cstring
    # `aither profile`: seconds per top-level statement and play (slots
    # in source order, named by the code's prof_names[]), added up while
    # `profiling` is 1; `profTicks` counts the samples timed.
    profPtr*:       pointer
    profiling*:     int
    profTicks*:     int

  NativeVoice* = ref object
    state*:     pointer          # malloc'd VoiceState buffer
    stateSize*: int
    tickFn*:    TickFn
    tickBlockFn*: TickBlockFn    # the same code's tick_block entry
    tccLib*:    TccState
    oldLibs*:   seq[TccState]    # kept alive across hot reloads (small leak)
    varNames*:  seq[string]
    # Play-block support (unused in phase 1; present so engine.nim's
    # per-sample gain loop is a zero-iter no-op without special cases).
    partNames*:       seq[string]
    unheard*:         seq[string]   # parts the final expression never reaches
    partGains*:       seq[float64]
    partFadeDeltas*:  seq[float64]
    partFadeTargets*: seq[float64]
    # Per-part output (L, R per part, written by tick) and the engine's
    # meters over it: decaying peak and smoothed mean square.
    partOut*:         seq[float64]
    partPeak*:        seq[float64]
    prof*:            seq[float64]   # `aither profile`: MaxProfSlots, see VoiceHeader.profPtr
    partRmsSq*:       seq[float64]
    # `park`: a part faded to silence whose body the tick skips, so it
    # costs nothing and its state is frozen until `unpark`. 1.0 = parked.
    # `partParkPending` marks a part that parks once its fade-out lands.
    partParked*:      seq[float64]
    partParkPending*: seq[bool]
    # Per-helper-type layout of this voice's pool (see codegen.Region).
    # Used at hot-reload time to copy matching (type, perTypeIdx) regions
    # from the old state into the new one.
    regions*:         seq[Region]
    # The C behind tickFn, and a counter bumped on every commit, so a
    # background -O2 build can tell whether it still matches the voice.
    csrc*:            string
    gen*:             int
    optimized*:       bool
    # The version that was playing before the last commit: its code and
    # its state as of the swap. Kept so the engine can roll a hot-swap
    # back when the new code blows up right after it lands, instead of
    # going silent. One level only; cleared by a rollback.
    prev*:            PrevVersion

  PrevVersion* = object
    state*:     pointer
    stateSize*: int
    tickFn*:    TickFn
    tickBlockFn*: TickBlockFn
    varNames*:  seq[string]
    regions*:   seq[Region]
    partNames*: seq[string]
    unheard*:   seq[string]
    csrc*:      string
    optimized*: bool

# TCC invokes this synchronously during compile/relocate; we stash the
# most recent message so compileProgram can report it on failure.
var lastTccError {.threadvar.}: string

# libtcc keeps global state, so only one thread may be inside it at a
# time: the server thread loading a patch, or the -O2 worker linking one.
var tccLock: Lock
initLock(tccLock)

proc errHandler(opaque: pointer; msg: cstring) {.cdecl.} =
  lastTccError = $msg
  stderr.writeLine "TCC: ", $msg

proc monoNow*(): float64 =
  ## Monotonic seconds, for deadlines.
  float64(getMonoTime().ticks) * 1e-9

proc nNow(): float64 {.cdecl.} = monoNow()   # `aither profile`'s clock, for the C

# A recursion asks every 65 536 steps whether its block is past the
# deadline. Past it, the recursion gives up, and so does every later one
# in the block: they see `stuck` before they start.
proc nOverrun(s: ptr VoiceHeader; where: cstring): clong {.cdecl.} =
  if s.deadline <= 0.0 or monoNow() < s.deadline: return 0
  s.stuck = 1
  s.stuckAt = where
  1

proc registerNatives(s: TccState) =
  # Every native a patch calls by name, from dsp.nim's table; then the
  # hoisted forms and the rest codegen calls itself.
  for name, spec in Natives:
    discard s.addSymbol(cstring("n_" & name), spec.fn)
  discard s.addSymbol("n_overrun",   cast[pointer](nOverrun))
  discard s.addSymbol("n_now",       cast[pointer](nNow))
  # Hoisted forms: coefficient once (hoist_init), step per sample.
  discard s.addSymbol("n_lp1_coef",  cast[pointer](nLp1Coef))
  discard s.addSymbol("n_lp1_c",     cast[pointer](nLp1C))
  discard s.addSymbol("n_hp1_c",     cast[pointer](nHp1C))
  discard s.addSymbol("n_svf_coef",  cast[pointer](nSvfCoef))
  discard s.addSymbol("n_lpf_c",     cast[pointer](nLpfC))
  discard s.addSymbol("n_hpf_c",     cast[pointer](nHpfC))
  discard s.addSymbol("n_bpf_c",     cast[pointer](nBpfC))
  discard s.addSymbol("n_notch_c",   cast[pointer](nNotchC))
  discard s.addSymbol("n_reverb_coef", cast[pointer](nReverbCoef))
  discard s.addSymbol("n_reverb_c",  cast[pointer](nReverbC))
  discard s.addSymbol("n_hilbert_a", cast[pointer](nHilbertA))
  discard s.addSymbol("n_hilbert_b", cast[pointer](nHilbertB))
  discard s.addSymbol("n_freq_shift", cast[pointer](nFreqShift))
  discard s.addSymbol("n_input_l",   cast[pointer](nInputL))
  discard s.addSymbol("n_input_r",   cast[pointer](nInputR))
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
  discard s.addSymbol("n_midi_voice_vel",  cast[pointer](nMidiVoiceVel))
  discard s.addSymbol("n_midi_voice_key",  cast[pointer](nMidiVoiceKey))
  discard s.addSymbol("n_midi_vel",        cast[pointer](nMidiVel))

# The whole allocation: the struct, then the pool its patch needs.
const SizeHelper = "\nint voice_state_size(void) { return sizeof(VoiceState) + " &
  "voice_pool_slots() * sizeof(double); }\n"

proc compileC(csrc: string):
    tuple[lib: TccState; tickFn: TickFn; size: int; csrc: string] =
  ## Generated C to a loaded library: tick(), and the state's size.
  acquire(tccLock)
  defer: release(tccLock)
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
  let withSize = csrc & SizeHelper
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
  (lib, fn, int(sizeFn()), withSize)

# A live engine builds -O2 code beside the playing voices, so the build
# waits its turn (nice 19): when it and the audio want the same core, the
# audio wins. It isn't pinned to the slow cores: the voice plays TCC's
# slower code until the build lands, so a slow build is a long overload.
# Offline renders build at normal priority: there the build is the job.
let backgroundPrefix = if findExe("nice").len > 0: "nice -n 19 " else: ""

proc compileOptimized*(csrc: string; stateSize: int; background = false):
    tuple[lib: TccState; tickFn: TickFn] =
  ## Build the same C that TCC just loaded with the system C compiler at
  ## -O2, then link the object in memory with TCC, so natives resolve
  ## exactly as they do for the fast path. Slow (seconds), so run it off
  ## the audio and server threads; the result is a drop-in tick() for
  ## the same state layout (checked via voice_state_size). `background`:
  ## a live engine's build, at low priority.
  let dir = getTempDir() / "aither-" & $getCurrentProcessId()
  createDir(dir)
  let base = dir / "opt-" & $getThreadId()
  let cfile = base & ".c"
  let ofile = base & ".o"
  writeFile(cfile, csrc)
  defer:
    removeFile(cfile)
    removeFile(ofile)
  # -ffp-contract=off: no fused multiply-adds. TCC never fuses, so with
  # them the two builds compute different bits and a voice jumps onto a
  # slightly different trajectory when this one lands (tests/exact_builds).
  let (output, code) = execCmdEx((if background: backgroundPrefix else: "") &
    "cc -O2 -march=native -ffp-contract=off -fno-stack-protector " &
    "-fno-asynchronous-unwind-tables -c -o " & quoteShell(ofile) & " " &
    quoteShell(cfile))
  if code != 0:
    raise newException(ValueError, "cc failed: " & output)
  acquire(tccLock)                     # cc ran unlocked; linking needs TCC
  defer: release(tccLock)
  lastTccError = ""
  let lib = tccNew()
  if cast[pointer](lib) == nil:
    raise newException(ValueError, "tcc_new failed")
  lib.setErrorFunc(nil, errHandler)
  if lib.setOutputType(OutputMemory) != 0 or lib.addLibrary("m") != 0:
    lib.delete()
    raise newException(ValueError, "tcc setup failed")
  registerNatives(lib)
  if lib.addFile(cstring(ofile)) < 0 or lib.relocate() < 0:
    lib.delete()
    raise newException(ValueError, "linking optimized object failed: " & lastTccError)
  let fn = cast[TickFn](lib.getSymbol("tick"))
  let sizeFn = cast[SizeFn](lib.getSymbol("voice_state_size"))
  if fn == nil or sizeFn == nil or int(sizeFn()) != stateSize:
    lib.delete()
    raise newException(ValueError, "optimized build doesn't match the loaded layout")
  (lib, fn)

proc newVoice*(sr: float64): NativeVoice =
  NativeVoice(prof: newSeq[float64](MaxProfSlots))

proc profSlotNames*(v: NativeVoice): seq[string] =
  ## `aither profile`'s slot names, as the voice's code lists them.
  let names = cast[ptr UncheckedArray[cstring]](v.tccLib.getSymbol("prof_names"))
  let count = cast[ptr clong](v.tccLib.getSymbol("prof_count"))
  if names == nil or count == nil: return
  for i in 0 ..< min(int(count[]), MaxProfSlots): result.add $names[i]

proc clockReadCost*(): float64 =
  ## What one of tick1's timing points costs, in seconds: `profile`
  ## takes it off each slot per sample timed.
  let f: proc (): float64 {.cdecl.} = nNow
  const N = 20000
  var sink = 0.0
  let t0 = monoNow()
  for _ in 0 ..< N: sink += f()
  result = (monoNow() - t0) / N
  if sink < 0: result = 0                # keep the calls

# Layout constants mirror the codegen's emitted struct — see the NOTE in
# codegen.nim near the VoiceState emission. Migration here writes directly
# into the buffer by computed offsets; stay in lockstep with codegen.
const HeaderSize  = sizeof(VoiceHeader)   # the struct's fixed fields
const VarSlotSize = sizeof(float64)

proc poolOf*(state: pointer): ptr UncheckedArray[float64] {.inline.} =
  cast[ptr VoiceHeader](state).pool

proc placePool(state: pointer; stateSize, slots: int) =
  ## Point a state's pool at its own slots, after the struct.
  let hdr = cast[ptr VoiceHeader](state)
  hdr.poolSlots = slots
  hdr.pool = cast[ptr UncheckedArray[float64]](
    cast[uint](state) + uint(stateSize - slots * sizeof(float64)))

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
    stateSize*: int              # the struct and its pool
    varNames*, partNames*, unheard*: seq[string]
    regions*: seq[Region]
    csrc*: string                # the C that was compiled, for compileOptimized

proc prepareC(csrc: string; varNames, partNames, unheard: seq[string];
              regions: seq[Region]; sr: float64): Prepared =
  let (lib, fn, size, src) = compileC(csrc)
  let newState = alloc0(size)
  let slotsFn = cast[SizeFn](lib.getSymbol("voice_pool_slots"))
  placePool(newState, size, int(slotsFn()))
  cast[ptr VoiceHeader](newState).sr = sr
  cast[ptr VoiceHeader](newState).dt = 1.0 / sr
  Prepared(lib: lib, tickFn: fn, state: newState, stateSize: size, csrc: src,
           varNames: varNames, partNames: partNames, unheard: unheard,
           regions: regions)

proc prepare*(program: Node; sr: float64; patchPath: string = ""): Prepared =
  ## Compile + allocate the new state buffer. Slow (TCC compile is 5-20 ms
  ## on big patches, state zero-init a ms or so a few MB). Caller must
  ## hand the result to `commit` to actually swap it in.
  let (csrc, varNames, partNames, unheard, regions) = generateFull(program, patchPath, sr)
  prepareC(csrc, varNames, partNames, unheard, regions, sr)

proc prepareSource*(stdlibSrc, src, tag: string; sr: float64; patchPath = "";
                    seedSelf = false): tuple[prepared: Prepared; callsInput: bool] =
  ## `prepare` from a patch's source, through the compiler in C.
  let g = compileSource(stdlibSrc, src, tag, patchPath, sr, seedSelf)
  (prepareC(g.csrc, g.varNames, g.partNames, g.unheard, g.regions, sr), g.callsInput)

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

# File-level `$vars` hold timers (`$m1start = t`) and sample counters,
# which legitimately grow large, so only NaN/Inf counts as poison there.
proc isBadVar(x: float64): bool {.inline.} =
  x != x or x - x != 0.0

# Defense 2: zero the stateful-primitive regions of the DSP pool.
# Called from the audio callback when a tick produces NaN/Inf — the
# next tick reads from clean filter / delay / reverb state. Regions
# whose typeName is "var" are skipped: those hold user-named state
# (counters, fade-in start times, etc.) that performance patches rely
# on, and resetting them would re-trigger user-authored fades on every
# NaN event. "pvar" regions (pair-valued state inside defs) are user
# state too. Top-level vars sit after the pool in the struct and
# aren't touched either.
proc restart*(v: NativeVoice) =
  ## Born again: every bit of the voice's state as a fresh voice has it,
  ## so its `$` state takes its first values once more (`retrigger`).
  if v.state == nil: return
  let hdr = cast[ptr VoiceHeader](v.state)
  zeroMem(hdr.pool, hdr.poolSlots * sizeof(float64))
  for i in 0 ..< v.varNames.len:
    varAddr(v.state, i)[] = 0.0
    initedAddr(v.state, v.varNames.len, i)[] = 0'u8

proc resetPool*(v: NativeVoice) =
  if v.state == nil: return
  let pool = poolOf(v.state)
  for r in v.regions:
    if r.typeName in ["var", "pvar"]: continue
    zeroMem(addr pool[r.offset], r.size * sizeof(float64))

proc scrubState*(v: NativeVoice; hard = false) =
  ## Clear poison out of the user state that `resetPool` leaves alone.
  ## A def's `$x`/pair region that is NaN or past 1e6 is zeroed, so its
  ## init runs again next tick (`hard` zeroes every one of them: the
  ## physics keeps blowing up even from clean filters). A file-level
  ## `$var` that is NaN/Inf re-inits; healthy ones (timers, counters)
  ## are kept, so the arrangement doesn't jump. Allocation-free: called
  ## from the audio callback.
  if v.state == nil: return
  let pool = poolOf(v.state)
  for r in v.regions:
    if r.typeName notin ["var", "pvar"]: continue
    if hard or isPoisonedRegion(pool, r.offset, r.size):
      zeroMem(addr pool[r.offset], r.size * sizeof(float64))
  for i in 0 ..< v.varNames.len:
    if isBadVar(varAddr(v.state, i)[]):
      varAddr(v.state, i)[] = 0.0
      initedAddr(v.state, v.varNames.len, i)[] = 0'u8

proc setPartNames(v: NativeVoice; names: seq[string]) =
  ## Re-map per-part gains and fades onto a new list of part names,
  ## keeping each surviving part's gain. Linear search, no tables: the
  ## rollback path calls this from the audio callback.
  var gains, targets, deltas, peaks, rms, parked = newSeq[float64](names.len)
  var pending = newSeq[bool](names.len)
  for i, n in names:
    gains[i] = 1.0
    targets[i] = 1.0
    for j, o in v.partNames:
      if o == n:
        gains[i] = v.partGains[j]
        targets[i] = v.partFadeTargets[j]
        deltas[i] = v.partFadeDeltas[j]
        peaks[i] = v.partPeak[j]
        rms[i] = v.partRmsSq[j]
        parked[i] = v.partParked[j]
        pending[i] = v.partParkPending[j]
        break
  v.partNames = names
  v.partGains = gains
  v.partFadeTargets = targets
  v.partFadeDeltas = deltas
  v.partPeak = peaks
  v.partRmsSq = rms
  v.partParked = parked
  v.partParkPending = pending
  v.partOut = newSeq[float64](2 * names.len)

proc pointPartGains(v: NativeVoice) =
  let hdr = cast[ptr VoiceHeader](v.state)
  template at(s: seq[float64]): pointer = (if s.len > 0: s[0].addr else: nil)
  hdr.partGainsPtr = at(v.partGains)
  hdr.partOutPtr = at(v.partOut)
  hdr.partParkPtr = at(v.partParked)
  hdr.partFadeDeltaPtr = at(v.partFadeDeltas)
  hdr.partFadeTargetPtr = at(v.partFadeTargets)
  hdr.partParkPendingPtr =
    if v.partParkPending.len > 0: v.partParkPending[0].addr else: nil
  hdr.partPeakPtr = at(v.partPeak)
  hdr.partSqPtr = at(v.partRmsSq)
  hdr.profPtr = at(v.prof)
  hdr.nParts = v.partNames.len
  hdr.peakDecay = PeakDecay
  hdr.rmsAlpha = RmsAlpha

proc dropPrev(v: NativeVoice) =
  if v.prev.state != nil: dealloc(v.prev.state)
  v.prev = PrevVersion()

proc canRollback*(v: NativeVoice): bool {.inline.} = v.prev.state != nil

proc rollback*(v: NativeVoice): bool =
  ## Put the previous version back: its code, and its state as it was at
  ## the swap (scrubbed in case it was already sick). The broken state
  ## is freed. Must be called under the audio mutex, like commit, but
  ## not from the audio callback (it allocates). Bumps
  ## `gen`, so a pending -O2 build of the broken code is dropped.
  if v.prev.state == nil: return false
  dealloc(v.state)
  v.state = v.prev.state
  v.stateSize = v.prev.stateSize
  v.tickFn = v.prev.tickFn
  v.tickBlockFn = v.prev.tickBlockFn
  v.varNames = move v.prev.varNames
  v.regions = move v.prev.regions
  v.csrc = move v.prev.csrc
  v.optimized = v.prev.optimized
  v.setPartNames(move v.prev.partNames)
  v.unheard = v.prev.unheard
  v.pointPartGains()
  v.prev = PrevVersion()
  inc v.gen
  v.scrubState()
  true

proc commit*(v: NativeVoice; p: Prepared) =
  ## Apply a prepared compile. Must be called under the audio mutex:
  ## touches v.state / v.tickFn which the audio thread reads each sample.
  ## This is the fast phase — just a handful of memcpys and pointer
  ## stores, a few microseconds, not milliseconds.
  if v.state != nil:
    for i, name in v.varNames:
      let old = varAddr(v.state, i)[]
      if isBadVar(old): continue       # NaN never migrates; it re-inits
      if initedAddr(v.state, v.varNames.len, i)[] == 0'u8: continue  # reset by scrub
      # copy into the new struct at the new field's offset
      for j, nName in p.varNames:
        if nName == name:
          varAddr(p.state, j)[] = old
          initedAddr(p.state, p.varNames.len, j)[] = 1'u8
          break

    # Region migration. Each new region takes the slots of the old one
    # in the same statement with the same type, owner (the def it sits
    # in, and the state's name) and index there (see Region.scope and
    # Region.owner), so an edit inside one play or let leaves every
    # other statement's filters, delays and physics untouched, and a
    # different def put in the same place starts clean. Regions of a
    # statement that didn't exist before (a renamed let, say) fall back
    # to the old (typeName, owner, perTypeIdx) match, but only against
    # old regions whose statement is gone, so they can't steal live
    # state. Mismatched sizes and unmatched regions start zeroed — one
    # helper loses its history rather than everything else shifting.
    # Poisoned old regions (NaN, >1e6) are never copied.
    let oldPool = poolOf(v.state)
    let newPool = poolOf(p.state)
    var oldScopes, newScopes = initHashSet[string]()
    for r in v.regions: oldScopes.incl r.scope
    for r in p.regions: newScopes.incl r.scope
    var oldByKey = initTable[(string, string, string, int), int]()
    for i, r in v.regions:
      oldByKey[(r.scope, r.typeName, r.owner, r.scopeIdx)] = i
    var claimed = newSeq[bool](v.regions.len)
    var pending: seq[int]
    for ni, newR in p.regions:
      let oi = oldByKey.getOrDefault(
        (newR.scope, newR.typeName, newR.owner, newR.scopeIdx), -1)
      if oi >= 0:
        claimed[oi] = true
        let oldR = v.regions[oi]
        if oldR.size == newR.size and
           not isPoisonedRegion(oldPool, oldR.offset, oldR.size):
          copyMem(addr newPool[newR.offset], addr oldPool[oldR.offset],
                  newR.size * sizeof(float64))
      elif newR.scope notin oldScopes:
        pending.add ni
    for ni in pending:
      let newR = p.regions[ni]
      for oi, oldR in v.regions:
        if not claimed[oi] and oldR.scope notin newScopes and
           oldR.typeName == newR.typeName and oldR.owner == newR.owner and
           oldR.perTypeIdx == newR.perTypeIdx and oldR.size == newR.size:
          claimed[oi] = true
          if not isPoisonedRegion(oldPool, oldR.offset, oldR.size):
            copyMem(addr newPool[newR.offset], addr oldPool[oldR.offset],
                    newR.size * sizeof(float64))
          break

  # Keep the outgoing version for rollback (its state stops changing
  # here, so it's a snapshot of the moment of the swap).
  let outgoing = PrevVersion(state: v.state, stateSize: v.stateSize,
    tickFn: v.tickFn, tickBlockFn: v.tickBlockFn,
    varNames: v.varNames, regions: v.regions,
    partNames: v.partNames, unheard: v.unheard, csrc: v.csrc,
    optimized: v.optimized)

  # Diff old partNames vs new; preserve gain/fade state by name.
  v.setPartNames(p.partNames)
  v.unheard = p.unheard

  # Swap. The audio callback is guaranteed to see a consistent pair
  # (tickFn + matching state) because we hold the mutex.
  if v.state != nil:
    v.dropPrev()
    v.prev = outgoing
    v.oldLibs.add v.tccLib   # keep old code alive — small leak per reload
  v.state = p.state
  v.pointPartGains()
  v.stateSize = p.stateSize
  v.tickFn = p.tickFn
  v.tickBlockFn = cast[TickBlockFn](p.lib.getSymbol("tick_block"))
  v.tccLib = p.lib
  v.varNames = p.varNames
  v.regions = p.regions
  v.csrc = p.csrc
  inc v.gen
  v.optimized = false

proc upgrade*(v: NativeVoice; gen: int; lib: TccState; fn: TickFn): bool =
  ## Swap in an -O2 tick() built from generation `gen`'s source. Same C,
  ## same state layout, so only the function pointer changes: no state
  ## copy, no glitch. Refused (false) if the voice was reloaded since.
  ## Must be called under the audio mutex, like commit.
  if v.gen != gen or v.state == nil: return false
  v.oldLibs.add v.tccLib
  v.tccLib = lib
  v.tickFn = fn
  v.tickBlockFn = cast[TickBlockFn](lib.getSymbol("tick_block"))
  v.optimized = true
  true

proc snapshotInto*(src: NativeVoice; dst: NativeVoice; buf: pointer) =
  ## Make `dst` a stand-in for `src` as it is right now, for a preview
  ## render: same layout and part gains, state copied into `buf` (an
  ## alloc0'd buffer of src.stateSize). Copies the struct and only the
  ## regions of the pool in use, not the whole pool, to keep the time
  ## under the audio mutex short. Caller holds the mutex. `dst` gets no
  ## code: commit a Prepared to it next, which migrates from this state.
  let slots = cast[ptr VoiceHeader](src.state).poolSlots
  copyMem(buf, src.state, src.stateSize - slots * sizeof(float64))
  placePool(buf, src.stateSize, slots)
  let sp = poolOf(src.state)
  let dp = poolOf(buf)
  for r in src.regions:
    copyMem(addr dp[r.offset], addr sp[r.offset], r.size * sizeof(float64))
  dst.state = buf
  dst.stateSize = src.stateSize
  dst.varNames = src.varNames
  dst.regions = src.regions
  dst.partNames = src.partNames
  dst.unheard = src.unheard
  dst.partGains = src.partGains
  dst.partFadeTargets = src.partFadeTargets
  dst.partFadeDeltas = src.partFadeDeltas
  dst.partPeak = src.partPeak
  dst.partRmsSq = src.partRmsSq
  dst.partParked = src.partParked
  dst.partParkPending = src.partParkPending
  dst.prof = newSeq[float64](MaxProfSlots)     # a preview isn't profiled
  cast[ptr VoiceHeader](buf).profiling = 0

proc dispose*(v: NativeVoice) =
  ## Free a preview voice: its state, its prev, and its compiled code.
  ## Only for voices the audio thread never saw.
  v.dropPrev()
  if v.state != nil: dealloc(v.state)
  v.state = nil
  acquire(tccLock)
  for lib in v.oldLibs:
    if cast[pointer](lib) != nil: lib.delete()
  if cast[pointer](v.tccLib) != nil: v.tccLib.delete()
  release(tccLock)
  v.oldLibs = @[]

proc load*(v: NativeVoice; program: Node; sr: float64;
           patchPath: string = "") =
  ## Convenience wrapper: prepare + commit. Call this only when NOT on
  ## the audio critical path (e.g. from tests). Engine.nim uses the
  ## split form so the heavy compile happens outside the mutex.
  v.commit(prepare(program, sr, patchPath))

proc loadSource*(v: NativeVoice; stdlibSrc, src, tag: string; sr: float64;
                 patchPath: string = "") =
  ## `load` from a patch's source, through the compiler in C.
  v.commit(prepareSource(stdlibSrc, src, tag, sr, patchPath).prepared)

proc tick*(v: NativeVoice; t: float64): tuple[l, r: float64] {.inline.} =
  ## One sample at engine time `t`, input() from the dsp globals: what
  ## render, try and the tests use.
  let hdr = cast[ptr VoiceHeader](v.state)
  hdr.t = t
  var l, r: float64
  v.tickFn(v.state, addr l, addr r)
  (l, r)

proc tickBlock*(v: NativeVoice; n: int; outL, outR: ptr float64) {.inline.} =
  ## `n` samples from the clock in the header (timeSec/timeFrac), with
  ## input() from hdr.inBuf and the fade and part ramps advanced per
  ## sample in C: the engine's path. The caller reads the fade, the bad
  ## count and the raw peak back from the header afterwards.
  v.tickBlockFn(v.state, clong(n), outL, outR)
