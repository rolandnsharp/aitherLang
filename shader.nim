## aither → GLSL: a patch's picture, for tools/vis (the `vis` branch).
##
## A patch's sound is f(t), run at every sample; its picture is the sum
## of its `draw`s, f(pixel, t), run at every pixel of every frame on the
## GPU. One file: a draw in a play sees that play's lets, so the picture
## runs on the same envelopes, beats and notes as the sound. Only what
## the draws use is compiled; what that needs and a GPU can't run
## (the sound's state, filters) is an error naming it.
##
## A picture's memory is `$state` too, kept per pixel: in a def called
## from a draw, `$x` is a field, one value (a pair) a pixel, frame to
## frame. `$x` is this pixel's (last frame's, until it's set), `$x[q]`
## last frame's at point q. Each field is a GPU texture; the picture and
## up to MaxFields fields are written in one pass. See VIS.md.

import std/[strutils, strformat, tables, os, sequtils]
import parser

type
  Val = tuple[code: string, pair: bool]
  Bind = object
    code: string
    pair: bool
    table: int                          # > 0: a constant table of that length
    fn: string                          # a function passed by name (`osc(saw, f)`)
    state: bool                         # a `$name` of the sound's: sound only
    field: int                          # a picture's `$name`: its field index + 1
    lazy: int                           # a file's or play's let, compiled when first used: its lazies index + 1
    steady: bool                        # a def parameter given something that never changes
  Scope = Table[string, Bind]
  Lazy = object
    node: Node
    env: Scope                          # the names it could see where it was written
    done: bool
    val: Bind
  Ctx = object
    defs: Table[string, Node]
    globals: string                     # constant tables
    hoist: string                       # the lets the draws use, in the order they're needed
    n: int
    inRec: seq[string]                  # the rec defs being unrolled into loops
    lazies: seq[Lazy]
    fields: seq[tuple[init: string, pair: bool]]   # the picture's memory, a texture each

const MaxFields* = 3                    # a GPU writes 4 targets at once: the picture and 3

const RecLimit* = 1000                  # a shader has no watchdog: steps, then give up

proc fail(n: Node; msg: string) {.noreturn.} =
  raise newException(ValueError, msg & (if n != nil and n.line > 0: &" (line {n.line})" else: ""))

proc fresh(c: var Ctx; base: string): string =
  inc c.n
  &"{base}_{c.n}"

proc lit(x: float64): string =
  result = $x
  if '.' notin result and 'e' notin result and "inf" notin result and "nan" notin result:
    result.add ".0"

proc ty(pair: bool): string = (if pair: "vec2" else: "float")
proc asPair(v: Val): string = (if v.pair: v.code else: &"vec2({v.code})")   # a number fills both sides

# Builtins that are GLSL's own, one number in (a pair: each side).
const Unary = {"sin": "sin", "cos": "cos", "tan": "tan", "exp": "exp", "log": "log",
               "log2": "log2", "abs": "abs", "floor": "floor", "ceil": "ceil",
               "sqrt": "sqrt", "tanh": "tanh", "int": "trunc", "fract": "fract",
               "saw": "a_saw", "tri": "a_tri", "sqr": "a_sqr"}.toTable

proc emit(c: var Ctx; sc: Scope; n: Node; buf: var string): Val
proc emitBody(c: var Ctx; sc: var Scope; n: Node; buf: var string): Val

proc emitTable(c: var Ctx; name: string; n: Node): Bind
proc isTable(n: Node): bool

# A local array, its entries computed (`[f * 2, f * 3, f * 4]`): a GLSL
# array, indexed as a table is.
proc emitArray(c: var Ctx; sc: Scope; name: string; n: Node; buf: var string): Bind =
  var vals: seq[string]
  for k in n.kids:
    let v = c.emit(sc, k, buf)
    if v.pair: fail(k, "an array's entries are numbers (a pair is two entries)")
    vals.add v.code
  let g = c.fresh(name)
  buf.add &"  float {g}[{vals.len}] = float[]({vals.join(\", \")});\n"
  Bind(code: g, table: vals.len)

proc isArray(n: Node): bool = n.kind == nkArr and n.kids.len != 2

const Math = ["sin", "cos", "tan", "exp", "log", "log2", "abs", "floor", "ceil", "sqrt",
              "tanh", "int", "fract", "min", "max", "pow", "clamp"]

