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

Once a voice is loaded, you can mute / unmute / solo
individual parts from the CLI without touching the file.
Mute and solo are *performance gestures* — quick, binary,
ephemeral. Mix decisions (specific gain levels) live in the
patch's `gain()` calls, not in the CLI.

```
./aither parts bass                   # list parts with gain + state
./aither mute bass kick               # silence one part within bass
./aither mute bass kick 2             # ...with a 2-second fade
./aither unmute bass kick             # resume
./aither solo bass kick               # fade other parts in bass to 0
```

Hot-reloading a patch preserves every part's current gain.
Delete a `play` block and resend — that part disappears
from the mix. Add a new `play` block and resend — it fades
in at gain 1.

To rebalance the mix, edit the patch's `gain()` calls and
resend. The patch *is* the mix; in-engine gain knobs would
be a parallel source of truth that drifts from the file.

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

### Recording a session

Capture aither's audio output with `ffmpeg` reading from PulseAudio's monitor
source. List your sinks with `pactl list short sinks`, find the monitor
matching your default output, then:

```bash
ffmpeg -f pulse -i alsa_output.pci-0000_00_1f.3.analog-stereo.monitor jam.wav
```

Or simpler — pipe the default monitor straight to file with `parec`:

```bash
parec --format=s16le --channels=2 --rate=48000 | \
  ffmpeg -f s16le -ar 48000 -ac 2 -i - jam.wav
```

Either way you capture the system audio, which is whatever aither is
sending to your speakers. Stop with `Ctrl+C`. Convert to opus / mp3 /
flac after the fact:

```bash
ffmpeg -i jam.wav -c:a libopus -b:a 192k jam.opus
```

### Versioning with git

Aither has no built-in undo. The patch file *is* the source
of truth, the engine holds runtime state but not history.
Use git as your time machine.

```bash
git init                              # before your first session
git add patches/ && git commit -m "set"
```

During the set:

```bash
git add bass.aither && git commit -m "drop"   # snapshot a sweet spot
git diff bass.aither                          # what did I just change?
git checkout bass.aither                      # restore last committed version
```

Two patterns that pay off:

- **Commit when something works.** Don't wait for "polished" — commit
  the working drop, the working breakdown, the working transition.
  You can `git rebase -i` to clean up later. During the set, every
  commit is a checkpoint you can fall back to in five seconds.
- **`git stash` to try a wild idea.** `git stash`, edit aggressively,
  send. If the new sound rules, commit. If it's a mess,
  `git stash pop` and you're back to where you were.

A failed `aither send` (compile error) doesn't lose the previously
loaded voice — the engine returns `ERR` and keeps running the prior
version. The risk is "patch compiled fine but produces silence /
NaN / wrong audio." That's exactly what `git checkout` is for.

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

For spectral feedback (top peaks, fundamental estimate,
centroid):

```
./aither spectrum                # FFT of master's recent ~0.5s
./aither spectrum bass           # FFT of one voice's recent buffer
./aither audit bass.aither 2     # offline render + FFT, no engine needed
```

`spectrum` works against the engine's recent samples — handy
for live diagnosis ("is my detuned saw really at 110 Hz?").
`audit` is offline (parses + compiles + ticks + analyses
in-process, ~100 ms turnaround) — handy for verifying a
patch produces the spectrum you intended before you bother
playing it.

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

## Spectral synthesis

`osc(saw, 440)` gives you a fine saw when timbre doesn't
matter. When it does, aither treats *sine* as the only real
oscillator primitive and builds everything else out of sums
of sines. This is the Fourier view: every physical resonant
system vibrates as a sum of sines. Writing your timbre as
a sum of sines means writing the physics directly.

The engine primitive is `sum(N, fn)` — evaluate `fn(1) +
fn(2) + ... + fn(N)` at codegen and emit the unrolled sum.
The stdlib wraps it in two user-facing defs.

### `additive(freq, shape, max_n)` — harmonic partials

Partials at integer multiples of `freq`. `shape(n, pf)`
gives the amplitude of the nth partial (whose frequency is
`pf`). Partials above Nyquist contribute zero, so the math
itself band-limits.

```
play saw_lead:
  additive(220, saw_shape, 16) * 0.3

saw_lead
```

That's a 16-harmonic saw. No aliasing, no filter needed.

Shape functions you get for free:

