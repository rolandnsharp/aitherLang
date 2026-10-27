## noise(): every call is its own stream. Each call site keeps its
## xorshift32 state in its own pool slot; the slots used to start from
## one fixed seed, so every noise() in a patch played the same sequence
## (a singer's aspiration, frication and pitch wander moved together).
## Now each slot's seed comes from its slot, so the streams are
## independent and still the same on every run (render stays repeatable).

import std/math
import ../ccompile, ../voice


proc loadWith(src: string): NativeVoice =
  let program = src
  result = newVoice(48000.0)
  result.load(program, 48000.0)

const N = 48000

proc corr(src: string): float64 =
  let v = loadWith(src)
  var sl, sr, sll, srr, slr = 0.0
  for i in 0..<N:
    let s = v.tick(float64(i) / 48000.0)
    sl += s.l; sr += s.r; sll += s.l * s.l; srr += s.r * s.r; slr += s.l * s.r
  let n = float64(N)
  (slr / n - sl / n * sr / n) /
    sqrt((sll / n - (sl / n) ^ 2) * (srr / n - (sr / n) ^ 2))

# --- 1. Two noise() calls side by side are uncorrelated.
block twoCalls:
  let c = corr("[noise(), noise()]")
  doAssert abs(c) < 0.02, "two noise() calls correlate: " & $c

# --- 2. The same def called twice gets two streams.
block defTwice:
  let c = corr("def hiss(g):\n  noise() * g\n[hiss(1), hiss(1)]")
  doAssert abs(c) < 0.02, "a def's noise is shared between calls: " & $c

# --- 3. Calls in different lets and plays, too.
block letsAndPlays:
  let c = corr("let a = noise()\nlet b = lp1(noise(), 48000)\n[a, b]")
  doAssert abs(c) < 0.02, "noise in two lets correlates: " & $c

# --- 4. Deterministic: the same patch gives the same samples every run.
block repeatable:
  const P = "[noise(), noise() * 0.5]"
  let a = loadWith(P)
  let b = loadWith(P)
  for i in 0..<1000:
    let x = a.tick(float64(i) / 48000.0)
    let y = b.tick(float64(i) / 48000.0)
    doAssert x.l == y.l and x.r == y.r, "noise differs between runs at " & $i

# --- 5. Still white noise in [-1, 1] with mean ~0 and variance ~1/3.
block stats:
  let v = loadWith("noise()")
  var s, ss = 0.0
  var lo, hi = 0.0
  for i in 0..<N:
    let x = v.tick(float64(i) / 48000.0).l
    s += x; ss += x * x; lo = min(lo, x); hi = max(hi, x)
  doAssert lo >= -1.0 and hi <= 1.0
  doAssert abs(s / N) < 0.01, "mean " & $(s / N)
  doAssert abs(ss / N - 1.0 / 3.0) < 0.01, "variance " & $(ss / N)

echo "noise_independent: OK"