# Does n never change: numbers, constants, tables read at a steady
# index, and maths of those? A phasor needs a steady frequency: the
# sound adds it up sample by sample, the picture can only multiply it
# by the time, and the two agree only while it holds still.
proc steady(c: Ctx; sc: Scope; n: Node): bool =
  case n.kind
  of nkNum: true
  of nkIdent:
    if n.str in sc:
      let b = sc[n.str]
      if b.lazy > 0:
        let z = c.lazies[b.lazy - 1]
        return z.node.kids[0].isTable or c.steady(z.env, z.node.kids[0])
      return b.steady or b.table > 0
    n.str in ["PI", "TAU", "sr", "dt"]
  of nkBinOp, nkUnary, nkArr: n.kids.allIt(c.steady(sc, it))
  of nkIf: n.kids.allIt(it == nil or c.steady(sc, it))
  of nkIdx: c.steady(sc, n.kids[0]) and c.steady(sc, n.kids[1])
  of nkCall: n.str in Math and n.kids.allIt(c.steady(sc, it))
  else: false

# A name's binding, a let compiled the first time it's used (at the top
# of main, so it's there for every draw after).
proc look(c: var Ctx; sc: Scope; name: string; at: Node): Bind =
  result = sc[name]
  if result.state:
    fail(at, &"{name} is the sound's state: a picture keeps its own, in a def a draw calls")
  if result.lazy > 0:
    let k = result.lazy - 1
    if not c.lazies[k].done:
      let node = c.lazies[k].node
      if node.kids[0].isTable:
        c.lazies[k].val = c.emitTable(name, node.kids[0])
      elif node.kids[0].isArray:
        var h = ""
        let env = c.lazies[k].env
        c.lazies[k].val = c.emitArray(env, name, node.kids[0], h)
        c.hoist.add h
      else:
        var h = ""
        let env = c.lazies[k].env
        let v = c.emit(env, node.kids[0], h)
        let g = c.fresh(name)
        c.hoist.add h & &"  {ty(v.pair)} {g} = {v.code};\n"
        c.lazies[k].val = Bind(code: g, pair: v.pair)
      c.lazies[k].done = true
    result = c.lazies[k].val

# The numbers a pair operation takes, pairs flattened (as the C backend's
# pairOperands): cmul(p, q) and cmul(a, b, c, d) are the same call.
proc flat(c: var Ctx; sc: Scope; n: Node; buf: var string; want: int; usage: string): seq[string] =
  for k in n.kids:
    let v = c.emit(sc, k, buf)
    let tmp = c.fresh("op")
    buf.add &"  {ty(v.pair)} {tmp} = {v.code};\n"
    if v.pair: result.add [&"{tmp}.x", &"{tmp}.y"]
    else: result.add tmp
  if result.len != want:
    fail(n, &"{n.str} takes {usage}, got {result.len} numbers")

proc emitTable(c: var Ctx; name: string; n: Node): Bind =
  var vals: seq[string]
  for k in n.kids: vals.add lit(k.num)
  let g = c.fresh("tab_" & name)
  c.globals.add &"const float {g}[{vals.len}] = float[]({vals.join(\", \")});\n"
  Bind(code: g, table: vals.len)

proc isTable(n: Node): bool =
  if n.kind != nkArr or n.kids.len == 2: return false
  for k in n.kids:
    if k.kind != nkNum: return false
  true

