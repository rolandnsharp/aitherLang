## aither → C source transpiler. Consumes parser.Node AST (stdlib
## prepended), produces a C translation unit TCC can compile.
##
## Contract:
##   emitC(program) -> (cSource, varNames)
## `varNames` lists top-level `var`s in definition order so the engine
## can migrate state field-by-field across hot reloads.
##
## Shape of the emitted C:
##   #include <math.h>
##   extern double n_<fn>(...);                    (one per dsp.nim primitive)
##   typedef struct { pool/idx/overflow/sr    ← DspState prefix (binary-
##                    t,dt,start_t,           ←  compatible cast to
##                    <top-level vars>,       ←  DspState*)
##                    inited[...]; } VoiceState;
##   void tick(VoiceState* s, double* outL, double* outR) { ... }
##
## Always emits a stereo tick. If the final expression is scalar, both
## outputs are set equal; if it's [L,R], they're set independently.

import std/[strutils, tables, sets, strformat, sequtils]
import parser

const PoolSize* = 524288

type
  # A per-helper-type state region. Each stateful call site owns one
  # region, keyed by (typeName, perTypeIdx). Across a hot reload, a new
  # region with a matching (typeName, perTypeIdx, size) inherits the old
  # region's slots via copyMem — so inserting a new helper type in a
  # patch doesn't shift existing state (filters, delays, reverb tails
  # survive structural edits).
  Region* = object
    typeName*: string
    perTypeIdx*: int
    offset*: int            # absolute start offset into pool (in slots)
    size*: int              # number of float64 slots

  Scope = ref object
    parent: Scope
    # name -> C expression text (for params, inlined let bindings)
    # and name -> "pool[<n>]" for inlined vars
    names: Table[string, string]
    # name -> aither function name (for fn-valued params like `shape`)
    fns: Table[string, string]
    # name -> (lExpr, rExpr). Stereo-valued bindings (params, lets).
    stereoNames: Table[string, tuple[l, r: string]]

  Ctx = ref object
    buf: string           # accumulating output
    userDefs: Table[string, Node]
    topVarNames*: seq[string]   # top-level vars in definition order
    topVarSet: HashSet[string]
    topLets: HashSet[string]    # top-level let names (scoped to tick())
    playNames*: seq[string]     # play blocks in definition order
    playSet: HashSet[string]
    stereoLets: HashSet[string] # top-level let names whose RHS is stereo
    # name -> (C array symbol, length). Populated when a top-level let
    # binds an nkArr literal of numeric constants.
    topArrays: Table[string, (string, int)]
    arrayDecls: string          # accumulates `static const double arr_N[...]`
    tmpCounter: int
    patchPath: string           # source path used in `#line` directives
    # Per-helper-type state layout. `regions` is filled during emission;
    # offsets are finalised after the walk by assigning per-type base
    # offsets in first-seen order.
    sr*: float64
    regions*: seq[Region]
    typeCounter: Table[string, int]     # name -> next perTypeIdx
    typeCumSize: Table[string, int]     # name -> cumulative size so far
    typeOrder: seq[string]              # types in first-seen order

# Native dsp.nim primitives callable from generated C.
# Name -> arity (not counting the state pointer).
const NativeArities = {
  "lp1": 2, "hp1": 2, "lpf": 3, "hpf": 3, "bpf": 3, "notch": 3,
  "delay": 3, "fbdelay": 4, "reverb": 3,
  "impulse": 1, "resonator": 3, "discharge": 2,
  "tremolo": 3, "slew": 2,
  "wave": -1,                      # special: freq + array
}.toTable

# Libm 1-arg pass-throughs.
const Libm1 = ["sin", "cos", "tan", "exp", "log", "log2",
               "floor", "ceil", "sqrt"]

# ---- small helpers -----------------------------------------------------

proc newCtx*(program: Node): Ctx =
  result = Ctx(userDefs: initTable[string, Node](),
               topVarSet: initHashSet[string](),
               topLets: initHashSet[string](),
               topArrays: initTable[string, (string, int)](),
               playSet: initHashSet[string](),
               stereoLets: initHashSet[string](),
               sr: 48000.0,
               typeCounter: initTable[string, int](),
               typeCumSize: initTable[string, int]())
  # Gather user defs so call sites can inline them.
  for n in program.kids:
    if n.kind == nkDef:
      result.userDefs[n.str] = n

# Register one stateful call site. Returns a placeholder token to embed
# in the C source — substituted with the absolute pool offset at the
# end of emission, once region bases are known.
proc registerRegion(c: Ctx; typeName: string; size: int): string =
  if typeName notin c.typeCounter:
    c.typeCounter[typeName] = 0
    c.typeCumSize[typeName] = 0
    c.typeOrder.add typeName
  let perTypeIdx = c.typeCounter[typeName]
  let intraOffset = c.typeCumSize[typeName]
  c.typeCounter[typeName] = perTypeIdx + 1
  c.typeCumSize[typeName] = intraOffset + size
  let rid = c.regions.len
  # offset is back-patched once per-type bases are assigned.
  c.regions.add Region(typeName: typeName, perTypeIdx: perTypeIdx,
                       offset: intraOffset, size: size)
  "__OFF_" & $rid & "__"

proc fresh(c: Ctx; prefix: string): string =
  inc c.tmpCounter
  prefix & "_" & $c.tmpCounter

proc push(parent: Scope): Scope =
  Scope(parent: parent,
        names: initTable[string, string](),
        fns: initTable[string, string](),
        stereoNames: initTable[string, tuple[l, r: string]]())

