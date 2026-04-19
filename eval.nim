## aither evaluator — tree walker over parsed AST

import std/[math, tables, sequtils]
import parser

type
  ValueKind* = enum vkFloat, vkFunc, vkArray

  Buffer* = ref object
    data*: seq[float64]

  Value* = object
    case kind*: ValueKind
    of vkFloat: f*: float64
    of vkFunc:  fname*: string
    of vkArray: buf*: Buffer

  FuncDef* = object
    params*: seq[string]
    body*: Node

  Voice* = ref object
    program*:        Node
    funcs*:          Table[string, FuncDef]
    vars*:           Table[string, Value]
    callSiteState*:  seq[Value]
    callSiteCounter*:int
    t*:              float64
    startT*:         float64           # wall-clock time when first loaded
    sr*:             float64
    rng*:            uint32

  Scope = ref object
    parent*:    Scope
    locals*:    Table[string, Value]      # let bindings + args
    callsites*: Table[string, int]        # var slots inside def
    isDef*:     bool

  EvalError* = object of CatchableError

# ----------------------------------------------------------------- helpers

proc f*(x: float64): Value {.inline.} = Value(kind: vkFloat, f: x)
proc isTruthy*(v: Value): bool {.inline.} =
  v.kind == vkFloat and v.f != 0.0

proc toFloat*(v: Value): float64 {.inline.} =
  case v.kind
  of vkFloat: v.f
  of vkArray:
    if v.buf.data.len > 0: v.buf.data[0] else: 0.0
  of vkFunc:  0.0

# Look up a name in the lexical scope chain. Returns (found, value, isMutableSlot,
# slotKind, slotIdx) — for assignment we need to know where it lives.
type LookupKind = enum lkNone, lkLocal, lkCallsite, lkVar
type Lookup = object
  kind: LookupKind
  scope: Scope
  slot: int
  val: Value

proc resolve(scope: Scope; voice: Voice; name: string): Lookup =
  var s = scope
  while s != nil:
    if name in s.locals:
      return Lookup(kind: lkLocal, scope: s, val: s.locals[name])
    if name in s.callsites:
      let idx = s.callsites[name]
      return Lookup(kind: lkCallsite, scope: s, slot: idx,
                    val: voice.callSiteState[idx])
    s = s.parent
  if name in voice.vars:
    return Lookup(kind: lkVar, val: voice.vars[name])
  Lookup(kind: lkNone)

# ---------------------------------------------------------------- builtins

const BuiltinNames* = ["sin", "cos", "tan", "exp", "log", "log2",
                      "abs", "floor", "ceil", "min", "max", "pow",
                      "sqrt", "clamp", "int",
                      "phasor", "noise", "array", "len"]

proc claimSlot(voice: Voice; init: Value): int {.inline.} =
  result = voice.callSiteCounter
  inc voice.callSiteCounter
  if result >= voice.callSiteState.len:
    voice.callSiteState.add init

proc evalBuiltin(voice: Voice; name: string; args: seq[Value]): Value =
  template a(i: int): float64 = args[i].toFloat
  case name
  of "sin":   f(sin(a(0)))
  of "cos":   f(cos(a(0)))
  of "tan":   f(tan(a(0)))
  of "exp":   f(exp(a(0)))
  of "log":   f(ln(a(0)))
  of "log2":  f(log2(a(0)))
  of "abs":   f(abs(a(0)))
  of "floor": f(floor(a(0)))
  of "ceil":  f(ceil(a(0)))
  of "min":   f(min(a(0), a(1)))
  of "max":   f(max(a(0), a(1)))
  of "pow":   f(pow(a(0), a(1)))
  of "sqrt":  f(sqrt(a(0)))
  of "clamp": f(clamp(a(0), a(1), a(2)))
  of "int":   f(float64(int(a(0))))
  of "phasor":
    let freq = a(0)
    let slot = claimSlot(voice, f(0.0))
    var phase = voice.callSiteState[slot].f
    phase = (phase + freq / voice.sr) mod 1.0
    if phase < 0.0: phase += 1.0
    voice.callSiteState[slot] = f(phase)
    f(phase)
  of "noise":
    voice.rng = voice.rng xor (voice.rng shl 13)
    voice.rng = voice.rng xor (voice.rng shr 17)
    voice.rng = voice.rng xor (voice.rng shl 5)
    f(float64(voice.rng) / 4294967295.0 * 2.0 - 1.0)
  of "array":
    let n = max(1, int(a(0)))
    let init = a(1)
    Value(kind: vkArray, buf: Buffer(data: newSeqWith(n, init)))
  of "len":
    if args[0].kind == vkArray: f(float64(args[0].buf.data.len))
    else: f(0.0)
  else:
    raise newException(EvalError, "unknown builtin: " & name)

