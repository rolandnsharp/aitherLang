# Using Aither

## Getting started

```bash
make                    # build the engine
./aither start          # start audio, listen on socket
```

In another terminal:

```
./aither send patch.aither
```

Write signals. Hear sound. Edit the file. Send again.
State persists — no clicks, no restart.

## Your first patch

Create `sine.aither`:

```
play beep:
  osc(sin, 440) * 0.2

beep
```

A file has `play` blocks (named parts) and a final
expression (the voice output). Here there's one part named
`beep` and the final line emits it directly.

Send it:

```
./aither send sine.aither
```

A 440 Hz sine wave plays. Edit `440` to `220`, resend.
The pitch drops. The phase continues — no click.

## Building sounds

### Oscillators

`osc(shape, freq)` is the oscillator. The shape is a math
function. The oscillator manages the clock.

```
osc(sin, 440)          # sine
osc(saw, 55)           # sawtooth
osc(tri, 220)          # triangle
osc(sqr, 110)          # square
```

### Piping through effects

`|>` sends signal left to right through a chain:

```
osc(saw, 55) |> lpf(800, 0.5) |> gain(0.4)
```

Saw wave → lowpass filter → volume.

**Gotcha**: `x |> f() * y` doesn't parse as `f(x) * y`.
Pipe is low-precedence (OCaml/Elixir style). Use parens:
`(x |> f()) * y`, or bind first: `let z = x |> f(); z * y`.
The error message tells you this.

### Mixing

Addition is mixing:

```
osc(sin, 440) * 0.5 + osc(sin, 880) * 0.25
```

Two oscillators summed. Multiplication is amplitude.

### State

`var` creates values that persist across samples:

```
play pluck:
  var phase = 0.0
  var env   = 1.0
  phase = (phase + 440 / sr) mod 1.0
  env = env * 0.9999
  if env < 0.001 then env = 1 else env
  sin(TAU * phase) * env

pluck
```

`var` survives hot-reload. Edit the file, resend — `phase`
and `env` keep their current values.

### Helper functions

`def` declares a reusable function. It takes parameters and
returns a value. Useful for DSP you want to reuse.

```
def pluck_voice(freq):
  noise() * impulse(3) |> resonator(freq, 0.2)

play harp:
  pluck_voice(330) + pluck_voice(440) * 0.5

harp
```

Each call to `pluck_voice` gets its own state. Two plucks,
two independent resonators.

## Play blocks

Parts are named, engine-controllable pieces of the voice:

```
let tempo = 140.0 / 60.0
let kEnv  = discharge(impulse(tempo), 10)
let sc    = 1 - kEnv * 0.75

play kick:
  sin(TAU * phasor(50 + discharge(impulse(tempo), 35) * 170)) * kEnv

play bass:
  osc(saw, 55) |> lpf(150 + kEnv * 1500, 0.85) * sc

(kick + bass) |> drive(1.1)
```

**Shared file-level signals** (like `sc`, `kEnv`, `tempo`)
are visible to every play. That's how sidechain and shared
modulators work.

**Forward cross-play references**: a play can reference any
part defined *earlier* in the file. `bass` can read `kick`
because `kick` is defined first.

**Parts return mono or stereo**: a play body can return a
single float (mirrored to both channels) or `[L, R]`
(stereo).

**The final expression is the voice's output**: it composes
the named parts into the mix. Drive / reverb / master
processing goes there.

## Controlling parts live

Once a voice is loaded, you can control individual parts
from the CLI without touching the file:

```
./aither parts bass                   # list parts with gain + state
./aither part bass kick mute          # instant silence (20ms fade)
./aither part bass kick unmute         # resume
./aither part bass kick play 4        # fade in over 4 seconds
./aither part bass kick stop 2        # fade out over 2 seconds
./aither part bass kick gain 0.5 1    # fade to 50% over 1 second
```

Hot-reloading a patch preserves every part's current gain.
Delete a `play` block and resend — that part disappears
from the mix. Add a new `play` block and resend — it fades
in at gain 1.

## Live performance

### Multiple voices

Each file is an independent voice. Load several:

```
./aither send kick.aither
./aither send bass.aither
./aither send hat.aither
./aither list                  # shows what's playing
```

### Fade in and out

```
./aither send pad.aither 4     # fade in over 4 seconds
./aither stop hat 2            # fade out over 2 seconds
./aither stop hat              # stop immediately
```