proc lookupStereo(sc: Scope; name: string): tuple[l, r: string, ok: bool] =
  var s = sc
  while s != nil:
    if name in s.stereoNames:
      let p = s.stereoNames[name]
      return (p.l, p.r, true)
    s = s.parent
  ("", "", false)

proc lookupFn(sc: Scope; name: string): string =
  var s = sc
  while s != nil:
    if name in s.fns: return s.fns[name]
    s = s.parent
  ""

proc lookup(sc: Scope; name: string): string =
  var s = sc
  while s != nil:
    if name in s.names: return s.names[name]
    s = s.parent
  ""

proc numLit(v: float64): string =
  # Emit with enough digits that round-trip is exact; append dot if
  # integer-valued so the literal type is double.
  let s = formatFloat(v, ffDefault, 17)
  if '.' in s or 'e' in s or 'E' in s: s else: s & ".0"

# ---- expression codegen ------------------------------------------------

proc emitExpr(c: Ctx; sc: Scope; n: Node): string
proc emitBlockExpr(c: Ctx; sc: Scope; blk: Node): string

# Stereo-aware emission. Returns (l, r, stereo) where stereo=false means
# l == r (the caller may collapse to a single emit). `pre` accumulates
# any C statements that must execute before l/r are referenced (used
# when a stereo-returning user def must be inlined once and its two
# channels captured into fresh temps — the inlining can't fit inside a
# single expression).
type StereoVal = tuple[l, r: string, stereo: bool]
proc emitStereo(c: Ctx; sc: Scope; n: Node; pre: var string): StereoVal

# Does this expression structurally return a stereo value? True when the
# final expression (all branches of nested ifs / blocks) is an nkArr of
# length 2 or a call to another stereo-returning def.
proc isStereoReturn(c: Ctx; n: Node): bool
proc isStereoDef(c: Ctx; def: Node): bool =
  isStereoReturn(c, def.kids[0])
proc isStereoReturn(c: Ctx; n: Node): bool =
  if n == nil: return false
  case n.kind
  of nkArr: n.kids.len == 2
  of nkBlock:
    if n.kids.len == 0: false
    else: isStereoReturn(c, n.kids[^1])
  of nkIf:
    isStereoReturn(c, n.kids[1]) or
      (n.kids[2] != nil and isStereoReturn(c, n.kids[2]))
  of nkCall:
    n.str in c.userDefs and isStereoDef(c, c.userDefs[n.str])
  else: false

# Walk a user def body to decide whether it can be safely inlined twice
# (once per channel) in stereo context. Stateful calls (phasor, noise,
# native dsp) and `var` bindings make the def unsafe to duplicate.
proc isPureForStereo(c: Ctx; n: Node): bool =
  if n == nil: return true
  case n.kind
  of nkVar: return false
  of nkCall:
    if n.str in ["phasor", "noise"]: return false
    if NativeArities.hasKey(n.str): return false
    if n.str in c.userDefs:
      if not isPureForStereo(c, c.userDefs[n.str].kids[0]): return false
  else: discard
  for k in n.kids:
    if k != nil and not isPureForStereo(c, k): return false
  true

proc callArgs(c: Ctx; sc: Scope; kids: openArray[Node]): string =
  var parts: seq[string] = @[]
  for k in kids: parts.add c.emitExpr(sc, k)
  parts.join(", ")

# Inline a user def call. Substitute param names with the evaluated C
# expressions (bound to fresh locals to avoid re-evaluating side effects),
# emit let/var handling inside a statement expression.
proc emitDefInline(c: Ctx; sc: Scope; def: Node; args: seq[Node]): string =
  if args.len != def.params.len:
    raise newException(ValueError,
      "wrong arg count for " & def.str & ": expected " &
      $def.params.len & " got " & $args.len)
  let inner = push(sc)
  var preface = ""
  for i, p in def.params:
    let a = args[i]
    # Function-valued arg: a bare ident naming a known builtin/def. Bind
    # in inner.fns so calls to `p(...)` resolve to the concrete function.
    if a.kind == nkIdent and
       (a.str in Libm1 or a.str in ["saw", "tri", "sqr", "sin", "cos",
                                     "phasor", "noise", "pow"] or
        a.str in c.userDefs or
        NativeArities.hasKey(a.str) or
        sc.lookupFn(a.str).len > 0):
      let resolved = if sc.lookupFn(a.str).len > 0: sc.lookupFn(a.str)
                     else: a.str
      inner.fns[p] = resolved
      continue
    # Scalar arg: evaluate in caller scope, bind to fresh local.
    let argC = c.emitExpr(sc, a)
    let tmp = c.fresh("p_" & p)
    preface.add &"double {tmp} = ({argC}); "
    inner.names[p] = tmp
  # def.kids[0] is the body (single expression or nkBlock of stmts).
  let body = def.kids[0]
  let bodyC =
    if body.kind == nkBlock: c.emitBlockExpr(inner, body)
    else: c.emitExpr(inner, body)
  "(({ " & preface & "double __r = (" & bodyC & "); __r; }))"

