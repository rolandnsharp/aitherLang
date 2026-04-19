# Composing music in aither

Notes to future-me. Read before writing a patch.

## The one idea

**The composition is the DSP.** A piece is one expression,
evaluated 48,000 times a second. There is no score, no
scheduler, no "when does X happen." Everything —
melody, rhythm, sections, dynamics — is a signal.

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

### Sections

```
let A = ... ; let B = ...
let x = ease((pos - 60) / 8)   # 8s crossfade at 1:00
A * (1 - x) + B * x
```

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

Always read `stdlib.aither` before inventing a function —
it is short and probably already has what you need.
