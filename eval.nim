## aither evaluator — bytecode compiler + stack VM
##
## Compiles the parsed AST into a flat array of opcodes and runs them
## per sample on a fixed-size stack. Hot-path math (sin/cos/exp/etc.)
## and the stateful primitives (phasor/noise) are direct opcodes — no
## table dispatch per sample.

import std/[math, tables]
import parser, dsp

# ============================================================ value type ======

type
  ValueKind* = enum vkFloat, vkArr, vkFunc

  Buffer* = ref object
    data*: seq[float64]

  Value* = object
    case kind*: ValueKind
    of vkFloat: f*: float64
    of vkArr:   buf*: Buffer
    of vkFunc:  fid*: int

proc f(x: float64): Value {.inline.} = Value(kind: vkFloat, f: x)
proc toFloat(v: Value): float64 {.inline.} =
  case v.kind
  of vkFloat: v.f
  of vkArr:   (if v.buf.data.len > 0: v.buf.data[0] else: 0.0)
  of vkFunc:  0.0

# ============================================================ builtins ========

const
  FidSin*    = 0
  FidCos*    = 1
  FidTan*    = 2
  FidExp*    = 3
  FidLog*    = 4
  FidLog2*   = 5
  FidAbs*    = 6
  FidFloor*  = 7
  FidCeil*   = 8
  FidSqrt*   = 9
  FidTrunc*  = 10
  FidMin*    = 11
  FidMax*    = 12
  FidPow*    = 13
  FidClamp*  = 14
  FidPhasor* = 15
  FidNoise*  = 16
  FidArray*  = 17
  FidLen*    = 18
  FidSaw*    = 19
  FidTri*    = 20
  FidSqr*    = 21
  BuiltinCount* = 22

const BuiltinTable = [
  ("sin", FidSin), ("cos", FidCos), ("tan", FidTan),
  ("exp", FidExp), ("log", FidLog), ("log2", FidLog2),
  ("abs", FidAbs), ("floor", FidFloor), ("ceil", FidCeil),
  ("sqrt", FidSqrt), ("int", FidTrunc),
  ("min", FidMin), ("max", FidMax), ("pow", FidPow),
  ("clamp", FidClamp),
  ("phasor", FidPhasor), ("noise", FidNoise),
  ("array", FidArray), ("len", FidLen),
  ("saw", FidSaw), ("tri", FidTri), ("sqr", FidSqr),
]

proc lookupBuiltin(name: string): int =
  for (n, id) in BuiltinTable:
    if n == name: return id
  -1

# Builtin arities for opCallFuncVal sanity (1 = unary, 2 = binary, etc.)
const BuiltinArity = [
  1, 1, 1, 1, 1, 1,    # sin cos tan exp log log2
  1, 1, 1, 1, 1,        # abs floor ceil sqrt int
  2, 2, 2,              # min max pow
  3,                    # clamp
  1, 0,                 # phasor (1) noise (0)
  2, 1,                 # array len
  1, 1, 1               # saw tri sqr
]

# Native DSP function ids (separate id space from builtins; dispatched
# via opCallNative). Each native function pops its own args from the
# value stack and claims its own state from voice.dspState.

const
  NaWave*      = 0
  NaLp1*       = 1
  NaHp1*       = 2
  NaLpf*       = 3
  NaHpf*       = 4
  NaBpf*       = 5
  NaNotch*     = 6
  NaDelay*     = 7
  NaFbdelay*   = 8
  NaReverb*    = 9
  NaImpulse*   = 10
  NaResonator* = 11
  NaDischarge* = 12
  NaTremolo*   = 13
  NaSlew*      = 14
  NativeCount* = 15

const NativeTable = [
  ("wave",      NaWave,      2),     # (freq, values_array)
  ("lp1",       NaLp1,       2),
  ("hp1",       NaHp1,       2),
  ("lpf",       NaLpf,       3),
  ("hpf",       NaHpf,       3),
  ("bpf",       NaBpf,       3),
  ("notch",     NaNotch,     3),
  ("delay",     NaDelay,     3),
  ("fbdelay",   NaFbdelay,   4),
  ("reverb",    NaReverb,    3),
  ("impulse",   NaImpulse,   1),
  ("resonator", NaResonator, 3),
  ("discharge", NaDischarge, 2),
  ("tremolo",   NaTremolo,   3),
  ("slew",      NaSlew,      2),
]

proc lookupNative(name: string): tuple[id: int, arity: int] =
  for (n, id, arity) in NativeTable:
    if n == name: return (id, arity)
  (-1, 0)

# ============================================================ bytecode types ==

