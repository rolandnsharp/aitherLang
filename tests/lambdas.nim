## Phase 1: lambdas (`n => expr`) + `sum(N, fn)` primitive + literal
## propagation through def params.
##
## Why these are grouped: the cohesive shippable unit is
## lambdas + sum + literal-propagation. Without literal-propagation,
## the stdlib's intended pattern `def additive(freq, shape, max_n):
##   sum(max_n, n => ...)` breaks because max_n is a def param, not a
## literal. Splitting the feature across phases would ship a sum that
## only works for raw inline use cases, then backtrack — so they land
## together.
##
## Scope covered:
##   - `n => expr` parses as nkLambda.
##   - sum(N, fn) unrolls at codegen. N must be a compile-time integer:
##     either nkNum literal, or an ident that resolves via numLits
##     (populated for def params bound to nkNum args, and for top-level
##     lets binding nkNum RHS).
##   - Lambda bodies capture enclosing scope via the existing scope walk
##     — no plumbing changes needed beyond what `emitDefInline` already
##     does for def params.
##   - Each unrolled iteration is a fresh codegen emission site, so
##     `phasor(...)` inside a lambda gets N distinct state regions.
##   - Lambdas are not first-class: they only appear as a sum() arg.

import std/[math, strutils]
import ../parser, ../voice, ../codegen

# --- 1. sum with literal N: constant lambda body.
block literalN:
  const P = "sum(5, n => 1.0)"
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 5.0) < 1e-9,
    "sum(5, n => 1.0) should be 5.0, got " & $s.l

# --- 2. Lambda param bound correctly: Σ n for n=1..10 = 55.
block triangleNumber:
  const P = "sum(10, n => n)"
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 55.0) < 1e-9,
    "sum(10, n => n) should be 55.0, got " & $s.l

# --- 3. Closure capture from enclosing top-level let.
block captureLet:
  const P = """
let f = 7.0
sum(4, n => f * n)
"""
  # f * (1+2+3+4) = 7 * 10 = 70
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 70.0) < 1e-9,
    "closure-capture of let should give 70.0, got " & $s.l

# --- 4. Closure capture from def parameter.
block captureDefParam:
  const P = """
def scaled_sum(base):
  sum(3, n => base * n)
scaled_sum(10.0)
"""
  # base=10: 10*1 + 10*2 + 10*3 = 60
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 60.0) < 1e-9,
    "closure-capture of def param should give 60.0, got " & $s.l

# --- 5. Literal propagation through def param (the critical test).
# amount is a def param; calling fixed_count(5) must propagate 5 as a
# numLit so sum(amount, ...) sees amount as compile-time 5.
block literalPropagation:
  const P = """
def fixed_count(amount):
  sum(amount, n => 1.0)
fixed_count(5)
"""
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 5.0) < 1e-9,
    "literal propagation through def param should give 5.0, got " & $s.l

# --- 6. Literal propagation from top-level `let H = 8`.
block literalPropagationFromLet:
  const P = """
let H = 8
sum(H, n => 1.0)
"""
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 8.0) < 1e-9,
    "literal propagation from let should give 8.0, got " & $s.l

# --- 7. Stateful lambda: each iteration owns a distinct phasor region.
# Σ sin(TAU * phasor(440n)) for n=1..3 — three harmonics of 440 Hz.
block statefulLambda:
  const P = "sum(3, n => sin(TAU * phasor(440.0 * n)))"
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  var maxAbs = 0.0
  for i in 0..<2000:
    let s = v.tick(float64(i) / 48000.0)
    doAssert s.l == s.l, "no NaN at sample " & $i
    if abs(s.l) > maxAbs: maxAbs = abs(s.l)
  doAssert maxAbs > 0.5, "polyphonic sum should be audible, maxAbs=" & $maxAbs
  doAssert maxAbs < 3.5, "should not exceed 3 harmonics peak"

# --- 8. Per-call-site state keying: sum(4, ...) with phasor should register
# 4 distinct phasor regions so each harmonic has its own phase state.
block regionCount:
  const P = "sum(4, n => sin(TAU * phasor(100.0 * n)))"
  let (_, _, _, regions) = generate(parseProgram(P), "", 48000.0)
  var phasorCount = 0
  for r in regions:
    if r.typeName == "phasor": inc phasorCount
  doAssert phasorCount == 4,
    "sum(4, ...) with phasor inside lambda should create 4 phasor regions, got " &
    $phasorCount

# --- 9. Nested sum: outer lambda's param visible in inner lambda via scope.
# n=1: m=1,2 -> 1+2=3;  n=2: 2+4=6;  n=3: 3+6=9;  total = 18.
block nestedSum:
  const P = "sum(3, n => sum(2, m => n * m))"
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 18.0) < 1e-9,
    "nested sum should give 18.0, got " & $s.l

# --- 10. Lambda used outside a builtin arg: must error clearly.
block lambdaMisuse:
  const P = "let f = n => n * 2\nf"
  try:
    let v = newVoice(48000.0)
    v.load(parseProgram(P), 48000.0)
    doAssert false, "lambda stored in let should error"
  except CatchableError as e:
    doAssert "lambda" in e.msg.toLowerAscii(),
      "error should mention lambda, got: " & e.msg