# A user def at its call site: arguments by position, by name, or the
# default; each bound once to a temp; the body inlined (defs see only
# their parameters, as in audio). A `def rec` becomes a loop.
proc callDef(c: var Ctx; sc: Scope; n: Node; d: Node; buf: var string): Val =
  var given = newSeq[Node](d.params.len)
  var pos = 0
  for a in n.kids:
    if a.kind == nkNamedArg:
      let i = d.params.find(a.str)
      if i < 0: fail(n, &"{d.str} has no parameter '{a.str}' (it takes {d.params.join(\", \")})")
      if given[i] != nil: fail(n, &"'{a.str}' given twice")
      given[i] = a.kids[0]
    else:
      if pos >= d.params.len: fail(n, &"{d.str} takes {d.params.len} arguments")
      given[pos] = a
      inc pos
  var inner: Scope
  var args: seq[Val]
  for i, p in d.params:
    let a = if given[i] != nil: given[i] else: d.defaults[i]
    if a == nil: fail(n, &"{d.str}: missing '{p}'")
    # A table by name: the parameter indexes the caller's, as in sound.
    if a.kind == nkIdent and given[i] != nil and a.str in sc and c.look(sc, a.str, a).table > 0:
      inner[p] = c.look(sc, a.str, a)
      args.add (a.str, false)
      continue
    # A function by name (`osc(saw, f)`): the parameter calls it.
    if a.kind == nkIdent and (a.str notin sc or sc[a.str].fn != "") and
       (a.str in Unary or a.str in c.defs or (a.str in sc and sc[a.str].fn != "")):
      inner[p] = Bind(fn: (if a.str in sc: sc[a.str].fn else: a.str))
      args.add (a.str, false)
      continue
    # A default is written in the def, so it reads the def's other parameters.
    let v = if given[i] != nil: c.emit(sc, a, buf) else: c.emit(inner, a, buf)
    let tmp = c.fresh(p)
    buf.add &"  {ty(v.pair)} {tmp} = {v.code};\n"
    inner[p] = Bind(code: tmp, pair: v.pair,
                    steady: (if given[i] != nil: c.steady(sc, a) else: c.steady(inner, a)))
    args.add v
  if not d.isRec:
    return c.emitBody(inner, d.kids[0], buf)
  if d.str in c.inRec: fail(n, &"{d.str} calls itself where its result is still needed: a def rec repeats, it can't nest")
  # The loop: a repeat assigns the parameters anew and goes round; any
  # other end is the result. Results are typed once all are seen.
  c.inRec.add d.str
  let res = c.fresh("rec")
  var leaves: seq[(string, Val)]
  var loop = ""
  proc tail(c: var Ctx; sc: var Scope; b: Node; out1: var string) =
    if b.kind == nkIf:
      let cond = c.emit(sc, b.kids[0], out1)
      if cond.pair: fail(b, "an if takes a number, not a pair")
      out1.add &"  if ({cond.code} != 0.0) {{\n"
      var s1 = sc
      c.tail(s1, b.kids[1], out1)
      out1.add "  } else {\n"
      if b.kids[2] == nil: fail(b, "a def rec's if needs an else")
      var s2 = sc
      c.tail(s2, b.kids[2], out1)
      out1.add "  }\n"
    elif b.kind == nkBlock:
      for k in b.kids[0 ..< ^1]:
        discard c.emitBody(sc, Node(kind: nkBlock, kids: @[k]), out1)
      c.tail(sc, b.kids[^1], out1)
    elif b.kind == nkCall and b.str == d.str:
      if b.kids.len != d.params.len or b.kids.anyIt(it.kind == nkNamedArg):
        fail(b, &"{d.str} repeats with all {d.params.len} arguments, by position")
      var tmps: seq[string]
      for i, a in b.kids:
        let v = c.emit(sc, a, out1)
        let p = d.params[i]
        if v.pair and not sc[p].pair:
          fail(b, &"{d.str} repeats with a pair for '{p}', which started as a number")
        let tmp = c.fresh("next")
        out1.add &"  {ty(sc[p].pair)} {tmp} = {(if sc[p].pair: asPair(v) else: v.code)};\n"
        tmps.add tmp
      for i, p in d.params: out1.add &"  {sc[p].code} = {tmps[i]};\n"
      out1.add "  continue;\n"
    else:
      let v = c.emit(sc, b, out1)
      let mark = &"@@{res}_{leaves.len}@@"
      leaves.add (mark, v)
      out1.add &"  {res} = {mark};\n  break;\n"
  var s0 = inner
  c.tail(s0, d.kids[0], loop)
  discard c.inRec.pop()
  var pair = false
  for (_, v) in leaves: pair = pair or v.pair
  for (m, v) in leaves: loop = loop.replace(m, if pair: asPair(v) else: v.code)
  buf.add &"  {ty(pair)} {res} = {ty(pair)}(0.0);\n"
  buf.add &"  for (int it = 0; it < {RecLimit}; it++) {{\n{loop}  }}\n"
  (res, pair)