# Body of a def (nkBlock). All statements but the last are stmts; last
# is the expression returned. `let` adds a C local; `var` is call-site
# state stored in the pool.
proc emitBlockExpr(c: Ctx; sc: Scope; blk: Node): string =
  var pre = ""
  let last = blk.kids.len - 1
  for i, s in blk.kids:
    if i == last:
      return "({ " & pre & "double __r = (" & c.emitExpr(sc, s) & "); __r; })"
    case s.kind
    of nkLet:
      let tmp = c.fresh("l_" & s.str)
      let v = c.emitExpr(sc, s.kids[0])
      pre.add &"double {tmp} = ({v}); "
      sc.names[s.str] = tmp
    of nkVar:
      # `var` inside a def body -> claim two pool slots keyed under a
      # "var" helper-type region: one "inited" flag, one value. On the
      # first evaluation the init expression runs; thereafter the stored
      # value persists across samples (and across hot reloads, because
      # the (type, perTypeIdx) pair survives a state migration).
      let flagSlot = c.fresh("vf")
      let valSlot = c.fresh("vs")
      let off = c.registerRegion("var", 2)
      pre.add &"long {flagSlot} = {off}; long {valSlot} = ({off}) + 1; "
      pre.add &"if (s->pool[{flagSlot}] == 0.0) {{ " &
              &"s->pool[{valSlot}] = ({c.emitExpr(sc, s.kids[0])}); " &
              &"s->pool[{flagSlot}] = 1.0; }} "
      sc.names[s.str] = &"s->pool[{valSlot}]"
    of nkAssign:
      let target = sc.lookup(s.str)
      if target.len == 0:
        raise newException(ValueError, "unknown assignment target: " & s.str)
      let rhs = c.emitExpr(sc, s.kids[0])
      pre.add &"{target} = ({rhs}); "
    else:
      # A bare expression statement is legal (ignored value except last).
      let v = c.emitExpr(sc, s)
      pre.add &"(void)({v}); "
  "0.0"   # empty block

# phasor(freq) inlined: one pool slot, advance, return wrapped value.
proc emitPhasor(c: Ctx; sc: Scope; freqNode: Node): string =
  let freq = c.emitExpr(sc, freqNode)
  let slot = c.fresh("ph")
  let off = c.registerRegion("phasor", 1)
  "(({ long " & slot & " = " & off & "; " &
    "s->pool[" & slot & "] = fmod(s->pool[" & slot & "] + (" & freq &
    ") / s->sr, 1.0); " &
    "if (s->pool[" & slot & "] < 0.0) s->pool[" & slot & "] += 1.0; " &
    "s->pool[" & slot & "]; }))"

# noise() inlined: xorshift32 on a pool slot interpreted as uint32.
proc emitNoise(c: Ctx): string =
  let slot = c.fresh("nz")
  let off = c.registerRegion("noise", 1)
  "(({ long " & slot & " = " & off & "; " &
    "unsigned int r = (unsigned int)s->pool[" & slot & "]; " &
    "if (r == 0) r = 2463534242u; " &
    "r ^= r << 13; r ^= r >> 17; r ^= r << 5; " &
    "s->pool[" & slot & "] = (double)r; " &
    "((double)r / 4294967295.0) * 2.0 - 1.0; }))"

# Size (in float64 slots) claimed by one call to a given native. For
# delay/fbdelay this depends on the `max_time` argument, which must be
# a numeric literal so the region size is compile-time known.
proc nativeSlotSize(c: Ctx; name: string; kids: seq[Node]): int =
  case name
  of "lp1", "hp1", "impulse", "discharge", "tremolo", "slew", "wave":
    1
  of "lpf", "hpf", "bpf", "notch", "resonator":
    2
  of "delay":
    let mt = kids[2]
    if mt.kind != nkNum:
      raise newException(ValueError,
        "delay max_time must be a numeric literal (line " & $mt.line & ")")
    1 + max(1, int(mt.num * c.sr))
  of "fbdelay":
    let mt = kids[2]
    if mt.kind != nkNum:
      raise newException(ValueError,
        "fbdelay max_time must be a numeric literal (line " & $mt.line & ")")
    1 + max(1, int(mt.num * c.sr))
  of "reverb":
    # Matches dsp.nim nReverb's total = sum(2+L for combLens) + sum(1+L for apLens).
    (2+1557) + (2+1617) + (2+1491) + (2+1422) + (1+225) + (1+556)
  else:
    raise newException(ValueError, "no slot size for native '" & name & "'")