| Shape           | Sounds like                              |
|-----------------|------------------------------------------|
| `saw_shape`     | clean saw (1/n)                          |
| `sqr_shape`     | clean square (odd harmonics only)        |
| `tri_shape`     | triangle (odd harmonics, 1/n²)           |
| `warm_shape`    | rolled-off (1/n²) — dark, hollow         |
| `bright_shape`  | slow roll-off (1/√n) — edgy, harsh       |
| `bowed_shape`   | softer than saw (1/(n+0.5))              |
| `vowel_ah`      | human "ah" — formant peaks near 700 / 1200 Hz |
| `vowel_ee`      | human "ee" — formant peaks near 270 / 2300 Hz |
| `cello_shape`   | cello body resonances + gentle high-freq taper |

### `inharmonic(freq, ratio, amp, max_n)` — arbitrary partials

Partials at user-chosen frequency ratios. `ratio(n)`
returns the multiplier (so the nth partial is at
`freq * ratio(n)`); `amp(n, pf)` its amplitude. Use this
for bells, plates, stiff strings — anything that doesn't
sit on integer multiples.

```
play bell:
  let strike = impulse(0.5)
  let env = pluck(strike, 2.0)
  inharmonic(440, bar_partials, bell_decay, 5) * env * 0.3

bell
```

Ratio functions (input `n`, returns frequency multiplier):

| Ratio            | Physics                                  |
|------------------|------------------------------------------|
| `stiff_string`   | piano / taut string, slight sharpening   |
| `stiff_cello`    | cello-sized string, gentler sharpening   |
| `bar_partials`   | metal bar (1, 2.756, 5.404, 8.933, 13.345) |
| `plate_partials` | metal plate (1, 2.295, 3.873, 5.612, 7.682) |
| `phi_partials`   | φⁿ spacing — shimmery, bell-like, no fundamental |

Amp functions (input `(n, pf)`, returns amplitude):

| Amp             | Roll-off                                  |
|-----------------|-------------------------------------------|
| `soft_decay`    | 1/√n — bright but not harsh               |
| `bell_decay`    | 1/n — classic bell roll-off               |
| `bright_decay`  | 1/n^0.3 — very slow roll-off, edgy bell   |

### Worked example — a cello

```
play cello:
  let f   = 110                         # A2
  let gate = if (t mod 4) < 2 then 1 else 0
  let env = swell(gate, 0.25, 0.6)      # bow attack + release
  inharmonic(f, stiff_cello, cello_shape, 24) * env * 0.2

cello |> reverb(2.5, 0.2)
```

Why this sounds cello-ish:

- **`stiff_cello`** gives a realistic partial spacing for a
  thick vibrating string — not quite-harmonic, with each
  higher partial slightly sharp. That slight inharmonicity
  is what distinguishes a physical string from a pure saw
  wave.
- **`cello_shape`** layers three Gaussian resonance peaks
  around 200, 400, and 1500 Hz (A0 top plate, T1 back, and
  bridge brightness) on top of a gentle 1/n base. Multiplied
  partial-by-partial, this is the body filter built into the
  amplitude spectrum rather than bolted on afterwards.
- **24 partials** covers the full audible range at 110 Hz
  without crossing Nyquist (24 × 110 ≈ 2.6 kHz is well
  under 24 kHz).

Try `additive(f, cello_shape, 24)` instead — you'll hear
a cleaner, more synthetic cello because the integer-ratio
partials lose the string-physics character.

### Cost

`sum(N, ...)` compiles to N textual instances of its body.
At `N = 16` with `phasor` + `sin` + scalar math inside the
lambda, that's 16 phasor state slots and 16 sine calls per
sample — around 1 µs per voice on modern hardware. Default
to `N = 8` for cheap pads, `16` for leads, `24` for
characterful instruments. Push to 32 for extreme formant
detail; past that, spend the CPU on something else.

### Writing your own shape or ratio

Any def `(n, pf) → float` is a valid `shape`. Any def
`(n) → float` is a valid `ratio`. Read a spectrogram of an
instrument you like and transcribe the peaks:

```
def my_shape(n, pf):
  let basis = 1.0 / n
  let peak1 = exp(-pow((pf - 450) / 80, 2)) * 3.0
  let peak2 = exp(-pow((pf - 1600) / 200, 2)) * 2.0
  basis * (1.0 + peak1 + peak2)

play mine:
  additive(220, my_shape, 16) * 0.3

mine
```

The shape is just math. Tune the constants until it
sounds right.

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
- [SOUND_FRONTIERS.md](SOUND_FRONTIERS.md) — unexplored regions of additive synthesis to chase
- [BUGS_AND_ISSUES.md](BUGS_AND_ISSUES.md) — known issues + session logs
- [stdlib.aither](stdlib.aither) — the composition layer