# ----------------------------------------------------------------- evaluator

proc eval(voice: Voice; n: Node; scope: Scope): Value

proc evalBin(op: string; lv, rv: Value): Value =
  let a = lv.toFloat; let b = rv.toFloat
  case op
  of "+":  f(a + b)
  of "-":  f(a - b)
  of "*":  f(a * b)
  of "/":  f(if b == 0.0: 0.0 else: a / b)
  of "mod":
    if b == 0.0: f(0.0)
    else: f(a - floor(a / b) * b)
  of "==": f(if a == b: 1.0 else: 0.0)
  of "!=": f(if a != b: 1.0 else: 0.0)
  of "<":  f(if a <  b: 1.0 else: 0.0)
  of ">":  f(if a >  b: 1.0 else: 0.0)
  of "<=": f(if a <= b: 1.0 else: 0.0)
  of ">=": f(if a >= b: 1.0 else: 0.0)
  of "and": f(if a != 0.0 and b != 0.0: 1.0 else: 0.0)
  of "or":  f(if a != 0.0 or  b != 0.0: 1.0 else: 0.0)
  else:
    raise newException(EvalError, "unknown operator: " & op)

proc callFunc(voice: Voice; fname: string; args: seq[Value]): Value =
  if fname in voice.funcs:
    let fd = voice.funcs[fname]
    if args.len != fd.params.len:
      raise newException(EvalError,
        "arity mismatch in call to '" & fname & "': expected " &
        $fd.params.len & ", got " & $args.len)
    var newScope = Scope(isDef: true)
    for i, p in fd.params:
      newScope.locals[p] = args[i]
    return voice.eval(fd.body, newScope)
  if fname in BuiltinNames:
    return voice.evalBuiltin(fname, args)
  raise newException(EvalError, "unknown function: " & fname)

