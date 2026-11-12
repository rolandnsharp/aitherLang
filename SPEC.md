# Aither Language Specification

A language for real-time audio signal processing.

A program is an expression evaluated at audio rate (48,000
times per second) that produces a single stereo sample. The
file defines named parts; the final expression is the voice's
output.

## Values

| Type     | Example                    |
|----------|----------------------------|
| float    | `440.0`, `0.5`, `-1.0`, `1e-3` |
| function | `sin`, `saw`, `my_shape`   |
| value    | `(0.6, 0.8)`, `((w, x), (y, z))` |
| places   | `[l, r]`                   |
| array    | `[220, 330, 440]`          |

Float is the default. Round brackets are one value, square brackets
many places (see "Places and values"); a number with one place is
everywhere, so a number and two places mix freely. Function values exist so a shape can
be passed to a def (`osc(shape, freq)`, `sum(N, fn)`).
Arrays hold data: lookup tables, `wave` note lists,
delay/reverb/wavetable buffers, per-sample scratch
(`let a = array(N)`), persistent fields (`$f = array(N)`,
see "State arrays").
Nothing maps over an array — a function that wants a float
gets a float.

## Globals

| Name      | Description                            |
|-----------|----------------------------------------|
| `t`       | time in seconds since engine start     |
| `sr`      | sample rate (48000)                    |
| `dt`      | 1 / sample rate                        |
| `sweep`   | this voice's own time: seconds along its timeline |
| `PI`      | 3.14159...                             |
| `TAU`     | 6.28318... (2π)                        |