type
  OpCode = enum
    opConst, opConstArr,
    opPop, opDup,
    opLoadT, opLoadStartT, opLoadSr, opLoadDt, opLoadPi, opLoadTau,
    opLoadLocal, opStoreLocal,
    opLoadVar, opStoreVar, opEnsureVarInit, opSkipIfVarInited,
    opLoadState, opStoreState, opEnsureStateInit, opSkipIfStateInited,
    opAdd, opSub, opMul, opDiv, opMod, opNeg,
    opEq, opNeq, opLt, opGt, opLte, opGte,
    opNot, opTruthy,
    opSin, opCos, opTan, opExp, opLog, opLog2,
    opAbs, opFloor, opCeil, opSqrt, opTrunc,
    opMin, opMax, opPow, opClamp,
    opSaw, opTri, opSqr,
    opPhasor, opNoise,
    opJmp, opJmpFalse, opJmpTrue,
    opCallUser, opLoadFunc, opCallFuncVal, opCallNative,
    opMakeArray, opArrayLit, opArrayGet, opArraySet, opArrayLen,
    opPartGain,              # pop sample, multiply by voice.partGains[arg], push
    opReturn

  Instruction = object
    op:   OpCode
    arg:  int32
    arg2: int32                  # used by ensure-init (skip offset), opCallFuncVal, etc.

  CallSite = object
    funcId:    int32             # absolute fid (>= BuiltinCount for user funcs)
    stateBase: int32             # offset into caller's stateBase

  Chunk* = ref object
    code:          seq[Instruction]
    constants:     seq[float64]
    constArrays:   seq[Buffer]
    callSites:     seq[CallSite]
    numLocals:     int
    numStateSlots: int
    arity:         int
    name:          string

  Frame = object
    chunk:      Chunk
    pc:         int
    stateBase:  int
    localsBase: int
    valBase:    int              # value-stack pos at frame entry (for return)

  Voice* = ref object
    sr*:     float64
    t*:      float64
    startT*: float64
    rng:     uint32

    mainChunk*: Chunk
    funcChunks: seq[Chunk]              # indexed by fid - BuiltinCount
    funcIds:    Table[string, int]      # name → absolute fid

    # Top-level vars: persist across hot-reload, keyed by name.
    varSlots:   Table[string, int]      # name → idx into vars[]
    vars:       seq[Value]
    varsInited: seq[bool]

    # Call-site state: persist across reload by slot index.
    callSiteState:  seq[Value]
    callSiteInited: seq[bool]

    # Native DSP state pool — flat float64 array, claimed by native
    # functions in execution order each tick. Persists across samples
    # (state lives between ticks), reset only on voice retrigger.
    dspState*: DspState

    # Per-part state (see `play` blocks). Names are set by the compiler in
    # top-level source order; gains, fade deltas and targets are maintained
    # by the engine. All four seqs are always the same length.
    partNames*:       seq[string]
    partGains*:       seq[float64]
    partFadeDeltas*:  seq[float64]
    partFadeTargets*: seq[float64]

  EvalError* = object of CatchableError

# ============================================================ helpers =========

proc emit(c: Chunk; op: OpCode; arg: int = 0; arg2: int = 0): int {.discardable.} =
  result = c.code.len
  c.code.add Instruction(op: op, arg: int32(arg), arg2: int32(arg2))

proc patch(c: Chunk; pos: int; arg: int; arg2: int = 0) =
  c.code[pos].arg = int32(arg)
  c.code[pos].arg2 = int32(arg2)

proc addConst(c: Chunk; v: float64): int =
  for i, k in c.constants:
    if k == v: return i
  result = c.constants.len
  c.constants.add v

proc addConstArr(c: Chunk; b: Buffer): int =
  result = c.constArrays.len
  c.constArrays.add b

# ============================================================ slot analysis ===
#
# For each function chunk we need to know: how many state slots do calls of
# it consume? Slots come from `phasor` calls and `var` declarations inside
# def bodies, plus (transitively) state used by called user functions.
# Iterate to fixed point so order of compilation doesn't matter.

proc isAllConstArr(n: Node): bool =
  if n.kind != nkArr: return false
  for k in n.kids:
    if k.kind != nkNum: return false
  true

proc countStateSlots(body: Node; voice: Voice; inDef: bool): int =
  if body == nil: return 0
  case body.kind
  of nkVar:
    if inDef: result = 1
    for k in body.kids: result += countStateSlots(k, voice, inDef)
  of nkCall:
    let bid = lookupBuiltin(body.str)
    if bid == FidPhasor:
      result = 1
    elif bid == FidArray:
      # `array(n, init)` allocates a persistent buffer per call site.
      result = 1
    elif bid < 0 and body.str in voice.funcIds:
      let fid = voice.funcIds[body.str] - BuiltinCount
      if fid < voice.funcChunks.len and voice.funcChunks[fid] != nil:
        result = voice.funcChunks[fid].numStateSlots
    for k in body.kids: result += countStateSlots(k, voice, inDef)
  of nkDef:
    # nested def: hoisted to its own chunk; doesn't contribute slots here
    discard
  else:
    for k in body.kids: result += countStateSlots(k, voice, inDef)

# ============================================================ compiler ========

type
  Local = object
    name: string
    slot: int

  Scope = ref object
    parent:      Scope
    locals:      seq[Local]                # name → local slot
    defVars:     Table[string, int]        # var name → state slot offset (def-internal)
    isDef:       bool

  Compiler = object
    chunk:           Chunk
    voice:           Voice
    scope:           Scope
    nextLocalSlot:   int
    nextStateSlot:   int
    isMain:          bool

proc newScope(parent: Scope; isDef: bool): Scope =
  Scope(parent: parent, isDef: isDef)

proc findLocal(s: Scope; name: string): int =
  var sc = s
  while sc != nil:
    for l in sc.locals:
      if l.name == name: return l.slot
    sc = sc.parent
  -1

proc findDefVar(s: Scope; name: string): int =
  var sc = s
  while sc != nil:
    if name in sc.defVars: return sc.defVars[name]
    sc = sc.parent
  -1

proc allocLocal(co: var Compiler; name: string): int =
  result = co.nextLocalSlot
  inc co.nextLocalSlot
  co.scope.locals.add Local(name: name, slot: result)
  if co.nextLocalSlot > co.chunk.numLocals:
    co.chunk.numLocals = co.nextLocalSlot

proc allocState(co: var Compiler): int =
  result = co.nextStateSlot
  inc co.nextStateSlot

proc compileExpr(co: var Compiler; n: Node; wantValue: bool = true)

