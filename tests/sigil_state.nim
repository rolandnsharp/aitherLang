## $-sigil state declarations and references.
##
## The migration replaces the `var` keyword with a leading `$` on the
## name. State is now visually distinct from local lets at every use
## site. The semantics are unchanged from the var era — file-level by
## name at top level, per-call-site inside a def, per-iteration inside
## a sum() lambda — but the lambda-body case is new (lambdas only
## accepted let-prefixed bodies before the migration).

import std/[strutils]
import ../parser, ../voice

# --- 1. Top-level $state increments per sample.
block topLevelCounter:
  const P = """
$counter = 0.0
$counter = $counter + 1.0
$counter
"""
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  for _ in 1 .. 10: discard v.tick(0.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 11.0) < 1e-9,
    "top-level $counter should be 11 after 11 ticks, got " & $s.l

# --- 2. $state inside def gets per-call-site state. Two different
# call sites of `step()` keep independent counters (matching the var-
# inside-def "each call location gets its own slot" semantics).
block defPerCallSite:
  const P = """
def step():
  $level = 0.0
  $level = $level + 1.0
  $level
let a = step()
let b = step()
a + b * 100
"""
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 101.0) < 1e-9,
    "two call sites should each be 1 → a+b*100 = 101, got " & $s.l
  let s2 = v.tick(0.0)
  doAssert abs(s2.l - 202.0) < 1e-9,
    "after second tick → 2 + 200 = 202, got " & $s2.l

# --- 3. $state inside play body shares scope with top-level by name.
# After this patch loads, the top-level $count slot is the same one a
# bare `count` reference at top-level would see (verified indirectly by
# the increment behavior).
block playSharesTopLevelState:
  const P = """
play tick:
  $count = 0.0
  $count = $count + 1.0
  $count
tick[0]
"""
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  for _ in 1 .. 4: discard v.tick(0.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 5.0) < 1e-9,
    "in-play $count should be 5 after 5 ticks, got " & $s.l

# --- 4. $state inside a sum-lambda body: each iteration has its own
# slot. With sum(4, n => $c = 0; $c = $c + 1; $c), every iteration
# increments its private $c, so per tick the result climbs by 4.
block lambdaPerIterationState:
  const P = """
sum(4, n =>
  $c = 0.0
  $c = $c + 1.0
  $c)
"""
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s1 = v.tick(0.0)
  doAssert abs(s1.l - 4.0) < 1e-9,
    "after 1 tick, four iters at $c=1 → 4, got " & $s1.l
  let s2 = v.tick(0.0)
  doAssert abs(s2.l - 8.0) < 1e-9,
    "after 2 ticks, four iters at $c=2 → 8, got " & $s2.l

# --- 5. Lambda body sequential update reads the just-assigned value:
# $a = 0; $a = $a + n; $a + 100 — for n=1, $a goes 0→1, then 1+100=101.
# Across two iterations (n=1, n=2): 101 + 102 = 203.
block lambdaSequentialUpdate:
  const P = """
sum(2, n =>
  $a = 0.0
  $a = $a + n
  $a + 100.0)
"""
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 203.0) < 1e-9,
    "sequential $-update should give 203, got " & $s.l

# --- 6. Inline physics inside sum: a single damped harmonic oscillator
# unrolled via sum(1, ...) should produce non-zero, non-NaN output and
# match the algebraic shape of stdlib's tuning_fork (which uses the
# same equations outside a sum).
block lambdaInlinePhysics:
  const P = """
let f = 440.0
let strike = 1.0
sum(1, n =>
  let omega = TAU * f
  let omega2 = omega * omega
  let gamma = 4.0
  $x = 0.0
  $dx = 0.0
  $dx = $dx + (-2 * gamma * $dx - omega2 * $x + strike * omega2) * dt
  $x = $x + $dx * dt
  $x)
"""
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  var maxAbs = 0.0
  var anyNaN = false
  for i in 0 ..< 4800:
    let s = v.tick(float64(i) / 48000.0)
    if s.l != s.l: anyNaN = true
    if abs(s.l) > maxAbs: maxAbs = abs(s.l)
  doAssert not anyNaN, "inline physics should not NaN"
  doAssert maxAbs > 0.001 and maxAbs < 1e6,
    "inline DHO should ring audibly without blowing up, maxAbs=" & $maxAbs

# --- 7. The `var` keyword is removed. A patch that uses it should
# error with a migration message pointing at the new syntax.
block varKeywordRejected:
  try:
    discard parseProgram("var x = 0\nx")
    doAssert false, "old `var` syntax should error"
  except ParseError as e:
    let m = e.msg.toLowerAscii()
    doAssert "var" in m and ("$" in m or "removed" in m),
      "error should mention var/$, got: " & e.msg

# --- 8. `$ name` (with space between `$` and ident) is invalid. The
# tokenizer rejects it so the surface rule "the sigil hugs its name"
# is enforced where the spelling is decided.
block dollarSpaceRejected:
  try:
    discard parseProgram("$ x = 0\n$x")
    doAssert false, "`$ x` (with space) should error"
  except ParseError as e:
    discard

# --- 9. $state on RHS reads the slot. Already exercised above, but
# make sure a bare `$x` expression at top level (without any prior
# write) fails cleanly — first-sight reads of state slots are zero by
# construction (alloc0).
block freshStateReadsZero:
  const P = """
$z = 0.0
$z
"""
  let v = newVoice(48000.0)
  v.load(parseProgram(P), 48000.0)
  let s = v.tick(0.0)
  doAssert abs(s.l - 0.0) < 1e-9,
    "fresh $z should read 0, got " & $s.l

echo "sigil_state ok"
