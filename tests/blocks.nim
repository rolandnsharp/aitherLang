## Blocks end at their expression. A def or play body is its bindings
## (`let`, `$state`, element stores, `each`), ended by the expression
## that is its value, so indentation means nothing. BUGS_AND_ISSUES.md
## issue 7. The whole patch corpus parses to identical trees.

import std/[strutils]
import ../ccompile, ../voice

proc evalAt(src: string): float64 =
  let v = newVoice(48000.0)
  v.load((src), 48000.0)
  v.tick(0.0).l

proc near(got, want: float64; what: string) =
  doAssert abs(got - want) < 1e-9, what & ": got " & $got & " want " & $want

proc errOf(src: string): string =
  try:
    parse(src)
    ""
  except ParseError as e: e.msg

# --- No indentation anywhere: the expression closes each block.
block flat:
  const src = """
def half(x):
let h = x * 0.5
h
play a:
half(1.0)
play b:
$n = 0.25
$n
a + b
"""
  near(evalAt(src), 0.75, "unindented def and plays")

# --- One-line bodies, and indentation that means nothing.
near(evalAt("def half(x): x * 0.5\nhalf(3.0)"), 1.5, "one-line def")
near(evalAt("def half(x):\n\t\tx * 0.5\n      half(3.0)"), 1.5, "stray tabs and spaces")

# --- `each` is a statement: the block reads on past it.
block eachMidBody:
  const src = """
def total(x):
let a = array(3)
each(3, i => a[i] = x * (i + 1))
a[0] + a[1] + a[2]
total(0.1)
"""
  near(evalAt(src), 0.6, "each mid-body")

# --- A body with no expression runs into the next def or the end.
doAssert "def f has no expression" in errOf("def f(x):\nlet y = x\ndef g(x): x\ng(1)"),
  errOf("def f(x):\nlet y = x\ndef g(x): x\ng(1)")
doAssert "play p has no expression" in errOf("play p:\nlet y = 1\n"),
  errOf("play p:\nlet y = 1\n")

# --- A body with a second expression spills out, and the top level
# has room for one.
let e = errOf("def f(x):\nx\nx * 2\nf(1)")
doAssert "second top-level expression at line 4 (the first is at line 3)" in e, e

echo "blocks ok"