proc eval(voice: Voice; n: Node; scope: Scope): Value =
  if n == nil: return f(0.0)
  case n.kind
  of nkNum:
    f(n.num)
  of nkIdent:
    case n.str
    of "t":       f(voice.t)
    of "start_t": f(voice.startT)
    of "sr":      f(voice.sr)
    of "dt":      f(1.0 / voice.sr)
    of "PI":      f(PI)
    of "TAU":     f(TAU)
    else:
      let r = resolve(scope, voice, n.str)
      case r.kind
      of lkLocal, lkCallsite, lkVar: r.val
      of lkNone:
        if n.str in voice.funcs or n.str in BuiltinNames:
          Value(kind: vkFunc, fname: n.str)
        else:
          raise newException(EvalError, "undefined name: " & n.str)
  of nkUnary:
    let v = voice.eval(n.kids[0], scope)
    case n.str
    of "-":   f(-v.toFloat)
    of "not": f(if v.toFloat == 0.0: 1.0 else: 0.0)
    else: raise newException(EvalError, "unknown unary: " & n.str)
  of nkBinOp:
    if n.str == "and":
      let l = voice.eval(n.kids[0], scope)
      if l.toFloat == 0.0: return f(0.0)
      let r = voice.eval(n.kids[1], scope)
      return f(if r.toFloat != 0.0: 1.0 else: 0.0)
    if n.str == "or":
      let l = voice.eval(n.kids[0], scope)
      if l.toFloat != 0.0: return f(1.0)
      let r = voice.eval(n.kids[1], scope)
      return f(if r.toFloat != 0.0: 1.0 else: 0.0)
    let lv = voice.eval(n.kids[0], scope)
    let rv = voice.eval(n.kids[1], scope)
    evalBin(n.str, lv, rv)
  of nkIf:
    let c = voice.eval(n.kids[0], scope)
    if c.isTruthy:
      voice.eval(n.kids[1], scope)
    else:
      voice.eval(n.kids[2], scope)
  of nkArr:
    var data = newSeq[float64](n.kids.len)
    for i, k in n.kids: data[i] = voice.eval(k, scope).toFloat
    Value(kind: vkArray, buf: Buffer(data: data))
  of nkIdx:
    let arrVal = voice.eval(n.kids[0], scope)
    let idxVal = voice.eval(n.kids[1], scope)
    if arrVal.kind != vkArray:
      raise newException(EvalError, "index target is not an array")
    let i = int(idxVal.toFloat)
    let dat = arrVal.buf.data
    if i < 0 or i >= dat.len: f(0.0) else: f(dat[i])
  of nkIdxAssign:
    let arrVal = voice.eval(n.kids[0], scope)
    let idxVal = voice.eval(n.kids[1], scope)
    let val    = voice.eval(n.kids[2], scope)
    if arrVal.kind != vkArray:
      raise newException(EvalError, "index target is not an array")
    let i = int(idxVal.toFloat)
    if i >= 0 and i < arrVal.buf.data.len:
      arrVal.buf.data[i] = val.toFloat
    val
  of nkCall:
    # If the call name resolves to a function value in scope, dispatch on it.
    var fname = n.str
    let r = resolve(scope, voice, n.str)
    if r.kind != lkNone and r.val.kind == vkFunc:
      fname = r.val.fname
    var args: seq[Value] = newSeq[Value](n.kids.len)
    for i, a in n.kids: args[i] = voice.eval(a, scope)
    callFunc(voice, fname, args)
  of nkLet:
    let v = voice.eval(n.kids[0], scope)
    scope.locals[n.str] = v
    v
  of nkVar:
    if scope.isDef:
      let slot = voice.callSiteCounter
      inc voice.callSiteCounter
      if slot >= voice.callSiteState.len:
        voice.callSiteState.add voice.eval(n.kids[0], scope)
      scope.callsites[n.str] = slot
      voice.callSiteState[slot]
    else:
      if n.str notin voice.vars:
        voice.vars[n.str] = voice.eval(n.kids[0], scope)
      voice.vars[n.str]
  of nkAssign:
    let v = voice.eval(n.kids[0], scope)
    let r = resolve(scope, voice, n.str)
    case r.kind
    of lkCallsite:
      voice.callSiteState[r.slot] = v
    of lkVar:
      voice.vars[n.str] = v
    of lkLocal:
      r.scope.locals[n.str] = v        # mutable let / arg (per-sample)
    of lkNone:
      raise newException(EvalError, "cannot assign to undefined name: " & n.str)
    v
  of nkDef:
    voice.funcs[n.str] = FuncDef(params: n.params, body: n.kids[0])
    f(0.0)
  of nkBlock:
    var v = f(0.0)
    for s in n.kids: v = voice.eval(s, scope)
    v

# ----------------------------------------------------------------- driver

proc newVoice*(program: Node; sampleRate: float64 = 48000.0): Voice =
  result = Voice(program: program, sr: sampleRate, rng: 0x12345678'u32)

proc tick*(voice: Voice; t: float64): float64 =
  voice.t = t
  voice.callSiteCounter = 0
  let scope = Scope(isDef: false)
  let v = voice.eval(voice.program, scope)
  let s = v.toFloat
  if s != s: 0.0                             # NaN
  elif s > 1e6 or s < -1e6: 0.0              # overflow
  else: s