proc compileCall(co: var Compiler; n: Node) =
  let name = n.str
  # Is the name a local that holds a function value? → dynamic dispatch.
  let localSlot = findLocal(co.scope, name)
  if localSlot >= 0:
    # Push args, push function value, opCallFuncVal nargs
    for a in n.kids: co.compileExpr(a)
    discard co.chunk.emit(opLoadLocal, localSlot)
    discard co.chunk.emit(opCallFuncVal, n.kids.len)
    return

  let bid = lookupBuiltin(name)
  if bid >= 0:
    # Built-in: emit direct opcode (or stateful-with-slot).
    case bid
    of FidPhasor:
      let slot = co.allocState()
      for a in n.kids: co.compileExpr(a)
      discard co.chunk.emit(opPhasor, slot)
    of FidNoise:
      discard co.chunk.emit(opNoise)
    of FidArray:
      let slot = co.allocState()
      for a in n.kids: co.compileExpr(a)
      discard co.chunk.emit(opMakeArray, slot)
    of FidLen:
      for a in n.kids: co.compileExpr(a)
      discard co.chunk.emit(opArrayLen)
    of FidSin..FidTrunc, FidMin..FidClamp:
      for a in n.kids: co.compileExpr(a)
      let opForBuiltin = [
        opSin, opCos, opTan, opExp, opLog, opLog2,
        opAbs, opFloor, opCeil, opSqrt, opTrunc,
        opMin, opMax, opPow, opClamp
      ]
      discard co.chunk.emit(opForBuiltin[bid])
    of FidSaw..FidSqr:
      for a in n.kids: co.compileExpr(a)
      case bid
      of FidSaw: discard co.chunk.emit(opSaw)
      of FidTri: discard co.chunk.emit(opTri)
      of FidSqr: discard co.chunk.emit(opSqr)
      else: discard
    else:
      raise newException(EvalError, "unhandled builtin: " & name)
    return

  let (nid, narity) = lookupNative(name)
  if nid >= 0:
    if n.kids.len != narity:
      raise newException(EvalError,
        "arity mismatch in call to '" & name & "': expected " &
        $narity & ", got " & $n.kids.len)
    for a in n.kids: co.compileExpr(a)
    discard co.chunk.emit(opCallNative, nid)
    return

  if name in co.voice.funcIds:
    let fid = co.voice.funcIds[name]
    let calleeChunk = co.voice.funcChunks[fid - BuiltinCount]
    if n.kids.len != calleeChunk.arity:
      raise newException(EvalError,
        "arity mismatch in call to '" & name & "': expected " &
        $calleeChunk.arity & ", got " & $n.kids.len)
    for a in n.kids: co.compileExpr(a)
    let stateBase = co.nextStateSlot
    co.nextStateSlot += calleeChunk.numStateSlots
    let csIdx = co.chunk.callSites.len
    co.chunk.callSites.add CallSite(
      funcId: int32(fid), stateBase: int32(stateBase))
    discard co.chunk.emit(opCallUser, csIdx)
    return

  raise newException(EvalError, "unknown function: " & name)

