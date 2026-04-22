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

type
  Scope = ref object
    parent: Scope
    # name -> C expression text (for params, inlined let bindings)
    # and name -> "pool[<n>]" for inlined vars
    names: Table[string, string]
    # name -> aither function name (for fn-valued params like `shape`)
    fns: Table[string, string]

  Ctx = ref object
    buf: string           # accumulating output
    userDefs: Table[string, Node]
    topVarNames*: seq[string]   # top-level vars in definition order
    topVarSet: HashSet[string]
    topLets: HashSet[string]    # top-level let names (scoped to tick())
    # name -> (C array symbol, length). Populated when a top-level let
    # binds an nkArr literal of numeric constants.
    topArrays: Table[string, (string, int)]
    arrayDecls: string          # accumulates `static const double arr_N[...]`
    tmpCounter: int

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
               "abs", "floor", "ceil", "sqrt"]

# ---- small helpers -----------------------------------------------------

proc newCtx*(program: Node): Ctx =
  result = Ctx(userDefs: initTable[string, Node](),
               topVarSet: initHashSet[string](),
               topLets: initHashSet[string](),
               topArrays: initTable[string, (string, int)]())
  # Gather user defs so call sites can inline them.
  for n in program.kids:
    if n.kind == nkDef:
      result.userDefs[n.str] = n

proc fresh(c: Ctx; prefix: string): string =
  inc c.tmpCounter
  prefix & "_" & $c.tmpCounter

proc push(parent: Scope): Scope =
  Scope(parent: parent,
        names: initTable[string, string](),
        fns: initTable[string, string]())

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
      # var inside def body -> claim one pool slot, init with given value
      # on first run via a pool-slot guard. Simpler approach: just claim,
      # and the init value is written once via an "inited" companion? That
      # needs extra state. Simplest here: claim a slot, write the init
      # expression on every sample *only if* we've never run — but tracking
      # that per-call-site needs a separate "inited" bit. For now, seed
      # the slot to the init value on the first sample by using a second
      # pool slot as an inited flag.
      let flagSlot = c.fresh("vf")
      let valSlot = c.fresh("vs")
      pre.add &"long {flagSlot} = s->idx++; long {valSlot} = s->idx++; "
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

# phasor(freq) inlined: claim one pool slot, advance, return wrapped value.
proc emitPhasor(c: Ctx; sc: Scope; freqNode: Node): string =
  let freq = c.emitExpr(sc, freqNode)
  let slot = c.fresh("ph")
  "(({ long " & slot & " = s->idx++; " &
    "s->pool[" & slot & "] = fmod(s->pool[" & slot & "] + (" & freq &
    ") / s->sr, 1.0); " &
    "if (s->pool[" & slot & "] < 0.0) s->pool[" & slot & "] += 1.0; " &
    "s->pool[" & slot & "]; }))"

# noise() inlined: xorshift32 on a pool slot interpreted as uint32.
proc emitNoise(c: Ctx): string =
  let slot = c.fresh("nz")
  "(({ long " & slot & " = s->idx++; " &
    "unsigned int r = (unsigned int)s->pool[" & slot & "]; " &
    "if (r == 0) r = 2463534242u; " &
    "r ^= r << 13; r ^= r >> 17; r ^= r << 5; " &
    "s->pool[" & slot & "] = (double)r; " &
    "((double)r / 4294967295.0) * 2.0 - 1.0; }))"

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
      return &"n_wave((DspState*)s, {freq}, (double*){sym}, {length})"
    # Native dsp calls
    if name in NativeArities:
      let arity = NativeArities[name]
      if arity == -1:
        raise newException(ValueError,
          name & " (array arg) not yet supported in codegen")
      if n.kids.len != arity:
        raise newException(ValueError,
          name & " takes " & $arity & " args, got " & $n.kids.len)
      return "n_" & name & "((DspState*)s, " & c.callArgs(sc, n.kids) & ")"
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
    # arr[idx] where arr is a compile-time array (`let notes = [...]`
    # or an inline literal passed through). For stereo-pair access on
    # inline 2-element array literals, unpack directly.
    let a = n.kids[0]
    let i = n.kids[1]
    if a.kind == nkIdent and a.str in c.topArrays:
      let (sym, length) = c.topArrays[a.str]
      let ix = c.emitExpr(sc, i)
      return &"({sym}[(int)({ix}) % {length}])"
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

# ---- top-level emission ------------------------------------------------

proc emit*(c: Ctx; program: Node): string =
  # First pass: collect top-level vars and lets so forward refs resolve.
  for s in program.kids:
    case s.kind
    of nkVar:
      if s.str notin c.topVarSet:
        c.topVarSet.incl s.str
        c.topVarNames.add s.str
    of nkLet:
      c.topLets.incl s.str
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
      raise newException(ValueError, "play blocks not yet supported")
    else:
      if i == finalIdx:
        # Final expression — check for stereo [L, R] literal.
        if s.kind == nkArr and s.kids.len == 2:
          let l = c.emitExpr(topSc, s.kids[0])
          let r = c.emitExpr(topSc, s.kids[1])
          body.add &"  *outL = ({l}); *outR = ({r});\n"
        else:
          let v = c.emitExpr(topSc, s)
          body.add &"  double __y = ({v}); *outL = __y; *outR = __y;\n"
      # Non-final bare expressions: ignore (could warn).
  body.add "}\n"

  pre & c.arrayDecls & body

proc generate*(program: Node): tuple[csrc: string, varNames: seq[string]] =
  let c = newCtx(program)
  let src = c.emit(program)
  (src, c.topVarNames)