proc emitCall(c: var Ctx; sc: Scope; n: Node; buf: var string): Val =
  let name = n.str
  if name in sc and sc[name].fn != "":                 # a parameter holding a function
    return c.emitCall(sc, Node(kind: nkCall, str: sc[name].fn, kids: n.kids, line: n.line), buf)
  if name in c.defs and name notin ["pixel", "pixel_size", "input", "level", "low"]:
    return c.callDef(sc, n, c.defs[name], buf)
  for a in n.kids:
    if a.kind == nkNamedArg: fail(n, &"{name}: name its arguments only in a def's call, in a field patch (for now)")
  template arg(i: int): Val = c.emit(sc, n.kids[i], buf)
  proc need(k: int) =
    if n.kids.len != k: fail(n, name & " takes " & $k & (if k == 1: " argument" else: " arguments"))
  if name in Unary:
    need 1
    let v = arg(0)
    return (&"{Unary[name]}({v.code})", v.pair)
  case name
  of "min", "max", "pow":
    need 2
    let a = arg(0)
    let b = arg(1)
    let p = a.pair or b.pair
    return (&"{name}({(if p: asPair(a) else: a.code)}, {(if p: asPair(b) else: b.code)})", p)
  of "clamp":
    need 3
    let x = arg(0)
    return (&"clamp({x.code}, {arg(1).code}, {arg(2).code})", x.pair)
  of "phasor":                           # the clock: exact, from the voice's start, while f holds
    need 1
    if not c.steady(sc, n.kids[0]):
      fail(n, "a picture's phasor needs a frequency that never changes: the sound adds a " &
              "changing one up sample by sample, which a picture can't, so they'd drift apart " &
              "(for a steady clock whose rate you vary, write TAU * f * t, or let the sound's own beat in through a let)")
    return (&"fract(({arg(0).code}) * (uT - uStart))", false)
  of "noise":                            # each pixel, each frame, each call its own
    need 0
    inc c.n
    return (&"a_noise({c.n}.0)", false)
  of "magnitude":
    let a = c.flat(sc, n, buf, 2, "one pair")
    return (&"length(vec2({a[0]}, {a[1]}))", false)
  of "phase":
    let a = c.flat(sc, n, buf, 2, "one pair")
    return (&"atan({a[1]}, {a[0]})", false)
  of "cmul":
    let a = c.flat(sc, n, buf, 4, "two pairs")
    return (&"a_cmul(vec2({a[0]}, {a[1]}), vec2({a[2]}, {a[3]}))", true)
  of "cdiv":
    let a = c.flat(sc, n, buf, 4, "two pairs")
    return (&"a_cdiv(vec2({a[0]}, {a[1]}), vec2({a[2]}, {a[3]}))", true)
  of "cscale":
    let a = c.flat(sc, n, buf, 3, "a number and a pair")
    return (&"({a[0]}) * vec2({a[1]}, {a[2]})", true)
  of "rotate":
    let a = c.flat(sc, n, buf, 3, "a pair and an angle")
    return (&"a_cmul(vec2({a[0]}, {a[1]}), vec2(cos({a[2]}), sin({a[2]})))", true)
  of "conj":
    let a = c.flat(sc, n, buf, 2, "one pair")
    return (&"vec2({a[0]}, -({a[1]}))", true)
  # The field's own: where this pixel is, and the music at the sample heard.
  of "pixel":
    need 0
    return ("vPix", true)
  of "input":
    need 0
    return ("uInput", true)
  of "level":
    need 0
    return ("uLevel", false)
  of "low":                              # the loudness below ~150 Hz: the kick, the bass
    need 0
    return ("uLow", false)
  of "pixel_size":                       # one pixel's width, in pixel()'s units
    need 0
    return ("uPx", false)
  of "last":
    fail(n, "last() is gone: keep memory in a $field (a $name in a def a draw calls), and read it at a point with $name[p]")
  else:
    fail(n, &"'{name}' can't be drawn: it has memory (only the sound runs it) or isn't known to pictures yet")

