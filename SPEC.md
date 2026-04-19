# Aither Language Specification

A language for real-time audio signal processing.

A program is a single expression that produces one sample,
evaluated 48,000 times per second. Everything is a number.

## Values

Three types:

| Type     | Example                    |
|----------|----------------------------|
| float    | `440.0`, `0.5`, `-1.0`     |
| function | `sin`, `saw`, `my_shape`   |
| array    | `[220, 330, 440]`          |

Float is the default. Function values exist for `osc(shape, freq)`.
Arrays exist for polyphony (input) and stereo (output).

## Globals

| Name  | Description          |
|-------|----------------------|
| `t`   | time in seconds      |
| `sr`  | sample rate (48000)  |
| `dt`  | 1 / sample rate      |
| `PI`  | 3.14159...           |
| `TAU` | 6.28318...           |

## State

`var` declares persistent state. Survives across samples
and hot-reloads. Auto-initialized on first load only.

```
var phase = 0.0
var env = 1.0
```

On hot-reload, existing `var` values are preserved.
New `var` declarations initialize to their default.

## Bindings

`let` declares per-sample values. Computed every sample.

```
let freq = 440 + osc(sin, 0.3) * 50
let mix = osc(sin, 440) + osc(saw, 220)
```

## Operators

### Arithmetic (precedence high to low)

| Operator       | Description              |
|----------------|--------------------------|
| `-x`           | unary negation           |
| `*`, `/`, `mod`| multiply, divide, modulo |
| `+`, `-`       | add, subtract            |
| `\|>`          | pipe (lowest)            |

### Comparison

`==`, `!=`, `<`, `>`, `<=`, `>=`

### Logical

`and`, `or`, `not`

True is non-zero. False is zero.

## Pipe

`|>` inserts the left side as the first argument of
the right side:

```
osc(saw, 55) |> lpf(800, 0.5) |> gain(0.3)

# equivalent to:
gain(lpf(osc(saw, 55), 800, 0.5), 0.3)
```

## Conditionals

Expression-oriented. Always returns a value.
No significant whitespace.

```
if t < 4 then osc(sin, 440) else osc(saw, 220)
```

Single-line. For multi-branch:

```
if t < 4 then osc(sin, 440)
else if t < 8 then osc(saw, 220)
else noise()
```

## Functions

```
def pluck(freq):
  noise() * impulse(3) |> resonator(freq, 0.2)

def chord(root):
  osc(sin, root) + osc(sin, root * 5/4) + osc(sin, root * 3/2)

pluck(330) + chord(220) * 0.2
```

The last expression is the return value. Functions can
use `var` (per-call-site state — each call location gets
independent state via a counter).

## First-class functions

Functions are values. They can be passed as arguments:

```
def my_shape(x):
  sin(x) + sin(3 * x) / 3

osc(my_shape, 440)
```

This exists for `osc(shape, freq)` — shape and clock are
separate concepts composed together.

## Arrays

### Literals

```
[220, 330, 440]
```

### Mutable arrays

```
var buf = array(48000, 0.0)   # create array of size n
buf[i]                         # read element
buf[i] = x                    # write element
len(buf)                       # length
```

Needed for delay lines, reverb buffers, wavetables.

### Polyphony (arrays as input)

When a function receives an array where it expects a
float, it runs once per element with independent state.
Results are summed.

```
osc(sin, [220, 330, 440]) * 0.3
```

### Stereo (arrays as output)

Return an array of two for stereo:

```
osc(sin, 440) |> pan(0.3)   # returns [L, R]
```

### Both

```
osc(sin, [220, 330, 440]) |> pan([-0.5, 0, 0.5])
```

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

The last expression is the output sample. The file is
the instrument.

```
var phase = 0.0

phase += 440 / sr
if phase >= 1.0: phase -= 1.0
sin(TAU * phase) * 0.3
```

No `proc tick*`. No imports. No boilerplate.
`var` lines are state. Everything else is the body.

---

## Builtins

Only two categories of builtins are hardcoded in the
evaluator. Everything else is defined in the stdlib.

### Math (hardcoded — wrappers around C math library)

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
| `clamp(x, l, h)`| clamp to range |
| `int(x)`        | truncate to int |

### Stateful primitives (hardcoded)

| Function      | Description            |
|---------------|------------------------|
| `phasor(freq)`| ramp 0 to 1 at freq Hz |
| `noise()`     | white noise            |

`phasor` is the only stateful oscillator primitive.
`noise` is the only stateless random source.
Everything else — oscillators, filters, effects — is
built from these in the stdlib.

---

## Stdlib (written in aither)

The stdlib is embedded in the binary as a const string.
Loaded before every user patch.

### Shapes (pure math, no state)

```
def saw(x): x * 2 - 1
def tri(x): abs(x * 4 - 2) - 1
def sqr(x): if x < 0.5 then 1 else -1
```

Usable as oscillator shapes or waveshapers.

### Oscillator

```
def osc(shape, freq):
  shape(TAU * phasor(freq))
```

The user picks their level:

```
osc(sin, 440)                 # convenience
sin(TAU * phasor(440))        # explicit
let p = phasor(440)           # raw phase, full control
sin(TAU * p) + sin(3*TAU*p)/3
```

### Filters