proc compileExpr(co: var Compiler; n: Node; wantValue: bool = true) =
  if n == nil:
    discard co.chunk.emit(opConst, co.chunk.addConst(0.0))
    return
  case n.kind
  of nkNum:
    discard co.chunk.emit(opConst, co.chunk.addConst(n.num))
  of nkIdent:
    case n.str
    of "t":       discard co.chunk.emit(opLoadT)
    of "start_t": discard co.chunk.emit(opLoadStartT)
    of "sr":      discard co.chunk.emit(opLoadSr)
    of "dt":      discard co.chunk.emit(opLoadDt)
    of "PI":      discard co.chunk.emit(opLoadPi)
    of "TAU":     discard co.chunk.emit(opLoadTau)
    else:
      let lslot = findLocal(co.scope, n.str)
      if lslot >= 0:
        discard co.chunk.emit(opLoadLocal, lslot)
        return
      let dslot = findDefVar(co.scope, n.str)
      if dslot >= 0:
        discard co.chunk.emit(opLoadState, dslot)
        return
      if n.str in co.voice.varSlots:
        discard co.chunk.emit(opLoadVar, co.voice.varSlots[n.str])
        return
      let bid = lookupBuiltin(n.str)
      if bid >= 0:
        discard co.chunk.emit(opLoadFunc, bid)
        return
      if n.str in co.voice.funcIds:
        discard co.chunk.emit(opLoadFunc, co.voice.funcIds[n.str])
        return
      raise newException(EvalError, "undefined name: " & n.str)
  of nkUnary:
    co.compileExpr(n.kids[0])
    case n.str
    of "-":   discard co.chunk.emit(opNeg)
    of "not": discard co.chunk.emit(opNot)
    else: raise newException(EvalError, "unknown unary: " & n.str)
  of nkBinOp:
    if n.str == "and":
      co.compileExpr(n.kids[0])
      let jPos = co.chunk.emit(opJmpFalse, 0)         # consumes top
      co.compileExpr(n.kids[1])
      discard co.chunk.emit(opTruthy)
      let endJmp = co.chunk.emit(opJmp, 0)
      let zPos = co.chunk.code.len
      discard co.chunk.emit(opConst, co.chunk.addConst(0.0))
      let endPos = co.chunk.code.len
      co.chunk.patch(jPos, zPos - jPos - 1)
      co.chunk.patch(endJmp, endPos - endJmp - 1)
      return
    if n.str == "or":
      co.compileExpr(n.kids[0])
      discard co.chunk.emit(opDup)
      let jPos = co.chunk.emit(opJmpTrue, 0)
      discard co.chunk.emit(opPop)
      co.compileExpr(n.kids[1])
      discard co.chunk.emit(opTruthy)
      let endJmp = co.chunk.emit(opJmp, 0)
      let truePos = co.chunk.code.len
      discard co.chunk.emit(opTruthy)                  # convert dup'd top to 0/1
      let endPos = co.chunk.code.len
      co.chunk.patch(jPos, truePos - jPos - 1)
      co.chunk.patch(endJmp, endPos - endJmp - 1)
      return
    co.compileExpr(n.kids[0])
    co.compileExpr(n.kids[1])
    case n.str
    of "+":  discard co.chunk.emit(opAdd)
    of "-":  discard co.chunk.emit(opSub)
    of "*":  discard co.chunk.emit(opMul)
    of "/":  discard co.chunk.emit(opDiv)
    of "mod": discard co.chunk.emit(opMod)
    of "==": discard co.chunk.emit(opEq)
    of "!=": discard co.chunk.emit(opNeq)
    of "<":  discard co.chunk.emit(opLt)
    of ">":  discard co.chunk.emit(opGt)
    of "<=": discard co.chunk.emit(opLte)
    of ">=": discard co.chunk.emit(opGte)
    else: raise newException(EvalError, "unknown binop: " & n.str)
  of nkIf:
    co.compileExpr(n.kids[0])
    let jFalse = co.chunk.emit(opJmpFalse, 0)
    co.compileExpr(n.kids[1])
    let jEnd = co.chunk.emit(opJmp, 0)
    let elsePos = co.chunk.code.len
    if n.kids[2] != nil:
      co.compileExpr(n.kids[2])
    else:
      discard co.chunk.emit(opConst, co.chunk.addConst(0.0))
    let endPos = co.chunk.code.len
    co.chunk.patch(jFalse, elsePos - jFalse - 1)
    co.chunk.patch(jEnd, endPos - jEnd - 1)
  of nkArr:
    if isAllConstArr(n):
      var data = newSeq[float64](n.kids.len)
      for i, k in n.kids: data[i] = k.num
      let idx = co.chunk.addConstArr(Buffer(data: data))
      discard co.chunk.emit(opConstArr, idx)
    else:
      for k in n.kids: co.compileExpr(k)
      discard co.chunk.emit(opArrayLit, n.kids.len)
  of nkIdx:
    co.compileExpr(n.kids[0])
    co.compileExpr(n.kids[1])
    discard co.chunk.emit(opArrayGet)
  of nkIdxAssign:
    co.compileExpr(n.kids[0])
    co.compileExpr(n.kids[1])
    co.compileExpr(n.kids[2])
    discard co.chunk.emit(opArraySet)
  of nkCall:
    co.compileCall(n)
  of nkLet:
    co.compileExpr(n.kids[0])
    var slot = findLocal(co.scope, n.str)
    if slot < 0:
      slot = co.allocLocal(n.str)
    discard co.chunk.emit(opStoreLocal, slot)
    if wantValue:
      discard co.chunk.emit(opLoadLocal, slot)
  of nkVar:
    if co.scope.isDef:
      var slot = findDefVar(co.scope, n.str)
      if slot < 0:
        slot = co.allocState()
        co.scope.defVars[n.str] = slot
        if wantValue:
          # Push-on-skip pattern: ensureSI pushes the value AND jumps
          # past store+load when inited.
          let ensurePos = co.chunk.emit(opEnsureStateInit, slot, 0)
          co.compileExpr(n.kids[0])
          discard co.chunk.emit(opStoreState, slot)
          let loadPos = co.chunk.code.len
          discard co.chunk.emit(opLoadState, slot)
          co.chunk.patch(ensurePos, slot, loadPos - ensurePos)
        else:
          # Pure-init pattern: just run init once, never push.
          let ensurePos = co.chunk.emit(opSkipIfStateInited, slot, 0)
          co.compileExpr(n.kids[0])
          discard co.chunk.emit(opStoreState, slot)
          let endPos = co.chunk.code.len
          co.chunk.patch(ensurePos, slot, endPos - ensurePos - 1)
      else:
        if wantValue:
          discard co.chunk.emit(opLoadState, slot)
    else:
      var slot: int
      if n.str in co.voice.varSlots:
        slot = co.voice.varSlots[n.str]
      else:
        slot = co.voice.vars.len
        co.voice.varSlots[n.str] = slot
        co.voice.vars.add f(0.0)
        co.voice.varsInited.add false
      if wantValue:
        let ensurePos = co.chunk.emit(opEnsureVarInit, slot, 0)
        co.compileExpr(n.kids[0])
        discard co.chunk.emit(opStoreVar, slot)
        let loadPos = co.chunk.code.len
        discard co.chunk.emit(opLoadVar, slot)
        co.chunk.patch(ensurePos, slot, loadPos - ensurePos)
      else:
        let ensurePos = co.chunk.emit(opSkipIfVarInited, slot, 0)
        co.compileExpr(n.kids[0])
        discard co.chunk.emit(opStoreVar, slot)
        let endPos = co.chunk.code.len
        co.chunk.patch(ensurePos, slot, endPos - ensurePos - 1)
  of nkAssign:
    co.compileExpr(n.kids[0])
    let lslot = findLocal(co.scope, n.str)
    if lslot >= 0:
      discard co.chunk.emit(opStoreLocal, lslot)
      if wantValue:
        discard co.chunk.emit(opLoadLocal, lslot)
      return
    let dslot = findDefVar(co.scope, n.str)
    if dslot >= 0:
      discard co.chunk.emit(opStoreState, dslot)
      if wantValue:
        discard co.chunk.emit(opLoadState, dslot)
      return
    if n.str in co.voice.varSlots:
      let slot = co.voice.varSlots[n.str]
      discard co.chunk.emit(opStoreVar, slot)
      if wantValue:
        discard co.chunk.emit(opLoadVar, slot)
      return
    raise newException(EvalError, "cannot assign to undefined name: " & n.str)
  of nkDef:
    # Defs are hoisted/compiled separately; here we just produce a 0.
    if wantValue:
      discard co.chunk.emit(opConst, co.chunk.addConst(0.0))
  of nkBlock:
    for i, s in n.kids:
      let isLast = i == n.kids.len - 1
      let childWant = isLast and wantValue
      case s.kind
      of nkLet, nkVar, nkAssign, nkDef:
        co.compileExpr(s, childWant)
      else:
        co.compileExpr(s, true)
        if not childWant:
          discard co.chunk.emit(opPop)
  of nkPlay:
    # play blocks only make sense at top level; bare occurrence pushes 0.
    if wantValue:
      discard co.chunk.emit(opConst, co.chunk.addConst(0.0))

# ============================================================ pre-compile =====

proc collectDefs(n: Node; out_defs: var seq[Node]) =
  if n == nil: return
  if n.kind == nkDef:
    out_defs.add n
  for k in n.kids:
    collectDefs(k, out_defs)

proc updateFuncSlotsOnce(voice: Voice; defs: seq[Node]): bool =
  result = false
  for i, d in defs:
    let n = countStateSlots(d.kids[0], voice, inDef = true)
    if n != voice.funcChunks[i].numStateSlots:
      voice.funcChunks[i].numStateSlots = n
      result = true

