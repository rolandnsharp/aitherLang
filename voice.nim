## The voice, which is C (voice.c), as the Nim engine sees it until the
## engine is C too (notes/c-port-2026-10-28.md). The same names the engine
## always used: part arrays are the C arrays, name lists are copied out
## (never on the audio thread).

import std/os
import ccompile, midi

{.compile: "voice.c".}
{.passL: "-ltcc -ldl".}

const VoiceH = currentSourcePath().parentDir / "voice.h"

type
  TickFn* = proc (s: pointer; outL, outR: ptr float64) {.cdecl.}
  TickBlockFn* = proc (s: pointer; n: clong; outL, outR: ptr float64) {.cdecl.}

  # The fixed head of every generated VoiceState (voice.h).
  VoiceHeader* {.importc: "VoiceHeader", header: VoiceH, bycopy.} = object
    pool* {.importc: "pool".}: ptr UncheckedArray[float64]
    poolSlots* {.importc: "pool_slots".}: clong
    sr* {.importc: "sr".}: float64
    t* {.importc: "t".}: float64
    dt* {.importc: "dt".}: float64
    sweep_origin* {.importc: "sweep_origin".}: float64
    inBuf* {.importc: "in_buf".}: pointer
    inL* {.importc: "in_l".}: float64
    inR* {.importc: "in_r".}: float64
    fadeGain* {.importc: "fade_gain".}: float64
    fadeDelta* {.importc: "fade_delta".}: float64
    timeSec* {.importc: "time_sec".}: float64
    timeFrac* {.importc: "time_frac".}: float64
    peakRaw* {.importc: "peak_raw".}: float64
    bad* {.importc: "bad".}: clong
    nParts* {.importc: "n_parts".}: clong
    deadline* {.importc: "deadline".}: float64
    stuck* {.importc: "stuck".}: clong
    stuckAt* {.importc: "stuck_at".}: cstring
    profiling* {.importc: "profiling".}: clong
    profTicks* {.importc: "prof_ticks".}: clong

  CRegion {.importc: "VRegion", header: VoiceH, bycopy.} = object
    type_name, scope, owner: cstring
    per_type_idx, scope_idx: cint
    offset, size: clong

  CVersion {.importc: "Version", header: VoiceH, bycopy.} = object
    lib: pointer
    tick: TickFn
    tick_block: TickBlockFn
    state: pointer
    state_size: clong
    var_names: ptr UncheckedArray[cstring]
    nvars: cint
    part_names: ptr UncheckedArray[cstring]
    nparts: cint
    unheard: ptr UncheckedArray[cstring]
    nunheard: cint
    regions: ptr UncheckedArray[CRegion]
    nregions: cint
    csrc: cstring
    optimized: cint

  CVoice {.importc: "Voice", header: VoiceH.} = object
    cur: CVersion
    prev: CVersion
    sweep_origin: float64
    gen: clong
    part_gains, part_fade_deltas, part_fade_targets, part_out, part_peak,
      part_rms_sq, part_parked, prof: ptr UncheckedArray[float64]
    part_park_pending: ptr UncheckedArray[bool]

  NativeVoice* = ptr CVoice

  CPrepared {.importc: "Prepared", header: VoiceH.} = object
    calls_input: cint
  Prepared* = ptr CPrepared

const VoiceOk = 0
const VoiceImport = 2
const VoiceCompile = 3
const VoiceTcc = 4

proc voice_new(): NativeVoice {.importc, header: VoiceH.}
proc voice_prepare(stdlibSrc, src, tag, patchPath: cstring; sr: cdouble; seedSelf, hoistOn: cint;
                   kind: ptr cint; err: cstring; errlen: cint): Prepared {.importc, header: VoiceH.}
proc voice_commit(v: NativeVoice; p: Prepared) {.importc, header: VoiceH.}
proc voice_rollback(v: NativeVoice): cint {.importc, header: VoiceH.}
proc voice_compile_optimized(csrc: cstring; stateSize: clong; background: cint; lib: ptr pointer;
                             tick: ptr TickFn; err: cstring; errlen: cint): cint {.importc, header: VoiceH.}