proc emit(c: var Ctx; sc: Scope; n: Node; buf: var string): Val =
  case n.kind
  of nkNum: (lit(n.num), false)
  of nkIdent:
    if n.str in sc:
      let b = c.look(sc, n.str, n)
      if b.field > 0:
        let k = b.field - 1
        let code = if c.fields[k].pair: &"fv_{k}" else: &"fv_{k}.x"
        return (code, c.fields[k].pair)
      if b.table > 0: fail(n, &"{n.str} is a table: index it")
      if b.fn != "": fail(n, &"{n.str} is a function: call it")
      return (b.code, b.pair)
    case n.str
    of "t": ("uT", false)
    of "sr": ("uRate", false)
    of "dt": ("(1.0 / uRate)", false)
    of "start_t": ("uStart", false)              # the voice's, from the engine (0 without one)
    of "PI": ("3.14159265358979", false)
    of "TAU": ("6.28318530717959", false)
    else:
      if n.str.startsWith("$"): fail(n, &"{n.str} isn't declared here: a picture's $state lives in a def a draw calls")
      fail(n, &"unknown name '{n.str}'")
  of nkArr:
    if n.kids.len != 2: fail(n, "an array here is a table: bind it with let, then index it")
    let a = c.emit(sc, n.kids[0], buf)
    let b = c.emit(sc, n.kids[1], buf)
    if a.pair or b.pair: fail(n, "a pair holds two numbers")
    (&"vec2({a.code}, {b.code})", true)
  of nkIdx:
    let tgt = n.kids[0]
    if tgt.kind == nkIdent and tgt.str in sc and sc[tgt.str].field > 0:
      let k = sc[tgt.str].field - 1
      let q = c.emit(sc, n.kids[1], buf)
      if not q.pair:                    # $w[0]: an element of this pixel's own
        if not c.fields[k].pair: fail(n, &"{tgt.str} holds a number: index it with a point")
        return (&"(a_wrap({q.code}, 2) == 0 ? fv_{k}.x : fv_{k}.y)", false)
      let v = &"a_field(uF{k}, {q.code}, fi_{k})"
      let code = if c.fields[k].pair: v else: v & ".x"
      return (code, c.fields[k].pair)
    let i = c.emit(sc, n.kids[1], buf)
    if i.pair: fail(n, "an index is a number")
    if tgt.kind == nkIdent and tgt.str in sc and c.look(sc, tgt.str, tgt).table > 0:
      let b = c.look(sc, tgt.str, tgt)
      return (&"{b.code}[a_wrap({i.code}, {b.table})]", false)
    let p = c.emit(sc, tgt, buf)
    if not p.pair: fail(n, "only a pair or a table can be indexed")
    if n.kids[1].kind == nkNum and n.kids[1].num in [0.0, 1.0]:
      return (&"({p.code}).{(if n.kids[1].num == 0: 'x' else: 'y')}", false)
    (&"(a_wrap({i.code}, 2) == 0 ? ({p.code}).x : ({p.code}).y)", false)
  of nkUnary:
    let a = c.emit(sc, n.kids[0], buf)
    if n.str == "-": return (&"(-({a.code}))", a.pair)
    if a.pair: fail(n, "not takes a number")
    (&"(({a.code}) == 0.0 ? 1.0 : 0.0)", false)
  of nkBinOp:
    let a = c.emit(sc, n.kids[0], buf)
    let b = c.emit(sc, n.kids[1], buf)
    let p = a.pair or b.pair
    case n.str
    of "+", "-", "*": (&"(({a.code}) {n.str} ({b.code}))", p)
    of "/": (&"a_div({a.code}, {b.code})", p)
    of "mod", "%": (&"a_mod({(if p: asPair(a) else: a.code)}, {(if p: asPair(b) else: b.code)})", p)
    of "==", "!=", "<", ">", "<=", ">=":
      if p: fail(n, &"{n.str} compares numbers, not pairs")
      (&"((({a.code}) {n.str} ({b.code})) ? 1.0 : 0.0)", false)
    of "and", "or":
      if p: fail(n, &"{n.str} takes numbers")
      let op = if n.str == "and": "&&" else: "||"
      (&"((({a.code}) != 0.0 {op} ({b.code}) != 0.0) ? 1.0 : 0.0)", false)
    else: fail(n, &"operator {n.str}")
  of nkIf:
    let cond = c.emit(sc, n.kids[0], buf)
    if cond.pair: fail(n, "an if takes a number, not a pair")
    var b1, b2: string
    let x = c.emit(sc, n.kids[1], b1)
    let y = if n.kids[2] == nil: (code: "0.0", pair: false) else: c.emit(sc, n.kids[2], b2)
    let p = x.pair or y.pair
    let r = c.fresh("if")
    buf.add &"  {ty(p)} {r};\n  if ({cond.code} != 0.0) {{\n{b1}  {r} = {(if p: asPair(x) else: x.code)};\n" &
            &"  }} else {{\n{b2}  {r} = {(if p: asPair(y) else: y.code)};\n  }}\n"
    (r, p)
  of nkCall: c.emitCall(sc, n, buf)
  of nkBlock:
    var s = sc
    c.emitBody(s, n, buf)
  of nkLambda: fail(n, "functions as values aren't in field patches yet")
  of nkStr: fail(n, "a string is only an import path")
  else: fail(n, &"this can't be in an expression ({n.kind})")

