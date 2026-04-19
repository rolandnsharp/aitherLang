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

Write math. Hear sound. Edit the file. Send again.
State persists — no clicks, no restart.

## Your first patch

Create `sine.aither`:

```
osc(sin, 440) * 0.3
```

Send it:

```
./aither send sine.aither
```

A 440 Hz sine wave plays. Edit `440` to `220`, resend.
The pitch drops. The phase continues — no click.

## Building sounds

### Oscillators

`osc(shape, freq)` is the oscillator. The shape is a
math function. The oscillator manages the clock.

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

### Mixing

Addition is mixing:

```
osc(sin, 440) * 0.5 + osc(sin, 880) * 0.25
```

Two oscillators summed. Multiplication is amplitude.

### State

`var` creates values that persist across samples:

```
var phase = 0.0
var env = 1.0

phase = (phase + 440 / sr) mod 1.0
env = env * 0.9999
if env < 0.001: env = 1.0
sin(TAU * phase) * env
```

`var` survives hot-reload. Edit the file, resend —
`phase` and `env` keep their current values.

### Custom functions

```
def pluck(freq):
  noise() * impulse(3) |> resonator(freq, 0.2)

pluck(330) + pluck(440) * 0.5
```

Each call to `pluck` gets its own state. Two plucks,
two independent resonators.

## Live performance

### Multiple voices

Each file is an instrument. Load several:

```
./aither send kick.aither
./aither send bass.aither
./aither send hat.aither
./aither list               # shows what's playing
```

### Fade in and out

```
./aither send pad.aither 4     # fade in over 4 seconds
./aither stop hat 2            # fade out over 2 seconds
./aither stop hat               # stop immediately
```

### Mute and solo

```
./aither mute bass              # silence, state keeps running
./aither unmute bass             # resume from where it was
./aither solo kick 2            # fade everything else out
```

### Clear

```
./aither clear                  # stop everything
./aither clear 4               # fade everything out over 4s
```

## Time-based expressions

### `t` — absolute time

`t` is seconds since the engine started. Use it for
anything that changes over time:

```
osc(sin, 440 + t * 10)           # pitch rises forever
osc(sin, 440) * exp(-t * 2)      # decays to silence
```

### `start_t` — when this voice loaded

`start_t` is the time when this patch was first sent.
Use it for per-voice timing:

```
# fade in over 5 seconds from when sent
let fade = clamp((t - start_t) / 5, 0, 1)
osc(saw, 55) |> lpf(800, 0.5) |> gain(fade * 0.4)
```

```
# play for 10 seconds then silence
let age = t - start_t
if age > 10 then 0
else osc(sin, 440) * 0.3
```

```
# filter sweep from when sent
let sweep = clamp((t - start_t) / 8, 0, 1)
osc(saw, 55) |> lpf(200 + sweep * 4000, 0.5)
```

### Per-voice envelopes

`start_t` resets when a voice is first created, but NOT
on hot-swap. So editing and resending continues the
timeline. To retrigger:

```
./aither stop bass; ./aither send bass.aither
```

Stop and resend — `start_t` resets to now.

## LLM-assisted workflow

Aither is ideal for LLM-assisted music making. The
language is small enough for an LLM to generate correct
patches every time. The workflow:

```
User: "add a bass line"
→ LLM writes bass.aither:
    osc(saw, 55) |> lpf(800, 0.5) |> gain(0.4)
→ aither send bass.aither

User: "make it darker"
→ LLM edits: lpf(800 → lpf(300
→ aither send bass.aither

User: "add a melody in A minor"
→ LLM writes melody.aither:
    let notes = [440, 494, 523, 587, 659, 698, 784, 880]
    osc(sin, wave(4, notes)) * discharge(impulse(4), 6) * 0.3
→ aither send melody.aither

User: "slowly open the bass filter"
→ LLM edits bass.aither:
    let sweep = clamp((t - start_t) / 10, 0, 1)
    osc(saw, 55) |> lpf(300 + sweep * 3000, 0.5) |> gain(0.4)
→ aither send bass.aither

User: "fade out the melody"
→ aither stop melody 4
```

The LLM doesn't iterate the REPL — it rewrites the
expression. Sweeps and transitions are math on `t`.
Structural changes are new patches. Each prompt is
one file write and one send.

## Recipes

### Kick drum

```
discharge(impulse(2), 6) * resonator(impulse(2), 60, 8)
```

### Hi-hat

```
noise() * discharge(impulse(8), 40) |> hpf(8000, 0.1)
```

### Acid bass

```
let freq = wave(2, [55, 55, 82, 55, 73, 55, 98, 55])
let env = discharge(impulse(2), 8)
osc(saw, freq) |> lpf(200 + env * 4000, 0.85) |> gain(0.4)
```

### Drone

```
let a = osc(saw, 55)
let b = osc(saw, 55.1)
let c = osc(saw, 54.9)
(a + b + c) / 3 |> lpf(400 + osc(sin, 0.1) * 300, 0.4) |> reverb(3, 0.5)
```

### FM feedback

```
var fb = 0.0
fb = sin(TAU * phasor(440 + fb * 500))
fb * 0.3
```

### Plucked string

```
noise() * impulse(3) |> resonator(330, 0.2)
```

### Looper

```
osc(saw, 110) |> fbdelay(0.5, 0.5, 1.0)
```

A delay with feedback = 1. That's a looper.

### Bell

```
let strike = impulse(1)
resonator(strike, 800, 0.3)
+ resonator(strike, 1260, 0.4)
+ resonator(strike, 1860, 0.6) * 0.3
```

Three resonant modes at inharmonic ratios.

### Sequenced rhythm

```
let bpm = 128
let pattern = [1, 0, 0, 1, 0, 0, 1, 0]
let hit = wave(bpm / 60, pattern)
hit |> resonator(180, 12) |> fold(1) * 0.4
```

## Going deeper

### Raw phasor access

`osc(sin, 440)` is sugar for `sin(TAU * phasor(440))`.
Use the phasor directly for custom waveforms:

```
let p = phasor(440)
sin(TAU * p) + sin(3 * TAU * p) / 3 + sin(5 * TAU * p) / 5
```

Additive synthesis — three harmonics from one phasor.

### Physics

The damped harmonic oscillator is a universal primitive:

```
var x = 0.0
var dx = 0.0
let w2 = 440 * 440
dx = dx + (-2 * dx - w2 * x + impulse(2) * w2) * dt
x = x + dx * dt
x
```

That's a struck tuning fork. Change the damping, the
frequency, the excitation — same equation, different sound.

### Feedback

Any `var` that reads itself is a feedback loop:

```
var fb = 0.0
fb = sin(TAU * phasor(440 + fb * 500))
fb * 0.3
```

FM self-modulation. One line of state creates an
entire class of synthesis (Yamaha OPL chips).

### Shapes as waveshapers

The shape functions work on any signal, not just
oscillators:

```
osc(sin, 440) * 5 |> fold(1)     # wavefold distortion
osc(sin, 440) |> saw              # transfer function
```

`saw` as a waveshaper maps the signal through the
sawtooth curve. Any math function is a waveshaper.
