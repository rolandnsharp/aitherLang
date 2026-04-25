# Aither Language Specification

A language for real-time audio signal processing.

A program is an expression evaluated at audio rate (48,000
times per second) that produces a single stereo sample. The
file defines named parts; the final expression is the voice's
output.

## Values

| Type     | Example                    |
|----------|----------------------------|
| float    | `440.0`, `0.5`, `-1.0`     |
| function | `sin`, `saw`, `my_shape`   |
| array    | `[220, 330, 440]`          |

Float is the default. Function values exist for
`osc(shape, freq)` (passing a shape function). Arrays exist
for polyphony (input), stereo output (length-2 arrays), and
delay/reverb/wavetable buffers.

## Globals

| Name      | Description                            |
|-----------|----------------------------------------|
| `t`       | time in seconds since engine start     |
| `sr`      | sample rate (48000)                    |
| `dt`      | 1 / sample rate                        |
| `start_t` | time when this voice was first loaded  |
| `PI`      | 3.14159...                             |
| `TAU`     | 6.28318... (2π)                        |

Use `pos = t - start_t` as the composition clock — seconds
since this voice started, independent of engine uptime.

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
| `-x`, `not x`  | unary                    |
| `*`, `/`, `mod`| multiplicative           |
| `+`, `-`       | additive                 |
| `==`, `<`, `>`, `<=`, `>=`, `!=` | comparison |
| `and`, `or`    | logical                  |
| `\|>`          | pipe (lowest)            |

Arithmetic is polymorphic for length-2 arrays (stereo pairs):

- `float + float → float`
- `float + [L, R] → [float+L, float+R]` (broadcast)
- `[L, R] + [L, R] → [L+L, R+R]` (element-wise)
- Other array shapes collapse to first element (legacy polyphony)

Same for `-`, `*`, `/`.

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

The last expression in the body is the return value.
Functions have their own local scope — they cannot see
file-level `let` bindings. File-level `$state` slots,
other `def`s, and globals are visible.

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
- A multi-arg lambda passed to a `def` is inlined at every call
  to that param: `def poly(n_voices, voice_fn): sum(n_voices, n =>
  voice_fn(midi_voice_freq(n), midi_voice_gate(n)))` — `voice_fn`
  is bound to the lambda the caller passed, not a runtime value.

Lambdas capture any variable in their enclosing lexical
scope — enclosing `let`s, `def` parameters, and globals all
work. Captures are free: the lambda body inlines into each
call site and resolves identifiers through the normal scope
walk.

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
part. Its body compiles inline into the main chunk (so it
sees all file-level lets and `$state` slots). Its value is
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
reads cycle rather than crash. v1 doesn't allow passing
arrays into `def` parameters or returning them from a `def`.

### Mutable arrays

```
$buf = array(48000, 0.0)   # size N, initial value
$buf[i]                    # read
$buf[i] = x                # write
len($buf)                  # length
```

Used for delay lines, reverb buffers, wavetables.

### Polyphony (arrays as input to functions)

When a function receives an array where it expects a float,
it runs once per element with independent state. Results
are **summed**:

```
osc(sin, [220, 330, 440]) * 0.3    # three oscillators → sum → mono
```

### Stereo (arrays as output)

Return `[L, R]` for stereo. Plays and the final expression
may return a float (mono, mirrored to both channels) or a
two-element array (stereo).

```
osc(sin, 440) |> pan(0.3)    # returns [L, R]
```

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

Outside brackets, newlines terminate statements as usual.
There is no backslash continuation yet; wrap a multi-line
expression in parens if you need to split it.

## Program structure

A file has four kinds of top-level declaration:

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

**`def` cannot see file-level lets** — defs are isolated
functions that take what they need as parameters. **`play`
can see file-level lets** — plays are inlined blocks that
share the file's scope.

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
| `log(x)`        | natural log     |
| `log2(x)`       | log base 2      |
| `abs(x)`        | absolute value  |
| `floor(x)`      | floor           |
| `ceil(x)`       | ceiling         |
| `min(a, b)`     | minimum         |
| `max(a, b)`     | maximum         |
| `pow(a, b)`     | exponentiation  |
| `sqrt(x)`       | square root     |
| `clamp(x, l, h)`| clamp to range  |
| `int(x)`        | truncate to int |