### Mute and solo

```
./aither mute bass              # silence voice, state keeps running
./aither unmute bass            # resume from where it was
./aither solo kick 2            # fade everything else out
```

### Retrigger

`start_t` is the per-voice composition clock. Reset it
without cycling stop+send:

```
./aither retrigger bass         # composition plays from pos=0 again
```

### Clear

```
./aither clear                  # stop everything
./aither clear 4                # fade everything out over 4s
```

## Observability

```
./aither scope                   # stats for master + all voices
./aither scope master            # master bus only
./aither scope bass              # one voice's stats
```

Shows RMS, peak, clip count, and a 20-bin envelope
sparkline (last second). Clip counter clears on read, so
"did my last edit introduce clipping?" is always answered
by one scope call.

## Time-based expressions

### `t` — absolute time

`t` is seconds since the engine started.

```
osc(sin, 440 + t * 10)           # pitch rises forever
```

### `start_t` — when this voice loaded

`start_t` is when the voice was first sent. Use
`pos = t - start_t` as the composition clock:

```
play swell:
  let pos = t - start_t
  let fade = clamp(pos / 5, 0, 1)     # fade in over 5 seconds
  osc(saw, 55) |> lpf(800, 0.5) * fade * 0.4

swell
```

```
# timed section change
play two_part:
  let pos = t - start_t
  if pos < 10 then osc(sin, 440)
  else              osc(sin, 660)

two_part * 0.2
```

Hot-swapping a voice preserves `start_t`. Use
`./aither retrigger <voice>` to reset.

## Scale lookups

Arrays with integer indexing — the idiomatic way to do
pitch quantisation:

```
def midi(n): 440 * pow(2, (n - 69) / 12)

let scale = [0, 2, 3, 5, 7, 10]         # D natural minor
let drift = (sin(TAU * t * 0.03) + 1) * 0.5 * 6
let step  = scale[int(drift) mod 6]

play melody:
  let freq = 293.66 * pow(2, step / 12)
  osc(sin, freq) * 0.2

melody
```

Constant literal arrays are compile-time hoisted (no per-
sample allocation).

## Stereo thinking

Plays can return `[L, R]`. Stdlib helpers:

```
osc(saw, 110) * 0.3 |> pan(sin(TAU * t / 50))    # slow pan
osc(sin, 220) * 0.2 |> haas(8)                    # right delayed 8 ms
[oscL, oscR] |> width(1.6)                        # exaggerate stereo
mono(bass)                                         # collapse to float
```

Binary arithmetic is polymorphic for length-2 arrays:
`kick + bass` works whether either is mono or stereo.

## Envelopes

```
play percussive:
  osc(sin, 220) * pluck(impulse(2), 0.3)          # fast attack, 0.3s decay

play bowed:
  let gate = if (t mod 4) < 2 then 1 else 0
  osc(saw, 110) * swell(gate, 1.0, 0.8)           # 1s attack, 0.8s release

play synth:
  let gate = if phasor(0.5) < 0.6 then 1 else 0
  let env  = adsr(gate, 0.02, 0.2, 0.4, 0.6)
  osc(saw, 220) |> lpf(200 + env * 3000, 0.7) * env

percussive + bowed + synth
```

## Character effects

Lo-fi / glitch stdlib:

```
osc(saw, 110) |> drive(1.5)            # soft-clip saturation
osc(sin, 440) |> bitcrush(8)           # bit-depth reduction
osc(saw, 220) |> downsample(2000)      # sample-rate reduction aliasing
osc(sin, 330) |> dropout(4, 0.5)       # gate on for 50% of each cycle
osc(saw, 110) |> wrap(1.8)             # hard wavefold
```

## Composition mode vs live-jam

**Live-jam** (many files, each one voice): spawn and
control parts as independent voices. Good for improvisation
and exploration. Parts can't see each other.

**Composition mode** (one file with many play blocks):
all parts share file-level state; final expression is the
mix; write sidechain and routing as arithmetic. Good for
composed pieces. Sacrifices independent per-file hot-reload
in exchange for true shared state.

Both coexist — a session can have a "composition mode"
patch loaded alongside a half-dozen live-jam voices. Which
model to use is stylistic, not technical.

## Feedback

### Self-feedback (`var` in one play)

```
play fm:
  var fb = 0.0
  fb = sin(TAU * phasor(440 + fb * 500))
  fb * 0.3

fm
```