proc compileTopLevel(co: var Compiler; body: Node) =
  # Main chunk: only `play` blocks contribute to the output. Other
  # statements (def, let, var, bare exprs) run for side effects.
  # Also: collect the play names into voice.partNames (in source order).
  if body.kind != nkBlock:
    raise newException(EvalError, "program must be a block of statements")
  co.voice.partNames.setLen(0)
  for s in body.kids:
    case s.kind
    of nkPlay:
      let idx = co.voice.partNames.len
      co.voice.partNames.add s.str
      # Push a play-local scope so `let` inside a play block doesn't leak
      # across parts. `var` stays file-level (keyed by name).
      let parentScope = co.scope
      let savedSlotCount = co.nextLocalSlot
      co.scope = newScope(parentScope, isDef = false)
      co.compileExpr(s.kids[0], wantValue = true)
      co.scope = parentScope
      co.nextLocalSlot = savedSlotCount
      discard co.chunk.emit(opPartGain, idx)
      if idx > 0:
        discard co.chunk.emit(opAdd)
    of nkLet, nkVar, nkAssign, nkDef:
      co.compileExpr(s, wantValue = false)
    else:
      co.compileExpr(s, wantValue = true)
      discard co.chunk.emit(opPop)
  if co.voice.partNames.len == 0:
    raise newException(EvalError,
      "no `play` blocks in file - at least one is required")

proc compileChunk(voice: Voice; chunk: Chunk; body: Node; isFunc: bool;
                  params: seq[string] = @[]) =
  chunk.code.setLen(0)
  chunk.constants.setLen(0)
  chunk.constArrays.setLen(0)
  chunk.callSites.setLen(0)
  chunk.numLocals = 0
  var co = Compiler(
    chunk: chunk,
    voice: voice,
    scope: newScope(nil, isFunc),
    nextLocalSlot: 0,
    nextStateSlot: 0,
    isMain: not isFunc)
  for p in params:
    discard co.allocLocal(p)
  if isFunc:
    co.compileExpr(body)
  else:
    co.compileTopLevel(body)
  discard chunk.emit(opReturn)
  # Total state slots actually allocated during compile (own slots +
  # all callee-call-site allocations).
  chunk.numStateSlots = co.nextStateSlot

proc compile*(voice: Voice; program: Node) =
  # Pass 0: collect defs.
  var defs: seq[Node]
  collectDefs(program, defs)

  voice.funcIds.clear()
  voice.funcChunks.setLen(defs.len)
  for i, d in defs:
    voice.funcChunks[i] = Chunk(name: d.str, arity: d.params.len)
    voice.funcIds[d.str] = BuiltinCount + i

  # Pass 1: iterate state-slot counts to fixed point. Compute slot need
  # by analyzing each def's body; updates funcChunks[i].numStateSlots.
  for i in 0 ..< 8:
    if not updateFuncSlotsOnce(voice, defs):
      break

  # Pass 2: compile each function body.
  for i, d in defs:
    compileChunk(voice, voice.funcChunks[i], d.kids[0], isFunc = true,
                 params = d.params)

  # Pass 3: compile main.
  voice.mainChunk = Chunk(name: "<main>", arity: 0)
  compileChunk(voice, voice.mainChunk, program, isFunc = false)

  # Pass 4: grow voice.callSiteState/Inited to max state slots ever needed.
  # Caller's state pool sees: mainChunk's slots + recursive expansion at
  # call sites. Mains slots already include callee allocations done at
  # compile-time, so mainChunk.numStateSlots is the total need for one
  # mainChunk invocation. We add the deepest function's own slots too
  # (for function-value calls that allocate dynamically — none yet).
  var totalSlots = voice.mainChunk.numStateSlots
  if voice.callSiteState.len < totalSlots:
    let oldLen = voice.callSiteState.len
    voice.callSiteState.setLen(totalSlots)
    voice.callSiteInited.setLen(totalSlots)
    for i in oldLen ..< totalSlots:
      voice.callSiteState[i] = f(0.0)
      voice.callSiteInited[i] = false

  when defined(dumpBytecode):
    stderr.write "main: " & $voice.mainChunk.code.len & " ops, " &
                 $voice.mainChunk.numStateSlots & " state, " &
                 $voice.mainChunk.numLocals & " locals\n"
    for i, fc in voice.funcChunks:
      stderr.write "  " & fc.name & ": " & $fc.code.len & " ops, " &
                   $fc.numStateSlots & " state, " & $fc.numLocals & " locals\n"

# ============================================================ VM ==============
#
# Per-tick execution. Single value stack shared by all frames, frame
# stack for user-function call/return, locals stack laid out frame-by-frame.

const
  ValStackSize    = 1024
  LocalsStackSize = 1024
  FrameStackSize  = 64

type
  VM = object
    valStack:    array[ValStackSize, Value]
    sp:          int
    locals:      array[LocalsStackSize, Value]
    localsTop:   int
    frames:      array[FrameStackSize, Frame]
    fp:          int
    voice:       Voice

template push(vm: var VM; v: Value) =
  vm.valStack[vm.sp] = v
  inc vm.sp

template pushF(vm: var VM; x: float64) =
  vm.valStack[vm.sp] = Value(kind: vkFloat, f: x)
  inc vm.sp

template pop(vm: var VM): Value =
  dec vm.sp
  vm.valStack[vm.sp]

template popF(vm: var VM): float64 = vm.pop().toFloat
template peek(vm: var VM): Value = vm.valStack[vm.sp - 1]
template peekF(vm: var VM): float64 = vm.valStack[vm.sp - 1].toFloat
template setTopF(vm: var VM; x: float64) =
  vm.valStack[vm.sp - 1] = Value(kind: vkFloat, f: x)

proc enterFrame(vm: var VM; chunk: Chunk; stateBase: int) {.inline.} =
  let arity = chunk.arity
  let baseLocals = vm.localsTop
  vm.frames[vm.fp] = Frame(
    chunk: chunk, pc: 0,
    stateBase: stateBase,
    localsBase: baseLocals,
    valBase: vm.sp - arity)
  inc vm.fp
  # Pop args off the value stack into locals[0..arity). The remaining
  # locals are reused from the previous frame's ghost values; the
  # bytecode is responsible for storing before reading any local.
  for i in 0 ..< arity:
    vm.locals[baseLocals + i] = vm.valStack[vm.sp - arity + i]
  vm.sp -= arity
  vm.localsTop += chunk.numLocals

