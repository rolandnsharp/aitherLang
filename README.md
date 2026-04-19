# aither

A language for real-time audio signal processing.

One expression. Evaluated 48,000 times per second. The result
is sound.

```
osc(saw, 55) |> lpf(800, 0.5) |> gain(0.4)
```

A saw wave at 55 Hz, through a low-pass filter, at 40% gain.
That is a complete program. Save it as `bass.aither` and play
it.

## Build

```
make
```

One binary, 750 KB. Linux-only for now (system audio via
miniaudio).

## Use

```
aither start                  launch engine
aither send bass.aither       load & play
aither send bass.aither 2     ...with 2-second fade in
aither stop bass 1            fade out over 1 second
aither list                   show active voices
aither kill                   shut down
```

Each file is a voice. The filename is the name. Edit the file,
resend it, and the voice hot-swaps without dropping state.

```
aither start &
aither send examples/bass.aither
aither send examples/kick.aither
aither send examples/hat.aither
aither clear 2                # fade everything out
aither kill
```

## A taste

Minimal:

```
osc(sin, 440) * 0.3
```

Acid bass:

```
let freq = wave(2, [55, 55, 82, 55, 73, 55, 98, 55])
let env  = discharge(impulse(2), 8)
osc(saw, freq) |> lpf(200 + env * 4000, 0.85) |> gain(0.4)
```

FM feedback (one sample of memory, written explicitly):

```
var fb = 0.0
fb = sin(TAU * phasor(440 + fb * 500))
fb * 0.3
```

Kick drum:

```
discharge(impulse(2), 6) * resonator(impulse(2), 60, 8)
```

## Why

The interface is `f(state) → sample`. The engine has zero
opinions about signal processing — it does not know what a sine
wave is, what a filter is, what decay means. It just calls your
function 48,000 times per second and sends the result to the
speakers.

Everything else — oscillators, filters, envelopes, reverb,
physical models — is your code, composed freely from two
stateful primitives (`phasor`, `noise`) and the C math library.
The standard library is written in aither and ships with the
binary.

`var` is memory: a value that survives across samples and
across hot-reloads. That is the entire state model. No graphs,
no message passing, no nodes, no wires. Just expressions.

## Compared to

**SuperCollider**: a client/server with a graph of precompiled
UGens, sequenced from a Smalltalk-derived language. Powerful
and battle-tested, but the DSP itself lives in C++ you don't
see. aither is one language with one model (`f(state) →
sample`), the stdlib is written in the language, and there is
no graph — patches are just expressions.

**FAUST**: a beautiful functional DSL that compiles a
block-diagram algebra to fast C++/Rust/LLVM/Wasm. Excellent for
designing plugins; the model is compile-then-run, and state is
implicit inside `~` and delay lines. aither is interpreted —
edits hit the speakers in milliseconds — and state is explicit
and named (`var x = 0.0`).

**Sonic Pi**: a friendly live-coding layer on top of
SuperCollider — you sequence pre-built synths and samples. You
do not define DSP. aither is a level lower: you write the
oscillator, the filter, the reverb. The sequencer is just a
function returning a sample.

The trade-off is honest: aither is slower per sample than a
compiled FAUST patch or a SuperCollider UGen written in C++.
For most patches that does not matter. For dozens of voices
running heavy reverb, it does.

## Read more

- [PHILOSOPHY.md](PHILOSOPHY.md) — the design vision
- [SPEC.md](SPEC.md) — the complete language reference
- [stdlib.aither](stdlib.aither) — the DSP library, in aither

## Architecture

```
parser.nim     440 lines    tokenizer + recursive descent → AST
eval.nim       298 lines    tree-walking evaluator
engine.nim     325 lines    miniaudio callback + UNIX socket CLI
stdlib.aither  233 lines    shapes, filters, delays, reverb, physics
```

About 1300 lines total. No dependencies beyond Nim's standard
library and the system audio library.