proc emitExpr(c: Ctx; sc: Scope; n: Node): string =
  case n.kind
  of nkNum:
    numLit(n.num)
  of nkIdent:
    case n.str
    of "t":       "s->t"
    of "sr":      "s->sr"
    of "dt":      "s->dt"
    of "start_t": "s->start_t"
    of "PI":      "3.14159265358979323846"
    of "TAU":     "(2.0 * 3.14159265358979323846)"
    else:
      let scoped = sc.lookup(n.str)
      if scoped.len > 0: scoped
      elif n.str in c.topLets: "l_" & n.str
      elif n.str in c.topVarSet: "s->v_" & n.str
      else:
        raise newException(ValueError, "unknown identifier: " & n.str &
                            " (line " & $n.line & ")")
  of nkUnary:
    let v = c.emitExpr(sc, n.kids[0])
    case n.str
    of "-":   "(-(" & v & "))"
    of "not": "((" & v & ") == 0.0 ? 1.0 : 0.0)"
    else:     raise newException(ValueError, "bad unary: " & n.str)
  of nkBinOp:
    let a = c.emitExpr(sc, n.kids[0])
    let b = c.emitExpr(sc, n.kids[1])
    case n.str
    of "+":  &"(({a}) + ({b}))"
    of "-":  &"(({a}) - ({b}))"
    of "*":  &"(({a}) * ({b}))"
    of "/":  &"(({b}) == 0.0 ? 0.0 : ({a}) / ({b}))"
    of "mod":
      # aither mod: `a - floor(a/b) * b` (safe on zero)
      &"(({b}) == 0.0 ? 0.0 : ({a}) - floor(({a}) / ({b})) * ({b}))"
    of "==": &"((({a}) == ({b})) ? 1.0 : 0.0)"
    of "!=": &"((({a}) != ({b})) ? 1.0 : 0.0)"
    of "<":  &"((({a}) < ({b})) ? 1.0 : 0.0)"
    of ">":  &"((({a}) > ({b})) ? 1.0 : 0.0)"
    of "<=": &"((({a}) <= ({b})) ? 1.0 : 0.0)"
    of ">=": &"((({a}) >= ({b})) ? 1.0 : 0.0)"
    of "and": &"(((({a}) != 0.0) && (({b}) != 0.0)) ? 1.0 : 0.0)"
    of "or":  &"(((({a}) != 0.0) || (({b}) != 0.0)) ? 1.0 : 0.0)"
    else: raise newException(ValueError, "bad binop: " & n.str)
  of nkIf:
    let cond = c.emitExpr(sc, n.kids[0])
    let thn  = c.emitExpr(sc, n.kids[1])
    let els  = if n.kids[2] != nil: c.emitExpr(sc, n.kids[2]) else: "0.0"
    &"(({cond}) != 0.0 ? ({thn}) : ({els}))"
  of nkCall:
    # If the call target is a fn-valued param, resolve to the concrete name.
    let resolved = sc.lookupFn(n.str)
    let name = if resolved.len > 0: resolved else: n.str
    # Stateful builtins inlined
    if name == "phasor":
      if n.kids.len != 1:
        raise newException(ValueError, "phasor takes 1 arg")
      return c.emitPhasor(sc, n.kids[0])
    if name == "noise":
      return c.emitNoise()
    # Libm 1-arg
    if name in Libm1 and n.kids.len == 1:
      return name & "(" & c.emitExpr(sc, n.kids[0]) & ")"
    # abs on doubles is fabs in C
    if name == "abs" and n.kids.len == 1:
      return "fabs(" & c.emitExpr(sc, n.kids[0]) & ")"
    # Shape helpers (builtin as bytecode ops previously) — inline via
    # phasor + shape fn. But these take a *phase* argument, not freq.
    # saw/tri/sqr are shape functions: saw(x) where x is TAU*phase.
    # Keeping the aitherism: shape_saw/tri/sqr map x in [0, TAU] to wave.
    if name in ["saw", "tri", "sqr"] and n.kids.len == 1:
      return "shape_" & name & "(" & c.emitExpr(sc, n.kids[0]) & ")"
    # Math misc
    if name == "pow" and n.kids.len == 2:
      return "pow(" & c.callArgs(sc, n.kids) & ")"
    if name == "min" and n.kids.len == 2:
      let a = c.emitExpr(sc, n.kids[0])
      let b = c.emitExpr(sc, n.kids[1])
      return &"fmin({a}, {b})"
    if name == "max" and n.kids.len == 2:
      let a = c.emitExpr(sc, n.kids[0])
      let b = c.emitExpr(sc, n.kids[1])
      return &"fmax({a}, {b})"
    if name == "clamp" and n.kids.len == 3:
      let v = c.emitExpr(sc, n.kids[0])
      let lo = c.emitExpr(sc, n.kids[1])
      let hi = c.emitExpr(sc, n.kids[2])
      return &"fmin(fmax({v}, {lo}), {hi})"
    if name == "int" and n.kids.len == 1:
      return "((double)(long)(" & c.emitExpr(sc, n.kids[0]) & "))"
    # wave(freq, array) — array must resolve to a compile-time constant.
    if name == "wave":
      if n.kids.len != 2:
        raise newException(ValueError, "wave takes 2 args")
      let freq = c.emitExpr(sc, n.kids[0])
      let arr = n.kids[1]
      var sym = ""
      var length = 0
      if arr.kind == nkIdent and arr.str in c.topArrays:
        (sym, length) = c.topArrays[arr.str]
      elif arr.kind == nkArr:
        sym = c.fresh("arr")
        length = arr.kids.len
        var items: seq[string] = @[]
        for k in arr.kids:
          if k.kind != nkNum:
            raise newException(ValueError,
              "wave array must be numeric literals only")
          items.add numLit(k.num)
        c.arrayDecls.add &"static const double {sym}[{length}] = {{" &
          items.join(", ") & "};\n"
      else:
        raise newException(ValueError,
          "wave: second arg must be an array literal or top-level array let")
      let size = c.nativeSlotSize("wave", n.kids)
      let off = c.registerRegion("wave", size)
      return &"(s->idx = {off}, n_wave((DspState*)s, {freq}, (double*){sym}, {length}))"
    # Native dsp calls. Before each call we set s->idx to the call
    # site's baked region offset so the native's internal claim()
    # writes into its dedicated region, regardless of surrounding
    # call structure. The comma expression forces left-to-right eval.
    if name in NativeArities:
      let arity = NativeArities[name]
      if arity == -1:
        raise newException(ValueError,
          name & " (array arg) not yet supported in codegen")
      if n.kids.len != arity:
        raise newException(ValueError,
          name & " takes " & $arity & " args, got " & $n.kids.len)
      let size = c.nativeSlotSize(name, n.kids)
      let off = c.registerRegion(name, size)
      return "(s->idx = " & off & ", n_" & name & "((DspState*)s, " &
             c.callArgs(sc, n.kids) & "))"
    # User def inline
    if name in c.userDefs:
      return c.emitDefInline(sc, c.userDefs[name], n.kids)
    raise newException(ValueError,
      "unknown function: " & name & " (line " & $n.line & ")")
  of nkArr:
    # Only inline stereo pair arrays are supported as values. Emit as
    # two scalar expressions — the caller context (a final expression or
    # an index) has to unpack.
    raise newException(ValueError,
      "array value can't appear here (line " & $n.line &
      ") — only numeric literals for wave() or top-level stereo return")
  of nkIdx:
    let a = n.kids[0]
    let i = n.kids[1]
    # Top-level numeric array (e.g. `let notes = [...]`).
    if a.kind == nkIdent and a.str in c.topArrays:
      let (sym, length) = c.topArrays[a.str]
      let ix = c.emitExpr(sc, i)
      return &"({sym}[(int)({ix}) % {length}])"
    # Play-name / stereo-let / scope-bound stereo indexed with a
    # constant 0 or 1 → L or R channel scalar.
    if a.kind == nkIdent and i.kind == nkNum and int(i.num) in {0, 1}:
      let stereo = sc.lookupStereo(a.str)
      if stereo.ok:
        return (if int(i.num) == 0: stereo.l else: stereo.r)
      if a.str in c.playSet or a.str in c.stereoLets:
        let chan = if int(i.num) == 0: "_l" else: "_r"
        let prefix = if a.str in c.playSet: "p_" else: "l_"
        return prefix & a.str & chan
    # Inline constant array literal with constant index.
    if a.kind == nkArr and i.kind == nkNum:
      let k = int(i.num)
      if k < 0 or k >= a.kids.len:
        raise newException(ValueError, "array index out of range")
      return c.emitExpr(sc, a.kids[k])
    raise newException(ValueError,
      "indexing non-constant arrays not supported (line " & $n.line & ")")
  else:
    raise newException(ValueError,
      "cannot emit expression node: " & $n.kind)