proc leaveFrame(vm: var VM): Value {.inline.} =
  let frame = vm.frames[vm.fp - 1]
  let r = vm.pop()
  vm.sp = frame.valBase
  vm.localsTop = frame.localsBase
  dec vm.fp
  r

proc nextNoise(vm: var VM): float64 {.inline.} =
  vm.voice.rng = vm.voice.rng xor (vm.voice.rng shl 13)
  vm.voice.rng = vm.voice.rng xor (vm.voice.rng shr 17)
  vm.voice.rng = vm.voice.rng xor (vm.voice.rng shl 5)
  float64(vm.voice.rng) / 4294967295.0 * 2.0 - 1.0

proc callBuiltinFid(vm: var VM; fid: int) =
  case fid
  of FidSin:   vm.setTopF(sin(vm.peekF))
  of FidCos:   vm.setTopF(cos(vm.peekF))
  of FidTan:   vm.setTopF(tan(vm.peekF))
  of FidExp:   vm.setTopF(exp(vm.peekF))
  of FidLog:   vm.setTopF(ln(vm.peekF))
  of FidLog2:  vm.setTopF(log2(vm.peekF))
  of FidAbs:   vm.setTopF(abs(vm.peekF))
  of FidFloor: vm.setTopF(floor(vm.peekF))
  of FidCeil:  vm.setTopF(ceil(vm.peekF))
  of FidSqrt:  vm.setTopF(sqrt(vm.peekF))
  of FidTrunc: vm.setTopF(float64(int(vm.peekF)))
  of FidMin:
    let b = vm.popF; vm.setTopF(min(vm.peekF, b))
  of FidMax:
    let b = vm.popF; vm.setTopF(max(vm.peekF, b))
  of FidPow:
    let b = vm.popF; vm.setTopF(pow(vm.peekF, b))
  of FidClamp:
    let hi = vm.popF; let lo = vm.popF; vm.setTopF(clamp(vm.peekF, lo, hi))
  of FidNoise:
    vm.pushF(vm.nextNoise())
  of FidSaw: vm.setTopF(shapeSaw(vm.peekF))
  of FidTri: vm.setTopF(shapeTri(vm.peekF))
  of FidSqr: vm.setTopF(shapeSqr(vm.peekF))
  else:
    raise newException(EvalError,
      "function-value call to fid " & $fid & " unsupported")

# Adapter table: each entry pops the function's args from the value
# stack, calls into dsp.nim's native, returns the result. Order must
# match the Na* constants.
type NativeFn = proc(vm: var VM): float64 {.nimcall.}

proc adWave(vm: var VM): float64 =
  let arrVal = vm.pop()
  let freq = vm.popF
  if arrVal.kind != vkArr: return 0.0
  nWave(vm.voice.dspState, freq, arrVal.buf.data)

proc adLp1(vm: var VM): float64 =
  let cutoff = vm.popF; let signal = vm.popF
  nLp1(vm.voice.dspState, signal, cutoff)

proc adHp1(vm: var VM): float64 =
  let cutoff = vm.popF; let signal = vm.popF
  nHp1(vm.voice.dspState, signal, cutoff)

proc adLpf(vm: var VM): float64 =
  let res = vm.popF; let cutoff = vm.popF; let signal = vm.popF
  nLpf(vm.voice.dspState, signal, cutoff, res)

proc adHpf(vm: var VM): float64 =
  let res = vm.popF; let cutoff = vm.popF; let signal = vm.popF
  nHpf(vm.voice.dspState, signal, cutoff, res)

proc adBpf(vm: var VM): float64 =
  let res = vm.popF; let cutoff = vm.popF; let signal = vm.popF
  nBpf(vm.voice.dspState, signal, cutoff, res)

proc adNotch(vm: var VM): float64 =
  let res = vm.popF; let cutoff = vm.popF; let signal = vm.popF
  nNotch(vm.voice.dspState, signal, cutoff, res)

proc adDelay(vm: var VM): float64 =
  let maxTime = vm.popF; let time = vm.popF; let signal = vm.popF
  nDelay(vm.voice.dspState, signal, time, maxTime)

proc adFbdelay(vm: var VM): float64 =
  let fb = vm.popF; let maxTime = vm.popF
  let time = vm.popF; let signal = vm.popF
  nFbdelay(vm.voice.dspState, signal, time, maxTime, fb)

proc adReverb(vm: var VM): float64 =
  let wet = vm.popF; let rt60 = vm.popF; let signal = vm.popF
  nReverb(vm.voice.dspState, signal, rt60, wet)

proc adImpulse(vm: var VM): float64 =
  let freq = vm.popF
  nImpulse(vm.voice.dspState, freq)

proc adResonator(vm: var VM): float64 =
  let decay = vm.popF; let freq = vm.popF; let input = vm.popF
  nResonator(vm.voice.dspState, input, freq, decay)

proc adDischarge(vm: var VM): float64 =
  let rate = vm.popF; let input = vm.popF
  nDischarge(vm.voice.dspState, input, rate)

proc adTremolo(vm: var VM): float64 =
  let depth = vm.popF; let rate = vm.popF; let signal = vm.popF
  nTremolo(vm.voice.dspState, signal, rate, depth)

proc adSlew(vm: var VM): float64 =
  let time = vm.popF; let signal = vm.popF
  nSlew(vm.voice.dspState, signal, time)

const NativeFnTable: array[NativeCount, NativeFn] = [
  adWave, adLp1, adHp1, adLpf, adHpf, adBpf, adNotch,
  adDelay, adFbdelay, adReverb,
  adImpulse, adResonator, adDischarge,
  adTremolo, adSlew,
]