# Lets, then the expression that is the body's value.
proc emitBody(c: var Ctx; sc: var Scope; n: Node; buf: var string): Val =
  let stmts = if n.kind == nkBlock: n.kids else: @[n]
  var last: Val = ("0.0", false)
  for s in stmts:
    case s.kind
    of nkLet:
      if s.kids[0].isTable:
        sc[s.str] = c.emitTable(s.str, s.kids[0])
      elif s.kids[0].isArray:
        sc[s.str] = c.emitArray(sc, s.str, s.kids[0], buf)
      else:
        let v = c.emit(sc, s.kids[0], buf)
        let name = c.fresh(s.str)
        buf.add &"  {ty(v.pair)} {name} = {v.code};\n"
        sc[s.str] = Bind(code: name, pair: v.pair)
    of nkDef: c.defs[s.str] = s
    of nkVar:
      # A field: this call site's, one value a pixel. It starts from its
      # declaration, which must be a number or a pair of numbers.
      var scratch = ""
      var empty: Scope
      let v = try: c.emit(empty, s.kids[0], scratch)
              except ValueError: fail(s, &"{s.str} starts from a number or a pair of numbers")
      if scratch.len > 0: fail(s, &"{s.str} starts from a number or a pair of numbers")
      if c.fields.len == MaxFields:
        fail(s, &"a picture keeps at most {MaxFields} fields (the GPU writes 4 targets at once: the picture and {MaxFields})")
      c.fields.add (v.code, v.pair)
      sc[s.str] = Bind(field: c.fields.len)
    of nkAssign:
      if s.str notin sc or sc[s.str].field == 0: fail(s, &"{s.str} isn't a field here")
      let k = sc[s.str].field - 1
      let v = c.emit(sc, s.kids[0], buf)
      if v.pair and not c.fields[k].pair: fail(s, &"{s.str} holds a number; this is a pair")
      buf.add &"  fv_{k} = {(if c.fields[k].pair: asPair(v) else: \"vec2(\" & v.code & \", 0.0)\")};\n"
    of nkIdxAssign: fail(s, "a field is set at its own pixel ($x = ...); $x[p] only reads")
    of nkPlay: fail(s, "play is for sound; a field patch is one picture")
    of nkImport: fail(s, "imports aren't in field patches yet")
    else: last = c.emit(sc, s, buf)
  last

