## Arrays as first-class values: let-bound array literals work in any
## scope (top-level, play, def body), and `arr[idx]` indexes them with
## any expression that evaluates to a number.
##
## Before this change, `let p = [...]` inside a def body errored
## ("array value can't appear here") so tabular partial-data tables
## (`bar_partials`, `prime_ratio`) had to be expressed as nested
## `if/else` cascades. This test pins each scope path.

import std/[math, strutils]
import ../parser, ../voice, ../codegen

proc evalAt(src: string): float64 =
  let v = newVoice(48000.0)
  v.load(parseProgram(src), 48000.0)
  v.tick(0.0).l

# --- 1. Top-level array, indexed by literal — pre-existing behaviour
# but pinned here so the new def/play paths can be compared against it.
block topLevelLiteralIdx:
  const P = """
let p = [10.0, 20.0, 30.0, 40.0, 50.0]
p[2]
"""
  doAssert abs(evalAt(P) - 30.0) < 1e-9, "top-level [2] should be 30"

# --- 2. Top-level array indexed by a runtime expression. The index is
# `int(t * 5) mod 5` evaluated at t=0 → 0 → arr[0]. (Use t directly so
# the index is plainly a runtime expression, not folded at codegen.)
block topLevelRuntimeIdx:
  const P = """
let p = [1.0, 2.0, 4.0, 8.0]
p[int(t * 5)]
"""
  doAssert abs(evalAt(P) - 1.0) < 1e-9,
    "runtime-indexed array at t=0 should be 1.0"

# --- 3. Array used in def body (the BUGS-issue-4b case). A def returning
# the nth element of a constant table — call it with each n and confirm.
block defBodyArray:
  const P = """
def lookup(n):
  let p = [2.0, 3.0, 5.0, 7.0, 11.0]
  p[n - 1]
lookup(NN)
"""
  for n in 1..5:
    let want = case n
      of 1: 2.0
      of 2: 3.0
      of 3: 5.0
      of 4: 7.0
      else: 11.0
    let prog = P.replace("NN", $n)
    doAssert abs(evalAt(prog) - want) < 1e-9,
      "def-body lookup(" & $n & ") = " & $evalAt(prog) & " want " & $want

# --- 4. Array used in play block (top-level array referenced from
# inside a play). The natural backing-track shape: let a chord-root
# table sit at top level and a play-block walk through it.
block playBlockUsesTopArray:
  const P = """
let roots = [110.0, 87.31, 130.81, 98.0]
play bass:
  roots[1]
bass
"""
  doAssert abs(evalAt(P) - 87.31) < 1e-9,
    "play block reading top-level array[1] should be 87.31"

# --- 5. Play-block let-bound array indexed by literal (already worked
# pre-fix; pin to confirm we haven't regressed).
block playLocalArray:
  const P = """
play t:
  let pat = [0.5, 0.7, 0.9]
  pat[2]
t
"""
  doAssert abs(evalAt(P) - 0.9) < 1e-9, "play-local array[2] = 0.9"

# --- 6. wave() still works after the change — arrays-as-values must
# not break the wave-with-literal-array-arg form.
block waveStillWorks:
  const P = """
let notes = [220.0, 330.0, 440.0]
wave(0.0, notes)
"""
  # phase(0)=0 → arr[0] = 220.
  doAssert abs(evalAt(P) - 220.0) < 1e-9,
    "wave(0, notes) at phase 0 should be notes[0] = 220.0"

# --- 7. The old workaround pattern from primes.aither (parens cascade)
# still works. Side-by-side with the new array form to confirm both
# valid surface syntaxes coexist.
block cascadeStillWorks:
  const P = """
def prime(n):
  if n == 1 then 2.0
  else (if n == 2 then 3.0
        else (if n == 3 then 5.0
              else 7.0))
prime(3)
"""
  doAssert abs(evalAt(P) - 5.0) < 1e-9, "old paren cascade still parses"

# --- 8. Region count: a let-bound array in a def body must NOT
# claim DSP pool slots — it's a static const, not stateful state.
# So a def using only the array shouldn't register any regions.
block noRegions:
  const P = """
def lookup(n):
  let p = [1.0, 2.0, 3.0]
  p[n]
lookup(0)
"""
  let (_, _, _, regions) = generate(parseProgram(P), "", 48000.0)
  doAssert regions.len == 0,
    "def-body array lookup must not allocate pool regions, got " & $regions.len

# --- 9. Used inside additive() — the natural use case. A custom ratio
# function with a tabular implementation, fed into inharmonic()-style
# sum() over partials. Verify the codegen unrolls the sum correctly.
block tabularRatioInSum:
  const P = """
def my_ratio(n):
  let r = [1.0, 2.0, 3.0, 4.0]
  r[n - 1]
sum(4, n => my_ratio(n))
"""
  # 1 + 2 + 3 + 4 = 10
  doAssert abs(evalAt(P) - 10.0) < 1e-9,
    "sum over def using array-lookup ratio fn = 10"

echo "array_values ok"