proc run(vm: var VM): Value =
  # Main interpretive loop. We cache the current frame's chunk, pc,
  # stateBase, and localsBase in locals to avoid array-indexing the
  # frame stack every instruction. On call/return we sync back and
  # re-fetch.
  var frame = addr vm.frames[vm.fp - 1]
  var chunk = frame.chunk
  var pc = frame.pc
  var stateBase = frame.stateBase
  var localsBase = frame.localsBase
  let voice = vm.voice
  let invSr = 1.0 / voice.sr

  while true:
    {.computedGoto.}
    let inst = chunk.code[pc]
    inc pc
    case inst.op
    of opConst:
      vm.pushF(chunk.constants[inst.arg])
    of opConstArr:
      vm.push Value(kind: vkArr, buf: chunk.constArrays[inst.arg])
    of opPop:
      dec vm.sp
    of opDup:
      vm.valStack[vm.sp] = vm.valStack[vm.sp - 1]
      inc vm.sp
    of opLoadT:      vm.pushF(voice.t)
    of opLoadStartT: vm.pushF(voice.startT)
    of opLoadSr:     vm.pushF(voice.sr)
    of opLoadDt:     vm.pushF(invSr)
    of opLoadPi:     vm.pushF(PI)
    of opLoadTau:    vm.pushF(TAU)
    of opLoadLocal:
      vm.push vm.locals[localsBase + inst.arg]
    of opStoreLocal:
      vm.locals[localsBase + inst.arg] = vm.pop()
    of opLoadVar:
      vm.push voice.vars[inst.arg]
    of opStoreVar:
      voice.vars[inst.arg] = vm.pop()
      voice.varsInited[inst.arg] = true
    of opEnsureVarInit:
      if voice.varsInited[inst.arg]:
        vm.push voice.vars[inst.arg]
        pc += inst.arg2
    of opSkipIfVarInited:
      if voice.varsInited[inst.arg]:
        pc += inst.arg2
    of opLoadState:
      vm.push voice.callSiteState[stateBase + inst.arg]
    of opStoreState:
      let slot = stateBase + inst.arg
      voice.callSiteState[slot] = vm.pop()
      voice.callSiteInited[slot] = true
    of opEnsureStateInit:
      let slot = stateBase + inst.arg
      if voice.callSiteInited[slot]:
        vm.push voice.callSiteState[slot]
        pc += inst.arg2
    of opSkipIfStateInited:
      if voice.callSiteInited[stateBase + inst.arg]:
        pc += inst.arg2
    of opAdd:
      let b = vm.popF; vm.setTopF(vm.peekF + b)
    of opSub:
      let b = vm.popF; vm.setTopF(vm.peekF - b)
    of opMul:
      let b = vm.popF; vm.setTopF(vm.peekF * b)
    of opDiv:
      let b = vm.popF
      vm.setTopF(if b == 0.0: 0.0 else: vm.peekF / b)
    of opMod:
      let b = vm.popF
      let a = vm.peekF
      vm.setTopF(if b == 0.0: 0.0 else: a - floor(a / b) * b)
    of opNeg:
      vm.setTopF(-vm.peekF)
    of opEq:
      let b = vm.popF; vm.setTopF(if vm.peekF == b: 1.0 else: 0.0)
    of opNeq:
      let b = vm.popF; vm.setTopF(if vm.peekF != b: 1.0 else: 0.0)
    of opLt:
      let b = vm.popF; vm.setTopF(if vm.peekF <  b: 1.0 else: 0.0)
    of opGt:
      let b = vm.popF; vm.setTopF(if vm.peekF >  b: 1.0 else: 0.0)
    of opLte:
      let b = vm.popF; vm.setTopF(if vm.peekF <= b: 1.0 else: 0.0)
    of opGte:
      let b = vm.popF; vm.setTopF(if vm.peekF >= b: 1.0 else: 0.0)
    of opNot:
      vm.setTopF(if vm.peekF == 0.0: 1.0 else: 0.0)
    of opTruthy:
      vm.setTopF(if vm.peekF != 0.0: 1.0 else: 0.0)
    of opSin:   vm.setTopF(sin(vm.peekF))
    of opCos:   vm.setTopF(cos(vm.peekF))
    of opTan:   vm.setTopF(tan(vm.peekF))
    of opExp:   vm.setTopF(exp(vm.peekF))
    of opLog:   vm.setTopF(ln(vm.peekF))
    of opLog2:  vm.setTopF(log2(vm.peekF))
    of opAbs:   vm.setTopF(abs(vm.peekF))
    of opFloor: vm.setTopF(floor(vm.peekF))
    of opCeil:  vm.setTopF(ceil(vm.peekF))
    of opSqrt:  vm.setTopF(sqrt(vm.peekF))
    of opTrunc: vm.setTopF(float64(int(vm.peekF)))
    of opSaw: vm.setTopF(shapeSaw(vm.peekF))
    of opTri: vm.setTopF(shapeTri(vm.peekF))
    of opSqr: vm.setTopF(shapeSqr(vm.peekF))
    of opMin:
      let b = vm.popF; vm.setTopF(min(vm.peekF, b))
    of opMax:
      let b = vm.popF; vm.setTopF(max(vm.peekF, b))
    of opPow:
      let b = vm.popF; vm.setTopF(pow(vm.peekF, b))
    of opClamp:
      let hi = vm.popF; let lo = vm.popF
      vm.setTopF(clamp(vm.peekF, lo, hi))
    of opPhasor:
      let slot = stateBase + inst.arg
      let freq = vm.popF
      var phase = voice.callSiteState[slot].f
      phase = (phase + freq * invSr) mod 1.0
      if phase < 0.0: phase += 1.0
      voice.callSiteState[slot] = f(phase)
      voice.callSiteInited[slot] = true
      vm.pushF(phase)
    of opNoise:
      vm.pushF(vm.nextNoise())
    of opJmp:
      pc += inst.arg
    of opJmpFalse:
      let v = vm.popF
      if v == 0.0: pc += inst.arg
    of opJmpTrue:
      let v = vm.popF
      if v != 0.0: pc += inst.arg
    of opCallUser:
      let cs = chunk.callSites[inst.arg]
      let calleeChunk = voice.funcChunks[cs.funcId - BuiltinCount]
      let newStateBase = stateBase + cs.stateBase
      frame.pc = pc                              # save caller pc
      vm.enterFrame(calleeChunk, newStateBase)
      frame = addr vm.frames[vm.fp - 1]
      chunk = frame.chunk
      pc = frame.pc
      stateBase = frame.stateBase
      localsBase = frame.localsBase
    of opLoadFunc:
      vm.push Value(kind: vkFunc, fid: inst.arg)
    of opCallNative:
      let r = NativeFnTable[inst.arg](vm)
      vm.pushF(r)
    of opCallFuncVal:
      let nargs = inst.arg.int
      let fnVal = vm.valStack[vm.sp - 1]
      dec vm.sp
      let fid = (if fnVal.kind == vkFunc: fnVal.fid else: -1)
      if fid < 0:
        raise newException(EvalError, "calling non-function value")
      if fid < BuiltinCount:
        if BuiltinArity[fid] != nargs:
          raise newException(EvalError,
            "function-value arity mismatch (fid " & $fid & ")")
        vm.callBuiltinFid(fid)
      else:
        let calleeChunk = voice.funcChunks[fid - BuiltinCount]
        if calleeChunk.arity != nargs:
          raise newException(EvalError,
            "function-value arity mismatch (" & calleeChunk.name & ")")
        # State slots for indirect calls aren't pre-allocated; reuse
        # caller's stateBase (state-using indirect calls unsupported).
        frame.pc = pc
        vm.enterFrame(calleeChunk, stateBase)
        frame = addr vm.frames[vm.fp - 1]
        chunk = frame.chunk
        pc = frame.pc
        stateBase = frame.stateBase
        localsBase = frame.localsBase
    of opMakeArray:
      let slot = stateBase + inst.arg
      let init = vm.popF
      let n = max(1, int(vm.popF))
      if not voice.callSiteInited[slot]:
        var data = newSeq[float64](n)
        for i in 0 ..< n: data[i] = init
        voice.callSiteState[slot] =
          Value(kind: vkArr, buf: Buffer(data: data))
        voice.callSiteInited[slot] = true
      vm.push voice.callSiteState[slot]
    of opArrayLit:
      let n = inst.arg.int
      var data = newSeq[float64](n)
      for i in 0 ..< n:
        data[n - 1 - i] = vm.popF
      vm.push Value(kind: vkArr, buf: Buffer(data: data))
    of opArrayGet:
      let idx = int(vm.popF)
      let arr = vm.pop()
      if arr.kind != vkArr:
        vm.pushF(0.0)
      else:
        let dat = arr.buf.data
        if idx < 0 or idx >= dat.len: vm.pushF(0.0)
        else: vm.pushF(dat[idx])
    of opArraySet:
      let val = vm.popF
      let idx = int(vm.popF)
      let arr = vm.pop()
      if arr.kind == vkArr:
        let dat = arr.buf.data
        if idx >= 0 and idx < dat.len:
          arr.buf.data[idx] = val
      vm.pushF(val)
    of opArrayLen:
      let arr = vm.pop()
      vm.pushF(if arr.kind == vkArr: float64(arr.buf.data.len) else: 0.0)
    of opPartGain:
      let idx = inst.arg.int
      let g = if idx >= 0 and idx < voice.partGains.len: voice.partGains[idx] else: 1.0
      let v = vm.popF
      vm.pushF(v * g)
    of opReturn:
      frame.pc = pc                              # not strictly needed
      let r = vm.leaveFrame()
      if vm.fp == 0: return r
      vm.push r
      frame = addr vm.frames[vm.fp - 1]
      chunk = frame.chunk
      pc = frame.pc
      stateBase = frame.stateBase
      localsBase = frame.localsBase

