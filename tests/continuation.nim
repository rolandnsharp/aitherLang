## Line continuation: a newline ends a statement unless the statement
## can't end there (the line ended on an operator or `=`) or the next
## line can't begin one (it starts with an infix operator, `then` or
## `else`). Indentation of a continued line means nothing.
##
## BUGS_AND_ISSUES.md issue 4. Every program this accepts was a parse
## error before; the whole patch corpus parses to identical trees.

import ../ccompile, ../voice

proc evalAt(src: string): float64 =
  let v = newVoice(48000.0)
  v.load((src), 48000.0)
  v.tick(0.0).l

proc near(got, want: float64; what: string) =
  doAssert abs(got - want) < 1e-9, what & ": got " & $got & " want " & $want

# --- A line ending on an operator continues.
near(evalAt("let a = 0.25 +\n  0.5\na"), 0.75, "trailing +")
near(evalAt("let a = 0.25 -\n0.5\na"), -0.25, "trailing -, unindented")
near(evalAt("let a =\n  0.75\na"), 0.75, "trailing =")

# --- A line starting with an infix operator continues the one above,
# however either is indented.
block pipes:
  const src = """
def half(x):
  x * 0.5
1.5
|> half
    |> half
"""
  near(evalAt(src), 0.375, "leading |>")
near(evalAt("let a = 3.0\n  * 0.25\na"), 0.75, "leading *")
near(evalAt("let a = 1.0\n  == 1.0\na"), 1.0, "leading ==")
near(evalAt("let a = 0.25\n  + 0.5\na"), 0.75, "leading + (no unary +)")

# --- `-`, `not`, `(` and `[` can begin a statement, so a newline
# before them still ends one.
near(evalAt("let a = 0.5\n-a"), -0.5, "leading - is a new statement")
near(evalAt("let a = 0.5\n(a * 2)"), 1.0, "leading ( is not a call")

# --- `or`, `and` and `mod` count only as whole words.
near(evalAt("let order = 0.25\norder"), 0.25, "`order` is not `or`")

# --- A continuation inside a def body leaves the block intact.
block inDef:
  const src = """
def f(x):
  let y = x +
      1.0
  y * 2
f(0.5)
"""
  near(evalAt(src), 3.0, "continuation in a def body")

echo "continuation ok"