### Shapes (native)

| Function | Description                |
|----------|----------------------------|
| `saw(x)` | sawtooth: `-1 → 1`         |
| `tri(x)` | triangle: `-1 → 1 → -1`    |
| `sqr(x)` | square: `-1 / 1`           |

`x` is a phase in radians (0..TAU). Also usable as osc
shapes via `osc(shape, freq)`.

### Stateful primitives (native)

| Function      | Description                     |
|---------------|---------------------------------|
| `phasor(freq)`| ramp 0→1 at freq Hz, with state |
| `noise()`     | white noise, stateless          |

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

These are compiled Nim (in `dsp.nim`) called from the VM.
Each call-site gets its own state slot from a per-voice
float64 pool (4 MB).

**Filters**: `lp1(sig, cut)`, `hp1(sig, cut)`,
`lpf(sig, cut, res)`, `hpf(sig, cut, res)`,
`bpf(sig, cut, res)`, `notch(sig, cut, res)`.

**Delays**: `delay(sig, time, max_time)`,
`fbdelay(sig, time, max_time, fb)`.

**Reverb**: `reverb(sig, rt60, wet)` (Schroeder).

**Physics**: `impulse(freq)`, `resonator(sig, freq, decay)`,
`discharge(sig, rate)`.

**Modulation**: `tremolo(sig, rate, depth)`,
`slew(sig, time)`.

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
| `midi_gate()`         | Velocity `0..1` of the most recent note-on; 0 after note-off. |
| `midi_trig(n)`        | 1.0 for a single sample on each note-on for `n`; 0 otherwise. Per-voice edge detect. |
| `midi_voice_freq(n)`  | Hz of the nth held voice (1..16); 0 if slot empty. Stable per held note. |
| `midi_voice_gate(n)`  | Velocity of the nth held voice (1..16); 0 after release or empty. |

If no MIDI device is connected, all return 0 and the
patch still runs. There is no binding step — the patch IS
the routing. Example:

```
play bass: osc(saw, midi_freq()) * midi_gate() * 0.3
play kick: discharge(midi_trig(36), 30) * sin(TAU * phasor(60))
(bass + kick) * midi_cc(80)
```

**Polyphony**. Use `midi_keyboard` (stdlib) — it wraps
`midi_voice_freq` / `midi_voice_gate` over 8 voices via
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
```

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
def prev(x):                              # one-sample memory
  $last = 0.0
  let out = $last
  $last = x
  out
```

### Character effects

- `drive(sig, amount)` — soft-clip saturation
- `wrap(sig, amount)` — hard wrap to ±1
- `bitcrush(sig, bits)` — bit-depth reduction
- `downsample(sig, rate)` — sample-rate reduction via hold
- `dropout(sig, rate, duty)` — periodic gate

### Envelopes

- `pluck(trig, decay_sec)` — percussive, fast attack + expo decay
- `swell(gate, attack, release)` — AR envelope, asymmetric
- `adsr(gate, a, d, s, r)` — classic ADSR

### Stereo

- `pan(mono_sig, pos)` — equal-power pan, pos in [-1, 1]
- `haas(mono_sig, ms)` — 1-30 ms delay on one channel for width
- `width(stereo_sig, amount)` — mid-side width (0 mono, 1 unchanged, >1 exaggerated)
- `mono(stereo_sig)` — collapse to single channel

### Polyphony

- `poly(n_voices, voice_fn)` — fan a per-key synth across N voices
- `midi_keyboard(voice_fn)` — `poly(8, voice_fn)`, the front-door form

`voice_fn` is a 2-arg lambda `(freq, gate) => synth_expr`. Each
held key drives one voice independently; sums of phasor-based
synthesis stack at codegen time.