# ============================================================ public API ======

proc newVoice*(sampleRate: float64 = 48000.0): Voice =
  result = Voice(sr: sampleRate, rng: 0x12345678'u32,
                 funcIds: initTable[string, int](),
                 varSlots: initTable[string, int]())
  result.dspState.sr = sampleRate

proc load*(voice: Voice; program: Node) =
  # Hot-reload diff: preserve gains/fade deltas for parts whose names survive.
  # New parts default to gain 1.0 (playing). Removed parts simply disappear.
  var oldGains = initTable[string, float64]()
  var oldDeltas = initTable[string, float64]()
  for i, name in voice.partNames:
    oldGains[name] = voice.partGains[i]
    oldDeltas[name] = voice.partFadeDeltas[i]
  voice.compile(program)
  voice.partGains.setLen(voice.partNames.len)
  voice.partFadeDeltas.setLen(voice.partNames.len)
  voice.partFadeTargets.setLen(voice.partNames.len)
  for i, name in voice.partNames:
    voice.partGains[i] = oldGains.getOrDefault(name, 1.0)
    voice.partFadeDeltas[i] = oldDeltas.getOrDefault(name, 0.0)
    voice.partFadeTargets[i] = voice.partGains[i]

proc sanitize(x: float64): float64 {.inline.} =
  if x != x or x > 1e6 or x < -1e6: 0.0 else: x

proc tick*(voice: Voice; t: float64): tuple[l, r: float64] =
  voice.t = t
  if voice.mainChunk == nil: return (0.0, 0.0)
  voice.dspState.idx = 0          # native funcs claim from 0 each tick
  var vm: VM
  vm.voice = voice
  vm.enterFrame(voice.mainChunk, 0)
  let v =
    try: vm.run()
    except CatchableError: Value(kind: vkFloat, f: 0.0)
  case v.kind
  of vkFloat:
    let s = sanitize(v.f)
    (s, s)
  of vkArr:
    let d = v.buf.data
    let l = sanitize(if d.len > 0: d[0] else: 0.0)
    let r = sanitize(if d.len > 1: d[1] else: l)
    (l, r)
  of vkFunc:
    (0.0, 0.0)