proc voice_upgrade(v: NativeVoice; gen: clong; lib: pointer; tick: TickFn): cint {.importc, header: VoiceH.}
proc voice_snapshot_into(src, dst: NativeVoice; buf: pointer) {.importc, header: VoiceH.}
proc voice_dispose(v: NativeVoice) {.importc, header: VoiceH.}
proc voice_reset_pool(v: NativeVoice) {.importc, header: VoiceH.}
proc voice_scrub_state(v: NativeVoice; hard: cint) {.importc, header: VoiceH.}
proc voice_tick(v: NativeVoice; t: cdouble; l, r: ptr float64) {.importc, header: VoiceH.}
proc voice_tick_block(v: NativeVoice; n: clong; outL, outR: ptr float64) {.importc, header: VoiceH.}
proc voice_prof_names(v: NativeVoice; names: pointer): cint {.importc, header: VoiceH.}
proc voice_clock_read_cost(): cdouble {.importc, header: VoiceH.}
proc voice_mono_now(): cdouble {.importc, header: VoiceH.}
proc c_calloc(n, size: csize_t): pointer {.importc: "calloc", header: "<stdlib.h>".}
proc c_free(p: pointer) {.importc: "free", header: "<stdlib.h>".}

proc strings(a: ptr UncheckedArray[cstring]; n: cint): seq[string] =
  for i in 0 ..< int(n): result.add $a[i]

# ---------------------------------------------------------------- the voice

proc newVoice*(sr = 48000.0): NativeVoice = voice_new()
proc monoNow*(): float64 = voice_mono_now()
proc clockReadCost*(): float64 = voice_clock_read_cost()

proc state*(v: NativeVoice): pointer {.inline.} = v.cur.state
proc stateSize*(v: NativeVoice): int {.inline.} = int(v.cur.state_size)
proc csrc*(v: NativeVoice): string = $v.cur.csrc
proc gen*(v: NativeVoice): int {.inline.} = int(v.gen)
proc optimized*(v: NativeVoice): bool {.inline.} = v.cur.optimized != 0
proc sweepOrigin*(v: NativeVoice): float64 {.inline.} = v.sweep_origin
proc `sweepOrigin=`*(v: NativeVoice; x: float64) {.inline.} = v.sweep_origin = x
proc varNames*(v: NativeVoice): seq[string] = strings(v.cur.var_names, v.cur.nvars)
proc partNames*(v: NativeVoice): seq[string] = strings(v.cur.part_names, v.cur.nparts)
proc partCount*(v: NativeVoice): int {.inline.} = int(v.cur.nparts)
proc unheard*(v: NativeVoice): seq[string] = strings(v.cur.unheard, v.cur.nunheard)
proc regions*(v: NativeVoice): seq[Region] =
  for i in 0 ..< int(v.cur.nregions):
    let r = v.cur.regions[i]
    result.add Region(typeName: $r.type_name, perTypeIdx: int(r.per_type_idx), scope: $r.scope,
                      scopeIdx: int(r.scope_idx), owner: $r.owner, offset: int(r.offset),
                      size: int(r.size))
template partGains*(v: NativeVoice): ptr UncheckedArray[float64] = v.part_gains
template partFadeDeltas*(v: NativeVoice): ptr UncheckedArray[float64] = v.part_fade_deltas
template partFadeTargets*(v: NativeVoice): ptr UncheckedArray[float64] = v.part_fade_targets
template partOut*(v: NativeVoice): ptr UncheckedArray[float64] = v.part_out
template partPeak*(v: NativeVoice): ptr UncheckedArray[float64] = v.part_peak
template partRmsSq*(v: NativeVoice): ptr UncheckedArray[float64] = v.part_rms_sq
template partParked*(v: NativeVoice): ptr UncheckedArray[float64] = v.part_parked
template partParkPending*(v: NativeVoice): ptr UncheckedArray[bool] = v.part_park_pending
template prof*(v: NativeVoice): ptr UncheckedArray[float64] = v.prof

