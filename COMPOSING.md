# Composing music in aither

Notes to future-me. Read before writing a patch.

## The one idea

**The composition is the DSP.** A piece is one expression,
evaluated 48,000 times a second. There is no score, no
scheduler, no "when does X happen." Everything —
melody, rhythm, sections, dynamics — is a signal.

## File structure

Every file has three kinds of thing and exactly one final
expression. The final expression is the voice's output.

```
# 1. helpers (reusable, isolated; do not see file-level lets)
def ease(x):
  let c = clamp(x, 0, 1)
  c * c * (3 - 2 * c)

# 2. shared file-level state (seen by every play)
let tempo = 140.0 / 60.0
let kTrig = impulse(tempo)
let kEnv  = discharge(kTrig, 10)
let sc    = 1 - kEnv * 0.75

# 3. named parts: each returns [L, R] (or a float for mono)
play kick:
  let s = sin(TAU * phasor(50 + discharge(kTrig, 35) * 170)) * kEnv
  [s, s]

play bass:
  let s = osc(saw, 55) |> lpf(150 + kEnv * 1500, 0.85) * sc
  [s, s]

# 4. final expression: the voice's output
(kick + bass) |> drive(1.2)
```

`def` is helpers. `play` is instruments — each becomes a
named value bound to the file's scope. The final
expression composes those named values into the stereo
mix. Binary arithmetic is element-wise on stereo pairs
with scalar broadcast, so `kick + bass` and `lead * 0.5`
do what you'd expect.

## `def` vs `play`

- **`def name(args):`** — a function. Reusable, parameterised,
  sees only what it receives. Good for filters, envelopes,
  math helpers. Calls get per-call-site state.
- **`play name:`** — a named block inlined into the file's
  flow. Sees all file-level lets and vars. Has an
  engine-controllable gain and a name you can `mute` /
  `solo` / `fade` from the CLI.

The asymmetry is by design: `def` is a *function*, `play`
is a *piece of the file's body with a name*. Think of it
as functions vs. top-level blocks in any language.

## Scope

| Declaration          | Visible                                     | State model                       |
|----------------------|---------------------------------------------|-----------------------------------|
| file-level `let`     | everywhere below                            | computed once per sample          |
| file-level `var`     | everywhere                                  | persistent, keyed by name         |
| file-level `def`     | everywhere (hoisted)                        | n/a (callable)                    |
| `play` body `let`    | inside that `play` only                     | computed once per sample          |
| `play` body `var`    | file-level by name (not play-local)         | persistent, shared by name        |
| `def` body `let`     | inside that `def` only                      | computed once per call            |
| `def` body `var`     | per-call-site (each call gets own state)    | persistent per call location      |

A few consequences worth knowing:

- `let` inside two different `play` blocks can share a name without collision — they're independently scoped.
- `var` inside a `play` is file-level by name. Write `var kick_count = 0` and `var bass_count = 0` (not `var count` twice) if you want them independent.
- A `def` called from multiple places gets independent state per call site. That's why `osc(sin, 440) + osc(sin, 880)` gives two independent phasors without any ceremony.

## The composition clock

`start_t` is the time the voice was first loaded. Use it:

```
let pos = t - start_t   # elapsed seconds since the piece started
```

`pos` is your composition timeline. Shape the piece as a
function of `pos`:

- fade-in, steady, fade-out → smoothstep ramps of `pos`
- section changes → crossfades keyed on `pos`
- climax at 2:30 → envelope peaking at `pos = 150`

## Piano-roll instincts to resist

When I find myself reaching for these, stop and think
signals instead:

| Piano-roll thinking                | Signal thinking                         |
|------------------------------------|-----------------------------------------|
| "2 notes per second"               | "frequency changes at 2 Hz"             |
| "ADSR envelope per note"           | "amplitude as a function of `pos`"      |
| "instant pitch change"             | frequency is continuous by default      |
| "play section B at 1:00"           | crossfade A→B centered on `pos = 60`    |
| "sequence of events"               | `wave`, LFO, or `freq(t)` directly      |
| "quantized grid"                   | only quantize if the *music* wants it   |
| "bar counter variable"             | a slow phasor *is* the bar position     |
| "shift note offsets for swing"     | warp the phase: `p + sin(p*π) * swing`  |
| "velocity table lookup"            | velocity is an amplitude oscillator     |

Continuous pitch, continuous amplitude, continuous
sections. Quantize only when the genre demands it.

## Idioms

### Smoothstep (ease 0→1)

```
def ease(x):
  let c = clamp(x, 0, 1)
  c * c * (3 - 2 * c)
```

Use for fades, section crossfades, any 0→1 transition
that shouldn't be linear.

### Gate (1 between t0 and t1)

```
def gate(pos, t0, t1):
  ease((pos - t0) / 0.5) * (1 - ease((pos - t1) / 0.5))
```

### Breathing / LFO

```
let breath = (sin(TAU * pos / 20) + 1) * 0.5   # 20s period, 0..1
```

### Slow pitch drift through a scale (signal-native melody)

Instead of `wave(...)` stepping through notes, quantize a
slow LFO to scale intervals:

```
let scale = [0, 2, 4, 7, 9]   # pentatonic semitone offsets
let drift = (sin(TAU * pos * 0.03) + 1) * 0.5 * 5
let step  = scale[int(drift) mod 5]
let freq  = 261.6 * pow(2, step / 12)
```

Use `wave` + `impulse` when you want *discrete events*
(bells, plucks). Use the quantized-LFO trick when you
want a melody that *breathes*.

### Rhythm is oscillation (see docs/guides/RHYTHM_PHILOSOPHY.md)

The phasor *is* the clock. Every rhythmic concept derives
from it by math — no scheduler, no event list. A gate is
a phasor crossing a threshold. An envelope is a function
of the phasor. A velocity pattern is an amplitude
wavetable.

