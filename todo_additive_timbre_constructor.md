# TODO — `sum` primitive + lambdas + additive synthesis

A handoff prompt for a fresh Claude Code session. Adds the fundamental
`sum(N, fn)` primitive to aither, with lambda/closure support to make
it expressive. Makes additive synthesis (and any "do N times and
combine" pattern) a first-class composable operation. Replaces the
historical need for special-cased `osc(saw, freq)` aliasing fixes with
something more fundamental, more general, and more aither-philosophical.

This is a multi-phase, multi-commit job — ~600-800 lines of work
across language, codegen, stdlib, tests, and docs. Estimate: 1-2
focused days.

---

## The principle

Aither has the wrong oscillator primitives. Saw and square are
*synthetic shortcuts* (analog-circuit historical accidents) that produce
mathematically infinite harmonics, causing aliasing artifacts in digital
audio. Sine is the only true oscillator primitive — every physical
resonant system vibrates as a sum of sines (Fourier theorem).

The fix is not to bolt PolyBLEP onto the existing oscillators. The fix
is to make **sum-of-sines** the natural way to construct timbres in
aither. That requires one primitive: a way to evaluate a function over
a range and sum the results.

```
sum(N, fn)   →   Σ fn(n) for n = 1..N
```

This is the truly fundamental operation. Additive synthesis is one
application; multi-tap delays, voice stacking, granular grain summation,
parallel filter banks are all the same pattern. ONE primitive, many uses.

To make it expressive, the function `fn` needs to be a lambda that can
capture variables from its enclosing scope. So the work bundles:

1. **Lambda support** — `n => expression` syntax with closure capture
2. **`sum(N, fn)` primitive** — compile-time unrolled fold-with-addition
3. **Stdlib defs** — `additive`, `inharmonic`, `partials`, plus a shape/
   ratio library
4. **Tests + docs**

---

## Design

### Surface area

ONE new primitive in the engine:

```
sum(N, fn)
```

Where `N` is a compile-time-constant integer (max iterations) and `fn`
is a lambda taking a single integer arg `n` and returning a sample value.
Returns: `fn(1) + fn(2) + ... + fn(N)`.

Lambdas use the syntax `n => expression`. They can capture any variable
in scope at the lambda's definition point.

### Examples — math-as-code

```aither
# Saw at 440 Hz with 16 harmonics, 1/n falloff. Reads literally as
# the math notation: Σ sin(2π·n·440·t) / n for n=1..16
play saw_lead:
  sum(16, n => sin(TAU * phasor(n * 440)) / n) * 0.3

# Triangle wave (odd harmonics, 1/n² falloff with sign alternation):
# Σ (-1)^((n-1)/2) sin(2π·n·f·t) / n²    for odd n only
play tri_lead:
  let f = midi_freq()
  sum(16, n =>
    if n mod 2 == 0 then 0
    else sin(TAU * phasor(n * f)) * (if (n-1)/2 mod 2 == 0 then 1 else -1) / (n*n)
  ) * 0.3

# Stiff piano string (slight inharmonicity from string stiffness):
# Σ (1/n) sin(2π · n·sqrt(1+n²·B) · f · t)
play stiff_piano:
  let f = midi_freq()
  let env = adsr(midi_gate(), 0.001, 0.4, 0.0, 0.4)
  sum(20, n =>
    let pf = f * n * sqrt(1.0 + n*n*0.0003)
    sin(TAU * phasor(pf)) / pow(n, 0.5)
  ) * env * 0.3

# Bell with inharmonic bar partials (1, 2.756, 5.404, 8.933, 13.345):
play bell:
  let f = 440
  let env = discharge(midi_trig(40), 0.5)
  sum(5, n =>
    let ratio = if n == 1 then 1.0
                else if n == 2 then 2.756
                else if n == 3 then 5.404
                else if n == 4 then 8.933
                else 13.345
    let pf = f * ratio
    sin(TAU * phasor(pf)) / pow(n, 0.5)
  ) * env * 0.3

# Multi-tap delay — same primitive, different application:
play multi_delay:
  sum(8, n => prev_signal |> delay(n * 0.0625, 0.5))
```

The lambda is whatever the user wants. Each iteration `fn(n)` is a
separate call site, so stateful operations (phasor, delay, var) inside
the lambda each get their own state region via aither's existing
per-helper-type indexing.

### Auto-band-limiting via the lambda

The shape function (lambda) decides whether to contribute. For
band-limiting, the user writes:

```aither
sum(32, n =>
  let pf = n * f
  if pf >= 24000 then 0
  else sin(TAU * phasor(pf)) / n)
```

The `if` returns 0 above Nyquist. No special handling in the primitive
required. **The aither way: the user's math is the band-limiter.**

For ergonomics, stdlib provides a wrapper:

```aither
def above_nyquist(pf):  pf >= 24000

# User writes:
sum(32, n => if above_nyquist(n*f) then 0 else sin(TAU * phasor(n*f))/n)
```

### `max_n` is required and visible

`N` is the third-thing-from-most-important arg (after the lambda body).
Aither's pitch is "nothing hidden" — the spectral budget is in the
patch. Users typically write `let H = 32` at the top of a patch and
pass `H` everywhere. Per-voice override is just writing a literal.

---

## Stdlib defs (built ON sum, not in the engine)

The stdlib provides convenience defs on top of `sum`. None of these are
engine primitives — they're all aither code that any user could write.

```aither
# === FUNDAMENTAL WRAPPERS ===

# Harmonic synthesis — partials at integer multiples of freq.
# shape is a lambda taking (n, partial_freq) returning amplitude.
def additive(freq, shape, max_n):
  sum(max_n, n =>
    let pf = n * freq
    if pf >= 24000 then 0
    else shape(n, pf) * sin(TAU * phasor(pf)))

# Inharmonic synthesis — partials at user-specified frequency multipliers.
# ratio is (n => freq_multiplier), amp is (n, partial_freq) => amplitude.
def inharmonic(freq, ratio, amp, max_n):
  sum(max_n, n =>
    let pf = freq * ratio(n)
    if pf >= 24000 then 0
    else amp(n, pf) * sin(TAU * phasor(pf)))

# === HARMONIC SHAPE FUNCTIONS (use with additive) ===
def saw_shape(n, pf):       1.0 / n
def sqr_shape(n, pf):       if n mod 2 == 1 then 1.0 / n else 0.0
def tri_shape(n, pf):       if n mod 2 == 1 then 1.0 / (n * n) else 0.0
def warm_shape(n, pf):      1.0 / (n * n)
def bright_shape(n, pf):    1.0 / pow(n, 0.5)
def bowed_shape(n, pf):     1.0 / (n + 0.5)

# === FORMANT SHAPES (use freq-positioned peaks) ===
def vowel_ah(n, pf):
  let basis = 1.0 / n
  let f1 = exp(-pow((pf - 700) / 150, 2)) * 4.0
  let f2 = exp(-pow((pf - 1200) / 150, 2)) * 3.0
  basis * (1.0 + f1 + f2)

def vowel_ee(n, pf):
  let basis = 1.0 / n
  let f1 = exp(-pow((pf - 270) / 100, 2)) * 4.0
  let f2 = exp(-pow((pf - 2300) / 200, 2)) * 3.5
  basis * (1.0 + f1 + f2)

def cello_shape(n, pf):
  let basis = 1.0 / n
  let A0     = exp(-pow((pf - 200) / 80, 2)) * 2.0
  let T1     = exp(-pow((pf - 400) / 100, 2)) * 1.5
  let bright = exp(-pow((pf - 1500) / 300, 2)) * 1.0
  let taper  = if n > 12 then exp(-(n - 12.0) * 0.3) else 1.0
  basis * (1.0 + A0 + T1 + bright) * taper

# === INHARMONIC RATIO FUNCTIONS (use with inharmonic) ===
def stiff_string(n):  n * sqrt(1.0 + n * n * 0.0003)
def stiff_cello(n):   n * sqrt(1.0 + n * n * 0.00008)
def bar_partials(n):
  if n == 1 then 1.0
  else if n == 2 then 2.756
  else if n == 3 then 5.404
  else if n == 4 then 8.933
  else 13.345
def plate_partials(n):
  if n == 1 then 1.0
  else if n == 2 then 2.295
  else if n == 3 then 3.873
  else if n == 4 then 5.612
  else 7.682
def phi_partials(n):  pow(1.618, n - 1)

# === COMMON AMP FUNCTIONS ===
def soft_decay(n, pf):    1.0 / pow(n, 0.5)
def bell_decay(n, pf):    1.0 / n
def bright_decay(n, pf):  1.0 / pow(n, 0.3)
```

Users use whichever level of abstraction fits:

```aither
# Raw — math is right there
play raw_saw:
  sum(16, n => sin(TAU * phasor(n * midi_freq())) / n) * 0.3

# Wrapper for common case
play wrapped_saw:
  additive(midi_freq(), saw_shape, 16) * 0.3

# Inharmonic
play stiff_piano:
  inharmonic(midi_freq(), stiff_string, soft_decay, 20) * env * 0.3

# Cello with inharmonicity
play cello:
  inharmonic(midi_freq(), stiff_cello, cello_shape, 24) * env * 0.3
```

---

## Phased implementation plan

Each phase is one commit. Land them in order. Don't bundle.

### Phase 1 — Lambda syntax + closure capture (~250 lines)

**Files**: `parser.nim`, `codegen.nim`, `tests/lambdas.nim`

**Goals**:
- Parse `n => expression` as a lambda value
- Lambda captures all variables in lexical scope at definition
- Lambda can only appear as an argument to a builtin (no first-class
  storage in `let`, no being returned from defs — keep it minimal for v1)
- Codegen substitutes lambda body inline at each call site

**Specifics**:
- Tokenizer: recognize `=>` as a new token (tkArrow)
- Parser: in the expression parser, allow `IDENT => expr` as a lambda
  literal. Bind the IDENT as the parameter.
- AST: new `nkLambda` node with `params: seq[string]` and `body: Node`
- Codegen: when emitting a builtin that expects a lambda arg (currently
  only `sum`), inline the lambda body with the parameter substituted.
  Captures are handled because the lambda body is just emitted in the
  caller's scope — variables from the enclosing scope are already
  visible by codegen's existing scoping rules.

**Test (`tests/lambdas.nim`)**:
```nim
## Lambdas substitute correctly and capture enclosing variables.
import ../parser, ../codegen

const Patch = """
let f = 440.0
sum(4, n => f * n)
"""
let ast = parseProgram(Patch)
discard generate(ast, "test.aither")  # should compile clean

const Capture = """
def make_sum(base):
  sum(3, n => base * n)
make_sum(10)
"""
discard generate(parseProgram(Capture), "test.aither")
echo "lambdas ok"
```

**Acceptance**: `tests/lambdas.nim` passes. All 21+ existing tests
still pass (no regression in non-lambda code paths).

### Phase 2 — `sum(N, fn)` primitive (~80 lines)

**Files**: `codegen.nim`, `tests/sum.nim`

**Goals**:
- Recognize `sum(N, lambda)` as a special form
- Require `N` to be a numeric literal (compile-time constant)
- Require 2nd arg to be a lambda (nkLambda from Phase 1)
- Unroll: emit `lambda_body[n=1] + lambda_body[n=2] + ... + lambda_body[n=N]`
- Each iteration is a separate codegen call site (so stateful primitives
  inside the lambda — `phasor`, `delay`, `var` — get their own state
  regions via aither's existing per-helper-type indexing)

**Implementation**:
- Add `"sum": -1` to `NativeArities` (special-cased)
- In `emitExpr`, handle `sum`:
  - Check 2 args
  - Check 1st is `nkNum` literal
  - Check 2nd is `nkLambda` with one param
  - For n in 1..max_n: emit lambda body with the param substituted
    by the literal n (use codegen's existing scope-binding to install
    `param_name → literal_value`)
  - Emit `(expr_1 + expr_2 + ... + expr_N)` as the result

**Test (`tests/sum.nim`)**:
```nim
## sum(N, fn) returns the sum of fn(1) + fn(2) + ... + fn(N).
import ../parser, ../codegen, ../voice

const ConstSum = """
sum(5, n => 1.0)
"""
let v = newVoice(48000.0)
v.load(parseProgram(ConstSum), 48000.0)
let s = v.tick(0.0)
doAssert abs(s.l - 5.0) < 1e-9, "sum of 5 ones should be 5"

const TriangleNumber = """
sum(10, n => n)
"""
v.load(parseProgram(TriangleNumber), 48000.0)
let s2 = v.tick(0.0)
doAssert abs(s2.l - 55.0) < 1e-9, "sum 1..10 = 55"

# Stateful inside lambda — each iteration has its own phasor state
const Polyphonic = """
sum(3, n => sin(TAU * phasor(440.0 * n)))
"""
v.load(parseProgram(Polyphonic), 48000.0)
# Just verify no NaN, audible content
var maxAbs = 0.0
for i in 0..1000:
  let s = v.tick(float64(i) / 48000.0)
  doAssert s.l == s.l, "no NaN"
  if abs(s.l) > maxAbs: maxAbs = abs(s.l)
doAssert maxAbs > 0.5, "polyphonic sum should be audible"

echo "sum ok"
```

**Acceptance**: `tests/sum.nim` passes. All previous tests pass.

### Phase 3 — Stdlib defs (~30 lines of stdlib.aither)

**Files**: `stdlib.aither`, `tests/additive_inharmonic.nim`

**Goals**:
- Add `additive`, `inharmonic` as stdlib defs built on `sum`
- Add the harmonic shape functions (saw_shape, sqr_shape, tri_shape,
  warm_shape, bright_shape, bowed_shape)
- Add the formant shapes (vowel_ah, vowel_ee, cello_shape)
- Add the inharmonic ratio functions (stiff_string, stiff_cello,
  bar_partials, plate_partials, phi_partials)
- Add the common amp functions (soft_decay, bell_decay, bright_decay)

**Test (`tests/additive_inharmonic.nim`)**:
```nim
## Verify additive and inharmonic stdlib defs produce expected output.
import std/math
import ../parser, ../voice

const SawAdditive = """
def saw_shape(n, pf):  1.0 / n
additive(440, saw_shape, 8) * 0.5
"""
let v = newVoice(48000.0)
v.load(parseProgram(SawAdditive), 48000.0)
var anyNonzero = false
for i in 0..1000:
  let s = v.tick(float64(i) / 48000.0)
  doAssert s.l == s.l, "no NaN"
  if abs(s.l) > 0.01: anyNonzero = true
doAssert anyNonzero, "additive saw should be audible"

const Bell = """
def bell_ratios(n):
  if n == 1 then 1.0 else if n == 2 then 2.756 else 5.404
def bell_amp(n, pf):  1.0 / pow(n, 0.5)
inharmonic(440, bell_ratios, bell_amp, 3) * 0.5
"""
v.load(parseProgram(Bell), 48000.0)
anyNonzero = false
for i in 0..1000:
  let s = v.tick(float64(i) / 48000.0)
  doAssert s.l == s.l, "no NaN"
  if abs(s.l) > 0.01: anyNonzero = true
doAssert anyNonzero, "bell should be audible"

echo "additive_inharmonic ok"
```

**Acceptance**: `tests/additive_inharmonic.nim` passes. Existing
patches in `patches/` still load (they don't use the new primitives,
just confirm no regression).

### Phase 4 — Documentation (~50 lines of doc updates)

**Files**: `SPEC.md`, `GUIDE.md`, `README.md`, `COMPOSING.md`

**SPEC.md**:
- Add `sum(N, fn)` to the primitives section. Document that fn must
  be a lambda and N must be a compile-time constant.
- Add a "Lambdas" subsection documenting `n => expr` syntax and that
  lambdas can only appear as builtin arguments.
- Document that `additive`, `inharmonic` are stdlib defs (not engine
  primitives) built on `sum`.

**GUIDE.md**:
- New "Designing timbres with sum" section showing:
  - Math-to-code correspondence (sigma notation → sum lambda)
  - Saw, square, triangle as direct sums of sines
  - Formant synthesis (vowel example)
  - Inharmonic example (bell, piano)
  - Cello example as a concrete demonstration
- Note that `osc(saw, freq)` still exists for chiptune/lofi but is
  no longer the recommended way to make clean leads — use `additive`
  or `sum`.

**README.md**:
- Update primitives list to mention `sum`
- Add one example of additive synthesis

**COMPOSING.md**:
- Add a "spectrum design" idiom showing how to think about timbres
  as sums of sines

**Acceptance**: Docs are coherent, examples compile and play.

### Phase 5 — Live test (no commit; just verification)

Send the cello patch from this doc to a running engine. Play notes via
the Minilab 3. Verify it sounds like a cello (warmer, more body
resonance, more natural attack than a subtractive saw+filter).

Send the bell patch. Hit pad 5 (note 40). Verify a bell-like inharmonic
ring with the canonical bar partials.

If both sound clearly identifiable as their target instruments and
playable in real time, the implementation is complete.

---

## What NOT to do

- **Don't add PolyBLEP or any anti-aliasing to existing oscillators.**
  The whole point of this design is that aliasing isn't a thing to fix
  — it's a symptom of having the wrong primitives. With `sum` and
  additive synthesis available, users who want clean audio use those
  instead of `osc(saw, freq)`.
- **Don't make lambdas first-class.** v1 lambdas only appear as args
  to builtins. No `let f = n => ...; f(3)`. Adding general first-class
  functions is a much bigger language change. Keep v1 minimal.
- **Don't add multi-arg lambdas in v1.** `n => expr` only. If we need
  `(a, b) => expr` later, add it then.
- **Don't add inharmonic-specific primitives.** No `bell()`, `piano()`,
  etc. All instruments are stdlib defs built on `inharmonic`/`additive`/
  `sum`.
- **Don't deprecate `osc(saw, freq)` or `osc(sqr, freq)`.** They stay
  available for chiptune/lofi/character work. Just document the new
  recommended path for clean audio.
- **Don't add a `for` loop or `map` primitive.** `sum` is enough for
  this design. Other patterns (collect-into-array, map-over-array) are
  separate future work.

---

## Confirm before

- Touching the audio mutex or hot-reload state migration plumbing.
- Adding language features beyond what's specified (no `for`, `while`,
  `match`, generics, types, etc).
- Modifying anything outside parser.nim, codegen.nim, voice.nim,
  stdlib.aither, tests/, or the markdown docs listed above.
- Making lambdas first-class (let-bindable, returnable from defs,
  storable in arrays).

---

## Order of work

1. **Phase 1** (lambdas) — biggest single piece. Land first.
2. **Phase 2** (sum primitive) — small once lambdas exist.
3. **Phase 3** (stdlib) — pure aither additions, no engine work.
4. **Phase 4** (docs) — documentation pass.
5. **Phase 5** (live test) — verification, no commit.

Each phase is one commit. After Phase 5 verification, push.

---

## First action

Read parser.nim end-to-end (~485 lines) and codegen.nim's expression
emission section (search for "emitExpr"). Understand the existing
scoping mechanism (Scope type, push/lookup, names table) — that's what
lambda capture will reuse. Understand how `wave()` and other builtins
that take compile-time-constant args are special-cased — `sum` follows
that pattern.

Then write `tests/lambdas.nim` first (TDD). Then implement Phase 1.
Then write `tests/sum.nim`. Then implement Phase 2. Then stdlib +
its test. Then docs. Then live test.

Estimate: 1-2 focused days. ~600-800 lines of net code change including
tests, ~50 lines of stdlib, ~80 lines of doc updates.

---

## Why this is the aither way

- **One primitive (`sum`), not two (`additive` + `inharmonic`).** The
  fundamental operation is "evaluate function over range and combine."
  Spectral synthesis is one application; multi-tap delays, voice stacking,
  filter banks are others.
- **Math reads as code.** `sum(16, n => sin(TAU * phasor(n*f)) / n)` IS
  the sigma-notation expression for a 16-harmonic saw. Anyone who knows
  summation can read it; anyone who doesn't learns by reading it.
- **No special-cased "harmonic vs inharmonic"** in the engine. Both are
  stdlib defs, distinguished only by their lambda body.
- **Lambdas are minimal but expressive.** Single-arg, scope-captured,
  builtin-arg-only. Smallest addition that unlocks the design.
- **Aliasing problem dissolves.** The user expressing a saw as
  `sum(16, ...)` chooses how many harmonics — the band-limit lives
  in the user's math, not in a hidden DSP magic.
- **Smaller language overall.** One primitive replaces what would have
  been `additive` + `inharmonic` + (maybe) PolyBLEP-aliased oscillators.
- **Composes with everything else.** The `sum` lambda body can use any
  aither primitive. Time-varying recipes, modulated formants, knob-
  controlled brightness — all just normal aither expressions.
