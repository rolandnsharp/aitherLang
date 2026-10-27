## Def parameters: arrays bound by name, defaults, named arguments.
##
##   - an array argument (table, local or state array, or a literal)
##     is aliased into the def; len(param) folds at the call site, so
##     one def serves two lengths at two sites and sizes its own state
##     by its argument
##   - a def writes into the caller's state array (by binding)
##   - `def f(a, k = 2)` fills k when the call leaves it out; a default
##     is an expression but may not name a variable
##   - `f(1, k = 3)` names an argument; positional first, then named

import std/strutils
import ../ccompile, ../voice

proc render(src: string; n = 480): seq[float64] =
  let prog = (src)
  let v = newVoice(48000.0)
  v.load(prog, 48000.0)
  for i in 0 ..< n:
    let s = v.tick(float64(i) / 48000.0)
    doAssert s.l == s.l, "NaN at sample " & $i
    result.add s.l

proc errorOf(src: string): string =
  try:
    discard compile((src), "patches/x.aither")
    doAssert false, "expected a compile error"
  except CatchableError as e:
    return e.msg

# 1. A table argument, two lengths at two call sites. sumTable adds the
# entries, so the two sites must differ by the extra entries.
block tables:
  let o = render("""
def total(t):
  sum(len(t), i => t[i - 1])
let a = [1.0, 2.0, 3.0]
let b = [10.0, 20.0, 30.0, 40.0, 50.0]
play p:
  total(a) * 0.01 + total(b) * 0.001
p
""", 8)
  # 6 * 0.01 + 150 * 0.001 = 0.21
  doAssert abs(o[0] - 0.21) < 1e-9, "got " & $o[0]

# 2. A literal array at the call, and a local (runtime) array.
block literals:
  let o = render("""
def total(t):
  sum(len(t), i => t[i - 1])
play p:
  let x = 0.5
  let r = [x, x * 2, x * 3]
  total([1.0, 2.0, 4.0]) * 0.01 + total(r) * 0.01
p
""", 8)
  # 7 * 0.01 + 3 * 0.01
  doAssert abs(o[0] - 0.10) < 1e-9, "got " & $o[0]

# 3. A def sizes its own state by its argument and writes into the
# caller's state array: a one-section "tube" that just delays.
block stateArrays:
  let o = render("""
def shift(f, x):
  let n = len(f)
  each(n - 1, j => f[n - 1 - j] = f[n - 2 - j])    # each counts from 0
  f[0] = x
  f[n - 1]
play p:
  $line = array(4)
  shift($line, t)
p
""", 10)
  # after 4 samples the output is the input from 3 samples ago
  doAssert abs(o[9] - 6.0 / 48000.0) < 1e-12, "got " & $o[9]

# 4. Defaults, positional and named.
block defaults:
  let o = render("""
def knob(a, k = 2, m = TAU / TAU + 1):
  a * k * m
play p:
  knob(1) * 0.01 + knob(1, 3) * 0.001 + knob(1, m = 5) * 0.0001 + knob(1, 7, m = 1) * 0.00001
p
""", 4)
  # 4*0.01 + 6*0.001 + 10*0.0001 + 7*0.00001 = 0.04707
  doAssert abs(o[0] - 0.04707) < 1e-9, "got " & $o[0]

# 5. The errors say what went wrong.
block errors:
  doAssert "missing 'k'" in errorOf("def f(a, k): a * k\nplay p: f(1)\np\n")
  doAssert "no parameter 'z'" in errorOf("def f(a, k = 1): a * k\nplay p: f(1, z = 2)\np\n")
  doAssert "before named" in errorOf("def f(a, k = 1): a * k\nplay p: f(k = 2, 1)\np\n")
  doAssert "can't use the name" in errorOf("let g = 2\ndef f(a, k = g): a * k\nplay p: f(1)\np\n")
  doAssert "by position" in errorOf("play p: sin(x = 1)\np\n")

echo "def_params ok"