proc poolOf*(state: pointer): ptr UncheckedArray[float64] {.inline.} =
  cast[ptr VoiceHeader](state).pool

proc allocState*(size: int): pointer = c_calloc(csize_t(size), 1)
  ## A state buffer the voice can own (snapshotInto): C's allocator.
proc freeState*(p: pointer) = c_free(p)

# ------------------------------------------------------------ compile, swap

proc prepareSource*(stdlibSrc, src, tag: string; sr: float64; patchPath = "";
                    seedSelf = false): tuple[prepared: Prepared; callsInput: bool] =
  ## Compile a patch's source and allocate its state. Slow (TCC is 5-20 ms
  ## on big patches): outside the audio mutex. Hand it to `commit`.
  var err = newString(4096)
  var kind: cint
  let hoistOn = cint(getEnv("AITHER_HOIST", "1") != "0")
  let p = voice_prepare(stdlibSrc, src, tag, patchPath, sr, cint(seedSelf), hoistOn,
                        addr kind, err.cstring, 4096)
  if p == nil:
    let msg = $err.cstring
    case kind
    of VoiceImport: raise newException(ImportError, msg)
    of VoiceCompile, VoiceTcc: raise newException(ValueError, msg)
    else: raise newException(ParseError, msg)
  (p, p.calls_input != 0)

proc commit*(v: NativeVoice; p: Prepared) = voice_commit(v, p)
  ## Migrate the old state into the new and swap: under the audio mutex.
proc rollback*(v: NativeVoice): bool = voice_rollback(v) != 0
proc canRollback*(v: NativeVoice): bool {.inline.} = v.prev.state != nil

proc compileOptimized*(csrc: string; stateSize: int; background = false):
    tuple[lib: pointer; tickFn: TickFn] =
  ## The same C at -O2 (cc), linked in memory: a drop-in tick for the
  ## same layout. Seconds: off the audio and server threads.
  var err = newString(2048)
  var lib: pointer
  var fn: TickFn
  if voice_compile_optimized(csrc, clong(stateSize), cint(background), addr lib, addr fn,
                             err.cstring, 2048) != 0:
    raise newException(ValueError, $err.cstring)
  (lib, fn)

proc upgrade*(v: NativeVoice; gen: int; lib: pointer; fn: TickFn): bool =
  voice_upgrade(v, clong(gen), lib, fn) != 0

proc snapshotInto*(src, dst: NativeVoice; buf: pointer) = voice_snapshot_into(src, dst, buf)
  ## `buf` from allocState(src.stateSize).
proc dispose*(v: NativeVoice) = voice_dispose(v)
proc resetPool*(v: NativeVoice) = voice_reset_pool(v)
proc scrubState*(v: NativeVoice; hard = false) = voice_scrub_state(v, cint(hard))

proc loadSource*(v: NativeVoice; stdlibSrc, src, tag: string; sr: float64; patchPath = "") =
  ## prepare + commit, off the audio path (tests, tools).
  v.commit(prepareSource(stdlibSrc, src, tag, sr, patchPath).prepared)

proc load*(v: NativeVoice; src: string; sr: float64; patchPath = "") =
  ## A patch's source with the stdlib, loaded: for tests and tools.
  v.loadSource(Stdlib, src, if patchPath.len > 0: patchPath else: "<inline>", sr, patchPath)

proc tick*(v: NativeVoice; t: float64): tuple[l, r: float64] {.inline.} =
  ## One sample at engine time t: render, try and the tests.
  voice_tick(v, t, addr result.l, addr result.r)

proc tickBlock*(v: NativeVoice; n: int; outL, outR: ptr float64) {.inline.} =
  ## n samples from the header's clock: the engine's path.
  voice_tick_block(v, clong(n), outL, outR)

proc profSlotNames*(v: NativeVoice): seq[string] =
  var names: ptr UncheckedArray[cstring]
  let n = voice_prof_names(v, addr names)
  strings(names, n)