```
let beat    = phasor(tempo)                   # 0..1 across each beat
let eighths = phasor(tempo * 2)               # eighth-note phasor
let bar     = phasor(tempo / 16)              # 16-beat bar cycle
let vel     = wave(tempo * 4, [1, .6, .8, .5, 1, .4, .9, .7])
```

No special "sequencer," no "bar counter" variable.

### Swing / groove via phase warping

Humanisation is not event-shifting; it is *phase warping*.
The beats still fall at the same phases; the *shape* of
time between them curves.

```
let phase  = phasor(tempo * 2)                # 8th-note phasor
let warped = phase + sin(phase * PI) * 0.08   # ~8% swing
let env    = exp(-warped * 40)                # envelope on warped time
```

Because time was warped continuously, the swing is alive
at every sample, not quantised to a 16th grid.

### Structural variation via slow phasor

To drop the kick every 16 bars, or swap bass patterns
every 32 bars — don't count bars. Build a slow phasor
that *is* the position in the long-form cycle, and gate
layers with it.

```
let barPhase = phasor(tempo / 16)         # 16-beat bar cycle
let kickGate = if barPhase > 0.94 then 0 else 1   # last beat dropped

let longPhase = phasor(tempo / 128)       # 32-bar cycle
let patternXfade = ease(longPhase * 2 - 0.5)      # A↔B every 32 bars
```

Slow phasors are bar positions. Fast phasors are beats.
Audio-rate phasors are pitches. Same primitive, different
timescale.

### Sidechain / pump (duck one layer with another's envelope)

Dance-music glue: the kick's envelope ducks the rest of
the mix momentarily so the kick has space to breathe.
Signal-native — just invert an envelope and multiply.

```
let kEnv     = discharge(impulse(tempo), 10)  # kick envelope
let sidechain = 1 - kEnv * 0.7                # ducks to 0.3 when kick hits
let bassDuck  = bass * sidechain
let leadDuck  = lead * sidechain
```

No new primitive. The thing that makes pro dance mixes
sound professional is literally one multiplication.

### Sections

```
let A = ... ; let B = ...
let x = ease((pos - 60) / 8)   # 8s crossfade at 1:00
A * (1 - x) + B * x
```

### Stereo

Return `[L, R]` at the end of the patch. Mono patches can
still return a single float — the engine mirrors it. Keep
heavy layers (drones) centred; pan motion layers on a
period much longer than the breath so stereo reads as a
slower layer, not a wobble.

```
let panLfo = sin(TAU * pos / 50)        # 50s cycle
let ang    = (panLfo + 1) * PI / 4
let L = centre + motion * cos(ang) * 1.41
let R = centre + motion * sin(ang) * 1.41
[L, R]
```

Equal-power pan (`cos`/`sin` of an angle in `[0, π/2]`)
keeps loudness constant as the source moves. The `1.41`
compensates for the single-channel sum; drop it if you
want the pan to also reduce total energy at the extremes.

## Common bugs I have hit

- **`wave(freq, notes)` cycles the whole array at `freq` Hz.**
  For N notes at R notes/sec: `wave(R / N, notes)`.
  Not `wave(R, notes)`. This cost an afternoon.

- **Multi-line array literals don't parse.** Keep the
  whole `[...]` on one line.

- **`x |> f(…) + y` is a parse error.** Pipe + binary op
  don't bind how you'd expect. Bind the chain first:
  `let z = x |> f(…); z + y`.

- **Expressions don't continue across newlines.** You
  can't write `let y = a + b\n          + c + d`. Keep
  the whole RHS on one line. Cost me another reload.

- **Per-voice state is keyed by call-site counter.** Same
  call order across reloads = state preserved. Reorder
  `osc` calls and phase jumps. Tolerable; just know it.

- **`if/else` chains as scale lookup are a smell.** If you
  find yourself writing `if i == 0 then a else if i == 1
  then b else ...`, use an array: `let scale = [a, b, c, d,
  e]; scale[i]`. Constant literal arrays are compile-time
  hoisted (zero runtime alloc).

## Performance

- The engine is a bytecode VM, not tree-walking. Even so,
  four voices with reverb each can hit 60%+ of one core.
- **Reverb is expensive.** A Schroeder reverb does ~10
  array accesses per sample. One shared reverb beats four
  per-voice reverbs.
- **`wave` and `impulse` are cheap.** `lpf`/`hpf`/`bpf`
  are cheap. `reverb` and `fbdelay` with long buffers are
  not.
- If a patch sounds muddy, suspect *too many voices doing
  their own reverb* before suspecting the synthesis.

## Before writing

1. What is the piece *as a function of `pos`*? Sketch the
   arc in words: silent → rise → breathe → recede.
2. What is the tonal centre (one frequency, fixed)?
3. What layers? Each one is a term in the final sum.
4. How do the layers relate (breathe against each other,
   crossfade, stack)?
5. One expression. One file. One voice.

## Stdlib quick reference

Signal sources: `phasor(f)`, `noise()`, `osc(shape, f)`,
`pulse(f, w)`, `wave(f, arr)`.
Shapes: `sin`, `cos`, `tan`, `saw`, `tri`, `sqr`.
Filters: `lp1`, `hp1`, `lpf`, `hpf`, `bpf`, `notch`.
Delays: `delay`, `fbdelay`. Reverb: `reverb(sig, rt60, wet)`.
Physics: `impulse`, `resonator`, `discharge`.
Helpers: `gain`, `fold`, `tremolo`, `slew`, `pan`.
Character: `drive`, `wrap`, `bitcrush`, `downsample`, `dropout`.

Always read `stdlib.aither` before inventing a function —
it is short and probably already has what you need.
