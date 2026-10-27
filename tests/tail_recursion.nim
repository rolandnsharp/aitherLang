## Tail recursion (notes/tail-recursion-2026-10-16.md): a def marked
## `def rec` that calls itself as the whole result of a branch repeats
## as a loop.
##
##   - numbers, pairs, defaults and functions as parameters
##   - state belongs to the code: one `$k` counts every step
##   - only the branch taken runs, so state in the base case advances
##     once a sample however many steps came before it
##   - a parameter no repeat changes keeps its compile-time value
##   - each call site is its own loop: inside `sum`, inside another
##     recursion
##   - `rec` on a def that doesn't call itself changes nothing
##   - compile errors: a self-call without `rec`, a self-call inside an
##     expression, a cycle through another def, a changed function
##   - a recursion past its block's deadline gives up: silent samples,
##     `stuck` set, and its name for the log

import std/strutils
import ../ccompile, ../voice

proc render(src: string; n = 4): seq[tuple[l, r: float64]] =
  let v = newVoice(48000.0)
  v.load((src), 48000.0)
  for i in 0 ..< n:
    result.add v.tick(float64(i) / 48000.0)

proc errorOf(src: string): string =
  try:
    discard compile(src, "patches/x.aither")
    doAssert false, "expected a compile error"
  except CatchableError as e:
    return e.msg

block gcd:
  let o = render("""
def rec gcd(a, b): if b == 0 then a else gcd(b, a mod b)
gcd(1071, 462)
""")
  doAssert o[0].l == 21.0, "got " & $o[0].l

# The accumulators are defaults, so the call reads fib(30).
block fib:
  let o = render("""
def rec fib(n, a = 0, b = 1): if n == 0 then a else fib(n - 1, b, a + b)
fib(30)
""")
  doAssert o[0].l == 832040.0, "got " & $o[0].l

# z -> z^2 + c until it leaves: c = 1 + i leaves after 2 steps, 0.5
# after 5, and -1 cycles 0, -1, 0 ... until n runs out.
block escape:
  let o = render("""
def rec escape(z, c, n):
  if n == 0 or magnitude(z) > 2 then n else escape(cmul(z, z) + c, c, n - 1)
[escape([0, 0], [1, 1], 64), escape([0, 0], [0.5, 0], 64) + escape([0, 0], [-1, 0], 64)]
""")
  doAssert o[0].l == 62.0, "got " & $o[0].l
  doAssert o[0].r == 59.0, "got " & $o[0].r

# A pair result, from a pair parameter: z = 0, i, -1 + i.
block orbit:
  let o = render("""
def rec orbit(z, c, n): if n == 0 then z else orbit(cmul(z, z) + c, c, n - 1)
orbit([0, 0], [0, 1], 2)
""")
  doAssert o[0].l == -1.0 and o[0].r == 1.0, "got " & $o[0]

# One `$k` for the code, not one per step: four steps a sample.
block state:
  let o = render("""
def rec count(n):
  $k = 0
  $k = $k + 1
  if n == 0 then $k else count(n - 1)
count(3)
""")
  for i in 0 ..< 4:
    doAssert o[i].l == float64(4 * (i + 1)), "sample " & $i & ": " & $o[i].l

# The base case's phasor runs once a sample, after five repeats.
block lazy:
  let o = render("""
def rec pick(n): if n == 0 then phasor(1000) else pick(n - 1)
[pick(5), phasor(1000)]
""", 64)
  for i, s in o:
    doAssert s.l == s.r, "sample " & $i & ": " & $s

# A function passed on unchanged.
block function:
  let o = render("""
def double(x): x * 2
def rec apply(f, x, n): if n == 0 then x else apply(f, f(x), n - 1)
apply(double, 1, 10)
""")
  doAssert o[0].l == 1024.0, "got " & $o[0].l