proc emitStereo(c: Ctx; sc: Scope; n: Node; pre: var string): StereoVal =
  case n.kind
  of nkIdent:
    let scoped = sc.lookupStereo(n.str)
    if scoped.ok:
      return (scoped.l, scoped.r, true)
    if n.str in c.playSet:
      return ("p_" & n.str & "_l", "p_" & n.str & "_r", true)
    if n.str in c.stereoLets:
      return ("l_" & n.str & "_l", "l_" & n.str & "_r", true)
    # Function-value idents (passed as args) aren't variables; let
    # emitExpr handle them via its call site. Report as scalar with a
    # placeholder so the caller falls through to scalar emission.
    if n.str in Libm1 or n.str in ["sin", "cos", "saw", "tri", "sqr",
                                    "phasor", "noise", "abs", "pow"] or
       n.str in c.userDefs or NativeArities.hasKey(n.str):
      return (n.str, n.str, false)
    let s = c.emitExpr(sc, n)
    return (s, s, false)
  of nkArr:
    if n.kids.len == 2:
      let l = c.emitExpr(sc, n.kids[0])
      let r = c.emitExpr(sc, n.kids[1])
      return (l, r, true)
    raise newException(ValueError,
      "array literal with " & $n.kids.len &
      " elements in stereo context (only [L,R] supported)")
  of nkIdx:
    let base = n.kids[0]
    let idx = n.kids[1]
    if base.kind == nkIdent and idx.kind == nkNum and
       int(idx.num) in {0, 1}:
      let stereo = sc.lookupStereo(base.str)
      if stereo.ok:
        let pick = if int(idx.num) == 0: stereo.l else: stereo.r
        return (pick, pick, false)
      if base.str in c.playSet or base.str in c.stereoLets:
        let chan = if int(idx.num) == 0: "_l" else: "_r"
        let prefix = if base.str in c.playSet: "p_" else: "l_"
        let pick = prefix & base.str & chan
        return (pick, pick, false)
    let s = c.emitExpr(sc, n)
    return (s, s, false)
  of nkBinOp:
    let a = c.emitStereo(sc, n.kids[0], pre)
    let b = c.emitStereo(sc, n.kids[1], pre)
    if not a.stereo and not b.stereo:
      let s = c.emitExpr(sc, n)
      return (s, s, false)
    proc combine(op, x, y: string): string =
      case op
      of "+":  &"(({x}) + ({y}))"
      of "-":  &"(({x}) - ({y}))"
      of "*":  &"(({x}) * ({y}))"
      of "/":  &"(({y}) == 0.0 ? 0.0 : ({x}) / ({y}))"
      else: raise newException(ValueError,
              "binop '" & op & "' not allowed on stereo values")
    return (combine(n.str, a.l, b.l), combine(n.str, a.r, b.r), true)
  of nkUnary:
    let a = c.emitStereo(sc, n.kids[0], pre)
    if not a.stereo:
      let s = c.emitExpr(sc, n)
      return (s, s, false)
    if n.str == "-":
      return ("(-(" & a.l & "))", "(-(" & a.r & "))", true)
    raise newException(ValueError, "unary '" & n.str & "' not allowed on stereo")
  of nkIf:
    let cond = c.emitExpr(sc, n.kids[0])
    let thn = c.emitStereo(sc, n.kids[1], pre)
    let els =
      if n.kids[2] != nil: c.emitStereo(sc, n.kids[2], pre)
      else: ("0.0", "0.0", false)
    if not thn.stereo and not els.stereo:
      let s = c.emitExpr(sc, n)
      return (s, s, false)
    return (&"(({cond}) != 0.0 ? ({thn.l}) : ({els.l}))",
            &"(({cond}) != 0.0 ? ({thn.r}) : ({els.r}))", true)
  of nkCall:
    # Stereo-returning user def (e.g. haas, pan): inline once, capture
    # the two output channels into fresh temps via the `pre` preamble.
    if n.str in c.userDefs and isStereoDef(c, c.userDefs[n.str]):
      let def = c.userDefs[n.str]
      if n.kids.len != def.params.len:
        raise newException(ValueError,
          "wrong arg count for " & n.str & ": expected " &
          $def.params.len & " got " & $n.kids.len)
      let inner = push(sc)
      let tmpL = c.fresh("st_" & n.str & "_l")
      let tmpR = c.fresh("st_" & n.str & "_r")
      var blk = ""
      blk.add &"  double {tmpL}, {tmpR};\n"
      blk.add "  {\n"
      for i, p in def.params:
        let a = n.kids[i]
        if a.kind == nkIdent and
           (a.str in Libm1 or
            a.str in ["sin","cos","saw","tri","sqr","phasor","noise",
                      "abs","pow"] or
            a.str in c.userDefs or NativeArities.hasKey(a.str) or
            sc.lookupFn(a.str).len > 0):
          let resolved =
            if sc.lookupFn(a.str).len > 0: sc.lookupFn(a.str) else: a.str
          inner.fns[p] = resolved
          continue
        # Stereo arg → bind param as a stereo local in the inlined body.
        var sub = ""
        let sv = c.emitStereo(sc, a, sub)
        pre.add sub
        if sv.stereo:
          let tmpL = c.fresh("p_" & p & "_l")
          let tmpR = c.fresh("p_" & p & "_r")
          blk.add &"    double {tmpL} = ({sv.l});\n"
          blk.add &"    double {tmpR} = ({sv.r});\n"
          inner.stereoNames[p] = (tmpL, tmpR)
        else:
          let tmp = c.fresh("p_" & p)
          blk.add &"    double {tmp} = ({sv.l});\n"
          inner.names[p] = tmp
      # Emit body as a block, storing the stereo final value into tmpL/tmpR.
      let bodyNode = def.kids[0]
      let stmts: seq[Node] =
        if bodyNode.kind == nkBlock: bodyNode.kids else: @[bodyNode]
      let lastIdx = stmts.len - 1
      for si, st in stmts:
        if si == lastIdx and st.kind notin {nkLet, nkVar, nkAssign}:
          var sub = ""
          let v = c.emitStereo(inner, st, sub)
          blk.add sub
          blk.add &"    {tmpL} = ({v.l});\n"
          blk.add &"    {tmpR} = ({v.r});\n"
        else:
          case st.kind
          of nkLet:
            let v = c.emitExpr(inner, st.kids[0])
            let tmp = c.fresh("l_" & st.str)
            blk.add &"    double {tmp} = ({v});\n"
            inner.names[st.str] = tmp
          of nkVar:
            # `var` inside a stereo-def inline: treat as pool-backed
            # call-site state, matching emitBlockExpr's approach.
            let flagSlot = c.fresh("vf")
            let valSlot = c.fresh("vs")
            let off = c.registerRegion("var", 2)
            blk.add &"    long {flagSlot} = {off};\n"
            blk.add &"    long {valSlot} = ({off}) + 1;\n"
            let initC = c.emitExpr(inner, st.kids[0])
            blk.add &"    if (s->pool[{flagSlot}] == 0.0) {{\n"
            blk.add &"      s->pool[{valSlot}] = ({initC});\n"
            blk.add &"      s->pool[{flagSlot}] = 1.0;\n"
            blk.add "    }\n"
            inner.names[st.str] = &"s->pool[{valSlot}]"
          of nkAssign:
            let target = inner.lookup(st.str)
            if target.len == 0:
              raise newException(ValueError, "unknown assign: " & st.str)
            let rhs = c.emitExpr(inner, st.kids[0])
            blk.add &"    {target} = ({rhs});\n"
          else:
            let v = c.emitExpr(inner, st)
            blk.add &"    (void)({v});\n"
      blk.add "  }\n"
      pre.add blk
      return (tmpL, tmpR, true)
    # Otherwise: evaluate args stereo-aware to see if any is stereo.
    var argVals: seq[StereoVal] = @[]
    var anyStereo = false
    for k in n.kids:
      let v = c.emitStereo(sc, k, pre)
      if v.stereo: anyStereo = true
      argVals.add v
    if not anyStereo:
      let s = c.emitExpr(sc, n)
      return (s, s, false)
    # One or more stereo args: try to split per channel. Safe only for
    # pure callees (libm / arithmetic builtins / pure user defs).
    let name = n.str
    let canSplit =
      name in Libm1 or name in ["pow", "min", "max", "clamp", "int",
                                 "saw", "tri", "sqr", "abs"] or
      (name in c.userDefs and isPureForStereo(c, c.userDefs[name].kids[0]))
    if not canSplit:
      raise newException(ValueError,
        "stateful call '" & name & "' receives a stereo value " &
        "(line " & $n.line & ") — split L/R first via bass[0]/bass[1]")
    # Substitute per-channel stereo args via fresh scope-bound names,
    # then emit two scalar calls via emitExpr.
    let innerL = push(sc)
    let innerR = push(sc)
    var callL = Node(kind: nkCall, str: n.str, line: n.line, kids: @[])
    var callR = Node(kind: nkCall, str: n.str, line: n.line, kids: @[])
    for i, k in n.kids:
      if argVals[i].stereo:
        let tmpL = c.fresh("stl")
        let tmpR = c.fresh("str")
        innerL.names[tmpL] = argVals[i].l
        innerR.names[tmpR] = argVals[i].r
        callL.kids.add Node(kind: nkIdent, str: tmpL, line: n.line)
        callR.kids.add Node(kind: nkIdent, str: tmpR, line: n.line)
      else:
        callL.kids.add k
        callR.kids.add k
    let lExpr = c.emitExpr(innerL, callL)
    let rExpr = c.emitExpr(innerR, callR)
    return (lExpr, rExpr, true)
  else:
    let s = c.emitExpr(sc, n)
    return (s, s, false)

