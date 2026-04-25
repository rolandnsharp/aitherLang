# Philosophy

## The core abstraction

There is one interface: a function that receives the state
of the world and returns a sample.

```
f(state) → sample
```

This is evaluated 48,000 times per second. The function
produces sound. The state carries memory forward. That's
the entire system.

## What state means

State is not a programming concept — it is the physical
reality that sound requires memory. A filter remembers its
previous output. A phase accumulator remembers where it is
in the cycle. An envelope remembers how much energy remains.

Without state, you can compute `sin(t * 440 * TAU)` — a
pure function of time. But you cannot build a filter, an
envelope, a feedback loop, or anything that evolves. Sound
is not a function of time. Sound is a function of state.
State has memory. Memory produces organic, living, evolving
sound.

In aither, state is `var`. A `var` declaration creates a
value that persists across samples. It survives hot-reload.
It is the instrument's memory — visible, named, yours.

## What the engine does

The engine has zero opinions about signal processing. It
does not know what a sine wave is. It does not know what
decay means. The engine:

1. Calls your function 48,000 times per second
2. Passes time (`t`), sample rate (`sr`), delta time (`dt`)
3. Takes the return value and sends it to the speakers
4. Preserves your `var` state across hot-reloads
5. Soft-clips the output through `tanh`

Everything else — oscillators, filters, envelopes, chaos
functions, physical models — is your code, composed freely.
The only primitive is `f(state) → sample`.

## Five paradigms, one interface

Different musical ideas require different ways of thinking.
Aither supports five paradigms through the same interface:

**Kanon (Fire)** — pure function of time. Stateless.
Mathematical beauty.
```
sin(TAU * 440 * t) * 0.3
```

**Rhythmos (Earth)** — explicit state, phase accumulation.
Stable, continuous, hot-swappable.
```
var phase = 0.0
phase = (phase + 440 / sr) mod 1.0
sin(TAU * phase) * 0.3
```

**Atomos (Air)** — discrete events, stochastic processes.
Grains, particles, emergent textures.
```
var next = 0.0
if t >= next: next = t + 0.01 + noise() * 0.02
```

**Physis (Water)** — physical simulation. Springs, strings,
membranes. Sound emerges from physics.
```
var x = 0.0
var dx = 0.0
dx = dx + (-decay * dx - freq * freq * x + input) * dt
x = x + dx * dt
```

**Chora (Aither)** — spatial fields, reverb, room acoustics.
Space itself becomes an instrument.
```
signal |> reverb(3.0, 0.5)
```

These are not separate APIs. They are expressive patterns
that emerge from the same `f(state) → sample` interface.
The engine doesn't know which paradigm you're using. It
doesn't care. It calls your function and plays the result.

## Why `osc(shape, freq)`

An oscillator is two independent ideas composed:

- A **clock** — a phasor ramping from 0 to 1 at a frequency
- A **shape** — a math function applied to the phase

`osc(sin, 440)` means: run a clock at 440 Hz, view it
through `sin`. `osc(saw, 55)` means: same clock, different
view. Custom shapes are just functions:

```
def my_shape(x):
  sin(x) + sin(3 * x) / 3

osc(my_shape, 440)
```

The shape and the clock are separate concepts that compose
freely. This is why `sin` is always the math function and
`osc` is always the oscillator — no ambiguity, no overloading.

The deeper truth: `phasor(freq)` is the only stateful
oscillator primitive. Everything else is math on top:

```
sin(TAU * phasor(440))         # what osc(sin, 440) does
phasor(55) * 2 - 1             # what osc(saw, 55) does
```

The user picks their level of abstraction.

## Why pipes

Signal processing is function composition. A signal flows
from source through effects to output. The pipe operator
makes this flow visible:

```
osc(saw, 55) |> lpf(800, 0.5) |> reverb(1.5, 0.3) |> gain(0.4)
```

Left to right. Each `|>` is a wire. The signal flows
through the chain. `|>` inserts the left side as the first
argument of the right side — that's the entire mechanism.

## Why feedback is mutation

Feedback requires memory. Sample N depends on sample N-1.
The pure functional version — threading state through a
monad, returning updated state alongside the output —
hides the loop in abstraction.

Aither doesn't hide it:

```
var fb = 0.0
fb = sin(TAU * phasor(440 + fb * 500))
fb * 0.3
```