```
play piano:
  midi_keyboard((freq, gate) =>
    additive(freq, warm_shape, 8) * adsr(gate, 0.01, 0.2, 0.7, 0.4))
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

The engine runs in a background process (`./aither start`)
and accepts commands over a UNIX socket.

### Voice-level

| Command                          | Description                                |
|----------------------------------|--------------------------------------------|
| `send <file> [fade]`             | load / hot-swap a patch                    |
| `stop <voice> [fade]`            | fade out & remove                          |
| `mute <voice>` / `unmute`        | silence / resume (state keeps running)     |
| `solo <voice> [fade]`            | fade out all other voices                  |
| `clear [fade]`                   | stop all voices                            |
| `list`                           | show active voices                         |
| `retrigger <voice>`              | reset `start_t` in place                   |
| `kill`                           | shut down engine                           |

### Part-level

| Command                                                | Description                      |
|--------------------------------------------------------|----------------------------------|
| `parts <voice>`                                        | list parts with gain + state     |
| `part <voice> <part> play [fade]`                      | fade gain to 1                   |
| `part <voice> <part> stop [fade]`                      | fade gain to 0                   |
| `part <voice> <part> mute` / `unmute`                  | instant silence / resume         |
| `part <voice> <part> gain <value> [fade]`              | set gain to arbitrary value      |

### Observability

| Command                          | Description                                         |
|----------------------------------|-----------------------------------------------------|
| `scope [voice]`                  | per-voice RMS / peak / clips / envelope sparkline   |
| `scope master`                   | master-bus stats (pre-tanh mix)                     |
| `spectrum [voice]`               | FFT analysis of voice's recent buffer (or master)   |
| `audit <patch> [seconds]`        | render patch offline + spectral analysis            |

Clips counters clear on read. `spectrum` runs against the
engine's last ~0.5 s of audio per voice; `audit` is fully
offline (no engine connection required, ~100 ms turnaround).

---

## Implementation

### Parser (`parser.nim`, ~560 lines)

Indentation-sensitive tokenizer with group-depth handling
(newlines inside `(...)` and `[...]` are whitespace) +
recursive-descent parser → AST. Handles: literals, identifiers,
operators, function calls, `$state`, `let`, `def`, `play`,
`lambda`, `if/then/else`, `else if` chains, `|>`, arrays,
array indexing, comments. Precedence climbing for expressions.

### Codegen (`codegen.nim`, ~1350 lines)

AST → C source. Each patch transpiles to a single C source
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

### Voice (`voice.nim`, ~260 lines)

Owns the TCC compilation, dlopen of the resulting library, and
the per-voice float64 state pool. Hot-reload commits a new
compile under a brief mutex, migrates state by region identity
(skips NaN-poisoned regions to recover cleanly).

### Native DSP (`dsp.nim`, ~205 lines)

Filters, delays, reverb, resonator, discharge, tremolo, slew,
wave. Each function claims state slots from the per-voice pool.
Pool access is bounds-checked — overflow degrades to a shared
safe slot rather than segfaulting.

### Engine (`engine.nim`, ~735 lines)

Audio callback via miniaudio. UNIX socket server. Voice table
with per-part gain fades. Per-voice and master-bus rolling stats
(RMS, peak, clips, 50 ms envelope bins for sparklines, plus a
0.5 s float32 ring per voice for `spectrum`'s FFT). Tanh
soft-clip on the master. Voice slots sweep on `send` so stopped
voices don't leak the table.

### MIDI (`midi.nim`, ~235 lines)

ALSA seq input thread. Auto-resubscribes if the port is dropped.
Engine logs a clear line on drop / recovery so silent failure
isn't a debugging blind spot.

### Engine types + CLI output (`engine_types.nim` ~50 lines,
`cli_output.nim` ~155 lines)

Engine procs return data structures (`VoiceInfo`,
`StatsSnapshot`, `MidiStatus`, `SpectrumSummary`); formatters in
`cli_output.nim` turn those into text. Lets engine state be
tested without parsing strings.

### Analysis + render (`analysis.nim` ~250 lines, `render.nim`
~65 lines)

`analysis.nim` is pure FFT + spectral feature extraction (no
engine knowledge). `render.nim` runs a patch offline through
the same parse → codegen → TCC → tick path the engine uses,
returning an in-memory buffer. Together they power
`./aither audit` and `./aither spectrum`.

### CLI dispatch (`aither.nim`, ~105 lines)

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

~4250 lines Nim + ~325 lines aither. One binary, ~970 KB.
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
  $fb = sin(TAU * phasor(440 + $fb * 500))
  $fb * 0.3

fm
```

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

### Drone with stereo pad

```
def ease(x):
  let c = clamp(x, 0, 1)
  c * c * (3 - 2 * c)

let pos    = t - start_t
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