# Detect whether a subtree references any play name or stereo let, or
# calls a stereo-returning user def — used to decide whether a top-level
# `let` after plays is stereo.
proc refsStereo(c: Ctx; n: Node): bool =
  if n == nil: return false
  case n.kind
  of nkIdent:
    return n.str in c.playSet or n.str in c.stereoLets
  of nkArr:
    return n.kids.len == 2
  of nkIdx:
    # Indexing a stereo (play / stereo-let) with a constant 0 or 1
    # collapses to scalar; only propagate stereo if the index isn't a
    # constant pick.
    let a = n.kids[0]; let i = n.kids[1]
    if a.kind == nkIdent and i.kind == nkNum and int(i.num) in {0, 1} and
       (a.str in c.playSet or a.str in c.stereoLets):
      return false
    return c.refsStereo(i)   # ignore base; its stereo-ness is consumed
  of nkCall:
    if n.str in c.userDefs and isStereoDef(c, c.userDefs[n.str]):
      return true
    for k in n.kids:
      if k != nil and c.refsStereo(k): return true
    return false
  else:
    for k in n.kids:
      if k != nil and c.refsStereo(k): return true
    return false

# ---- top-level emission ------------------------------------------------

proc emit*(c: Ctx; program: Node): string =
  # First pass: collect top-level vars (including ones nested in play
  # bodies — `var` is name-keyed, play-scope doesn't change that) and
  # top-level lets (play-local lets stay local).
  proc scanVars(c: Ctx; n: Node) =
    if n == nil: return
    case n.kind
    of nkVar:
      if n.str notin c.topVarSet:
        c.topVarSet.incl n.str
        c.topVarNames.add n.str
    of nkPlay:
      let body = n.kids[0]
      if body.kind == nkBlock:
        for k in body.kids: c.scanVars(k)
      else: c.scanVars(body)
    else: discard
  for s in program.kids:
    case s.kind
    of nkVar:
      if s.str notin c.topVarSet:
        c.topVarSet.incl s.str
        c.topVarNames.add s.str
    of nkLet:
      c.topLets.incl s.str
    of nkPlay:
      let body = s.kids[0]
      if body.kind == nkBlock:
        for k in body.kids: c.scanVars(k)
      else: c.scanVars(body)
    else: discard

  # Prelude
  var pre = ""
  pre.add "#include <math.h>\n"
  pre.add "typedef struct DspState DspState;\n"
  # extern declarations for every dsp.nim primitive we might call.
  pre.add "extern double n_lpf(DspState*,double,double,double);\n"
  pre.add "extern double n_hpf(DspState*,double,double,double);\n"
  pre.add "extern double n_bpf(DspState*,double,double,double);\n"
  pre.add "extern double n_notch(DspState*,double,double,double);\n"
  pre.add "extern double n_lp1(DspState*,double,double);\n"
  pre.add "extern double n_hp1(DspState*,double,double);\n"
  pre.add "extern double n_delay(DspState*,double,double,double);\n"
  pre.add "extern double n_fbdelay(DspState*,double,double,double,double);\n"
  pre.add "extern double n_reverb(DspState*,double,double,double);\n"
  pre.add "extern double n_impulse(DspState*,double);\n"
  pre.add "extern double n_resonator(DspState*,double,double,double);\n"
  pre.add "extern double n_discharge(DspState*,double,double);\n"
  pre.add "extern double n_tremolo(DspState*,double,double,double);\n"
  pre.add "extern double n_slew(DspState*,double,double);\n"
  pre.add "extern double n_wave(DspState*,double,double*,int);\n"
  pre.add "extern double shape_saw(double);\n"
  pre.add "extern double shape_tri(double);\n"
  pre.add "extern double shape_sqr(double);\n"
  # VoiceState — DspState prefix is *exactly* the Nim layout; new fields
  # go after.
  # NOTE: voice.nim depends on this exact layout for state migration:
  #   [DspState prefix (sizeof DspState = 4194328 bytes)]
  #   [double t, dt, start_t]  (3*8 = 24 bytes, header ends at 4194352)
  #   [double v_<name0>, v_<name1>, ...]  (one slot per top-level var)
  #   [unsigned char inited[N]]  (one byte per top-level var)
  pre.add "typedef struct {\n"
  pre.add "  double pool[524288];\n"
  pre.add "  long   idx;\n"
  pre.add "  char   overflow;\n"
  pre.add "  double sr;\n"
  pre.add "  double t, dt, start_t;\n"
  pre.add "  double* part_gains;\n"
  for vn in c.topVarNames:
    pre.add "  double v_" & vn & ";\n"
  pre.add "  unsigned char inited[" & $max(1, c.topVarNames.len) & "];\n"
  pre.add "} VoiceState;\n\n"

  # Body of tick()
  var body = ""
  body.add "void tick(VoiceState* s, double* outL, double* outR) {\n"
  body.add "  s->idx = 0;\n"
  let topSc = push(nil)
  # Lazy-init top-level vars by name.
  for i, vn in c.topVarNames:
    # Find the var node to get its init expression.
    for st in program.kids:
      if st.kind == nkVar and st.str == vn:
        let init = c.emitExpr(topSc, st.kids[0])
        body.add &"  if (!(s->inited[{i}] & 1)) {{ s->v_{vn} = ({init}); s->inited[{i}] |= 1; }}\n"
        break

  # Top-level statements: let -> local; var handled above (init) and via
  # assign below; def -> skipped (inlined at call sites); final expression
  # -> set outputs.
  var finalIdx = -1
  for i in countdown(program.kids.len - 1, 0):
    let s = program.kids[i]
    if s.kind notin {nkDef, nkVar, nkLet, nkAssign, nkPlay}:
      finalIdx = i; break

  for i, s in program.kids:
    # Map generated-C line back to aither source. Best-effort: puts the
    # directive at a statement boundary; errors inside inlined expressions
    # still report under their enclosing top-level line. TCC honors these.
    if c.patchPath.len > 0 and s.line > 0:
      body.add &"#line {s.line} \"{c.patchPath}\"\n"
    case s.kind
    of nkDef, nkVar: discard       # def inlined; var already lazy-inited
    of nkLet:
      # Numeric array literal -> hoist to a static const, record it.
      if s.kids[0].kind == nkArr and
         s.kids[0].kids.allIt(it.kind == nkNum):
        let arrKids = s.kids[0].kids
        let sym = "arr_" & s.str
        var items: seq[string] = @[]
        for k in arrKids: items.add numLit(k.num)
        c.arrayDecls.add &"static const double {sym}[{arrKids.len}] = {{" &
          items.join(", ") & "};\n"
        c.topArrays[s.str] = (sym, arrKids.len)
      elif c.refsStereo(s.kids[0]):
        var pre = ""
        let v = c.emitStereo(topSc, s.kids[0], pre)
        body.add pre
        body.add &"  double l_{s.str}_l = ({v.l});\n"
        body.add &"  double l_{s.str}_r = ({v.r});\n"
        c.stereoLets.incl s.str
      else:
        let v = c.emitExpr(topSc, s.kids[0])
        body.add &"  double l_{s.str} = ({v});\n"
    of nkAssign:
      let target =
        if s.str in c.topVarSet: "s->v_" & s.str
        elif s.str in c.topLets: "l_" & s.str
        else: raise newException(ValueError, "unknown assign: " & s.str)
      let rhs = c.emitExpr(topSc, s.kids[0])
      body.add &"  {target} = ({rhs});\n"
    of nkPlay:
      # A play's lets and computed value live inside a `{...}` block so
      # the lets can't collide with other plays or later code. Each play
      # also registers a gain slot (s->part_gains[partIdx]) applied to
      # the value. Bodies may be scalar (mirrored to both channels) or
      # stereo ([L,R] literal / referencing earlier plays).
      let partIdx = c.playNames.len
      c.playNames.add s.str
      c.playSet.incl s.str
      let inner = push(topSc)
      body.add &"  double p_{s.str}_l, p_{s.str}_r;\n"
      body.add "  {\n"
      let bodyNode = s.kids[0]
      let stmts: seq[Node] =
        if bodyNode.kind == nkBlock: bodyNode.kids else: @[bodyNode]
      let lastIdx = stmts.len - 1
      for si, st in stmts:
        if si == lastIdx and st.kind notin {nkLet, nkVar, nkAssign}:
          var sub = ""
          let v = c.emitStereo(inner, st, sub)
          body.add sub
          body.add &"    p_{s.str}_l = ({v.l}) * s->part_gains[{partIdx}];\n"
          body.add &"    p_{s.str}_r = ({v.r}) * s->part_gains[{partIdx}];\n"
        else:
          case st.kind
          of nkLet:
            # Stereo-producing RHS (haas, pan, arr literal, or references
            # to a prior stereo local): emit two locals, register name
            # as a stereo let for downstream emission.
            if c.refsStereo(st.kids[0]):
              var sub = ""
              let v = c.emitStereo(inner, st.kids[0], sub)
              body.add sub
              body.add &"    double l_{st.str}_l = ({v.l});\n"
              body.add &"    double l_{st.str}_r = ({v.r});\n"
              c.stereoLets.incl st.str
            else:
              let v = c.emitExpr(inner, st.kids[0])
              let tmp = "l_" & st.str
              body.add &"    double {tmp} = ({v});\n"
              inner.names[st.str] = tmp
          of nkVar:
            discard              # top-level-var init already ran
          of nkAssign:
            let target =
              if st.str in c.topVarSet: "s->v_" & st.str
              elif inner.lookup(st.str).len > 0: inner.lookup(st.str)
              else: raise newException(ValueError,
                "unknown assignment target '" & st.str &
                "' (line " & $st.line & ")")
            let rhs = c.emitExpr(inner, st.kids[0])
            body.add &"    {target} = ({rhs});\n"
          else:
            # Non-final expression statements are ignored (would be a
            # no-op in aither too — last expr is the play's value).
            discard
      body.add "  }\n"
    else:
      if i == finalIdx:
        var pre = ""
        let v = c.emitStereo(topSc, s, pre)
        body.add pre
        body.add &"  *outL = ({v.l}); *outR = ({v.r});\n"
      # Non-final bare expressions: ignore (could warn).
  body.add "}\n"

  pre & c.arrayDecls & body

proc generate*(program: Node; patchPath: string = "";
               sr: float64 = 48000.0):
    tuple[csrc: string; varNames, partNames: seq[string];
          regions: seq[Region]] =
  let c = newCtx(program)
  c.patchPath = patchPath
  c.sr = sr
  var src = c.emit(program)
  # Finalise per-type region bases in first-seen order.
  var bases = initTable[string, int]()
  var total = 0
  for t in c.typeOrder:
    bases[t] = total
    total += c.typeCumSize[t]
  if total > PoolSize:
    raise newException(ValueError,
      "patch needs " & $total & " pool slots, pool is " & $PoolSize &
      " — reduce delays / reverbs or raise DspPoolSize")
  for r in c.regions.mitems:
    r.offset = bases[r.typeName] + r.offset     # intra-offset -> absolute
  # Substitute placeholder tokens with absolute slot offsets. Iterate
  # in reverse so "__OFF_10__" isn't accidentally matched by "__OFF_1__".
  for i in countdown(c.regions.len - 1, 0):
    src = src.replace("__OFF_" & $i & "__", $c.regions[i].offset)
  (src, c.topVarNames, c.playNames, c.regions)
