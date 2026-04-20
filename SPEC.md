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

`var` declares persistent state. Survives across samples
and across hot-reloads. Auto-initialized on first load only.

```
var phase = 0.0
var env   = 1.0
```

On hot-reload, existing `var` values are preserved by name.
New `var` declarations initialize to their default.

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
file-level `let` bindings. File-level `var`s, other `def`s,
and globals are visible.

## First-class functions

Functions are values; pass as arguments:

```
def my_shape(x): sin(x) + sin(3 * x) / 3

osc(my_shape, 440)
```

## Play blocks

`play name:` declares a named, independently-controllable
part. Its body compiles inline into the main chunk (so it
sees all file-level lets and vars). Its value is bound to
a local named after the play, readable from the final
expression and from later plays.

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

### Mutable arrays

```
var buf = array(48000, 0.0)   # size N, initial value
buf[i]                         # read
buf[i] = x                     # write
len(buf)                       # length
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
var phase = 0.0; phase += 440 / sr; sin(TAU * phase)
```

## Program structure

A file has four kinds of top-level declaration:

- `def name(args): body` — helper function
- `let name = expr` — file-level binding, visible in all plays
- `var name = init` — file-level persistent state
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
| file-level `var`     | everywhere (by name)                     | persistent, keyed by name          |
| file-level `def`     | everywhere (hoisted)                     | n/a (callable)                     |
| `play` body `let`    | inside that `play` only                  | computed once per sample           |
| `play` body `var`    | file-level by name                       | persistent, shared by name         |
| `def` body `let`     | inside that `def` only                   | computed once per call             |
| `def` body `var`     | per-call-site                            | persistent per call location       |

**`def` cannot see file-level lets** — defs are isolated
functions that take what they need as parameters. **`play`
can see file-level lets** — plays are inlined blocks that
share the file's scope.

**`var` inside a play is file-level by name.** If two plays
both write `var count = 0`, they share the slot. Use
explicit names (`var kick_count`, `var bass_count`) for
independence.

**`var` inside a def is per-call-site.** Each location the
def is called from gets its own state slot. This is what
makes `osc(sin, 440) + osc(sin, 880)` work — two
independent phasors without any ceremony.

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

---

## Stdlib (written in aither)

Embedded in the binary as a const string. Loaded before
every user patch. Source: `stdlib.aither`.

### Oscillator wrappers

```
def osc(shape, freq):  shape(TAU * phasor(freq))
def pulse(freq, width): if phasor(freq) < width then 1 else -1
```

### Helpers

```
def gain(signal, amount): signal * amount
def fold(signal, amount): ...             # triangle wrap
def prev(x):                              # one-sample memory
  var last = 0.0
  let out = last
  last = x
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

---

## State semantics

### Top-level `var`

Keyed by variable name. Safe to reorder. Shared by all
plays and defs that reference the name.

### `var` inside `def`

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

### `var` inside `play`

Currently file-level by name (same slot if two plays share
a name). For per-play independence, use unique names or
move state into a local `def` called from the play.

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

Clips counters clear on read.

---

## Implementation

### Parser (`parser.nim`, ~465 lines)

Indentation-sensitive tokenizer + recursive-descent parser
→ AST. Handles: literals, identifiers, operators, function
calls, `var`, `let`, `def`, `play`, `if/then/else`, `|>`,
arrays, array indexing, comments. Precedence climbing for
expressions.

### Evaluator (`eval.nim`, ~1150 lines)

Bytecode compiler + stack VM. Each patch compiles to a
main chunk plus one chunk per `def`. Play blocks compile
inline into the main chunk with their result stored to a
named local. Polymorphic binary arithmetic for length-2
stereo. Per-voice state: float64 pool (4 MB) for native
DSP, call-site state for def-local `var`, named slots for
top-level `var`.

### Native DSP (`dsp.nim`, ~200 lines)

Filters, delays, reverb, resonator, discharge, tremolo,
slew, wave. Each function claims state slots from the
per-voice pool. Pool access is bounds-checked — overflow
degrades to a shared safe slot rather than segfaulting.

### Engine (`engine.nim`, ~500 lines)

Audio callback via miniaudio. Socket CLI. Voice table with
per-part gain fades. Per-voice and master-bus rolling stats
(RMS, peak, clips, 50 ms envelope bins for sparklines).
Tanh soft-clip on the master.

### Stdlib (`stdlib.aither`, ~100 lines)

Oscillator wrappers, helpers, character effects, envelopes,
stereo helpers. Pure aither code, baked into the binary as
a const string.

### Totals

~2300 lines Nim + ~100 lines aither. One binary, ~500 KB.
No runtime dependencies beyond the system audio library.

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
  var fb = 0.0
  fb = sin(TAU * phasor(440 + fb * 500))
  fb * 0.3

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