One sample of memory = the entire FM feedback family.

### Forward cross-play feedback (`prev`)

```
play src:
  osc(sin, 220) * 0.2

play echo:
  prev(src) * 0.7       # src's previous-sample value

(src + echo) * 0.5
```

`prev(x)` is `x` delayed one sample. Works on any
expression (stdlib def, not a compiler magic). For
self-reference or referencing a later-defined play, use
inline `var` instead.

## Recipes

### Kick drum

```
play kick:
  let trig = impulse(2)
  discharge(trig, 6) * resonator(trig, 60, 8)

kick
```

### Hi-hat

```
play hat:
  noise() * pluck(impulse(8), 0.05) |> hpf(8000, 0.1)

hat * 0.2
```

### Drone

```
play drone:
  let a = osc(saw, 55)
  let b = osc(saw, 55.1)
  let c = osc(saw, 54.9)
  (a + b + c) / 3 |> lpf(400 + osc(sin, 0.1) * 300, 0.4)

drone |> reverb(3, 0.5) |> haas(15)
```

### Plucked string

```
play pluck:
  noise() * impulse(3) |> resonator(330, 0.2)

pluck
```

### Sequenced rhythm

```
let bpm = 128
let pattern = [1, 0, 0, 1, 0, 0, 1, 0]
let hit = wave(bpm / 60, pattern)

play rhythm:
  hit |> resonator(180, 12) |> fold(1) * 0.4

rhythm
```

### Bell (inharmonic partials)

```
play bell:
  let strike = impulse(1)
  resonator(strike, 800, 0.3)
  + resonator(strike, 1260, 0.4)
  + resonator(strike, 1860, 0.6) * 0.3

bell * 0.3
```

## LLM-assisted workflow

The language is small enough for an LLM to generate correct
patches without hallucinating APIs. The workflow:

```
User: "add a bass line"
→ LLM writes bass.aither with a `play bass` block
→ aither send bass.aither

User: "make it darker"
→ LLM edits: lpf(800 → lpf(300
→ aither send bass.aither

User: "mute the kick for now"
→ aither part bass kick mute

User: "add a melody in A minor"
→ LLM writes melody.aither with a play block
→ aither send melody.aither

User: "slowly open the bass filter"
→ LLM edits bass.aither: lpf(300 + (t - start_t) * 100, 0.5)
→ aither send bass.aither

User: "fade out the melody"
→ aither stop melody 4
```

Structural changes are edits + resends. Parameter tweaks
are edits. Live adjustments to mute / solo / gain are CLI
commands.

## Going deeper

### Raw phasor access

`osc(sin, 440)` is sugar for `sin(TAU * phasor(440))`.
Use the phasor directly for custom waveforms:

```
play additive:
  let p = phasor(440)
  sin(TAU * p) + sin(3 * TAU * p) / 3 + sin(5 * TAU * p) / 5

additive * 0.3
```

### Physics

The damped harmonic oscillator is a universal primitive:

```
play tuning_fork:
  var x = 0.0
  var dx = 0.0
  let w2 = 440 * 440
  dx = dx + (-2 * dx - w2 * x + impulse(2) * w2) * dt
  x = x + dx * dt
  x * 0.3

tuning_fork
```

Change the damping, the frequency, the excitation — same
equation, different sound.

### Shapes as waveshapers

The shape functions work on any signal, not just
oscillators:

```
osc(sin, 440) * 5 |> fold(1)     # wavefold distortion
osc(sin, 440) |> saw              # transfer function
```

`saw` as a waveshaper maps the signal through the sawtooth
curve. Any math function is a waveshaper.

### Scopes & parts for debugging

Silent patch? `./aither scope master` shows the mix RMS.
If it's `-inf`, you're outputting silence — check your
fade multipliers. Peak clipping? `./aither scope master`
shows clips-per-second. Per-voice breakdown?
`./aither scope bass` for one, plain `./aither scope` for
all voices including master.

## Read more

- [SPEC.md](SPEC.md) — complete language reference
- [COMPOSING.md](COMPOSING.md) — signal-native composition idioms
- [PHILOSOPHY.md](PHILOSOPHY.md) — design vision
- [ARCHITECTURE.md](ARCHITECTURE.md) — implementation overview
- [stdlib.aither](stdlib.aither) — the composition layer