```
def lp1(signal, cutoff):
  var y = 0.0
  let a = clamp(cutoff / sr, 0, 1)
  y = y + a * (signal - y)
  y

def lpf(signal, cutoff, res):
  var s1 = 0.0
  var s2 = 0.0
  let g = tan(PI * min(cutoff, sr * 0.49) / sr)
  let k = 2 * (1 - res)
  let a1 = 1 / (1 + g * (g + k))
  let a2 = g * a1
  let a3 = g * a2
  let v3 = signal - s2
  let v1 = a1 * s1 + a2 * v3
  let v2 = s2 + a2 * s1 + a3 * v3
  s1 = 2 * v1 - s1
  s2 = 2 * v2 - s2
  v2
```

Similarly: `hpf`, `bpf`, `notch`, `hp1`.

### Effects

```
def delay(signal, time, max_time):
  var buf = array(int(max_time * sr), 0.0)
  var cursor = 0.0
  let size = len(buf)
  let rd = int(cursor - time * sr + size) mod size
  let output = buf[rd]
  buf[int(cursor)] = signal
  cursor = (cursor + 1) mod size
  output

def fbdelay(signal, time, max_time, fb):
  var buf = array(int(max_time * sr), 0.0)
  var cursor = 0.0
  let size = len(buf)
  let rd = int(cursor - time * sr + size) mod size
  let output = buf[rd]
  buf[int(cursor)] = signal + output * fb
  cursor = (cursor + 1) mod size
  output
```

Similarly: `reverb`, `tremolo`, `slew`.

### Physics

```
def impulse(freq):
  var prev = 0.0
  let p = phasor(freq)
  let hit = if p < prev then 1 else 0
  prev = p
  hit

def resonator(signal, freq, decay):
  var x = 0.0
  var dx = 0.0
  let w2 = freq * freq
  dx = dx + (-decay * dx - w2 * x + signal * w2) * dt
  x = x + dx * dt
  x

def discharge(signal, rate):
  var level = 0.0
  level = max(signal, level * (1 - rate * dt))
  level
```

### Helpers

```
def gain(signal, amount): signal * amount
def fold(signal, amount):
  let x = ((signal * amount) mod 4 + 4) mod 4
  if x < 2 then x - 1 else 3 - x
def pan(signal, pos):
  let angle = (pos + 1) * PI / 4
  [signal * cos(angle), signal * sin(angle)]
```

---

## State semantics

### Top-level `var`

Keyed by variable name. Safe to reorder.

```
var phase = 0.0   # name "phase" → state slot
var env = 1.0     # name "env" → state slot
```

### `var` inside `def`

Keyed by call-site counter. Each call location gets
independent state.

```
osc(sin, 440) + osc(sin, 880)
# two calls to osc → two separate phasors
# because each call site has a unique counter position
```

Counter resets to 0 at the start of each sample. Same
call order = same state slots = phase continuity.

On hot-reload: same call order preserves state. If the
user changes the call order, state remaps (possible
phase discontinuity — acceptable).

---

## MIDI (future)

| Name         | Description              |
|--------------|--------------------------|
| `midi_freq`  | current note frequency   |
| `midi_gate`  | 1 while held, 0 released |
| `midi_vel`   | velocity 0-1             |
| `cc(n)`      | control change 0-1       |

```
osc(sin, midi_freq) * midi_vel * discharge(midi_gate, 4)
```

## Composition (future)

```
osc(sin, 440) |> lpf(800, 0.5)
  |> hold(8)
  osc(saw, 220) |> reverb(1.5, 0.3)
  |> hold(8)
  |> fadeout(4)
```

## Signal references (future)

```
# kick (separate file)
impulse(2) |> resonator(60, 8)

# mix (references kick by name)
kick + hat * 0.3 |> reverb(1.5, 0.3)
```

---

## Implementation

### Parser (Nim, ~250 lines)

Tokenizer + recursive descent → AST.
Handles: literals, identifiers, operators, function
calls, `var`, `let`, `def`, `if/then/else`, `|>`,
arrays, array indexing, comments.

### Evaluator (Nim, ~250 lines)

Tree-walking interpreter. Calls compiled math builtins.
Manages per-voice state (var table + call-site counter).
Returns float, function, or array values.

### Engine (Nim, ~200 lines)

Audio callback via miniaudio. Socket CLI. Voice table.
Loads patches, wraps in tick function, evaluates per
sample.

### Stdlib (aither, ~200 lines)

Shapes, osc, filters, effects, physics, helpers.
Embedded in binary as const string.

### Total: ~900 lines Nim + ~200 lines aither

One binary. Under 1 MB. No dependencies beyond the
system audio library.

---

## Complete examples

**Minimal:**
```
osc(sin, 440) * 0.3
```

**Acid bass:**
```
let freq = wave(2, [55, 55, 82, 55, 73, 55, 98, 55])
let env = discharge(impulse(2), 8)
osc(saw, freq) |> lpf(200 + env * 4000, 0.85) |> gain(0.4)
```

**Chaos:**
```
var chaos = 0.5
var tick_count = 0.0

tick_count = tick_count + 1
if tick_count >= 2000 then tick_count = 0; chaos = 3.59 * chaos * (1 - chaos)
let freq = 200 + chaos * 400
sin(TAU * phasor(freq)) * 0.3
```

**FM feedback:**
```
var fb = 0.0

fb = sin(TAU * phasor(440 + fb * 500))
fb * 0.3
```

**Drone:**
```
let a = osc(saw, 55)
let b = osc(saw, 55.1)
let c = osc(saw, 54.9)
(a + b + c) / 3 |> lpf(400 + osc(sin, 0.1) * 300, 0.4) |> reverb(3, 0.5)
```

**Kick drum:**
```
discharge(impulse(2), 6) * resonator(impulse(2), 60, 8)
```

**Looper:**
```
osc(saw, 110) |> fbdelay(0.5, 0.5, 1.0)
```
