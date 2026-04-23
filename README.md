# aither

A language for real-time audio signal processing.

Everything is a signal computed 48,000 times per second.
Files are named parts composed into a single stereo output.
Hot-reload any file; state persists.

```
def ease(x):
  let c = clamp(x, 0, 1)
  c * c * (3 - 2 * c)

let tempo = 140.0 / 60.0
let kEnv  = discharge(impulse(tempo), 10)
let sc    = 1 - kEnv * 0.75           # sidechain, in scope for every play

play kick:
  sin(TAU * phasor(50 + discharge(impulse(tempo), 35) * 170)) * kEnv

play bass:
  osc(saw, 55) |> lpf(150 + kEnv * 1500, 0.85) * sc

(kick + bass) |> drive(1.1)           # final expression = voice output
```

Save as `bass.aither` and play it.

## Build

```
make
```

One binary, ~500 KB. Linux-only for now (system audio via miniaudio).

## Use

```
aither start                          launch engine
aither send bass.aither               load & play
aither send bass.aither 2             ...with 2-second fade in
aither stop bass 1                    fade out over 1 second
aither mute bass                      silence whole voice (state keeps running)
aither mute bass kick                 silence one play within bass
aither mute bass kick 2               ...with 2-second fade
aither unmute bass kick               restore
aither solo bass kick                 fade other plays in bass to 0
aither list                           show active voices + per-play gains
aither parts bass                     focused per-play view for one voice
aither scope master                   master-bus RMS / peak / clips / envelope
aither retrigger bass                 reset start_t so the piece plays from top
aither clear 2                        fade everything out
aither kill                           shut down
```

Each file is a voice. The filename is the name. Edit the
file, resend it, and the voice hot-swaps without dropping
state. Edit individual parts live with `aither mute /
unmute / solo …`. To rebalance the mix, edit the patch's
`gain()` calls and resend — mixing decisions live in the
score, not in the CLI.

```
aither start &
aither send examples/bass.aither
aither send examples/kick.aither
aither send examples/hat.aither
aither clear 2                    # fade everything out
aither kill
```

## The model

A file has three kinds of declaration, then a final expression.

```
def ease(x): …                    # helper function (reusable, isolated)
let tempo = 140.0 / 60.0           # file-level binding, visible to all parts
var counter = 0                    # file-level persistent state

play kick: …                       # named, engine-controllable part
play bass: …

(kick + bass) |> drive(1.1)        # final expression = voice output
```

Parts return either a float (mono) or `[L, R]` (stereo).
Arithmetic is polymorphic: `kick + bass` works whether
either is mono or stereo. The final expression *is* the
mix; subgrouping, sidechain, and mastering are just
expressions:

```
let lowend = (kick + bass) |> drive(1.2)
let mids   = lead + pad
(lowend + mids) |> reverb(2.5, 0.2)
```

## Signal primitives

Hardcoded: `sin cos tan exp log pow sqrt abs floor clamp
int min max` (math), `saw tri sqr` (shapes), `phasor noise`
(stateful sources).

Native DSP: `lpf hpf bpf notch lp1 hp1` (filters),
`delay fbdelay reverb` (time/space), `impulse resonator
discharge` (physics), `wave tremolo slew` (modulation /
sequence).

Stdlib (pure aither, in `stdlib.aither`): `osc pulse gain
fold prev drive wrap bitcrush downsample dropout pluck
swell adsr pan haas width mono`.

## A taste

### Minimal

```
play beep:
  osc(sin, 440) * 0.2

beep
```

### FM feedback (one sample of memory)

```
play fm:
  var fb = 0.0
  fb = sin(TAU * phasor(440 + fb * 500))
  fb * 0.3

fm
```

### Acid bass

```
let notes = wave(2, [55, 55, 82, 55, 73, 55, 98, 55])
let env   = discharge(impulse(2), 8)

play acid:
  osc(saw, notes) |> lpf(200 + env * 4000, 0.85) |> gain(0.4)

acid
```

### Coupled physics (Lorenz attractor)