# --- 11. sum with non-literal N: error clearly. State (`$counter`) is
# pool-backed at runtime, not a compile-time constant — so sum() must
# refuse it the same way it refuses a let bound to a non-literal RHS.
block nonLiteralN:
  const P = """
$counter = 3
sum($counter, n => n)
"""
  try:
    let v = newVoice(48000.0)
    v.load(parseProgram(P), 48000.0)
    doAssert false, "sum with runtime-valued N should error"
  except CatchableError as e:
    let m = e.msg.toLowerAscii()
    doAssert "literal" in m or "constant" in m or "compile-time" in m,
      "error should mention literal/constant, got: " & e.msg

# --- 12. sum with non-lambda second arg: error clearly.
block nonLambdaFn:
  const P = """
def ident_fn(n): n
sum(5, ident_fn)
"""
  try:
    let v = newVoice(48000.0)
    v.load(parseProgram(P), 48000.0)
    doAssert false, "sum with non-lambda second arg should error"
  except CatchableError as e:
    doAssert "lambda" in e.msg.toLowerAscii(),
      "error should mention lambda, got: " & e.msg

# --- 13. sum with wrong number of args: error clearly.
block sumArity:
  const P = "sum(3)"
  try:
    let v = newVoice(48000.0)
    v.load(parseProgram(P), 48000.0)
    doAssert false, "sum with 1 arg should error"
  except CatchableError as e:
    doAssert "sum" in e.msg.toLowerAscii(),
      "error should mention sum, got: " & e.msg

# --- 14. Multi-arg lambda parses as nkLambda with N params.
# `(a, b) => a + b` is valid syntax; tryParseParenLambda picks it up
# at the open paren, restoring on misses so plain grouping `(expr)`
# still works.
block multiArgParse:
  let ast = parseProgram("def f(g): g(3, 4)\nf((a, b) => a + b)")
  # Find the call to f, walk to the lambda arg.
  var foundLam = false
  proc walk(n: Node) =
    if n == nil: return
    if n.kind == nkLambda and n.params.len == 2 and
       n.params[0] == "a" and n.params[1] == "b":
      foundLam = true
    for k in n.kids: walk(k)
  walk(ast)
  doAssert foundLam, "expected nkLambda with params [a, b] in AST"

# --- 15. Multi-arg lambda inlined through a def-call: 2-arg lambda
# bound to `g`, body of f calls g(3, 4), result = 3 + 4 = 7.
block multiArgInline:
  const P = "def f(g): g(3, 4)\nf((a, b) => a + b)"
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 7.0) < 1e-9,
    "f((a,b) => a+b) where f(g) = g(3,4) should give 7.0, got " & $s.l

# --- 16. Per-param binding with different values in each position. Pin
# correctness when the args are non-symmetric (a appears once, b twice).
block multiArgAsymmetric:
  const P = "def f(g): g(3, 5)\nf((a, b) => a + b * 2)"
  # 3 + 5 * 2 = 13
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 13.0) < 1e-9,
    "asymmetric multi-arg lambda should give 13.0, got " & $s.l

# --- 17. Multi-arg lambda with let-prefixed body — same body grammar
# as single-arg lambdas, just with a different param header.
block multiArgLetBody:
  const P = """
def f(g): g(2, 3)
f((a, b) =>
  let s = a + b
  s * s)
"""
  # (2 + 3)^2 = 25
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 25.0) < 1e-9,
    "multi-arg lambda with let body should give 25.0, got " & $s.l

# --- 18. Plain grouping `(expr)` still parses correctly — the lookahead
# in tryParseParenLambda must restore on miss so this isn't mis-routed.
block parenGroupingPreserved:
  const P = "(2 + 3) * 4"
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 20.0) < 1e-9,
    "(2 + 3) * 4 should give 20.0, got " & $s.l

# --- 19. Single-arg paren lambda `(n) => ...` works the same as `n => ...`.
# Lets users write either form when they want explicit parens.
block parenSingleArg:
  const P = "def f(g): g(7)\nf((n) => n * 3)"
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 21.0) < 1e-9,
    "(n) => n*3 invoked as g(7) should give 21.0, got " & $s.l

# --- 20. sum(N, multi-arg-lambda) errors clearly. sum's iteration
# protocol is one-arg (the iteration index), so a 2-arg lambda is
# meaningless there.
block sumRejectsMultiArg:
  const P = "sum(4, (a, b) => a + b)"
  try:
    let v = newVoice(48000.0)
    v.load(parseProgram(P), 48000.0)
    doAssert false, "sum with multi-arg lambda should error"
  except CatchableError as e:
    doAssert "one parameter" in e.msg or "lambda" in e.msg,
      "error should explain the arity, got: " & e.msg

# --- 21. Lambda call arity mismatch surfaces a clear error.
block lambdaArityMismatch:
  const P = "def f(g): g(1)\nf((a, b) => a + b)"
  try:
    let v = newVoice(48000.0)
    v.load(parseProgram(P), 48000.0)
    doAssert false, "calling 2-arg lambda with 1 arg should error"
  except CatchableError as e:
    doAssert "arg" in e.msg or "lambda" in e.msg,
      "error should mention args/lambda, got: " & e.msg

# --- 22. Lambda passed to def, which forwards it to another def.
# Pins the def-of-def lambda-binding case (the foundation of how
# midi_keyboard delegates to poly).
block lambdaForwarding:
  const P = """
def inner(h): h(10, 20)
def outer(h): inner(h)
outer((a, b) => a + b)
"""
  # 10 + 20 = 30
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 30.0) < 1e-9,
    "lambda forwarded through two defs should give 30.0, got " & $s.l

echo "lambdas ok"