const Prelude = """#version 300 es
precision highp float;
in vec2 uv;
uniform float uT, uRate, uLevel, uLow, uFrame, uFresh, uPx, uStart;
uniform vec2 uInput, uSpan;
uniform sampler2D uF0, uF1, uF2;
layout(location = 0) out vec4 o;
float a_div(float a, float b) { return b == 0.0 ? 0.0 : a / b; }
vec2 a_div(vec2 a, float b) { return b == 0.0 ? vec2(0.0) : a / b; }
vec2 a_div(float a, vec2 b) { return vec2(a_div(a, b.x), a_div(a, b.y)); }
vec2 a_div(vec2 a, vec2 b) { return vec2(a_div(a.x, b.x), a_div(a.y, b.y)); }
float a_mod(float a, float b) { return b == 0.0 ? 0.0 : a - floor(a / b) * b; }
vec2 a_mod(vec2 a, vec2 b) { return vec2(a_mod(a.x, b.x), a_mod(a.y, b.y)); }
int a_wrap(float i, int n) { int k = int(i) % n; return k < 0 ? k + n : k; }
float a_saw(float x) { return 2.0 * fract(x / 6.28318530717959) - 1.0; }
vec2 a_saw(vec2 x) { return 2.0 * fract(x / 6.28318530717959) - 1.0; }
float a_tri(float x) { return 1.0 - 4.0 * abs(fract(x / 6.28318530717959 - 0.25) - 0.5); }
vec2 a_tri(vec2 x) { return 1.0 - 4.0 * abs(fract(x / 6.28318530717959 - 0.25) - 0.5); }
float a_sqr(float x) { return fract(x / 6.28318530717959) < 0.5 ? 1.0 : -1.0; }
vec2 a_sqr(vec2 x) { return vec2(a_sqr(x.x), a_sqr(x.y)); }
vec2 a_cmul(vec2 a, vec2 b) { return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }
vec2 a_cdiv(vec2 a, vec2 b) { float d = dot(b, b); return d == 0.0 ? vec2(0.0) : vec2(dot(a, b), a.y * b.x - a.x * b.y) / d; }
float a_noise(float k) {
  uvec3 v = uvec3(uvec2(gl_FragCoord.xy), uint(uFrame) * 64u + uint(k));
  v = v * 1664525u + 1013904223u; v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  v ^= v >> 16u; v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  return float(v.x) / 2147483648.0 - 1.0;
}
vec2 a_field(sampler2D f, vec2 p, vec2 init) { return uFresh > 0.5 ? init : texture(f, p / uSpan * 0.5 + 0.5).rg; }
"""

const Stdlib = staticRead("stdlib.aither")

# A file's or a play's statements: lets wait to be used, defs are
# known, `$state` is marked sound-only, plays are walked in their own
# scope, and each draw is compiled and added to the picture.
proc walk(c: var Ctx; sc: var Scope; stmts: seq[Node]; draws: var seq[Val]; buf: var string) =
  for s in stmts:
    case s.kind
    of nkLet:
      c.lazies.add Lazy(node: s, env: sc)
      sc[s.str] = Bind(lazy: c.lazies.len)
    of nkDef: c.defs[s.str] = s
    of nkVar: sc[s.str] = Bind(state: true)
    of nkDraw: draws.add c.emit(sc, s.kids[0], buf)
    of nkPlay:
      var inner = sc
      let body = s.kids[0]
      c.walk(inner, (if body.kind == nkBlock: body.kids else: @[body]), draws, buf)
    of nkImport: fail(s, "imports aren't in pictures yet")
    else: discard                       # the sound, and stores to state

proc toGlsl*(source: string): string =
  ## The fragment shader for a patch's picture: the sum of its draws at
  ## each pixel, a pair (a number fills both sides, as a mono sample does).
  var c: Ctx
  for s in parseProgram(Stdlib).kids:
    if s.kind == nkDef: c.defs[s.str] = s
  let prog = parseProgram(source, keepDraws = true)
  for s in prog.kids:
    if s.kind == nkDef: c.defs[s.str] = s          # a def can be used above it
  var sc: Scope
  var draws: seq[Val]
  var body = ""
  c.walk(sc, prog.kids, draws, body)
  if draws.len == 0:
    raise newException(ValueError, "nothing to draw: a picture is the sum of the patch's `draw`s")
  let sum = draws.mapIt(asPair(it)).join(" + ")
  # Each field: its start, its value here (last frame's until set), and
  # where it's written.
  var outs, starts, writes = ""
  for k, f in c.fields:
    let init = if f.pair: f.init else: &"vec2({f.init}, 0.0)"
    outs.add &"layout(location = {k + 1}) out vec4 oF{k};\n"
    starts.add &"  vec2 fi_{k} = {init};\n  vec2 fv_{k} = a_field(uF{k}, vPix, fi_{k});\n"
    writes.add &"  oF{k} = vec4(fv_{k}, 0.0, 1.0);\n"
  Prelude.replace("#version 300 es\n", &"#version 300 es\n// fields: {c.fields.len}\n") & outs & c.globals &
    "void main() {\n  vec2 vPix = (uv * 2.0 - 1.0) * uSpan;\n" & starts & c.hoist & body &
    &"  o = vec4({sum}, 0.0, 1.0);\n" & writes & "}\n"

when isMainModule:
  try: stdout.write toGlsl(readFile(paramStr(1)))
  except CatchableError as e:
    stderr.writeLine e.msg
    quit 1