```
play chaos:
  var x = 0.1; var y = 0.0; var z = 0.0
  let ddt = dt * 50
  let dx = 10 * (y - x); let dy = x * (28 - z) - y; let dz = x * y - 2.67 * z
  x = x + dx * ddt; y = y + dy * ddt; z = z + dz * ddt
  [x / 22, (z - 25) / 18] * 0.1

chaos
```

## Why

The interface is `f(state) → sample`. The engine has zero
opinions about signal processing — it does not know what a
sine wave is, what a filter is, what decay means. It calls
your function 48,000 times per second and sends the result
to the speakers.

Everything else — oscillators, filters, envelopes, reverb,
physical models — is your code, composed freely from two
stateful primitives (`phasor`, `noise`) and the C math
library. Most stdlib is compiled Nim for performance; the
composition layer on top is written in aither itself.

`var` is memory: a value that survives across samples and
across hot-reloads. That is the entire state model. No
graphs, no message passing, no nodes, no wires.

Hot-reload preserves more than top-level `var`s. Every
stateful helper call site — each filter, delay buffer,
reverb tail, phasor phase — is keyed by helper type and
per-type index, so inserting a new oscillator or effect
doesn't shift the storage of everything after it. Existing
voices keep ringing while the edit takes effect.

`play` blocks make named parts controllable live. CLI
commands mute / solo / fade / retrigger individual parts
without touching the file. Composition mode (one file with
the final expression written as a mix of parts) and live-jam
mode (many files, each one part) are both valid — you pick
per piece.

## Compared to

**SuperCollider**: a client/server with a graph of
precompiled UGens, sequenced from a Smalltalk-derived
language. Powerful and battle-tested, but the DSP itself
lives in C++ you don't see. aither is one language with one
model (`f(state) → sample`), the stdlib's composition layer
is written in the language, and there is no graph — patches
are just expressions.

**FAUST**: a beautiful functional DSL that compiles a
block-diagram algebra to fast C++/Rust/LLVM/Wasm. Excellent
for designing plugins; the model is compile-then-run, and
state is implicit inside `~` and delay lines. aither also
compiles — each patch is transpiled to C and compiled
in-process by TCC on load, so edits are running native code
within a few milliseconds. State is explicit and named
(`var x = 0.0`), and hot reload preserves it.

**Sonic Pi / Tidal / Strudel**: friendly live-coding layers
on top of synths and samples — you sequence pre-built
instruments through pattern notation. aither is a level
lower: you write the oscillator, the filter, the reverb.
Sequencing is just a wavetable oscillator running at beat
rate, not a separate concept.

The trade-off is honest: aither is slower per sample than a
compiled FAUST patch or a SuperCollider UGen written in
C++. For the kind of pieces aither targets — live-coded,
evolving, feedback-heavy, generative — that trade-off is
worth it. For mixing 50 high-quality reverb tails
simultaneously, use a DAW.

## Read more

- [PHILOSOPHY.md](PHILOSOPHY.md) — the design vision
- [SPEC.md](SPEC.md) — complete language reference
- [COMPOSING.md](COMPOSING.md) — the signal-native composition way
- [GUIDE.md](GUIDE.md) — hands-on how-to for writing and performing
- [ARCHITECTURE.md](ARCHITECTURE.md) — how the implementation fits together
- [stdlib.aither](stdlib.aither) — the composition layer, in aither

## Architecture

```
parser.nim      ~485 lines   tokenizer + recursive descent → AST
codegen.nim     ~975 lines   AST → C source + per-helper-type state layout
voice.nim       ~225 lines   TCC compile → dlopen'd tick(); hot-reload migration
dsp.nim         ~200 lines   native DSP primitives (filters, delay, reverb...)
engine.nim      ~565 lines   audio callback + UNIX socket CLI + stats
stdlib.aither   ~140 lines   composition layer (osc, drive, adsr, pan, prev…)
```

About 2500 lines total. A patch is parsed to an AST,
transpiled to C, handed to TCC which compiles it to machine
code in memory, and the resulting `tick(state)` function
pointer is called once per sample from the audio callback.
Hot reload compiles the new code off the audio thread, then
swaps pointers under a brief mutex while copying matching
state regions across.
Dependencies: Nim's stdlib, libtcc, and the system audio
library.