# n is passed on unchanged, so sum(n, ...) still folds at compile time.
block constant:
  let o = render("""
def rec tri(n, k, acc = 0): if k == 0 then acc else tri(n, k - 1, acc + sum(n, i => i))
tri(4, 3)
""")
  doAssert o[0].l == 30.0, "got " & $o[0].l

# Each call site is its own loop: three inside sum, one inside another
# recursion. gcd(6, 4) + gcd(12, 4) + gcd(18, 4) = 8.
block sites:
  let o = render("""
def rec gcd(a, b): if b == 0 then a else gcd(b, a mod b)
def rec outer(n, acc): if n == 0 then acc else outer(n - 1, acc + gcd(n * 6, 4))
[sum(3, i => gcd(i * 6, 4)), outer(3, 0)]
""")
  doAssert o[0].l == 8.0 and o[0].r == 8.0, "got " & $o[0]

# A long recursion runs to the end when nothing sets a deadline.
block long:
  let o = render("""
def rec down(n): if n == 0 then 7 else down(n - 1)
down(1000000)
""", 1)
  doAssert o[0].l == 7.0, "got " & $o[0].l

# `rec` on a def that never calls itself: an ordinary def.
block plain:
  let o = render("""
def rec twice(x): x * 2
twice(21)
""")
  doAssert o[0].l == 42.0, "got " & $o[0].l

# A def ending in a pair parameter, or a let bound to a pair, returns a
# pair, recursive or not.
block pairs:
  let o = render("""
def norm(z): z / magnitude(z)
def twice(z):
  let w = cmul(z, [2, 0])
  w
norm([3, 4]) + twice([1, 1])
""")
  doAssert abs(o[0].l - 2.6) < 1e-12 and abs(o[0].r - 2.8) < 1e-12, "got " & $o[0]

block errors:
  let unmarked = errorOf("""
def countdown(n): if n == 0 then 0 else countdown(n - 1)
countdown(3)
""")
  doAssert "countdown calls itself (patches/x.aither:1)" in unmarked, unmarked
  doAssert "def rec countdown" in unmarked, unmarked
  let inside = errorOf("""
def rec fib(n): if n < 2 then n else fib(n - 1) + fib(n - 2)
fib(10)
""")
  doAssert "calls itself inside an expression" in inside, inside
  doAssert "(patches/x.aither:1)" in inside, inside
  let cycle = errorOf("""
def ping(n): if n == 0 then 0 else pong(n - 1)
def pong(n): if n == 0 then 1 else ping(n - 1)
ping(5)
""")
  doAssert "ping -> pong -> ping" in cycle, cycle
  let changed = errorOf("""
def double(x): x * 2
def inc(x): x + 1
def rec walk(f, x, n): if n == 0 then x else walk(inc, f(x), n - 1)
walk(double, 1, 3)
""")
  doAssert "passes a different array or function as 'f'" in changed, changed

# Past the deadline the recursion gives up: the block is silent, every
# sample counted bad, `stuck` set and the def named for the log.
block guard:
  let v = newVoice(48000.0)
  v.load("""
def rec forever(x): forever(x + 1)
forever(0)
""", 48000.0, "patches/stuck.aither")
  let hdr = cast[ptr VoiceHeader](v.state)
  hdr.fadeGain = 1.0
  hdr.inBuf = nil
  hdr.deadline = 1e-9            # long gone
  var l = newSeq[float64](16)
  var r = newSeq[float64](16)
  v.tickBlock(16, l[0].addr, r[0].addr)
  doAssert hdr.stuck == 1, "stuck " & $hdr.stuck
  doAssert hdr.bad == 16, "bad " & $hdr.bad
  for i in 0 ..< 16:
    doAssert l[i] == 0.0 and r[i] == 0.0, "sample " & $i & ": " & $l[i]
  doAssert $hdr.stuckAt == "forever (patches/stuck.aither:1)", $hdr.stuckAt

echo "tail_recursion ok"