`fb` on the right is the previous sample. `fb` on the left
is the current sample. The loop is an assignment. The
physics requires memory, the code has memory, you can see it.

## Why no graph

Traditional audio systems (Max, PD, SuperCollider, modular
synths) use dataflow graphs. You create objects, connect
them with wires, data flows through the graph at runtime.

Aither uses function composition instead. You don't create
objects — you call functions. You don't draw wires — you
write pipes. The result is a single expression that the
evaluator walks per sample:

```
osc(saw, 55) |> lpf(800 + osc(sin, 0.3) * 400, 0.5) |> gain(0.4)
```

This is not a graph. It's a value. One expression, one
result, one sample. No nodes, no edges, no scheduling,
no message passing. Just math.

## Why files

Each file is a signal. The filename is the name. The file
is the instrument.

- Hot-swap one sound without touching the others
- Each file fits in your head
- Git tracks changes per-instrument
- The arrangement lives in a conductor file

```
aither send kick.aither     # play the kick
aither send bass.aither     # add the bass
aither send kick.aither     # edit the kick, resend — instant
aither stop kick 4          # fade out over 4 seconds
```

## Why the language defines itself

The only builtins hardcoded in the evaluator are math
functions (`sin`, `cos`, `exp`, `pow`...), two stateful
primitives (`phasor`, `noise`), and one compile-time fold
(`sum(N, lambda)`). The fold isn't really runtime — it
unrolls into N parallel expressions at codegen, each with
its own state slot. That's why `additive(f, shape, 16)`
gives you 16 independent phasor states with no extra
ceremony.

Everything else — oscillators, filters, effects, envelopes,
physics models, additive synthesis, formant instruments — is
written in aither itself and shipped as the standard library.

```
def osc(shape, freq):
  shape(TAU * phasor(freq))

def lpf(signal, cutoff, res):
  var s1 = 0.0
  var s2 = 0.0
  # ... SVF filter math ...
  v2
```

The user can read the DSP source. Modify it. Extend it.
No hidden internals. No compiled black boxes. The
instrument is transparent.

## The Pythagorean-Heraclitean duality

Pythagoras said all is number. Sound waves are timeless
blueprints. Harmony is mathematical ratio. The universe
is a pre-existing block of perfection.

Heraclitus said everything flows. The universe is a river
of fire in constant transformation. You can't step in
the same river twice.

Both views are true. Both are necessary.

`sin(TAU * 440 * t)` is Pythagorean — a crystal, eternal,
stateless. It exists in its entirety as a mathematical
object.

`var phase = 0.0; phase += 440 / sr` is Heraclitean — a
process, evolving, stateful. You can't compute sample
10,000 without living through samples 0-9,999.

Aither lets you use both simultaneously. The first is
purer. The second has memory. Together they make music.

## Why the language stays small

Every primitive in aither had to earn its place against the
alternatives. The default is no.

When a feature is proposed — a master compressor, a bus,
a spectral transform, a pattern DSL, a feedback primitive,
a harmonic bank — the question is not "would this be
useful?" (everything is useful to someone). The question
is: *can the existing primitives already express this?*

If yes, adding the feature obscures the composition it was
meant to support. The user writes the feature's name
instead of writing the composition that produces the same
result. A layer of abstraction inserts itself between the
musician and the math.

A feedback loop does not need `prev()` — `var` already
holds one sample of memory. A sidechain does not need a
bus — file-level `let` bindings are already shared scope.
A time-stretched hold does not need `hold()` — a `pos`-keyed
ease is already a timeline. A scale lookup does not need
a `case` — an array with an integer index is already a
dispatch table.

Each primitive that survives is load-bearing. Each one
that's refused keeps the language small enough to hold in
the head.

This discipline is not minimalism for its own sake. It is
the discovery, each time, that the small set of things
aither already has is more expressive than it first
appears. Resisting features is how that expressiveness
stays visible.

## The goal

A scientific instrument for signal processing, synthesis,
and inspection. Write the equation, hear it, see it on
the oscilloscope, change it live, inspect every variable.

Nothing between you and the signal. No GUI, no graph,
no abstraction layer. Just math and a speaker.

The entire studio — synthesizer, sequencer, looper, live
coder, composer, oscilloscope — in one language. Because
they're all the same operation: a function of state,
evaluated in a loop, producing samples.

One interface. Five paradigms. Infinite expression.
