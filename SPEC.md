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
| array    | `[220, 330, 440]`          |

Float is the default, and a float is a pair whose two sides
are equal: every value is a pair (see "Pairs"), and a number is
the pair computed once. Function values exist so a shape can
be passed to a def (`osc(shape, freq)`, `sum(N, fn)`).
Arrays hold data: lookup tables, `wave` note lists,
delay/reverb/wavetable buffers, per-sample scratch
(`let a = array(N)`), persistent fields (`$f = array(N)`,
see "State arrays"), and a returned `[L, R]` stereo pair.
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

`t` is the one clock, shared by every voice. `sweep` is this voice's
own: 0 when it is first sent, running on through edits, and set with
`aither sweep <voice> <seconds>` (COMPOSING.md, "The composition
clock"). It is `t` less the moment it was last set: exact, and free
to move.

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
any other is `pow(x, y)`. On a pair it takes each side. There is no
bitwise xor, so `^` means nothing else.

Arithmetic is polymorphic for length-2 arrays (stereo pairs):

- `float + float → float`
- `float + [L, R] → [float+L, float+R]` (broadcast)
- `[L, R] + [L, R] → [L+L, R+R]` (element-wise)
- Any other array in arithmetic is a compile error: index it, or
  hand it to `wave`

Same for `-`, `*`, `/`, `^`.

`a mod b` is floored: it takes the sign of `b`, so `-1 mod 4` is 3,
and `mod 0` is 0. `%` is not an operator; written, it is an error that
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
(`let root = 293.66 * 2`), or a table of literal numbers,
like `TAU`. File-level `$state` slots, other `def`s and
globals are visible too. A file-level let that is a signal
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
numbers at a call are a pair, and a pair indexes like a
two-entry table (see Stereo below), so a def that indexes its
parameter takes `[220, 330]` as it takes `[220, 330, 440]`.
Defs don't return arrays; the pair is the only multi-value
return.

A def uses a parameter as a pair when it indexes it (`p[0]`),
hands it to a pair operation where that takes a pair (`turn(p, q)`,
`amplitude(p)`; `analytic`'s signal is a number),
passes it to a def that takes a pair there, or does
arithmetic with it and a pair (`turn(z, z) + c`), or names its sides
(`let [a, b] = p`); a `def rec`
that passes itself a pair there counts too. Such a parameter
takes a pair whole, and a number passed to it fills both
sides. A def returns a pair when its result is one: a literal
`[a, b]`, a pair call, a pair parameter, a `let` bound to a
pair, pair state (`$z = [0, 0]`), or arithmetic on one
(`def norm(z): z / amplitude(z)`).

`sum`, `each` and `array(N)` accept any compile-time number:
a literal, a name bound to one, `len(array)`, or arithmetic
over those (`each(n - 1, …)`).

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
  the def it replaces needs a name of its own.) `rec` on a def
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

Indexes are truncated to `int` and cycle like a constant table's
(`a[-1]` is the last element). A constant table (all literals) is
hoisted and read-only; storing into one is a compile error.

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
to a def is a pair array there. Indexing is as for numbers: a state
array reads `[0, 0]` past either end, a local array cycles. A
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

Unrolls like `sum` (so `N` must be a compile-time integer), with
`i` running from 0 to N−1 (an index; `sum`'s `n` runs from 1 to N,
a harmonic number), but the body is statements — element stores, `let`s, `$state` — and the
value is 0. Every store to a **state array** inside an `each` is
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
engine clock. It's `each`'s held-back stores, held back for M samples.
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

### Arrays are data, not a map

An array never fans a function out over its elements.
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

### Pairs

Every value is a pair, its two sides; a number is a pair whose
sides are equal, worked out once. So anything takes a pair:
**a function given a pair where it takes a number runs once per
side, each side with its own state.**

```
osc(saw, [55, 55.3])               # a detuned stereo saw: two phasors
lpf(stereo, 800, 0.5)              # two filters, one a side
resonator(strike, [440, 442], 0.5) # a bell whose sides beat
phasor([1, 1.01])                  # two LFOs drifting apart
```

A number given to the same call runs it once and both sides
read it. A def's own pair parameters (see "Array parameters":
`p[0]`, `turn(p, q)`, arithmetic with a pair) take a pair whole
instead, so `mono(x)` and `turn(z, z)` see both sides. A def that
makes a pair from a number (`pan`) given a pair on that number
would make a pair of pairs, and says so. A condition is one
number; an `if` runs only the branch it takes.