`t` is the one clock, shared by every voice. Underneath it is a count:
the engine counts steps (samples) from its start, a whole number, and
`t` is the count over the rate, so the first sample is at `t = 0`, the
same live and in a render, to the bit, and it never drifts however long
the set (notes/step-first-2026-11-05.md). `sweep` is this voice's own:
0 when it is first sent, running on through edits, and set with
`aither sweep <voice> <seconds>` (COMPOSING.md, "The composition
clock"). It is the steps since the moment it was last set, over the
rate: exact, and free to move.

## State

The `$` sigil declares persistent state. Survives across
samples and across hot-reloads. Auto-initialized on first
load only.

```
$phase = 0.0
$env   = 1.0
```

Every reference to state wears the `$`, at every use site —
so a reader can tell at a glance which values carry memory
and which are recomputed each sample. The sigil is part of
the name token, not a separator: `$phase` is one token;
`$ phase` (with whitespace) is a tokenizer error.

The first `$name = expr` in a scope is a declaration that
allocates the slot; every later `$name = expr` is an
assignment.

The sigil is for the reader; to the compiler `$x` and `x` are one
name. So state can't share its name with a parameter or a `let` it
could be confused with, and the compiler says so, naming both lines:
`'x' is both a parameter (patch.aither:1) and the state $x
(patch.aither:2)`. Before 2026-11-05 it was silent and the two were one
value: a def's parameter `on` read its state `$on`, a top-level `let x`
took over `$x`'s reads. What is checked is everything one piece of code
sees: a def's parameters and lets against its own state; the top
level's lets against the file's state, which includes every play's
(`$state` in a play is the file's, by name); a play's lets and a
lambda's parameters and lets against the state of the code they are
written in, where `$x` meant that state and would read the name. State
declared inside a lambda with a `let` of its name outside is ordinary
shadowing: inside, `$x` and `x` both mean the lambda's state. A def never sees the file's state, so a def's `x` and the
file's `$x` are fine. A state array is read by index, through the
arrays, never as a plain name, so `let z = $z[k]` (this element of it) is
fine too. In a live set the error is a refused send: the version playing
plays on, and the fix is a rename.

```
$x = 0.0
$x = $x + 0.001     # subsequent assignment, reads then writes
```

On hot-reload, existing state values are preserved by name.
Newly-introduced state initializes to its default.

`let` declares per-sample values. Computed fresh each sample.

```
let freq = 440 + osc(sin, 0.3) * 50
let mix  = osc(sin, 440) + osc(saw, 220)
```

## Operators

Arithmetic (precedence high to low):

| Operator       | Description              |
|----------------|--------------------------|
| `x ^ y`        | power                    |
| `-x`, `not x`  | unary                    |
| `*`, `/`, `mod` | multiplicative          |
| `+`, `-`       | additive                 |
| `==`, `<`, `>`, `<=`, `>=`, `!=` | comparison |
| `and`, `or`    | logical                  |
| `\|>`          | pipe (lowest)            |

`^` is the power, as mathematics writes it: tighter than a sign, so
`-x^2` is `-(x^2)`, and grouping to the right, so `2^3^2` is `2^9`. A
whole power from 0 to 4 that is known when the patch is compiled is
the products (`x^2` is `x * x` to the bit, and costs what it does);
any other is `pow(x, y)`. On a value it is the ladder's, `z ^ 2` being
`z * z`, and at each place on places. There is no bitwise xor, so `^`
means nothing else.

Arithmetic is at each place, and a value with one place is everywhere
("Places and values"):

- `float + float → float`
- `float + [L, R] → [float+L, float+R]`
- `[L, R] + [L, R] → [L+L, R+R]`
- `(a, b) + 1 → (a + 1, b)`, `(a, b) * (c, d)` the complex product
- Any array of other than two in arithmetic is a compile error: index
  it, or hand it to `wave`

Same for `-`, `*`, `/`, `^`.

`a mod b` is floored: it takes the sign of `b`, so `-1 mod 4` is 3,
and `mod 0` is 0. So `x mod 1` is the fractional part, `x - floor(x)`,
from 0 up to 1 for any sign: a phase, a position in a loop. `%` is not an operator; written, it is an error that
says to write `mod`.

True is non-zero. False is zero. Conditional results are `1`
or `0`.

## Pipe

`|>` inserts the left side as the first argument of the
function call on the right:

```
osc(saw, 55) |> lpf(800, 0.5) |> gain(0.3)

# equivalent to:
gain(lpf(osc(saw, 55), 800, 0.5), 0.3)
```

Pipe is the *lowest-precedence* operator (matching OCaml /
Elixir / F#). The RHS must be a function call or ident.

**Gotcha**: `x |> f() * y` does NOT parse as `f(x) * y`.
Because pipe is low precedence and only accepts a call on
its RHS, the `*` is orphaned. Wrap in parens or bind first:

```
(x |> f()) * y                  # parens
let z = x |> f(); z * y         # bind
```

The parser emits a clear error in this case.

## Conditionals

Expression-oriented. Always returns a value. Single-line:

```
if t < 4 then osc(sin, 440) else osc(saw, 220)
```

Multi-branch via chained `else if`:

```
if t < 4 then osc(sin, 440)
else if t < 8 then osc(saw, 220)
else noise()
```

## Functions

`def` declares a reusable function:

```
def pluck(freq):
  noise() * impulse(3) |> resonator(freq, 0.2)

def chord(root):
  osc(sin, root) + osc(sin, root * 5/4) + osc(sin, root * 3/2)
```

### Blocks end at their expression

A body is its bindings, ended by the expression that is its
value. `let`, `$state = ...`, element stores (`a[i] = v`) and
`each(...)` are bindings: the block reads on past them. The first other expression is the return value, and
it closes the block. There is no terminator, and indentation
means nothing; indent for the reader:

```
def ease(x):                   def ease(x):
  let c = clamp(x, 0, 1)       let c = clamp(x, 0, 1)
  c * c * (3 - 2 * c)          c * c * (3 - 2 * c)    # the same block
def half(x): x * 0.5           # one line
```

A body that reaches the next `def`, `play` or the end of the
file without an expression is an error ("def f has no
expression"). A body with a second expression ends at the first;
the second spills out to the top level, which has room for one
expression, so that is an error too. To call something for its
effect and drop its value, bind it: `let _ = array_pop(a)`.

Functions have their own local scope: a def sees the
file's constants, never its signals, nor the names of
whoever calls it. A constant is a file-level `let` known
when the patch compiles: a number, arithmetic on numbers
(`let root = 293.66 * 2`), or a table of literal numbers
(`let ps = [2, 3, 2, 3]`), as `TAU` and `PI` are. File-level
`$state` slots, other `def`s and globals are visible too:

```
let ps = [2, 3, 2, 3, 3, 2]              # a table of literal numbers: seen
let ph = TAU * phasor(f)                 # a signal: passed in
def shadow(k, ph): [cos(ps[k] * ph), sin(ps[k] * ph)]
```
 A file-level let that is a signal
(a knob, a clock, anything that changes as it runs) used in
a def is an error that says to pass it in; a name neither
the def nor the file has is an unknown identifier, whatever
the caller binds. Turning a constant into a signal makes
every def that read it say so.

### Defaults and named arguments

A parameter can carry a default, and a call can name its
arguments, so a def with many knobs is usable without
spelling all of them:

```
def sing(freq, gate, tract = 1, register = 0.5, breath = 0.2,
         press = 0.4, vibrato = 0.1, robot = 0):
  ...

sing(220, g)                       # the defaults
sing(220, g, vibrato = 0.3)        # one knob by name
sing(220, g, 1.2, robot = 1)       # positional first, then named
```

A default is an expression of numbers, `PI`/`TAU`, operators
and calls (`decay = 1 / 4`, `q = sqrt(2)`); it may not name
a variable or another parameter.

Any call can name its arguments, a builtin's as well as a def's:

```
reverb(x, rt60 = 2.4, wet = 0.3)
x |> lpf(cut = 800, res = 0.5)      # the pipe fills the first, signal
$u = array(w * h, every = w * h)
```

A builtin's parameters have the names SPEC writes them with:
`lp1`/`hp1(signal, cut)`, `lpf`/`hpf`/`bpf`/`notch(signal, cut, res)`,
`delay(signal, time, max_time)`, `fbdelay(signal, time, max_time, fb)`,
`tape(signal, on, w, r, max_time, keep = 0)`, `reverb(signal, rt60, wet)`,
`impulse(freq)`, `phasor(freq)`, `discharge(signal, rate)`,
`wave(freq, arr)`, `pow`/`min`/`max(a, b)`,
`clamp(x, l, h)`, `array(n, init = 0, every = 0)`. A parameter
that takes the sound is called `signal` wherever it appears. One-argument
builtins (`sin`, `midi_cc`, …) take theirs by position. Stdlib
functions are defs and name theirs. A named call compiles to exactly
the positional one. An unknown name says what the function takes. A
builtin's defaults fill in for trailing arguments left out, by
position too, as a def's do: `tape(x, on, w, r, 8)` is `keep = 0`.

### Array parameters

An array argument is bound by name, never copied. Inside
the def the parameter indexes the caller's array (a constant
table, this sample's local array, or a `$state` array), and
`len(param)` is a compile-time number at each call site, so
one def serves any length and can size its own state by its
argument:

```
def tube(src, areas):
  let n = len(areas)
  $f = array(n)                    # per call site, sized by the argument
  $b = array(n)
  each(n - 1, j => ...)
  $f[n - 1]

tube(glottis, [3.5, 3.4, 3.1, 2.9, 2.7, 2.6, 2.5])
tube(glottis, mouth)               # a table or array declared outside
```

Writes to the parameter land in the caller's array. Two
numbers at a call are two places, and places index like a
two-entry table, so a def that indexes its parameter takes
`[220, 330]` as it takes `[220, 330, 440]`. Defs don't return
arrays; places and values are the only multi-value returns. What a def gives an array is its rule: a def of the index,
tabulated by `array(n, i => …)` (see "Local arrays"), or, for several
arrays filled from shared work, the caller's arrays filled in place,
since a def's writes land in them:

```
def reflection(a, j): if j == 0 then 0 else (a[j - 1] - a[j]) / (a[j - 1] + a[j])
let r = array(len(areas), j => reflection(areas, j))
```

A def takes a parameter's places together when it indexes it (`p[0]`),
names them (`let [a, b] = p`), passes it to a def that does, or does
arithmetic with it and places; a `def rec` that passes itself places
there counts too. A number passed to such a parameter is at both. Else
it runs once a place. A value is always taken whole.

`sum`, `each` and `array(N)` accept any compile-time number:
a literal, a name bound to one, `len(array)`, or arithmetic
over those (`each(n - 1, …)`). In a def that includes the
file's constants, which a def sees (`let K = 8` at the top,
`sum(K, …)` in a def), unless the def binds the name itself.

### Recursion

A def marked `rec` can call itself when the call is the whole
result of a branch: the def's body, either branch of an `if` in
that position, or the last line of a block. The call repeats the
def instead of nesting it. The new arguments are all computed
from the old values, the parameters take them, and the body runs
again, so it is a loop that stops when the data says so:

```
# z -> z^2 + c until it leaves, or n runs out: the steps left
def rec escape(z, c, n):
  if n == 0 or amplitude(z) > 2 then n else escape(turn(z, z) + c, c, n - 1)

def rec fib(n, a = 0, b = 1): if n == 0 then a else fib(n - 1, b, a + b)
fib(30)                            # 832040
```

- `rec` says the def loops, as OCaml's `let rec` does: a patch
  without it always finishes each sample. A def that calls
  itself without `rec` is a compile error. (A patch's def
  replaces the stdlib's of the same name, so a wrapper that calls
  the def it replaces needs a name of its own; the stdlib's
  constants `i`, `j` and `k` give way the same, to any let, state,
  parameter or lambda parameter of the patch's of that name.) `rec` on a def
  that never calls itself changes nothing.
- A call to itself anywhere but a branch's whole result
  (`fib(n - 1) + fib(n - 2)`) is a compile error: carry the rest
  of the work in a parameter, as `fib` does. Defs that come back
  to themselves through another def (`f` → `g` → `f`) are an
  error too, for now.
- Only the branch taken runs, so a base case costs nothing
  until it is reached.
- Numbers and pairs change from step to step; an array or
  function parameter is passed on unchanged. A parameter that
  every repeat passes on unchanged keeps its compile-time
  number, so `each(n, …)` in the body still folds.
- Each call site is its own loop, with its own state (see
  State semantics). Stores to a state array land at once, even
  when the call sits inside an `each`.
- Nothing counts the steps. A recursion still running when its
  voice's block has taken `AITHER_WATCHDOG` seconds (0.5 by
  default; 0 turns the guard off) gives up, and the block is
  silent. Within 20 s of a send the voice rolls back to the
  previous version; otherwise it is muted until it is sent
  again. The log names the def and its line.

## First-class functions

Functions are values; pass as arguments:

```
def my_shape(x): sin(x) + sin(3 * x) / 3

osc(my_shape, 440)
```

## Lambdas

Anonymous function. Single-argument form is bare:

```
sum(16, n => sin(TAU * phasor(n * 440)) / n)
#   ^^^^^^^  the lambda binds `n` for the body
```

Multi-argument form puts the params in parens:

```
midi_keyboard((freq, gate) =>
  additive(freq, warm_shape, 8) * adsr(gate, 0.01, 0.2, 0.7, 0.4))
```

The body may be a plain expression, or a `let`-prefixed
sequence terminated by a final expression:

```
sum(16, n =>
  let pf = n * 440
  sin(TAU * phasor(pf)) / n)
```

Each `let` line introduces a binding visible to the final
expression (and to later `let`s). Semicolons between lets
are accepted but optional — newlines inside the enclosing
`(...)` are whitespace (see "Group-depth tokenization").

**Restrictions**:
- Lambda is only legal as an argument to a builtin or a `def`
  parameter. A lambda stored in a `let` or returned from a `def`
  is a compile error.
- `sum` requires a single-arg lambda (its iteration index).
- A lambda may take **fewer** arguments than it is called with;
  the extras are ignored, not evaluated. A caller passes all it
  knows and each lambda names what it uses, so adding an argument
  later breaks no lambda: `midi_keyboard((freq, gate) => ...)` and
  `midi_keyboard((freq, gate, vel, key) => ...)` both work. Fewer
  arguments than the lambda takes is an error.
- A multi-arg lambda passed to a `def` is inlined at every call
  to that param: `def poly(n_voices, voice_fn): sum(n_voices, n =>
  voice_fn(midi_voice_freq(n), midi_voice_gate(n), ...))` — `voice_fn`
  is bound to the lambda the caller passed, not a runtime value.

Lambdas capture any variable in their enclosing lexical
scope — enclosing `let`s, `def` parameters, and globals all
work. Captures are free: the lambda body inlines into each
call site, and its names resolve where the lambda was
written, never inside the def that calls it. So a lambda
written in a `play` sees the file's lets even when a def
runs it: an instrument written where it plays,
`midi_keyboard((freq, gate, vel) => ...)`, uses the file's
knobs directly.

## Compile-time evaluation

Numeric literals *propagate* through `def` parameters and
`let` bindings. If you call `fixed_count(5)`, the body sees
`amount = 5` not only as a runtime C double but also as a
compile-time integer constant — so `sum(amount, n => ...)`
resolves `amount` to the literal `5` at codegen and unrolls
accordingly.

```
def additive(freq, shape, max_n):
  sum(max_n, n => ...)         # max_n is a literal at call time

additive(440, saw_shape, 16)   # sum unrolls to 16 iterations
```

Propagation is transitive across multiple def hops and
through `let` bindings whose RHS is a numeric literal. For
runtime-valued `N`, `sum` errors with a clear message —
keep the loop bound a literal, or let it flow through a
def/let from a literal.

## Play blocks

`play name:` declares a named, independently-controllable
part. Its body is a block like a def's (its bindings, ended by
its expression; see "Blocks end at their expression") and
compiles inline into the main chunk (so it sees all file-level
lets and `$state` slots). Its value is
bound to a local named after the play, readable from the
final expression and from later plays.

```
def ease(x):
  let c = clamp(x, 0, 1)
  c * c * (3 - 2 * c)

let tempo = 140.0 / 60.0
let kEnv  = discharge(impulse(tempo), 10)
let sc    = 1 - kEnv * 0.75                    # sidechain

play kick:
  sin(TAU * phasor(50 + discharge(impulse(tempo), 35) * 170)) * kEnv * 0.9

play bass:
  osc(saw, 55) |> lpf(150 + kEnv * 1500, 0.85) * sc

(kick + bass) |> drive(1.1)                    # final expression = output
```

Each part has an engine-controlled gain (default 1.0),
adjustable via CLI. Hot-reloading preserves gains by part
name.

**Forward references only** — a play can reference any part
defined *earlier* in the file. For reading a later play's
value (or its own), use `prev(name)` for a one-sample delay.

See "Program structure" below.

## Arrays

### Literals

```
[220, 330, 440]
```

Literal arrays of all-constant numbers are compile-time
hoisted; they allocate once per voice, not per sample.

### Let-bound arrays + indexing

A `let` binding whose RHS is a numeric-literal array is
hoisted to a static const and may be indexed by any
expression that evaluates to a number. The legal scopes
are top-level, play body, and `def` body — so a constant
table can live next to the code that uses it:

```
def bar_partials(n):
  let p = [1.0, 2.756, 5.404, 8.933, 13.345]
  p[n - 1]

let roots = [110.0, 87.31, 130.81, 98.0]
play bass:
  osc(saw, roots[int(phasor(0.25) * 4)]) * 0.2
```

The index expression is truncated to `int` and wrapped to
the table size (`p[n] === p[(int)n % len]`), so out-of-range
reads cycle rather than crash. An array can be passed to a `def`
(which reads it, and can store into a state array), but not returned
from one, and not passed to a lambda.

### Local arrays (this sample's numbers)

A `let` array whose elements aren't all literals, or `array(N)` /
`array(N, init)`, is a per-sample local array: it is filled afresh
every sample and its elements can be read and stored by any index.
`N` must be a compile-time integer. Legal at top level, in a play
and in a def.

```
let p = [p1, p2, p3, p4]          # runtime values
let k = array(23)                 # 23 zeros
let w = array(8, 0.5)
k[3] = (p[0] - p[1]) / (p[0] + p[1])
len(k)                            # 23, a constant
```

Indexes are truncated to `int`. The edge is how the array was made:

- **A list** (`[p1, p2, p3]`) is a sequence, and cycles like a constant
  table's (`p[-1]` is the last element), so a melody or a set of roots
  indexed by a counter goes round.
- **`array(N)`** is a field or a buffer: past either end it reads 0
  (`[0, 0]` for places) and a store there is dropped, as a state array's
  does. Beyond the end of a tube there is nothing, whether the tube
  keeps its values or not.

The initial value may be a rule, a function of the index: `array(n, i
=> …)` is the array whose element `i` is the rule at `i`, for `i` from
0. A number or a pair as `init` is the rule that ignores its index.

```
let h = array(24, j => max(0, 1 - abs(j - centre)))           # a bump around centre
let r = array(len(a), j => if j == 0 then 0 else (a[j - 1] - a[j]) / (a[j - 1] + a[j]))
let z = array(8, k => rotate((1, 0), k * TAU / 8))           # eight directions: pairs
```

The index is bound as `each` binds it: one element after another, a
number at each, so `if j == 0` and `a[j - 1]` are what they read. The
rule runs on the sample the array is born. It takes one parameter. Its body may
be `let`s ended by an expression. The elements are what it returns:
numbers, or pairs if any element is one (a number then fills both
sides). A rule is for one array made by one formula; `each` is for a
step that writes several arrays from shared work.

A constant table (all literals) is hoisted and read-only; storing into
one is a compile error.

### State arrays (a field that persists)

`$name = array(N)` or `array(N, init)` declares `N` persistent
elements: a tube's waves, a string's displacement, a membrane's
rows. They live in the pool next to the other `$state`, keep their
values across samples and hot reloads (same name, same def, same
size), and start at `init` (0 when omitted).

```
$f = array(24)                    # forward waves, one per section
$f[i]                             # read; past either end reads 0
$f[i] = x                         # store
len($f)                           # 24
```

A rule as `init` is the field's starting shape, u(x, 0) = f(x). It is
evaluated on the sample the array is born (its first load, or when it
is declared anew), with the values of that sample, and then the array
keeps its own values, as with a constant `init`:

```
$u = array(64, i => sin(PI * i / 63) ^ 2)     # a string drawn into a bump before it's let go
```

An element past either end reads 0 and a store there is dropped:
beyond the end of a tube there is nothing, and the boundary is what
the patch writes at `$f[0]` and `$f[len($f) - 1]`.

Stores happen at once, in order, like `$x = ...` does, except inside
`each`.

**Elements are what the initial value is.** Every value is a pair, and
an array's elements are no exception: `array(N, [1, 0])` makes `N`
pairs, as `$z = [0, 0]` makes pair state. `$z[i]` is then a pair,
`$z[i] = p` stores one (a number fills both sides). It holds for state
arrays, local arrays and stepped arrays alike, and a pair array passed
to a def is a pair array there. Indexing is as for numbers: `array(N, [1, 0])`,
state or local, reads `[0, 0]` past either end. A
field of phasors (a ring of coupled oscillators, a complex wave on tv,
each pixel's z) is then one array:

```
$z = array(12, [1, 0])            # twelve phasors
each(12, i => $z[i] = law($z[i], $z[i - 1], $z[i + 1]))   # the medium's law, at every place at once
```

### each: one simultaneous step

```
each(N, i => stmts)
```

One step at every place at once: `N` places, a whole number known when
the patch compiles, with `i` running from 0 to N−1 (an index; `sum`'s `n`
runs from 1 to N, a harmonic number), but the body is statements —
element stores, `let`s, `$state` — and the value is 0. A `$x = ...` in
the body is each place's own state, as in any lambda ("`$state` inside
lambda body"), not the `$x` outside: `$total = $total + v` in an `each`
gives every place a `$total` of its own and leaves the outer one as it
was. To add the places up, `sum(N, n => ...)` after the step. Every store to a **state array** inside an `each` is
held back and lands when the `each` ends, so each body reads the
arrays as they were before the step: section `i + 1` sees section
`i`'s wave from the last sample, not the one just written. Stores
to local arrays and scalars land in order, as usual.

A waveguide is then its own equations and nothing else:

```
def tube(src, a1, a2, a3, a4):
  let s = [a1, a2, a3, a4]                       # areas, glottis -> lips
  let k = array(3)
  each(3, j => k[j] = (s[j] - s[j + 1]) / (s[j] + s[j + 1]))
  $f = array(4)                                  # forward waves
  $b = array(4)                                  # backward waves
  let out = $f[3]
  each(3, j =>
    $f[j + 1] = (1 + k[j]) * $f[j] - k[j] * $b[j + 1]
    $b[j]     = k[j] * $f[j] + (1 - k[j]) * $b[j + 1])
  $f[0] = src + 0.9 * $b[0]                      # the glottis end
  $b[3] = -0.85 * $f[3]                          # the open lips
  out
```

The 24-section vocal tract this replaces was 219 generated lines.
See `tests/state_arrays.c` for the full one.

### Stepped state arrays (every)

```
$u = array(w * h, every = w * h)    # steps once a tv frame
```

`every = M` (`array`'s third parameter) makes a state array step: reads see the elements as they
were at the last step, and stores land together every M samples of the
engine clock, from its first (at samples 0, M, 2M, ..., as tv's frames
begin). It's `each`'s held-back stores, held back for M samples.
A field that every element must advance from the same moment (a wave on
a tv raster, where pixels are computed one after another) reads its
neighbours and itself with no bookkeeping:

```
$u = array(w * h, every = w * h)   # the field now
$v = array(w * h, every = w * h)   # a frame ago
let c = $u[k]
$u[k] = 2 * c - $v[k] + 0.25 * lap
$v[k] = c
```

A step begins at the sample where `pixel_index(M, 1)` is 0, so with M =
w·h steps land on `tv`'s frames. An element nobody stores keeps its
value. Inside `each`, stores land at the end of the `each` and then at
the step. M must be a whole number known when the patch compiles;
0, the default, is an ordinary state array, and `every = 1` makes
every store a one-sample delay. It also suits sound: a string or
membrane that steps every 16 samples and is read at audio rate costs a
sixteenth as much. `every` is `array`'s third parameter, usually named
(`array(n, 0, 4)` is `array(n, every = 4)`), and only state arrays
(`$name = array`) take it.

### Fields

**A field is the rule of `[l, r]` at many places.** Each line is what
happens at one place, and arithmetic is at each place:

```
$x = field(period = 1.66, ends = round)   # a push goes round in 1.66 s
$v = field(period = 1.66, ends = round)

$v' = Δ$x - $x * held - $v * loss + force * δ(where)
$x' = $v

let height = $x(phasor(note))                  # the field at a place
```

It is written as the equations are printed, in three symbols and a call:

| Written | Said | What it is |
|---------|------|------------|
| `∇$i` | del i | how steep the field is here: one slope |
| `Δ$x` | del squared x | how bent it is: two slopes, the Laplacian |
| `force * δ(p)` | delta at p | a force at one place, a term of a law: a source |
| `$x(p)` | x at p | the field at p: a field is a function of place, and this calls it |

`∇` and `Δ` bind tighter than anything but a call: `-∇$i * k` is
`-((∇$i) * k)`, and `ΔΔ$x` is four slopes. Of a field only: `∇$v(0.3)`, the
slope of a number, is an error. `δ` stands only in a law, multiplying a term
of it; there is no other way to push a ring, line or sheet, and no `$v'[p] += ...`. A
place `p` is a fraction of the field, 0 to 1 from end to end (once round on
a ring, and `(p, q)` on a sheet), as a field has a period, not a length: a
pluck at 1/7 of a string, its node, where the 7th harmonic isn't. Brackets
are for arrays: `$x[p]` on a field is an error, and so is `$a(i)` on an
array that isn't one. The symbols are UTF-8; aither's names stay ASCII.

Inside a field there is no here and no now, only what changes and what
it touches (notes/nabla-and-medium-2026-11-06.md). **The patch says what
the medium does; the engine owns the places, the wrap, the stepping and
the grain** (notes/field-by-its-period-2026-11-06.md).

- **A field's domain is its ends, not a name** (notes/one-system-2026-11-11.md,
  "Domains, without names"): one word, `field`, and in `ends` each piece of
  the domain, an interval's two ends (`fixed`, `free`, `matched`; one word for
  both, or `[left, right]`) or `round`, a circle. One period is one piece: a
  line, or, `round`, a ring; a list of periods, `[T, T]`, is one piece an axis,
  a sheet. `ring`, `line` and `sheet` are what the engine calls them inside,
  not words a patch writes. Other shapes, given by a function or a mesh, are
  designed and not built.
- `field(period = T, ends = round)` is a field a push goes round in T seconds, known when
  the patch compiles. `start =` is its first value at every place: a
  number, by default 0, or a value. The engine picks `sqrt(T * sr / 4)`
  places, so the same patch has the same motion at any rate.
- `field(period = T, ends = fixed)` is a ring opened up: a field with two
  ends, and everything here holds on it. T is there and back, the line's
  own period, so `ends = fixed` sounds at exactly 1 / T. An end is `fixed`,
  the field held at 0, so a pulse comes back upside down, or `free`, left to
  move, so it comes back as it went. `ends = [fixed, free]` is one each, as
  square brackets are many places, left then right, and a single word is
  both: one of each is an octave down, with odd harmonics only. `p` goes 0
  to 1 from end to end, reads past them stop there, and what a `gauss` lays
  past an end is gone. A line held by neither end slides away, so `free`
  at both wants a `held` or a loss. Of a pair, each end is one fixed and
  the other free: `$v` free and `$i` fixed is an open line, the reverse a
  shorted one. `ends` is known when the patch compiles.
- **`matched` is an end that takes what arrives**: a load as strong as the
  line itself, so a pulse leaves and doesn't come back. On a wave it is a
  free end with a dashpot, and needs the law `$x' = $v` to know what moves;
  on a pair both fields are `matched` there. About 2% of a pulse comes back,
  -35 dB, and -26 dB for one a few places wide, which is the grain. It is
  matched to the line as declared: `Δ$x * firm` moves the
  speed and not the match, a `held` is a line no end can match, and a pair
  matches `$v' = -∇$i`, `$i' = -∇$v`, as written with the minus.
  It works with `Δ$x` and pairs, not the squeeze and twist, which
  go at two speeds.
- **The field core holds what it can exactly** (`medium/`,
  notes/one-system-2026-11-11.md). A field with two ends, round, or a square
  sheet (`period = [T, T]`, its edge all `fixed` or all `free`), under a
  linear wave law at the file's level, `$v' = A Δ$x + B $x + C $v + D Δ$v + E
  ΔΔ$x + force * δ(p)` and `$x' = $v + ...` with A to E from outside, ends
  `fixed`, `free` or `round` (E, a bar, pinned at both), is held as its modes:
  each turned exactly, so its partials are exact at any rate, and a force or a
  reading at a place is exact there. The core knows no shapes, only axes: an
  interval with its two ends, or a circle, each with modes of its own, and a
  domain of two axes has their products, the lowest under Nyquist (a square
  fixed all round: sqrt(m^2 + n^2) / T, exact). A spread force, `δ(p, width)`, gives each
  mode the Gaussian's exact share, round wrapped, what lies past an end gone.
  A pair, `$v' = Cv ∇$i + Gv $v + Dv Δ$v` and `$i' = Ci ∇$v + Gi $i + Di Δ$i`
  (Heaviside's line), on a ring or a line with the partner's ends the others,
  is held the same way, each mode a 2x2 law stepped exactly, its losses equal
  or apart; a force on either field, a reading of either. A pair whose period
  moves, of equal loss, is held as two travelling waves (d'Alembert's), delay
  lines with cubic taps, an end turning what arrives: exact for it, where
  turning every mode anew each sample cost 30 times as much; its laws on plain
  state step with it by Gauss-Legendre, so a wake locks the same at any rate (329.8 Hz
  within 0.1 cents from 48 to 384 kHz, where it wandered by 6). A def holding
  a field is written out first, so each call's field is a medium of the core's.
  When a field keeps fewer modes than it has under Nyquist (the cap, 256), it
  says so as it loads, with the band it is limited to; held as modes, a pulse
  is then felt ahead of its front at a few parts in 10^4. A coefficient that moves is
  followed to 1 part in 10^4 (at most 0.17 cents behind in pitch), so a drifting
  loss costs a recompute every few dozen samples. Up to 256 modes, those under Nyquist. Laws
  on plain state beside it step with it, by Gauss-Legendre, and everything couples within
  one step: each medium turns half a sample, the laws and forces act at the
  midpoint, it turns the other half. The patch's own lines read the media and
  that state as they are at the sample's start, wherever they stand. Anything
  else, for now, goes the old way; `AITHER_CORE=0` sends everything the old way.
- **`width = p => ...` gives a pair a shape**: how wide the line is at p, 0
  to 1 along it, stated where the line is, so a where stays out of the law.
  The field that says it is the pressure, and its partner takes it as flow:
  flow changes pressure over the area, and pressure changes flow times it,
  the area being width squared. The laws don't change. Tapered, the line is
  a horn, Webster's equation, as a tapered telegraph line is the same thing
  with L and C changing along it. Closed at one end, a cylinder's modes are
  1, 3, 5 and a cone from 0.1 past its apex measures 1.80, 3.65, 5.55, 7.45
  against theory's 1.82, 3.67, 5.54, 7.45. Only a pair has one, and a
  `matched` end is for a line of one width.
- **A line's period can move**: `field(period = 1 / note, ends = fixed)`, with
  any signal for `note`. Such a line is a pair of equal loss, `$v' = -∇$i
  - $v * loss`, `$i' = -∇$v - $i * loss`, and anything else is an error,
  as only that stays two travelling waves. The engine holds it as those, d'Alembert's
  solution, each a delay, so it has no places: its partials are exact, up to
  a period of 2.7 s, and it costs about a tenth of a uConsole core. Held still
  it matches the same line on places in level, where the places run flat at the
  top. Ends do as they do; a spread `δ(p, width)` is an error, as a shorter blow at a
  point is a sharper one; and it is read at a place, `$v(p)`, not bare. Its
  losses have to be the same expression. Moved fast, the highs dull a little.
- **`field(period = [T, T], ends = fixed)` is the same field on a surface**: a
  square, `ends` one word for its whole edge, `fixed` or `free`. Two slopes
  are how far the neighbours all round stand above this place, so the law is
  a string's, and four a plate ("Four slopes are a bar"), pinned, so `fixed` only. One slope has no
  direction here, and is an error. A point on it is one value, `(p, q)`, each
  0 to 1, so `$x((0.3, 0.6))` reads, a term `force * δ((p, q), width)`
  strikes, and reading round a circle is `$x(spin(note) * 0.3 + (0.5, 0.5))`.
  Each period is its axis's, there and back along it as a line's, so a square
  fixed all round sounds first at sqrt 2 / T: measured (in the old spelling,
  whose period was that first mode's) 0.500 Hz where theory has 0.5, 99.8 Hz of
  100, the next at 0.790 and 157.4 of 0.79 and 158; on the field core now 100.0
  and 158.0, and a plate, its stiffness 1 / (8 pi^2) for sqrt 2 / T, 100.0 and 250.0 of 100
  and 250. Two axes have one period as yet, a square. It is as fine as a place a sample allows, about 31 a side at
  2 s, for a place of cost. Too short for 12 a side, it takes several a sample
  at 12 a side, about a third of a uConsole core at 100 Hz, and a plate 8. The
  period has to be plain numbers and file-level lets.
- **Two slopes of a field's own motion lose more the higher it is**:
  `$v' = ... + Δ$v * soften`, or on a pair `$p' = ... + Δ$p * soften`. `- $v * loss` takes the same from every mode;
  this takes more from each mode up, as a real bar or pipe does. On
  `field_bar` a `soften` of 0.0001 gives its first three modes 27, 44 and 64
  dB a second, where `loss` alone gives them 11.5 each. It is a small number,
  and too much goes unstable, with no warning: on that bar past about 0.1, on
  `field_didge`'s bore past about 0.003. A law smoothing itself doesn't change
  the order laws step in.
- **Four slopes are a bar**: `$v' = -ΔΔ$x * stiff`, `$x' = $v`. `ΔΔ` is
  exactly `Δ` twice, on any domain, and `period` means what it does for every
  field, the time a `Δ` wave takes there and back: a bar has no one speed, so
  its pitch is its stiffness, as in physics. A `fixed` end is pinned, a `free`
  one is free to bend. Pinned at both ends the modes are sines, n^2 apart, the
  first at `2 pi sqrt(stiff) / T`: `stiff = 1 / (4 pi^2)` puts it at 1 / T,
  exactly, on the field core. Free at both, the first is at `2 b^2 sqrt(stiff)
  / (pi T)`, b = 4.73004 (cos b cosh b = 1), so `stiff = 0.004929242983150253`
  puts it at 1 / T; a free bar's modes aren't sines, so it is on places as yet:
  1 : 2.7 : 5.1 at 220 Hz, nearer the textbook's 2.76 and 5.40 the slower the
  bar, its highest mode setting the step. There a stiffness much past what
  puts the first mode at 1 / T goes unstable. A free bar drifts as it's struck,
  so read `$v`, or hold it. A plate is the same on a square, pinned all round:
  its first mode, (1, 1), is at `4 pi sqrt(stiff) / T`, so `stiff = 1 / (16 pi^2)`
  puts it at 1 / T (and `1 / (8 pi^2)` at sqrt 2 / T, a membrane's first), the
  next at 2.5 times, exactly. The period has to be plain numbers and file-level lets.
- A bare `$x` is the field at this place, and `$x = ...` stores there. A
  later line at the place sees what was stored; a neighbour sees the last
  step. A number or a value is everywhere.
- **`$x' = ...` is a law: the field's rate of change, the equation
  itself.** Rates are **a round**, the ring being its own clock: `loss` of
  0.2 is a fifth lost each trip round, and `period` is the only second in
  the law, so changing it retunes the whole ring together. A ring's line
  has no `dt` or `t`, and says so. `+=` and `*=` on a ring are errors that
  point here, and a rate is said, never read. A field has one law, in the
  file's lines or a play's, and only a ring has one.
- **A block's laws are one system, so the order they're written in means
  nothing.** They are stepped together where the last of them stands, so a
  line before it reads the fields as they were and a line after it reads
  them stepped. The engine steps a field whose neighbours a law reads
  last, on the rates just found: force before motion. By hand one order
  of a wave's two lines rings and the other blows up; a law can't get this
  wrong.
- **`∇$i` makes a pair**: two fields, each changed by the other's
  slope, as voltage and current on a line are, and Heaviside's equations
  are the laws themselves:

  ```
  $v' = -∇$i - $v * leak
  $i' = -∇$v - $i * resist
  ```

  A pair steps staggered, the second declared a place behind the first, so
  each sees the other fresh on both sides; stepped together they blow up.
  Declared the other way round it is the same line. A pair shares one
  period, a law reads one partner, and a slope with no partner, or outside a
  law, is an error. A sharp pulse slowly widens as it travels, by about a
  third in 4 rounds: that is the grain, not the law.
- A field steps **one place a sample**, as `tv` draws a pixel, so it costs
  one place. Its stepping shows as a buzz at `sr / N`, about 40 dB under
  the note at 128 places. It suits a field that moves slowly against the
  sound, read at audio rate. A push goes a quarter of a place a step at
  the ring's period, and one place a step is the limit, so stiffer than
  sixteen times, a law multiplying the word by more than 16, is unstable.
- **A field too short for that is fine: it takes several places a sample.**
  At a place a sample a line at 110 Hz would have 7 places, and its
  harmonics go out of tune. Under 32 places the engine goes to at least 32,
  stepping as many a sample as that needs, so `field(period = 1 / 110, ends
  = fixed)` holds a note: 110 Hz, its third harmonic 329 Hz. The patch says
  nothing different. It costs: about 11 places a sample there, a tenth of a
  uConsole core for the law, and more for a spread source; a shorter period
  costs more. A push then goes half a place a step, so only four times
  stiffer is safe. The period has to be plain numbers and file-level lets
  for the engine to see it; anything else stays at a place a sample.
**Slopes compose.** `∇` is the one way to reach a neighbour: how much a field
changes from here to the next place. Taken again it is how much *that* changes,
`Δ`, and each says something physical:

| Slopes | Written | What it is | What it makes |
|--------|---------|------------|---------------|
| 1 | `∇$i` | how steep the field is here | a pair: voltage and current, pressure and flow |
| 2 | `Δ$x` | how bent it is: how far its neighbours stand above it | a wave: a string, a ring, a membrane |
| 4 | `ΔΔ$x` | how its bending changes | stiffness: a bar, a plate |

A string pulls towards straight, so its force is its bend, two slopes. A bar
resists being bent, so its force is how the bend changes, four. That is the only
difference in their laws. Slopes come in ones, twos and fours: one lands between
places, so it needs a partner stepping there; two and four land back on them,
so a field can read itself (`∇∇$x` is `Δ$x`, which is how to write it). Name
them, as a name says what they are in your instrument:

```
let curve = Δ$x                     # how bent it is here
let bend = Δcurve                   # how its bending changes
$v' = -bend - $v * loss
$x' = $v
```

A name here stands for the words, not a value: a law happens at every place, so
`curve` goes into the law as if written there, and `∇curve` is one slope
more. A slope with a name is for laws: anywhere else it is an unknown name. A
let that reads a field bare, `let back = $x * held`, goes into a law the same
way, being what it says at each place, and stays a let everywhere else. Two
slopes of a field's own motion are a loss that grows with pitch, below.

- A neighbour is reached only through these words, which on a ring carry
  its speed:

  | Word                          | What it is                               |
  |-------------------------------|------------------------------------------|
  | `Δ$x`                         | a wave's: how much the neighbours stand above this place. Two slopes land back on the places, so a field can read itself |
  | `ΔΔ$x`                        | a bar's bending: four slopes, on a `line` |
  | `∇(convergence($d))`          | of a field of directions: the slope of its squeeze, the scalar part of nabla |
  | `curl(curl($d))`              | ... the curl of its twist, the vector part |
  | `∇$i`                         | of one of a pair: how it changes along the ring, a half place from here |

  Along a line nabla is Hamilton's `i` times a difference, a step ahead
  and a step behind. Two speeds are one period and a ratio: `curl(curl($d))
  * 0.17`, slower by its square root.
- From outside a field has two touch points. `$x(p)` reads it at p, between
  places. **`force * δ(p)` is a source**: a term of v's law, F δ(s − p), as
  the equation is printed, so a force is said where the field's change is.
  Take it away with `-`; it may stand anywhere among the law's terms, and a
  law may have several, each its own. A term is the δ times what it
  multiplies (`*`, a numerator, a sign), one δ to a term, and its force can
  read lets and state the law can. It lands at the nearest place when the
  field reaches it, and is sized as a delta should be. However the laws of
  a block are written, their sources are pushed after the last of them, so
  the order they're written in still means nothing.
- **`force * δ(p, width)` is a source spread**: a Gaussian of area 1 and
  standard deviation `width`, in rounds, wrapped round the ring, its copies
  summed. A place is a stretch of ring, so each holds the curve's integral
  over that stretch, and the places sum to 1 at any width and any centre: a
  wider blow is the same blow, spread, out to flat all round the ring, and
  as `width` goes to 0 it all falls in the nearest place, so `δ(p, 0)` lands
  as `δ(p)` does. The width you write is the width computed, with no floor;
  a ring's resolution is a place, 1/N of a round, and a bump narrower than
  that sits in one place. The ring reaches a place once a sweep, so the
  engine keeps the running total of the force and gives each place all
  that came since it last passed: a blow shorter than a sweep lands whole at
  any rate, whichever side of the law the line stands. The shape is held as
  the force stops, so all of a blow lands where it was struck, even if the
  centre jumps the moment it ends. While force is flowing the shape is laid
  where the centre is at each visit, so a centre moving fast against a
  sweep shears the bump and changes its total, and a new blow within a
  sweep of the last, at a new place, carries what hadn't landed there with
  it.
- `δ` is a source only: outside a law, not multiplying a term (`sin(δ(p))`),
  two to a term, or on plain state, which has no places, it is an error.
  So is `$v[p] = ...` on a ring, which would claim to replace the law there.

**A law on plain state** is the same thing at one place, with no neighbours:

```
$x = 0
$v = 0
$v' = -$x * (TAU * f) ^ 2 - $v * damp + push
$x' = $v
```

With no round to count in, its rates are a second. The laws of a block are
one system, in the file, a `play`, a `def` or a lambda, and each call of a def
and each run of a `sum` has its own: they step together where the last stands,
whatever order they're written in, by the 4th-order Gauss-Legendre method.
It is symplectic, so a lossless spring keeps its energy, and close: a spring
at 3 kHz at 48 kHz is 0.06 cents out and holds its level to 0.03 dB over 2 s
(velocity then place, the way before, ran it 11 cents sharp; RK4 would drain
it). Its two stages are implicit, solved by eight fixed-point turns from an
explicit guess, so a law is worked out 19 times a sample; a `let` of the block
that reads a law's state is written into the law, so each stage sees it as it
would be there. Beside a field, the laws on plain state step in the field
core's one step, by the same method. There is no `dt` or `t` in a law, one law a state,
and a rate is said, never read. What varies with time comes in by a `let`
above it.

An array assigned bare (`$x = array(128)`, `$x = $x + ...`) is a field too,
with the size in the patch: a push then takes `N² / (sqrt(stiff) * sr)`
seconds to go round, the words carry no speed, and `+=` is plain, a
step's worth, with the order its lines are in. A push on one has no rate
to join, so it is an amount, `$v[p] += e`, each sample: a store, so in
brackets, though the field is read `$v(p)`. Use
`ring`, unless a number of places is the point. Fields are at the file's
level, in its plays too. **A def can hold a field**, and each call is its own:
`def string(note, at): $v = field(...) ... $v(0.13)` and `sum(5, n => string(...))`
are five strings. A call is written out where it stands, its names made its own,
so it is called in the file's lines, or in a `sum` of a known count with a
one-line body, not in a play or another lambda. Its fields see what it is given,
as a def does, and its laws are a system of their own, stepped where its own
last law stands, so each call is the string a lone one would be. Not built: a round sheet, and a voice each for the keyboard.

### Arrays are data, not a map

Unless it is a field, an array never fans a function out over its elements.
`osc(sin, [220, 330, 440])` is a compile error, not a
chord. Write the repetition out — that is what `sum` and
tables are for:

```
let notes = [220, 330, 440]
sum(3, n => osc(sin, notes[n - 1])) * 0.3
```

So a two-element array means the same thing everywhere it
can legally appear: a table of two, or a stereo pair when
it is returned (below).

### Places and values

> **Round brackets are one value. Square brackets are many places.**

A **value** is a number, `(a, b)`, a complex number, or `((w, x), (y, z))`,
one of Hamilton's: each rung is the one below doubled (Cayley–Dickson),
and one more doubling, the octonions, is an error, as their product
doesn't associate. **Places** are `[l, r]`: two numbers, as stereo is, or
`[z1, z2]`, a complex number at each.

```
let z = (0.6, 0.8)                    # one complex number
let q = ((w, x), (y, z))              # one of Hamilton's
let stereo = [left, right]            # two places
stereo * [0.8, 0.6]                   # a gain at each place
stereo + voice                        # one place: everywhere, so both sides
z * w                                 # turning
stereo * spin(50)                     # each side turned: two places of complex
```

**Arithmetic is the algebra at each place, and a value with one place is
everywhere.** At a place the narrower sits inside the wider, the rest
zero, and `+ - * / ^` are the ladder's:

| Written      | Means                                                       |
|--------------|-------------------------------------------------------------|
| `z * w`      | turning: the complex product, Hamilton's on his numbers, order mattering (`i * j` is `k`, `j * i` is `-k`) |
| `z / w`      | `z conj(w)` over the size of `w` squared, 0 where `w` is 0; `z * w / w` is `z` |
| `z + 1`      | the real part moves: `(a + 1, b)`                           |
| `z * 2`      | scales all of it                                            |
| `z ^ 2`      | `z * z`: turned twice. Any power: the size to the power and the angle times it (de Moivre), so `(0, 1) ^ 0.5` is an eighth of a turn, the principal root, and `(0, 2) ^ -1` the reciprocal. On Hamilton's numbers about his axis: `r ^ 0.5` is half the rotation, as slerp takes a fraction of one. A whole power from 0 to 64 known when the patch compiles is the products, exactly |
| `(0, 1) * q` | the narrower sits inside: `((0, 1), (0, 0)) * q`            |

**Multiplying is turning**: a value of size 1 turns what it multiplies
by its angle, and any other size scales it too. Multiplying spins adds
their speeds, and `cos(a) + dir * sin(a)` is Hamilton's formula for a
rotation, as he wrote it. This is why aither has no `turn`: `*` is it.
Comparisons take numbers.

| Reader          | On a value                                           |
|-----------------|------------------------------------------------------|
| `conj(z)`       | the same value turning the other way: Hamilton's K q, the vector part negated |
| `amplitude(z)`  | how big: Hamilton's tensor T q                       |
| `phase(z)`      | which way it points, -π..π; his angle, 0..π, on his numbers |
| `axis(q)`       | his axis, a unit direction `((0, x), (y, z))`        |
| `analytic(x)`   | makes one: `x` and its Hilbert transform, a quarter turn behind at every frequency |

These read a value: given `[l, r]`, they say to name the sides, as two
places aren't one number.

A rotation applies to a direction v (a value with w = 0) as `r * v *
conj(r)`, Hamilton's r v r⁻¹ for a unit r, turning it by twice r's angle
about r's axis (`apply`, in the stdlib).

**Reading parts.** `let (a, b) = z` and `let ((w, x), (y, z)) = q` name a
value's parts, `_` for what isn't wanted. That is the only way: a value
has no places, so `z[0]` and `let [a, b] = z` are errors, and so is
assigning part of one. `let [l, r] = stereo` names places, and `s[i]`
reads one: `i` truncates and wraps at 2, whether a constant or worked out
as it runs, so `s[int(phasor(2) * 2)]` alternates a part's sides. A
number gives both names the number.

**Functions** run at each place, and within a value once a number, each
run its own state:

```
osc(saw, [55, 55.3])               # a detuned stereo saw: two phasors
lpf(stereo, 800, 0.5)              # two filters
lp1(stereo, [100, 200])            # a cutoff a side
resonator(strike, [440, 442], 0.5) # a bell whose sides beat
```

**A def** given places runs once a place, each run its own state, unless
it reads the places themselves (`signal[0]`, `let [l, r] = signal`) or
meets places in its arithmetic: `mono`, `pan`, `width` and `haas` take
them together ("Array parameters"). If it gives a value at each place the
result is two places of values: `dho(push, [f, f * 1.0013], damp)` is a
doublet, the two modes of a partial. A value is always taken whole, so a
def that shapes each number (`drive`) sees it whole and its arithmetic is
the ladder's: give it `[a, b]` if it means each side.

**State, arrays, `if` and `sum`** keep the shape: `$z = (1, 0)`,
`array(N, (1, 0))`, `[(0, 0), (0, 0)]`. A state or an array keeps its
first value's shape; a number or a value goes everywhere in places, and
the narrower sits inside a value. In `if` and `sum` the widest shape wins.
An `if` runs only the branch it takes, and its condition is one number.

**Translating** is one line each way, the numbers unchanged:

```
let (a, b) = z          # a value's two numbers
let s = [a, b]          # now two sides
let [l, r] = stereo
let z = (l, r)          # now one complex number, ready to turn
```

**At the edge** the pipe carries two numbers. Two places of numbers are
left and right; a complex value its two numbers, so `spin(220)` is a
circle on the scope and a sound in the ears, and I and Q on a radio; a
number is everywhere, so both speakers. A play's result, read by another,
is two places.

**Read the whole value.** A value from an oscillator, `analytic` or
`spin` is a phasor: two numbers a quarter turn apart. Most of what is
wanted from it reads both at once, exactly and at every sample: how loud
is `amplitude(p)` (no rectifying and smoothing, which is how one number
has to get it); how fast it turns, its frequency, is
`phase(p * conj(prev(p))) / (TAU * dt)`; moving or mixing it is `*`;
hearing it is the value itself. Taking one part (`let (x, _) = p`)
throws the other away, and is for where one number is all there is room
for, such as a mono mix.

**Stand-ins, not decisions.** Where a meaning isn't settled the compiler
stops and says so, and the rule can change: a patch or play ending in
anything else (more than two numbers: say which two), and a function
given shapes that don't line up number for number. Not built: two places
of Hamilton's numbers, fields of more than two places.

**Errors name shapes.** Where a value's shape is wrong, the error quotes
each argument as written with its shape: `$z is a complex number, and is
given `[3, 4]`, two places`.

## Comments

```
# single line comment
```

## Statements

Separated by newlines or semicolons:

```
$phase = 0.0; $phase = $phase + 440 / sr; sin(TAU * $phase)
```

Whitespace carries no other meaning. A newline ends a statement
where one can end (see below), and a block ends at its
expression (see "Blocks end at their expression"), so neither
indentation nor brackets delimit anything.

## Group-depth tokenization

Newlines inside `(...)` calls / grouping, and `[...]`
array literals, are whitespace. Expressions may span as
many lines as you like inside any bracket pair:

```
let notes = [
  110.0,
  146.83,
  164.81,
  220.0,
]

sum(16, n =>
  let pf = n * 440
  sin(TAU * phasor(pf)) / n)
```

A call's `(` and an index's `[` must be on the same line as
the name or value they apply to. A `[` or `(` that starts a
line begins a new expression, so a lambda body can end on a
pair of its own:

```
sum(8, n =>
  let a = band(mic, 250 * n, 0.12)
  [a, a * a])                     # a pair, not a[...] of the line above
```

Outside brackets, a newline ends a statement unless the
statement can't end there or the next line can't begin one:

- A line ending on an operator (`+ - * / |> == < mod and or not
  =>`, and so on), `=`, `then` or `else` continues onto the next.
- A line starting with an infix operator (`|> + * / == != < > <=
  >= mod and or =>`), `then` or `else` continues the line above.

```
piano                          let y = a + b +
|> lpf(800)                      c - d
|> reverb(2.5, 0.2)
```

A continued line's indentation means nothing: indent it however
reads best. `-`, `not`, `(` and `[` can begin a statement, so a line
starting with one is a new statement (`-x` on its own line is a
negation, and `(a)` is never a call on the line above); to split a
difference, end the first line with the `-`.

## Program structure

A file has five kinds of top-level declaration:

- `import "lib/voice.aither"` — splice a library file in (see Imports)
- `def name(args): body` — helper function
- `let name = expr` — file-level binding, visible in all plays
- `$name = init` — file-level persistent state (declaration on first sight)
- `play name: body` — named, controllable part

Followed by exactly one **final expression** — the voice's
output. The final expression is typically a composition of
play names and effects:

```
(kick + bass + lead) |> drive(1.1)
```

Files **must** end with an expression, not a declaration.
The engine errors clearly if either rule is violated (no
play block, no final expression, or an expression in a
non-final position).

## Imports

```
import "lib/voice.aither"
```

Splices the file's statements in at that point, so instruments
live in a library and a patch is just the arrangement. The path is
relative to the importing file. A library may hold `def`s, constant
`let`s (a number or a table of numbers) and further imports; a
`play`, `$state` or a sound belongs in the patch that imports it,
and the compiler says so. A file imported twice is included once
(cycles end there). A def in the patch overrides a library's of
the same name. Imports are re-read on every `send`. Errors inside a library point at the library
file and line.

## Scope rules

| Declaration          | Visible                                  | State                              |
|----------------------|------------------------------------------|------------------------------------|
| file-level `let`     | everywhere below it                      | computed once per sample           |
| file-level `$state`  | everywhere (by name)                     | persistent, keyed by name          |
| file-level `def`     | everywhere (hoisted)                     | n/a (callable)                     |
| `play` body `let`    | inside that `play` only                  | computed once per sample           |
| `play` body `$state` | file-level by name                       | persistent, shared by name         |
| `def` body `let`     | inside that `def` only                   | computed once per call             |
| `def` body `$state`  | per-call-site                            | persistent per call location       |
| lambda body `let`    | inside that lambda iteration only        | computed once per iteration        |
| lambda body `$state` | per-iteration of the unrolled `sum`      | persistent per iteration / sample  |

**A `def` sees the file's constants, never its signals** —
defs are isolated functions that are given the signals they
work on, so a def means the same wherever it is called. **`play`
can see file-level lets** — plays are inlined blocks that
share the file's scope.

**A parameter shadows a def of the same name.** Inside
`def inharmonic(freq, ratio, amp, max_n)`, `ratio(n)` calls the
function passed as `ratio`, even if the file also has its own
`def ratio`.

**`$state` inside a play is file-level by name.** If two plays
both write `$count = 0`, they share the slot. Use explicit
names (`$kick_count`, `$bass_count`) for independence.

**`$state` inside a def is per-call-site.** Each location the
def is called from gets its own state slot. This is what
makes `osc(sin, 440) + osc(sin, 880)` work — two
independent phasors without any ceremony.

**`$state` inside a lambda body is per-iteration.** When
`sum(N, n => …)` unrolls, every one of the N iterations
gets its own slot — so `sum(8, n => $x = 0; $x = $x + n; $x)`
runs eight independent counters. This is how inline
modal banks (`sum(K, n => $x = 0; $dx = 0; …)`) get
per-mode physics state without delegating to a helper def.

---

## Builtins

### Math (native, C library)

| Function        | Description     |
|-----------------|-----------------|
| `sin(x)`        | sine            |
| `cos(x)`        | cosine          |
| `tan(x)`        | tangent         |
| `exp(x)`        | e^x             |
| `log(x, base)`  | logarithm; `base` defaults to e (natural log): `log(x, 2)` octaves, `20 * log(a, 10)` decibels |
| `abs(x)`        | absolute value  |
| `floor(x)`      | floor           |
| `ceil(x)`       | ceiling         |
| `min(a, b)`     | minimum         |
| `max(a, b)`     | maximum         |
| `pow(a, b)`     | exponentiation, also written `a ^ b` |
| `sqrt(x)`       | square root     |
| `clamp(x, l, h)`| clamp to range  |
| `int(x)`        | truncate to int |
| `tanh(x)`       | soft-clip to ±1 (the limiter, when you want one) |
| `erf(x)`        | the error function: the integral of a Gaussian, 0 at 0, ±1 far out |

### Oscillators: a clock and a shape

An oscillator is not a primitive. It is a clock read
through a shape:

```
sin(TAU * phasor(220))
```

| Function      | Description                                  |
|---------------|----------------------------------------------|
| `phasor(freq)`| the clock: ramp 0→1 at freq Hz, with state   |
| `noise()`     | white noise with a fixed level per Hz, the level uniform noise in −1..1 has at 48 kHz, so it sounds the same at any rate (its samples are ±`sqrt(sr / 48000)`); a die in −1..1 each sample is `noise() * sqrt(48000 * dt)`. Each call is its own stream, the same every run, seeded by where the call is (its statement, the defs it is inlined through, which `noise()` it is there), so editing other code leaves it unchanged |

`phasor` is the only stateful part. Its output is a
*position in the cycle*, 0..1 — a fraction of time, not an
angle. That is why the same value also serves as a
wavetable index, a pulse-width comparator
(`phasor(f) < width`) and a sequencer clock.

`TAU * phasor(f)` converts the position to an angle in
radians, which is what every shape takes:

| Function | Description                |
|----------|----------------------------|
| `sin(x)` | sine (the C library one)   |
| `cos(x)` |                            |
| `saw(x)` | sawtooth: `-1 → 1`         |
| `tri(x)` | triangle: `-1 → 1 → -1`    |
| `sqr(x)` | square: `-1 / 1`           |

`x` is an angle in radians; shapes wrap, so any angle is
legal. Radians are kept on purpose: phase modulation reads
exactly as the literature writes it
(`sin(TAU * phasor(f) + I * sin(TAU * phasor(fm)))`, with
`I` the modulation index), and the angle unit is the same
one `rotate`, `phase` and the other complex-number
operations use.

`osc(shape, freq)` is not a builtin — it is a one-line def
in the stdlib, `shape(TAU * phasor(freq))`, and any
one-argument function (including a user def) is a valid
shape. Anything that needs the phase itself — PM, hard
sync, a wavetable read, two shapes on one clock — spells
the clock out.

### Compile-time fold

```
sum(N, fn)
```

Evaluate `fn(1) + fn(2) + ... + fn(N)` at codegen time
and emit the unrolled sum as one scalar expression. `N`
must be a compile-time integer literal (directly, or via
a def-param / let that carries a literal through literal
propagation — see above). `fn` must be a single-argument
lambda.

```
sum(8, n => 1.0 / n)                        # harmonic series up to 8
sum(16, n => sin(TAU * phasor(n*f)) / n)    # 16-harmonic saw
```

Because each iteration is a distinct textual emission
site, stateful primitives inside the lambda each claim
their own state region. `sum(16, n => phasor(n*f))` yields
16 independent phasor states — exactly what additive
synthesis needs.

**Cost**: linear in `N`. Each iteration compiles to its
own C expression; 32 harmonics means 32 phasor slots and
32 sine computations per sample. Default to `N = 8..16`
unless you have a reason to push higher.

### Native DSP functions

These are C (in `dsp.c`) called from a patch's generated C; `dsp.c`'s
`dsp_natives` lists them for the compiler. A function is native only when it needs
a buffer or a C loop; whatever a few lines of aither can say (the
oscillators, `slew`, `tremolo`, `resonator`) is a stdlib def.
Each call-site gets its own state slots from a per-voice
float64 pool. The pool is as big as the patch needs: every delay,
tape, reverb and state array is added up when the patch compiles,
and the voice gets that much, up to 512 MB (a 30-second stereo tape
is about 23 MB). An edit that needs more gets a bigger pool, and
hot-reload carries the state across as ever.

**Filters**: `lp1(signal, cut)`, `hp1(signal, cut)`,
`lpf(signal, cut, res)`, `hpf(signal, cut, res)`,
`bpf(signal, cut, res)`, `notch(signal, cut, res)`. Every `cut` is in Hz.
`lp1`/`hp1` are one-pole filters with the -3 dB point at `cut`
(`a = 1 - exp(-2 pi cut / sr)`; before 2026-10-12 it was `cut / sr`,
which put `lp1(x, 4000)` at 665 Hz, and patches were converted).

**Delays**: `delay(signal, time, max_time)`,
`fbdelay(signal, time, max_time, fb)`. `time` is in seconds and need
not be a whole number of samples: the read point is interpolated
linearly between the two nearest samples, so a tube, string or
comb tunes to its length rather than to the sample grid. `delay`
with `time` 0 is the current input; `fbdelay` starts at one sample
(its feedback needs last sample's output). `max_time` must be a
literal (it sizes the buffer) and the longest delay is one sample
under it.

**Tape**: `tape(signal, on, w, r, max_time, keep = 0)`. A piece of tape
`max_time` seconds long, with a write head and a read head. While
`on` > 0 it records `signal` at `w`; it always plays what is under `r`.
Both heads are positions in seconds, any signal, wrapping round the
tape, so the tape machine's moves are choices of them:

```
tape(x, 1, t, t - 0.5, 1)                 # a delay: the write head at now
tape(x, recording, t, h, 30)              # a loop: h = start + (t - start) mod len
tape(x, 1, t, t * 0.5, 30)                # half speed: half the pitch, as on a deck
tape(x, 1, t, 2 * start - t, 30)          # reverse
tape(x, 1, t, knob * 4, 4)                # scrubbing: the head is a hand
tape(x, 1, t, t, 8, keep = 0.8)           # sound on sound, old passes fading
```

The controls are signals like any other, so the pedal is whatever
you write: `midi_cc(64) > 0.5`, a key, `bar < 0.25`, the loudness of
the mic. A loop pedal is a def over `tape` (COMPOSING.md, "Looping
your playing"). `signal` may be a pair: two tapes on the same heads,
which keeps a frequency and its gate together. What is looped is a
signal, so a loop of `[midi_freq(), midi_gate()]` loops the playing,
not the sound: it plays through whatever patch it feeds, as you edit
it.

It reads before it writes, as a deck's play head sits behind its
record head: a read at the write position hears what was there one
trip round the tape ago, which is what a loop recording over itself
must play. The read is between samples (linear, as `delay`), and a
head within a millionth of a sample of one reads that sample, so a
loop comes back exact. The write lands on the nearest sample, as
`signal` plus what was there times `keep` (the erase head: 0 records
over, 1 adds, between lets old passes fade). A fresh tape is silence.
`max_time` must be a literal (it sizes the tape); a pair takes twice
the memory. `delay` is a tape whose write head stays at now.

A tape is a function, not a program in the pipe, by the rule that
keeps the two apart: **programs are the ends, functions are the
middle.** Whatever touches a device or a file (`play`, `card`,
`scope`, `tv`, a recording with `tee`) is a tool in the pipe; whatever
is steered by signals or feeds back is a function in aither.

**Reverb**: `reverb(signal, rt60, wet)` (Schroeder); a pair in runs one
reverb per side. Its room is in seconds, so it is the same room at any
rate: four combs of 35.3, 36.7, 33.8 and 32.2 ms into two allpasses of
5.1 and 12.6 ms (Schroeder's lengths in samples at 44.1 kHz), each comb
damped by a one-pole whose time constant is the same at every rate.

**Physics**: `impulse(freq)`, `discharge(signal, rate)`.

`impulse(freq)` is an event: 1 for one sample, `freq` times a second,
and 0 between. That is what a trigger wants (`pluck`, `discharge`,
`swell`) at any rate. As a force it is a push of area `dt`, so it is
weaker as the rate rises. A strike, a push of unit area at any rate, is
`impulse(freq) / dt` times its size: `dho(impulse(2) / dt * 2000, 440,
0.005)`. A shaped strike of unit area in seconds (`bell`'s and
`contour`'s, rising and falling over a millisecond or so) is softer and
also the same at any rate.

**Sequence**: `wave(freq, arr)` — wavetable oscillator;
with low freq it's a step sequencer, with audio freq it's
a custom waveform.

### MIDI input

I/O primitives are prefixed by their source (`midi_`) so a
reader can tell "this number comes from a knob you turned"
apart from a derived DSP signal. State is engine-owned —
hot-reloading a patch preserves held notes and knob values.

| Function              | Description                                      |
|-----------------------|--------------------------------------------------|
| `midi_cc(n)`          | CC `n` value, `0..1`. `n` in `0..127`; 0..31 are 14-bit when paired with 32..63. |
| `midi_note(n)`        | Velocity `0..1` while note `n` is held; 0 else. |
| `midi_freq()`         | Hz of the most recent note-on (mono), bent by its channel's pitch bend. |
| `midi_gate()`         | 1 while the most recent note is held, 0 after its note-off. |
| `midi_vel()`          | Velocity `0..1` of the most recent note-on; stays after the note-off. |
| `midi_trig(n)`        | 1.0 for a single sample on each note-on for `n`; 0 otherwise. Per-voice edge detect. |
| `midi_voice_freq(n)`  | Hz of the nth held voice (1..16), bent by its own bend; 0 if slot empty. Stable per held note. |
| `midi_voice_gate(n)`  | 1 while the nth voice's key is held (1..16), 0 after release or empty. |
| `midi_voice_vel(n)`   | Velocity `0..1` the nth voice was struck at; stays after release, 0 if never used. |
| `midi_voice_key(n)`   | MIDI key number of the nth voice, `0..127`; stays after release, 0 if never used. |
| `midi_voice_cc(n, k)` | The nth voice's own controller `k`, `0..1`: its channel's, or set for that note alone. |
| `midi_voice_press(n)` | The nth voice's pressure, `0..1`. |
| `midi_voice_bend(n)`  | The nth voice's bend in semitones (its own and its MPE zone master's). |
| `midi_voice_on(n)`    | Which note-on the nth voice is: a count that moves on with every note-on its slot takes (0: never used), so a voice tells a new note from the last, even on the same key. |

What MIDI holds is read once a block: a knob, a key and a voice's
values change between blocks as far as a block can tell (an offline
`--play` run cuts its block at each move, so a move still lands on its
sample). In a voice loop each voice's values are read into an array once
a block, not per voice per sample. `midi_trig` is the exception: it
consumes its edge, every sample.

**A gate is a gate; velocity is separate.** Gates are 1 or 0, so
`adsr(gate, ...)` sounds however softly a key is played (`adsr`
starts above 0.5, so a velocity used as a gate dropped every key
softer than half). Velocity and key stay after release, the way
freq does: `swell(gate, a, r) * vel` rings out its release tail
instead of cutting off.

If no MIDI device is connected, all return 0 and the
patch still runs. There is no binding step — the patch IS
the routing. Example:

```
play bass: osc(saw, midi_freq()) * swell(midi_gate(), 0.01, 0.2) * midi_vel() * 0.3
play kick: discharge(midi_trig(36), 30) * sin(TAU * phasor(60))
(bass + kick) * midi_cc(80)
```

**Polyphony**. Use `midi_keyboard` (stdlib) — it wraps
`midi_voice_freq` / `_gate` / `_vel` / `_key` over 8 voices via
sum-unrolling. The raw primitives are for unusual cases
(non-default voice counts via `poly(N, ...)`, or hand-rolled
allocation logic).

Voice stealing: when a 17th note arrives, the slot with the
lowest `onAt` (oldest held) is evicted. When a slot's note is
released its velocity drops to 0 and the slot becomes
re-allocatable; the freq stays set so a synth's release tail
still reads a valid pitch.

**Per-note expression and MPE.** Each held note carries its channel,
its pitch bend, its pressure and its own 128 controllers, and keeps them
after release for its tail. A note takes its channel's values when it
starts, and the channel's later bend, pressure and controllers move the
notes it holds. A keyboard plays every note on one channel, so its notes
all share the wheel, the aftertouch and the knobs, as before (the pitch
wheel now bends: two semitones, MIDI's default, or what RPN 0 sets).
An MPE controller (MIDI Polyphonic Expression, MMA RP-053, 2018) puts
each note on a channel of its own, so the bend, pressure and slide
(CC 74) sent there belong to that note alone: a chord in which one note
bends and another swells. The configuration message (RPN 6 on channel 1
or 16) sets a zone: its member channels bend 48 semitones and its master
2, and the master's bend moves every member's note. One key held on two
channels is two voices. A program sets one held note's values directly,
by key, at full resolution (`aither_note_cc` and friends in an export,
below): that is how a game gives each instance its own distance or
material. The three dimensions reach `poly`'s voices as `freq` (bent),
`press` and `slide`; any controller is `midi_voice_cc(n, k)`.

Every source arrives as the MIDI 1.0 message it is, channel and all, at
one reader: the ALSA thread turns its events back into those bytes, an
export's host hands them over raw (`aither_midi`), and a `--play` script
names them.

**14-bit controllers.** Controllers 0..31 pair with 32..63, as MIDI
1.0 has them: `n` carries the coarse 7 bits and `n + 32` the fine 7, and
`midi_cc(n)` reads the pair, `(coarse * 128 + fine) / 16383`, 16384
steps where a single controller has 128. A coarse value alone reads as
it always did, `m / 127`, so a 7-bit knob still reaches 1: until the fine
half arrives it is taken to be the coarse value itself, which comes to
exactly `m / 127` (MIDI 1.0 resets it to 0, which would stop a 7-bit
knob at 0.994). A sender of 14 bits sends the coarse half, then the fine,
and the value is exact from the fine one on. The fine half reads on its
own too (`midi_cc(n + 32)`, 0..1 in 128 steps), and ALSA's own 14-bit
event arrives as the pair it stands for. Controllers from 64 up pair with
nothing. A program driving a patch smoothly (a game, a tracker) sends
pairs on 0..31; the undefined ones, 3, 9, 14, 15 and 20..31, collide with
no keyboard's usual knobs.

**Which port.** At start the engine listens to the first MIDI port that
offers itself. `AITHER_MIDI` chooses instead: a port by name or number
(`AITHER_MIDI=yalanji`, `Minilab3:0`, `28:0`), as `midi connect` takes
it, or `none` for nothing until a `midi connect`. A program that starts
an engine of its own names its own port, so a keyboard plugged in doesn't
play into it. See "CLI" below for `aither midi list / connect /
disconnect`.

---

## Stdlib (written in aither)

Embedded in the binary as a const string. Loaded before
every user patch. Source: `stdlib.aither`.

### Oscillator wrappers

```
def osc(shape, freq):  shape(TAU * phasor(freq))
def pulse(freq, width): if phasor(freq) < width then 1 else -1
def spin(freq):                           # the unit value turning at freq Hz
  let a = TAU * phasor(freq)
  [cos(a), sin(a)]
```

`spin` is the unit value turning at `freq` Hz: one phase, both parts.
Multiply by it to set a value spinning; turning by a spin adds speeds,
so `spin(220) * spin(50)` is `spin(270)` and `p * p` doubles
`p`'s speed.

```
def rotate(z, angle): z * (cos(angle), sin(angle))
def freq_shift(signal, hz): analytic(signal) * spin(hz)
```

```
let i = (0, 1)                            # Hamilton's units ("Values on the ladder")
let j = ((0, 0), (1, 0))
let k = ((0, 0), (0, 1))
def about(dir, angle): cos(angle / 2) + dir * sin(angle / 2)
def apply(r, v): r * v * conj(r)
def hopf(s): s * ((0, 0), (0, 1)) * conj(s)
```

`i`, `j` and `k` are Hamilton's units, i j = k and j i = −k; a patch's
own names come first. `about(dir, angle)` is the rotation by `angle`
about a unit direction, his versor of half the angle (applied, a
rotation turns by twice its own angle). `apply(r, v)` turns a direction
v (one of his numbers with w = 0) by r, his r v r⁻¹; composed rotations
apply right first, `apply(r2 * r1, v)` being r1 then r2.
`hopf(s)` is the way a spinor points, the pole k turned by it: a spinor
turned all the way round is negated and points the same way.

`rotate` turns a value by a fixed angle, in radians. `freq_shift` moves
every frequency in a signal up by `hz` Hz (down if negative), with no
mirror image, unlike a ring modulator: the analytic signal turned by a
spin. Hz are added, not multiplied, so harmonics stop being harmonic.
It returns a value, the shifted signal in quadrature, and what hears
it decides: played, its sides are the shifted sound a quarter turn
apart, and summed to mono it doesn't cancel; an SDR takes it as IQ.
Clean above about 30 Hz at 48 kHz, where the Hilbert pair holds its
quarter turn: measured, its sides are a quarter turn apart within 0.7
degrees and the same size within 1% from 30 Hz to 20 kHz (tests/hilbert).
The pair's band is a fraction of the rate, clean above about `sr / 1600`
and up to Nyquist: above about 1.5 kHz at an SDR's 2.4 MHz, the whole
sampled band as an IQ band wants it. Before 2026-11-05 it wasn't: the
stages used Niemitalo's coefficients unsquared and missed his one-sample
delay, so the pair was 4.5 degrees off at 441 Hz and 85 at 10 kHz, and
`analytic`, `freq_shift` and everything turned by them were spins only
below a few hundred hertz.

### Damped oscillator

```
def dho(force, freq, damp)                # x'' = force - 2 damp w x' - w^2 x
def resonator(signal, freq, decay)        # a signal through a resonance
```

`dho` is the spring every resonance is (`w = TAU freq`). Sweep `damp`
and it walks through sustained sine (0), bell (~0.005), pluck (~0.2)
and thud (~0.9); `force` is an acceleration, so a strike is
`impulse(freq) / dt` times its size (about 2000 for unit amplitude at
audio frequencies; see "Physics" above), and a constant force stretches it to about `force / w²`
(within 0.03% below 500 Hz; short of it towards Nyquist, since each
sample's force is a strike at that instant, which keeps a strike and the
resonance itself exact: PHASORS.md, "The push"). It returns
its phasor, the value `(x, q)`: `x` is the position, the sound; `q` the
position a quarter turn behind. It turns the way `spin` does, at
`wd = w · sqrt(1 - damp²)`, its length is how far it swings, and
`amplitude` and `phase` read it whole (see "Read the whole value").

Each sample the phasor turns by `wd · dt` and shrinks by
`exp(-damp · w · dt)`, then the force pushes it by `force · dt / wd`:

```
$z = $z * (exp(-damp * w * dt) * (cos(wd * dt), sin(wd * dt))) + (0, -force * dt / wd)
```

That is the spring's motion sampled exactly, not stepped towards, so
the pair is a circle at every frequency up to Nyquist and never runs
away (the phasor filter: PHASORS.md). An oscillator turns, so `damp`
stops just under 1; past critical damping a spring only creeps back,
and `swell` or `slew` is that tool. `q` is the velocity only when
undamped; the velocity takes both sides, `v = -(wd · q + damp · w · x)`.
Given a pair where it takes a number it makes a pair of pairs, one
phasor a side: `dho(push, [f, f * 1.0013], damp)` is a doublet, the two
modes of one partial, as one value ("Places and values"). Two of them, or
`resonator`, make a stereo pair.

`resonator` is a `dho` driven by a signal and scaled so a steady signal
comes out at its own level, returning the position: `decay` is the
damping rate per second (`2 damp w`), larger dies faster. A number in,
a number out, so `resonator(strike, [440, 442], 0.5)` is a bell whose
sides beat.

### Self-sustaining oscillator

`vdp(freq, mu = 0.1, push = 0, start = 0.01)` is the Van der Pol
oscillator, `x'' = mu w (1 - 4x²) x' - w² (x - push)`: it feeds energy
in while small and takes it out while large, so from `start` it grows
to a circle of radius 1 and holds it, as a bow, a reed or lips do.
`dho` and `resonator` ring only while something drives them; `vdp`
sounds by itself, with an attack its physics shapes (about 80 ms at
220 Hz and `mu` 0.1). `mu` walks from a sine (0.1) to a buzzing
relaxation tone with odd harmonics (3: 220 Hz sounds at about 168 Hz,
as the physics says). `push` moves its rest point: a hand, a signal,
or a spring to another `vdp`, and two detuned ones pushed toward each
other lock to one pitch (COMPOSING.md, "Voices that lock"). Returns
its phasor `[x, -v / w]`, the position and the position a quarter turn
behind, turning the way `spin` does; round when `mu` is small.

### Spectral synthesis (built on `sum`)

```
def additive(freq, shape, max_n):
  sum(max_n, n =>
    let pf = n * freq
    if pf >= 24000 then 0
    else shape(n, pf) * sin(TAU * phasor(pf)))

def inharmonic(freq, ratio, amp, max_n):
  sum(max_n, n =>
    let pf = freq * ratio(n)
    if pf >= 24000 then 0
    else amp(n, pf) * sin(TAU * phasor(pf)))
```

`additive` builds timbres from partials at integer
multiples of `freq`. `inharmonic` uses user-supplied
frequency ratios for bell, piano, plate, stiff-string
textures.

Shape functions `(n, pf) → amplitude` for `additive`:
`saw_shape`, `sqr_shape`, `tri_shape`, `warm_shape`,
`bright_shape`, `bowed_shape`, `vowel_ah`, `vowel_ee`,
`cello_shape`.

Ratio functions `n → multiplier` for `inharmonic`:
`stiff_string`, `stiff_cello`, `bar_partials`,
`plate_partials`, `phi_partials`.

Amp functions `(n, pf) → amplitude` for `inharmonic`:
`soft_decay`, `bell_decay`, `bright_decay`.

See GUIDE.md "Spectral synthesis" for worked examples.

### Physical instruments

Excitation-response physics for sounds whose identity is in HOW
they respond to being struck, plucked, or bowed. Read each def's
body in `stdlib.aither` to see the pattern; each mode is a
`resonator`, so the partials stay on their pitches at any rate.

| Function                                    | Description                                    |
|---------------------------------------------|------------------------------------------------|
| `pluck_string(strike, freq, brightness)`    | Karplus-Strong: delay + LP feedback            |
| `bowed_string(bow, freq)`                   | 8-mode bank, continuous excitation             |
| `struck_bar(strike, freq)`                  | 5-mode bank with mode-dependent damping        |

These are recommended for plucked / bowed / struck sounds where
additive's static spectrum + bolted-on envelope misses the
character. See COMPOSING.md "Physical instruments" for the
paradigm-choice rationale.

### Helpers

```
def gain(signal, amount): signal * amount
def fold(signal, amount): ...             # triangle wrap
def ease(x): ...                          # smoothstep: canonical fade curve
def prev(signal):                         # one-sample memory
  $last = 0.0
  let out = $last
  $last = signal
  out
def slew(signal, time):                   # follow, catching up in about `time` s
  if time > 0 then lp1(signal, 1 / (TAU * time)) else signal
def tremolo(signal, rate, depth): ...     # amplitude wobble, depth 0..1
```

`slew` is `lp1` with its time constant written instead of its cutoff.

### Character effects

- `drive(signal, amount)` — soft-clip saturation
- `wrap(signal, amount)` — hard wrap to ±1
- `bitcrush(signal, bits)` — bit-depth reduction
- `downsample(signal, rate)` — sample-rate reduction via hold
- `dropout(signal, rate, duty)` — periodic gate

### Envelopes

- `pluck(trig, decay_sec)` — percussive, fast attack + expo decay
- `swell(gate, attack, release)` — AR envelope, asymmetric
- `adsr(gate, a, d, s, r)` — classic ADSR

### Stereo

- `pan(signal, pos)` — equal-power pan, pos in [-1, 1]
- `haas(signal, ms)` — 1-30 ms delay on one channel for width; mono in (both sides start equal) or a pair
- `width(signal, amount)` — mid-side width (0 mono, 1 unchanged, >1 exaggerated)
- `mono(signal)` — collapse to single channel

### Raster (`tv`)

- `pixel(w, h)` — the pixel the current sample fills on `tv --size WxH`,
  as a pair: its centre, x across −1..1, y up, square pixels (y spans
  ±h/w)
- `pixel_index(w, h)` — its raster index, 0 .. w·h − 1 (rows top to
  bottom, each left to right), for state arrays
- `frame_number(w, h)` — the frame it is in, from 0: for what changes
  once a frame (a zoom, a morph, which of two state arrays holds last
  frame's values)

All three follow the engine clock (`t`), so they line up with the samples
`tv` counts whatever was sent when; the patch and `tv` must agree on
the size. What the patch returns for a pixel is a complex number: `tv`
shows its phase as hue and its amplitude as brightness, full at 1.
See TV.md.

### Polyphony

- `poly(n_voices, voice_fn, tail = 0)` — fan a per-key synth across N voices
- `midi_keyboard(voice_fn)` — `poly(8, voice_fn)`, the front-door form

`voice_fn` is called with everything known about a key:
`(freq, gate, vel, key, press, slide)`, Hz (bent by the note's own
bend), 1 while held, the velocity it was struck at, the MIDI key
number, its pressure and its slide (CC 74), each 0..1: on an MPE
controller every note has its own, on a keyboard every note shares the
channel's. A lambda names as many as it
uses (a lambda may take fewer arguments than it's called with), so
`(freq, gate) => ...` works and `(freq, gate, vel) => ...` is
velocity-sensitive. Each held key drives one voice independently;
sums of phasor-based synthesis stack at codegen time.

```
play piano:
  midi_keyboard((freq, gate) =>
    additive(freq, warm_shape, 8) * adsr(gate, 0.01, 0.2, 0.7, 0.4))

play pad:                              # louder the harder it's played
  midi_keyboard((freq, gate, vel) =>
    additive(freq, warm_shape, 8) * swell(gate, 0.3, 1.5) * vel)
```

**A voice that has finished stops.** A released note's slot keeps its
note so its tail can ring, and before 2026-11-05 its voice ran on after
the tail had died, until another note took the slot: sixteen notes played
and let go cost nearly half an A53 for the rest of the set. Now a voice
that has finished computes nothing and costs what an empty slot does
until a note-on in its slot wakes it. Finished is the voice's own, kept
in its own state in each patch (two patches holding one note keep their
own tails, and nothing is written back to MIDI's table, which another
thread owns), and `tail` says when:

| `tail` | The voice finishes |
|--------|--------------------|
| `0` (default) | released, and its sound has stayed 80 dB under its own peak since the note began, for 100 ms. Relative, so a loud kick, a quiet pad and a radio signal at 1e-6 all ring out to the end and no further; an absolute threshold would cut a weak signal off |
| seconds | that long after release, whatever it sounds like: for a voice that goes quiet and comes back (a delay, a pre-delay, a gated texture) |
| `-1` | never: every voice ever played runs on, as before |

Measured on one A53 (an eight-partial voice through a filter): sixteen
notes released cost 6% where they cost 30%; sixty-four empty slots 2.5%;
four held notes 11% where they cost 10%, the bookkeeping's price. The
voice's code is inlined through one more def than before, so a voice's
`noise()` draws a different stream (as an edit there would): the same
noise, other numbers.

---

## State semantics

### Top-level `$state`

Keyed by name. Safe to reorder. Shared by all plays and
defs that reference the name.

### `$state` inside `def`

Keyed by call-site counter. Each call location gets
independent state.

```
osc(sin, 440) + osc(sin, 880)
# two calls to osc → two independent phasors
```

Counter resets to 0 at the start of each sample. Same
call order = same state slots = phase continuity.

On hot-reload: same call order preserves state. Reordering
calls may cause phase discontinuity (not a crash).

### `$state` inside a `def rec`

One slot per call site, as in any def, shared by every step of
the recursion: state belongs to the code, not the iteration.
`$k = $k + 1` counts every step, `noise()` gives a fresh number
each step, and an `osc` advances each step, which is rarely
what you want. `sum` and `each` are the opposite: they unroll
into copies, and each copy owns its state.

### `$state` inside `play`

File-level by name (same slot if two plays share a name).
For per-play independence, use unique names or move state
into a local `def` called from the play.

### `$state` inside lambda body

Per-iteration when the lambda is unrolled by `sum(N, ...)`.
Each iteration claims its own slot with the same call-site
counter mechanism the existing per-iteration `phasor` /
`delay` state already uses. The first `$x = init` in a
lambda body is the declaration; subsequent `$x = expr`
lines assign — sequential update reads the just-written
value, so `$dx = $dx + …; $x = $x + $dx * dt` integrates
the way physics expects.

---

## CLI

The bare command is the engine: `./aither [patch ...] [seconds]`
runs the voices it is given (and any sent later) and writes the mix
to standard output as raw interleaved float32 pairs, no header, at
`AITHER_RATE` (48000). It refuses a terminal on stdout; pipe it into
a player, a scope, an SDR or a file. With `seconds` it runs that
long from `t = 0` and exits, offline, with no socket; without, it
runs until `kill` and accepts the commands below over a UNIX socket
(`AITHER_SOCK`, default `/tmp/aither.sock`). `input()` reads the
same format from standard input when it is a pipe. Live, the engine
reads that pipe whatever the patch does; offline (with `seconds`)
and under `audit`, it reads standard input only if the patch calls
`input()`, so a render started with an idle pipe on stdin (over
`ssh`, from a script) doesn't wait. A live engine given an idle pipe
waits for it: run it with `</dev/null`. **Input that runs out ends a live
engine**, as a filter's does: once audio has come in on stdin and its source
ends, the engine writes the block that last input made, says so and exits,
so `./aither fx.aither < take.raw > wet.raw` is as long as the take (give
`seconds` for a tail to ring past it). Input that never came, `</dev/null`
or a pipe closed before anything arrived, is silence and plays on. The engine is
lockstep on the pipe: it reads one block of input, then writes one
block of output, and holds nothing back, so a program on both ends
(`tools/card`, which runs a command between a sound card's input and
output) gets a fixed loop delay. A block is `AITHER_BUFFER` frames
(512).

The pipe out holds one block too. When standard output is a pipe the
engine sizes it, before its first write, to one block or one memory
page (4 KiB, 512 frames), whichever is bigger. Left at Linux's 64 KiB
it filled, and every sample waited behind the rest: 180 ms on the way
to `tools/play` at 44.1 kHz. Sized, the pipe holds at most 11.6 ms, and
what you play is heard one block, the pipe and the card's buffer later
(about 35 ms with `tools/play hw:… 20`). A reader that wants slack
against a stalled engine (a network stream) asks for it with a bigger
block: `AITHER_BUFFER=2048` makes the pipe 2048 frames.

A run on the command line can start with only some plays heard, from
its first sample: `--solo a,b` silences every other play, `--mute a,b`
silences those, as `solo` and `mute` do (they keep computing, and the
final line still runs). A play `--solo` leaves out is parked instead,
costing nothing, when nothing else reads it but its sound: silent
either way, and what is heard is the same to the bit. One a heard play
reads (its `$state`, say) runs on silent; the engine says which. A name
that isn't a play stops the engine. So one instrument can go down a
pipe of its own, at about its own cost:

```
./aither song.aither --solo horn | tools/fft 1024 \
  | AITHER_SOCK=/tmp/s.sock ./aither spectral.aither | tools/ifft 1024 | tools/play
```

`tools/fft N` writes a stream's spectrum as a stream of pairs at the
same rate, a frame of N / 2 bins every N / 2 samples: pair k is bin k's
`[re, im]`, and pair 0 is `[bin 0, bin N/2]`, the two that are real. A
patch reads it through `input()` a bin at a time: `pixel_index(N / 2, 1)`
is the bin, and an array stepped `every = N / 2` steps once a frame.
`tools/ifft N` turns it back, the signal N / 2 samples later, whatever
the engine's block. Such a patch starts with the stream, on the command
line. `tools/fft N FILE FILE ...` hears several streams at once: a
frame holds each one's lowest N / 2 / K bins, one after another (pixel
s·N/2/K + k is stream s's bin k), so a patch hears one sound through
another, as `patches/spectrum_cross.aither` does; each stream's band
stops at a K-th of the half rate. `tools/add [--delay M] FILE ...` adds streams in step. See
notes/spectrum-2026-11-06.md.

An offline run takes its keys and knobs from a script with `--play`:

```
./aither cello.aither 10 --play moves.txt > take.raw
```

Each line of the script is one move at a time in seconds, as MIDI
plays it, 0..127, on channel 1 unless a channel (1..16) follows:

```
0.0  note 60 100      # key 60 down at velocity 100
1.5  note 60 0        # velocity 0: let go
2.0  cc 21 64         # knob 21 to the middle
2.5  note 64 90 2     # on channel 2, as an MPE controller plays a note
2.6  bend 9000 2      # channel 2's pitch bend, 0..16383, 8192 centred
2.7  press 80 2       # channel 2's pressure
```

`#` starts a comment; the lines may come in any order. Each move lands
on its own sample (the block is cut short there) through the writers the
live MIDI thread calls, so `midi_freq`, `midi_gate`, `midi_trig`,
`midi_cc` and the polyphonic voices read a scripted run exactly as they
read a played one. It is how a patch played from the keyboard or the
gamepad is tested, measured and rendered without hands. Offline only,
and the run starts with every key up and every knob at 0, as a fresh
engine does.

A script is a record of playing, never a place to compose: a time, a
kind, its numbers and a channel a line, with no loops, variables, repeats or
includes, and it stays that way (PHILOSOPHY refuses a pattern DSL).
Sequencing written as music belongs in the patch, as signals (`wave`,
tables, `sweep`, `tape`); sequencing from another program (a DAW, a
tracker, a script) reaches aither as MIDI, live, or as a script it
wrote, offline. Being plain is what lets any tool write one.

### Voice-level

| Command                          | Description                                |
|----------------------------------|--------------------------------------------|
| `send <file> [fade] [--mute <play>,...]` | load / hot-swap a patch, then report level, quiet parts, guard trips; `--mute` names plays to start silent |
| `stop <voice> [fade]`            | fade out & remove                          |
| `clear [fade]`                   | stop all voices                            |
| `list`                           | show active voices and their parts         |
| `status`                         | `list` plus the master level               |
| `sweep <voice> [seconds]`        | where the voice's sweep is; with seconds, set it (0: from the top) |
| `kill`                           | shut down engine                           |

**An error never stops the music.** A `send` whose patch doesn't parse or
compile is refused with the error, and the version playing plays on; one
that compiles but blows up within 20 s rolls back to it. So a set is one
engine, started once, and every change a `send` to it: a mistake costs
nothing but the edit.

`send`'s `--mute` names plays of the patch, commas between and no
spaces (`send harbour.aither 3 --mute tension,danger`). A voice that is
new, or starting again, has those plays at gain 0 from its first
sample, so a layer a program brings in later (`unmute
harbour.tension 4`) is never heard before it is wanted: a `mute` sent
after the `send` would land a block or more late. On a voice already
playing, the edit lands and those plays fade out over the send's fade
(20 ms when left out). A name the compiled patch has no play for is an
error, as a compile error is: nothing is loaded, and the version
playing carries on. Anything after the file that is neither the fade
nor `--mute` is an error.

### Mute, solo, park

Each takes a target, a voice (`bass`) or one of its plays
(`bass.drone`), and a fade in seconds (20 ms when left out). A voice
and a play mean the same by each word:

| Command                   | Description                                   |
|---------------------------|-----------------------------------------------|
| `parts <voice>`           | list parts: state, gain, rms, peak            |
| `mute <target> [fade]`    | fade to silence; it keeps computing, so its LFOs, sequencers and sweep stay in step |
| `unmute <target> [fade]`  | fade back in (a parked one runs again too)    |
| `solo <target> [fade]`    | mute the others: every other voice, or the voice's other plays |
| `park <target> [fade]`    | fade out, then stop computing it: no CPU, state frozen where it was |
| `unpark <target> [fade]`  | run it again from where it stopped, fading in |

A play's name has no dot, so the last dot splits a target; a voice
whose file name has a dot (`bass.v2`) is found by its whole name
first. A fade that isn't a number is an error, which catches the old
two-word form (`mute bass drone`). `send` to a muted or parked voice
hot-swaps it and leaves it so; `stop` and `clear` remove it.

There are no special part names: a part called `scope` is a part
like any other and is heard. The oscilloscope is a patch whose final
expression is the trace, piped into a scope program
([SCOPE.md](SCOPE.md)). The live stream's container still runs the
`scope` branch, where `play scope:` / `play beam:` were drawn and not
heard; `patches/didge_scope.aither` is written for that engine.

### Observability

| Command                          | Description                                         |
|----------------------------------|-----------------------------------------------------|
| `check <voice>`                  | level, quiet parts, rollbacks and guard trips; parts the final expression never reaches (NOT IN THE FINAL EXPRESSION, a compile-time fact, also shown by `parts` and `audit`); QUIET VOICE when the loudest heard part is healthy but the voice's output is 30 dB or more below it; how it runs as tasks (how many, on how many cores, the largest by the compiler's estimate), and each task of parts kept together that holds a tenth of the voice or more, with why: a loop (named by the values read from below their writer), both write the same `$x`, or one reads another's array |
| `profile <voice> [seconds]`     | CPU for `seconds` (2): every play and top-level statement, by line (`play organ (line 486)`, `let roomD (line 755)`, `out` for the final expression), as a share of one core and of the voice, heaviest first; the cost of the timing itself is measured and taken off. Timing is on only while asked for; off, the code checks a flag per statement and reads no clock |
| `levels [voice]`                 | per-voice RMS / peak / clips / envelope sparkline   |
| `levels master`                  | master-bus stats (the mix as written to the pipe)   |
| `spectrum [voice]`               | FFT analysis of voice's recent buffer (or master)   |
| `try <patch> [seconds] [--from S]` | render the patch as a send would land it now: the live voice's state and time |
| `audit <patch> [seconds] [--from S]` | render offline from t=0: spectrum + per-part levels; raw pairs on stdin feed `input()`. At `AITHER_RATE` like the engine (44.1 kHz on the uConsole), 48 kHz when unset; `make corpus` stays at 48 kHz so fingerprints match on every machine |

Clips counters clear on read. `spectrum` runs against the
engine's last ~0.5 s of audio per voice; `audit` is fully
offline (no engine connection required, ~100 ms turnaround).

### Export

```
AITHER_RATE=44100 aither export country.aither out/        # out/country.c, country.h, aither_rt.c, aither_rt.h
cc -O2 -ffp-contract=off game.c out/country.c out/aither_rt.c -lm
```

`aither export <patch> [dir] [--name n]` writes a patch as C to build
into any program: a game, a plugin, a board. It is the C the engine
compiles the patch to, not a translation of it, so built with
`-ffp-contract=off` it plays the engine's samples bit for bit (the
`export_exact` test checks two patches, one played from a script with
keys, a 14-bit knob and `input()`). Without the flag the arithmetic may
fuse and the sound is the same to the ear, not to the bit. It is C11
(the MIDI state is `_Atomic`) as gcc and clang take it (one GNU range
initializer; `-pedantic` warns), needing only `<math.h>`, `<stdlib.h>`,
`<string.h>` and `<stdatomic.h>`; `#line` marks in it point a compiler's
complaints at the patch's own lines.

The patch is written as `<n>.c` and `<n>.h`, `n` the file's name made an
identifier (`--name` chooses), every one of its names prefixed with
`n`, so any number of patches go into one program. Its interface:

| Call | Does |
|------|------|
| `n* n_new(void)` / `n_free(v)` | an instance from t = 0, state fresh; instances are independent |
| `n_render(v, out, in, frames)` | the next `frames` samples as interleaved float pairs, the engine's own format; `in` is what `input()` hears, the same shape, or NULL |
| `n_part(name)`, `n_part_name(i)`, `n_PARTS` | the plays, by index |
| `n_gain(v, part, gain, seconds)` | fade a play to a gain (a mute is 0); it runs on silent, as a muted play does |
| `n_time(v)`, `n_sweep(v, seconds)` | `t`, and move the sweep |
| `n_RATE` | the rate it was exported for |

The code is fixed to its rate (`AITHER_RATE`, 48 kHz when unset): a
delay's buffer and a hoisted coefficient are sized for it, so a patch for
another rate is exported again.

Beside it go `aither_rt.c` and `aither_rt.h`, the runtime: the engine's
own natives (`dsp.c`) and MIDI state (`midi_state.c`), written out from
the binary, one per program however many patches it holds. Keys and
knobs reach every exported patch through it, as every voice in an engine
reads one MIDI state: `aither_midi(bytes, len)` (one raw MIDI 1.0
message, on its channel: a plugin's or device's MIDI as it comes, MPE
included), `aither_note_cc(note, k, value)`, `aither_note_bend(note,
semitones)` and `aither_note_pressure(note, value)` (a held note's own
values, by key, at full resolution: one instance's, for a game),
`aither_cc(n, value)`, `aither_cc14(n, value)` (a
14-bit pair, 0..16383), `aither_note_on(note, velocity)`,
`aither_note_off(note)`, `aither_midi_reset()`. Each lands on the next
sample rendered; to land one mid-block, render up to it first.

What the engine does around a voice and an export doesn't: it has no
socket, no hot reload, no fades between versions, no recursion
deadline (its host owns the clock, so a recursion runs to the end it was
written with), and no scrubbing of state after a NaN (a sample that
isn't a number is played as 0, as in the engine, but the state isn't
reset). Instances of one patch read the same knobs; per-instance
controls are not there yet.

---

## Implementation

### Parser (`parser.c`, ~840 lines; `imports.c`, ~170)

Tokenizer with group-depth handling (newlines inside `(...)`
and `[...]` are whitespace) and line continuation (a newline
ends a statement only where one can end); indentation is
skipped, never measured. Recursive-descent parser → syntax tree; a
def or play body ends at its expression. Handles: literals, identifiers,
operators, function calls, `$state`, `let`, `def`, `play`,
`lambda`, `if/then/else`, `else if` chains, `|>`, arrays,
array indexing, comments. Precedence climbing for expressions.

### Codegen (`codegen.c`, ~2700 lines)

Syntax tree → C source. Every value is emitted as its
two sides, pure C expressions; what has state is a statement
leaving its result in a temporary. Each patch transpiles to a single C source
string containing `tick(state, t)` and `init(state)`. The C
is compiled in-process by TCC (~milliseconds). Per-helper-type
state region tracking lets hot reload migrate state across
edits — a new oscillator inserted in the middle doesn't shift
the storage of every state slot after it.

`sum(N, lambda)` is a special form: walks the lambda body N
times with `n` substituted, emitting N parallel C expressions.
A sum whose index only picks a MIDI voice slot (`poly`'s, so
`midi_keyboard`'s) is emitted once instead, as a C loop over the
voices: each voice's state at its own place in the pool, laid out
as the unrolled copies are, its per-block values in arrays a voice,
the voices added in the same order, so the bits are the unrolled
sum's (`AITHER_LOOP=0` unrolls it). Nothing in the language
changes; the C of a keyboard patch is several times smaller, so its
`cc -O2` build lands sooner (notes/loops-2026-10-30.md).
An `each`, and an array's rule, is a C loop over its places in the same
way wherever its index is only ever a run-time number: where it, or
what is made from it, goes only into arithmetic, comparisons, `if`,
maths, a value, a `let`, a store, the index of a state or local array,
or a def whose body passes the same test. A power whose exponent comes
from the index (`step ^ k`) is computed as the compiler folds one, the
products for a whole power from 0 to 4 and `pow` otherwise. The stores
an `each` holds back go through scratch arrays, one a store, and land
after the loop, place by place, in the copies' order. A rule's element
is computed in its loop when the array is born, not hoisted to every
block. Anywhere else (a count, an array's size, a built-in that holds
state, a nested `sum` or lambda, a field's calls) it unrolls as before.
Every place's bits are the unrolled copies' (`AITHER_LOOP=0` unrolls
all of it): `patches/field_ear.aither`'s hundred resonators went from
1.2 MB of C and a minute of gcc to 43 KB.
Numeric literals propagate through `let` and `def` parameters
so `additive(f, shape, 16)` resolves `max_n = 16` at compile
time.

### Voice (`voice.c`, ~660 lines)

Owns the TCC compilation, dlopen of the resulting library, and
the per-voice float64 state pool, allocated after the voice's struct
at the size its patch reports (`voice_pool_slots`). Hot-reload commits a new
compile under a brief mutex, migrates state by region identity
(skips NaN-poisoned regions to recover cleanly).

### Native DSP (`dsp.c`, ~330 lines)

Filters, delays, tape, reverb, impulse, discharge, wave, the Hilbert
pair behind `analytic`. Each function claims state slots from the per-voice pool.
Pool access is bounds-checked — overflow degrades to a shared
safe slot rather than segfaulting.

### Engine (`engine.c`)

The mix, a period at a time, to standard output on its own thread;
`input()` from standard input. Each voice runs a block at a time
inside its compiled code (`tick_block`: the clock, `input()`, the
parts, the voice fade and part-gain ramps, park and the part meters
all advance per sample in C); the engine's own work is per block,
and the output is bit-identical to ticking one sample at a time
(`tests/block_tick.c`). Each voice starts on TCC's code and swaps to
a `cc -O2` build of the same C when that's ready, keeping its state
(the live engine builds at `nice 19`, so the audio wins any core both
want; offline renders build at normal priority);
the `-O2` build doesn't fuse multiply-adds (`-ffp-contract=off`), so
both compute the same bits and the swap can't move a voice off its
trajectory (`make exact` renders every patch both ways and compares). The stream is gapless: the mix waits for the
voice table's lock rather than skipping a block, so every sample
written is one tick of `t`, whatever the control socket is doing. A
command waiting for the lock goes first: the mix yields to it before
its next block, so a `send` or `park` answers in milliseconds even
when an overloaded voice keeps the mix running flat out. `tv`
and `scope` count pixels and points by the samples they receive, and
rely on it (`tests/engine_live.c`). Values that can't change within a block are
computed once per block instead of every sample: a number, `sr`,
`dt`, `TAU`, a knob (`midi_cc(k)`) and what MIDI holds for a note or
a voice (`midi_freq`, `midi_voice_freq(n)`, its gate, velocity and
key: the input thread's writes are read once a block), an entry of a
constant table at a compile-time index, a let or def parameter bound
to one, arithmetic and pure math over those, and a call to a def whose
body is one expression of its parameters, given such arguments (a def
sees only its arguments, so `def bandf(i): 50 * pow(2, i / 8)` called
as `bandf(n - 1)` in a `sum` is computed once a block, as the
expression written out would be). That covers `lp1`/`hp1`'s `exp`, the SVF's
`tan`, `reverb`'s `pow`s and `phasor`'s division when their
coefficients are invariant, and any `exp`/`log`/`pow`/`sin`/`sqrt`
... over invariant inputs, such as a knob-driven spectral morph's
per-partial `exp`. Same arithmetic once, so the same bits
(`tests/hoist.c`); a modulated value keeps the per-sample path. A
knob is read at the start of each block, so a knob moved mid-block
lands at the next one (11 ms at 512 frames). `AITHER_HOIST=0` turns
it off.

A sine of a steady clock, `sin(TAU * phasor(f))` or `cos` (inline, or
through a let, as `spin` and `osc(sin, f)` write it) with `f` steady
over a block, is a point turning by a fixed angle each sample, and the
compiler writes it so (a `turner` region): a pair turned by
`[cos, sin]` of `TAU · f / sr`, computed once a block, with its length
put back to 1 each sample. Four multiplies instead of a sine, and the
same wave to rounding (within 1e-9). Where `f` moves within a block
(vibrato, FM) the phasor and the sine stay. `AITHER_TURN=0` turns it
off (PHASORS.md, "Steady sines turn").
Cores are chosen from those the process was started on (its affinity
as it began, so `taskset -c 0-3 aither` keeps the engine, its workers
and its compiles on cores 0-3 and leaves the rest to other programs),
all online cores if none of those is online. The live mix thread runs
on the audio core (the highest-numbered of them online,
`AITHER_RT_CPU` to choose) at `SCHED_FIFO` priority
`AITHER_RT_PRIO` (70), with memory locked when `RLIMIT_MEMLOCK` is
unlimited, and is pinned again about once a second as cores come and go;
`tools/play` and `tools/card` do the same. Without the `rtprio` limit it
says so and runs at normal priority; `AITHER_RT=0` turns it off. Offline
renders don't. See notes/realtime-2026-10-19.md (`realtime.c`).
A voice runs as tasks, cut where its parts can run apart (a `$state`
written twice, an array, or a loop between them keeps them together),
merged by cost for the cores it runs on, each task a block at a time.
The engine runs them on a pool (`pool.c`): `AITHER_THREADS` threads in
all, counting the mix thread (default: the fastest of those cores online, within
3/4 of the best by the kernel's `cpu_capacity`: the uConsole's two A72s
on mains, its four A53s on battery). Each worker is pinned to a core of
its own, at the mix thread's priority. Any split, on any number of
threads, gives the same bits (`make blocks`); `AITHER_TASKS` sets the
cores the split is planned for (0: the finest). See
notes/multicore-2026-10-18.md.
Each voice's block has a deadline, `AITHER_WATCHDOG` seconds (0.5;
0 for none), which only a recursion can overrun: it checks the clock
every 65 536 steps, and past the deadline it gives up (see
Recursion).
UNIX socket server. Voice table
with per-part gain fades. Per-voice and master-bus rolling stats
(RMS, peak, clips, 50 ms envelope bins for sparklines, plus a
0.5 s float32 ring per voice for `spectrum`'s FFT). No limiter on
the master: the mix goes out as it is, because a scope or a radio
would read a `tanh` as distortion; a player clamps at ±1 for the
speaker's sake. Voice slots sweep on `send` so stopped
voices don't leak the table.

### MIDI (`midi_state.c`, `midi.c`)

`midi_state.c` is what the keys and knobs are doing (each held note's
channel, bend, pressure and controllers; each channel's; MPE zones), its
writers and the natives a patch reads, with no ALSA in it, so an
export's runtime carries it. `midi_message` is its one door for raw
MIDI. `midi.c` is the ALSA seq input thread that feeds it, each event
turned back into its bytes. Auto-resubscribes if the port is dropped.
Engine logs a clear line on drop / recovery so silent failure
isn't a debugging blind spot.

### CLI output (`cli_output.c`)

The engine takes snapshots of its state under the lock
(`VoiceSnap`, `StatsSnap`, `CheckSnap`, engine.h); formatters in
`cli_output.c` turn those into text. Lets engine state be
tested without parsing strings.

### Analysis + render (`analysis.c`, `render.c`)

`analysis.c` is pure FFT + spectral feature extraction (no
engine knowledge). `render.c` runs a patch offline through
the same parse → codegen → TCC → tick path the engine uses,
returning an in-memory buffer. Together they power
`./aither audit` and `./aither try`.

### Export (`export.c`, `export_text.c`, `export/`)

`aither export`: the patch's generated C with its names prefixed and an
interface after it, and the runtime, `export/aither_rt.h` with `dsp.h`,
`midi.h`, `dsp.c`, `midi_state.c` and `export/glue.c` (the public setters
and the two hooks the engine gives a voice), concatenated from text
`#embed`ded in the binary, their local includes dropped.

### CLI dispatch (`aither.c`)

Entry point. Either runs the engine in-process (`start`) or
sends a command over the socket. `audit` is the one offline
command — uses `render` + `analysis` + `cli_output` directly
without engine connection.

### Stdlib (`stdlib.aither`, ~325 lines)

Oscillator wrappers, spectral synthesis (`additive`,
`inharmonic`, plus the shape/ratio/amp library), physical
instruments (`pluck_string`, `bowed_string`, `struck_bar`), character effects, envelopes, stereo helpers,
prev. Pure aither code, baked into the binary as a const string.

### Totals

About 8,200 lines of C (the language, the engine and the CLI) and
460 of aither (the stdlib). One binary, built by `cc`; the tests are
C and aither too.
Runtime dependencies: libtcc, ALSA, the system audio library.

---

## Complete examples

### Minimal

```
play beep:
  osc(sin, 440) * 0.2

beep
```

### Acid bass

```
let notes = wave(2, [55, 55, 82, 55, 73, 55, 98, 55])
let env   = discharge(impulse(2), 8)

play acid:
  let s = osc(saw, notes) |> lpf(200 + env * 4000, 0.85) |> gain(0.4)
  [s, s]

acid |> drive(1.1)
```

### FM feedback

```
play fm:
  $fb = 0.0
  $fb = sin(TAU * phasor(440 + $fb * 300))
  $fb * 0.3

fm
```

The phase obeys Adler's equation, dφ/dt = 440 + 300·sin φ, so this
sounds at √(440² − 300²) ≈ 322 Hz. With a feedback of 440 or more the
pitch reaches zero at some phase and the oscillator locks there: a
constant, no sound (the example used 500 until 2026-10-18 and was
silent).

### Kick drum

```
play kick:
  discharge(impulse(2), 6) * resonator(impulse(2), 60, 8)

kick
```

### Looper

```
play loop:
  osc(saw, 110) |> fbdelay(0.5, 0.5, 0.9)

loop
```

A loop of the playing rather than the sound is a `tape` with a pedal
def over it; see COMPOSING.md, "Looping your playing".

### Drone with stereo pad

```
def ease(x):
  let c = clamp(x, 0, 1)
  c * c * (3 - 2 * c)

let pos    = sweep
let breath = (sin(TAU * pos / 20) + 1) * 0.5

play drone:
  let raw  = osc(saw, 55) + osc(saw, 55.1) + osc(saw, 54.9)
  let filt = raw / 3 |> lpf(400 + breath * 400, 0.4)
  filt |> haas(14)

play shimmer:
  let raw  = osc(sin, 440) + osc(sin, 441.2) * 0.5
  let wide = [raw, raw * 0.98] |> width(1.5)
  wide * breath * 0.08

(drone + shimmer) * ease(pos / 8) |> reverb(3, 0.3)
```

### Forward-feedback composition

```
let trig = impulse(0.3)

play src:
  let exc = noise() * discharge(trig, 80) * 0.3
  osc(sin, 220) * 0.2 + exc

play echo:
  prev(src) * 0.7             # one-sample-delayed src

(src + echo) |> drive(1.0)
```
