## else if chaining regression test.
##
## BUGS_AND_ISSUES.md issue 3 claimed `else if` required nested parens.
## In fact `parsePrimary` handles `if` as a fresh primary, so after
## parseIfExpr consumes `else` and recurses into parseExpr, an `if`
## token starts a new conditional. This works at all depths and at
## both indented and one-line positions.
##
## This test pins the behaviour so a future parser refactor can't
## accidentally break what's been load-bearing for the docs all along.

import std/[strutils]
import ../parser, ../voice

proc evalAt(src: string): float64 =
  let v = newVoice(48000.0)
  v.load(parseProgram(src), 48000.0)
  v.tick(0.0).l

# --- 1. 5-arm chain inside a def body, indented form.
block fiveArm:
  const Tmpl = """
def f(n):
  if n == 1 then 2.0
  else if n == 2 then 3.0
  else if n == 3 then 5.0
  else if n == 4 then 7.0
  else 11.0
f(NN)
"""
  for arg in [1, 2, 3, 4, 5]:
    let want = case arg
      of 1: 2.0
      of 2: 3.0
      of 3: 5.0
      of 4: 7.0
      else: 11.0
    let got = evalAt(Tmpl.replace("NN", $arg))
    doAssert abs(got - want) < 1e-9,
      "5-arm chain at n=" & $arg & ": got " & $got & " want " & $want

# --- 2. Top-level chain (no def wrapping), one expression spanning lines.
block topLevel:
  const P = """
let x = 3.0
if x == 1 then 1.0
else if x == 2 then 2.0
else if x == 3 then 4.0
else 99.0
"""
  doAssert abs(evalAt(P) - 4.0) < 1e-9, "top-level 3-arm chain"

# --- 3. One-line chain (no internal newlines).
block oneLine:
  const P = "if 1 == 1 then 7.0 else if 1 == 2 then 0.0 else 9.0"
  doAssert abs(evalAt(P) - 7.0) < 1e-9, "one-line chain takes first branch"

# --- 4. Inside a play block — exercises a different scope path.
block insidePlay:
  const P = """
play t:
  let x = 2.0
  if x == 1 then 1.0
  else if x == 2 then 42.0
  else 99.0
t
"""
  doAssert abs(evalAt(P) - 42.0) < 1e-9, "play-body chain"

# --- 5. Six-arm chain confirms there's no recursion-depth quirk.
block sixArm:
  const P = """
def g(n):
  if n == 1 then 10.0
  else if n == 2 then 20.0
  else if n == 3 then 30.0
  else if n == 4 then 40.0
  else if n == 5 then 50.0
  else 60.0
g(5)
"""
  doAssert abs(evalAt(P) - 50.0) < 1e-9, "6-arm chain at fifth branch"

echo "elif_chain ok"