A pair indexes like a two-entry table: `p[i]` truncates `i`
and wraps at 2, so `p[0]` is the left side, `p[1]` the right,
`p[-1]` the right and `p[2]` the left, whether `i` is a
constant or worked out as it runs (`a[int(phasor(2) * 2)]`
alternates a part's sides). So a pair and a two-entry table
read alike wherever they meet: indexing, arithmetic (element
by element), and `wave`, which reads any array as a table.

`let [a, b] = p` names a pair's two sides where the patch reads
them, with names of its meaning: `let [pos, vel] = dho(...)`,
`let [l, r] = sig`, `let [i, q] = input()`. `_` names a side not
wanted; a number gives both names the number; two names, no more.

Operators work side by side: `+`, `-`, `*` and `/` on two pairs
act on each side (`sig * [0.8, 0.6]` is a gain a side). A pair is
read as a complex number only by the operations that turn it
(PHILOSOPHY, "Complex numbers as operations, not types"):

| Function        | Description                                         |
|-----------------|-----------------------------------------------------|
| `turn(z, w)`    | `z` turned by `w`'s angle and scaled by its size (complex multiplication); turning by a spin adds its speed |
| `conj(z)`       | the same pair turning the other way                 |
| `unturn(z, w)`  | `z` turned back by `w`'s angle and divided by its size (complex division); `unturn(turn(z, w), w)` is `z`, and by `[0, 0]` it is `[0, 0]`, as `x / 0` is 0 |
| `amplitude(z)`  | how big: `sqrt(z0² + z1²)`                          |
| `phase(z)`      | which way it points, -π..π                          |
| `analytic(x)`   | a signal as spins: `x` and its Hilbert transform, a quarter turn behind at every frequency |

Undoing a turn by a unit pair is turning by its conjugate:
`turn(turn(z, w), conj(w))` is `z`.

**Read the whole pair.** A pair from an oscillator, `analytic` or
`spin` is a phasor: two sides a quarter turn apart. Most of what is
wanted from it reads both sides at once, exactly and at every sample:
how loud is `amplitude(p)` (no rectifying and smoothing, which is how
one side has to get it); how fast it turns, its frequency, is
`phase(turn(p, conj(prev(p)))) / (TAU * dt)`; moving or mixing it is
`turn`; hearing it is the pair itself, its sides played a quarter turn
apart. Taking one side (`let [x, _] = p`) throws the other away, and is
for where one number is all there is room for, such as a mono mix.

Return `[L, R]` for stereo. Plays and the final expression
may return a float (mono, mirrored to both channels) or a
two-element array (stereo).

```
osc(sin, 440) |> pan(0.3)    # returns [L, R]
```

A mono part should return its float, not `[s, s]`: the
engine mirrors it, and the two spellings render
identically. Reserve the brackets for the moment the
channels actually differ.

Binary ops on length-2 arrays are element-wise (see
Operators). Mixing a stereo play with a mono play in the
final expression broadcasts the mono to both channels.

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

### Oscillators: a clock and a shape

An oscillator is not a primitive. It is a clock read
through a shape:

```
sin(TAU * phasor(220))
```

| Function      | Description                                  |
|---------------|----------------------------------------------|
| `phasor(freq)`| the clock: ramp 0→1 at freq Hz, with state   |
| `noise()`     | white noise; each call its own stream, the same every run, seeded by where the call is (its statement, the defs it is inlined through, which `noise()` it is there), so editing other code leaves it unchanged |

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
reverb per side.

**Physics**: `impulse(freq)`, `discharge(signal, rate)`.

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
| `midi_cc(n)`          | CC `n` value, `0..1`. `n` in `0..127`.           |
| `midi_note(n)`        | Velocity `0..1` while note `n` is held; 0 else. |
| `midi_freq()`         | Hz of the most recent note-on (mono).            |
| `midi_gate()`         | 1 while the most recent note is held, 0 after its note-off. |
| `midi_vel()`          | Velocity `0..1` of the most recent note-on; stays after the note-off. |
| `midi_trig(n)`        | 1.0 for a single sample on each note-on for `n`; 0 otherwise. Per-voice edge detect. |
| `midi_voice_freq(n)`  | Hz of the nth held voice (1..16); 0 if slot empty. Stable per held note. |
| `midi_voice_gate(n)`  | 1 while the nth voice's key is held (1..16), 0 after release or empty. |
| `midi_voice_vel(n)`   | Velocity `0..1` the nth voice was struck at; stays after release, 0 if never used. |
| `midi_voice_key(n)`   | MIDI key number of the nth voice, `0..127`; stays after release, 0 if never used. |

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

See "CLI" below for `aither midi list / connect / disconnect`.

---

## Stdlib (written in aither)

Embedded in the binary as a const string. Loaded before
every user patch. Source: `stdlib.aither`.

### Oscillator wrappers

```
def osc(shape, freq):  shape(TAU * phasor(freq))
def pulse(freq, width): if phasor(freq) < width then 1 else -1
def spin(freq):                           # the unit pair turning at freq Hz
  let a = TAU * phasor(freq)
  [cos(a), sin(a)]
```

`spin` is the unit pair turning at `freq` Hz: one phase, both sides.
Turn a pair by it to set it spinning; turning by a spin adds speeds,
so `turn(spin(220), spin(50))` is `spin(270)` and `turn(p, p)` doubles
`p`'s speed.

```
def rotate(z, angle): turn(z, [cos(angle), sin(angle)])
def freq_shift(signal, hz): turn(analytic(signal), spin(hz))
```

`rotate` turns a pair by a fixed angle, in radians. `freq_shift` moves
every frequency in a signal up by `hz` Hz (down if negative), with no
mirror image, unlike a ring modulator: the analytic signal turned by a
spin. Hz are added, not multiplied, so harmonics stop being harmonic.
It returns the pair, the shifted signal in quadrature, and what hears
it decides: played, its sides are the shifted sound a quarter turn
apart, and summed to mono it doesn't cancel; an SDR takes it as IQ.
Clean above about 30 Hz, where the Hilbert pair holds its quarter
turn.

### Damped oscillator

```
def dho(force, freq, damp)                # x'' = force - 2 damp w x' - w^2 x
def resonator(signal, freq, decay)        # a signal through a resonance
```

`dho` is the spring every resonance is (`w = TAU freq`). Sweep `damp`
and it walks through sustained sine (0), bell (~0.005), pluck (~0.2)
and thud (~0.9); `force` is an acceleration, so a strike is a big
number, and a constant force stretches it to about `force / w²`
(within 0.03% below 500 Hz; short of it towards Nyquist, since each
sample's force is a strike at that instant, which keeps a strike and the
resonance itself exact: PHASORS.md, "The push"). It returns
its phasor, the pair `[x, q]`: `x` is the position, the sound; `q` the
position a quarter turn behind. The pair turns the way `spin` does, at
`wd = w · sqrt(1 - damp²)`, its length is how far it swings, and
`amplitude` and `phase` read it whole (see "Read the whole pair").

Each sample the phasor turns by `wd · dt` and shrinks by
`exp(-damp · w · dt)`, then the force pushes it by `force · dt / wd`:

```
$z = turn($z, exp(-damp * w * dt) * [cos(wd * dt), sin(wd * dt)]) + [0, -force * dt / wd]
```

That is the spring's motion sampled exactly, not stepped towards, so
the pair is a circle at every frequency up to Nyquist and never runs
away (the phasor filter: PHASORS.md). An oscillator turns, so `damp`
stops just under 1; past critical damping a spring only creeps back,
and `swell` or `slew` is that tool. `q` is the velocity only when
undamped; the velocity takes both sides, `v = -(wd · q + damp · w · x)`.
Given a pair where it takes a number it would make a pair of pairs,
and says so: two of them, or `resonator`, make a stereo pair.

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
body in `stdlib.aither` to see the pattern; `tuning_fork` is the
canonical inline-integration reference.

| Function                                    | Description                                    |
|---------------------------------------------|------------------------------------------------|
| `tuning_fork(strike, freq)`                 | single damped HO; teaching reference           |
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

- `poly(n_voices, voice_fn)` — fan a per-key synth across N voices
- `midi_keyboard(voice_fn)` — `poly(8, voice_fn)`, the front-door form

`voice_fn` is called with everything known about a key:
`(freq, gate, vel, key)`, Hz, 1 while held, the velocity it was
struck at, and the MIDI key number. A lambda names as many as it
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
waits for it: run it with `</dev/null`. The engine is
lockstep on the pipe: it reads one block of input, then writes one
block of output, and holds nothing back, so a program on both ends
(`tools/card`, which runs a command between a sound card's input and
output) gets a fixed loop delay. A block is `AITHER_BUFFER` frames
(512).

### Voice-level

| Command                          | Description                                |
|----------------------------------|--------------------------------------------|
| `send <file> [fade]`             | load / hot-swap a patch, then report level, quiet parts, guard trips |
| `stop <voice> [fade]`            | fade out & remove                          |
| `clear [fade]`                   | stop all voices                            |
| `list`                           | show active voices and their parts         |
| `status`                         | `list` plus the master level               |
| `sweep <voice> [seconds]`        | where the voice's sweep is; with seconds, set it (0: from the top) |
| `kill`                           | shut down engine                           |

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
| `check <voice>`                  | level, quiet parts, rollbacks and guard trips; parts the final expression never reaches (NOT IN THE FINAL EXPRESSION, a compile-time fact, also shown by `parts` and `audit`); QUIET VOICE when the loudest heard part is healthy but the voice's output is 30 dB or more below it |
| `profile <voice> [seconds]`     | CPU for `seconds` (2): every play and top-level statement, by line (`play organ (line 486)`, `let roomD (line 755)`, `out` for the final expression), as a share of one core and of the voice, heaviest first; the cost of the timing itself is measured and taken off. Timing is on only while asked for; off, the code checks a flag per statement and reads no clock |
| `levels [voice]`                 | per-voice RMS / peak / clips / envelope sparkline   |
| `levels master`                  | master-bus stats (the mix as written to the pipe)   |
| `spectrum [voice]`               | FFT analysis of voice's recent buffer (or master)   |
| `try <patch> [seconds] [--from S]` | render the patch as a send would land it now: the live voice's state and time |
| `audit <patch> [seconds] [--from S]` | render offline from t=0: spectrum + per-part levels; raw pairs on stdin feed `input()`. At `AITHER_RATE` like the engine (44.1 kHz on the uConsole), 48 kHz when unset; `make corpus` stays at 48 kHz so fingerprints match on every machine |

Clips counters clear on read. `spectrum` runs against the
engine's last ~0.5 s of audio per voice; `audit` is fully
offline (no engine connection required, ~100 ms turnaround).

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
The live mix thread runs on the audio core (the highest-numbered core
online, `AITHER_RT_CPU` to choose) at `SCHED_FIFO` priority
`AITHER_RT_PRIO` (70), with memory locked when `RLIMIT_MEMLOCK` is
unlimited, and is pinned again about once a second as cores come and go;
`tools/play` and `tools/card` do the same. Without the `rtprio` limit it
says so and runs at normal priority; `AITHER_RT=0` turns it off. Offline
renders don't. See notes/realtime-2026-10-19.md (`realtime.c`).
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

### MIDI (`midi.c`)

ALSA seq input thread. Auto-resubscribes if the port is dropped.
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

### CLI dispatch (`aither.c`)

Entry point. Either runs the engine in-process (`start`) or
sends a command over the socket. `audit` is the one offline
command — uses `render` + `analysis` + `cli_output` directly
without engine connection.

### Stdlib (`stdlib.aither`, ~325 lines)

Oscillator wrappers, spectral synthesis (`additive`,
`inharmonic`, plus the shape/ratio/amp library), physical
instruments (`tuning_fork`, `pluck_string`, `bowed_string`,
`struck_bar`), character effects, envelopes, stereo helpers,
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
