# Composing music in aither

Notes to future-me. Read before writing a patch.

## The one idea

**The composition is the DSP.** A piece is one expression,
evaluated 48,000 times a second. There is no score, no
scheduler, no "when does X happen." Everything —
melody, rhythm, sections, dynamics — is a signal.

## What aither is for (and what it isn't)

Aither is not a sequencer. It is not Tidal Cycles. It is not
Sonic Pi. Those tools already do pattern-based melodic
composition extremely well; aither would be a worse copy of
them. What aither uniquely does is **build instruments and
the orchestra around them** — sounds and textures and
dynamics that no slider-driven synth and no batch-rendered
DSL has ever been able to express.

The natural division of labour:

- **Aither makes the instrument and the orchestra.**
  The contract is `f(state) → sample` evaluated 48,000 times
  per second; aither's job is to make that function easy to
  write and reload. Stdlib starter kits cover the common
  paradigms — `additive` / `inharmonic` for spectrum-first
  design (cheap, perfectly band-limited, hot-reload-clean),
  `$state` + `dt` for time-domain physics (transients and decay
  built into the equations), `osc(saw)` for chiptune /
  aliased character, FM via inline `sin(... + sin(...) * d)`
  for sidebanded grit. Pick whichever paradigm fits the
  sound. Plus signal-native textures and dynamics (LFO-modulated
  everything, polyrhythm via products of LFOs, slow morphing
  via incommensurate rates), reactive backing tracks designed
  for live performance.
- **The human plays the melody.** The MIDI keyboard is the
  voice in the music. The most striking aither result so far
  (`fm_swarm.aither` — the "gothic Tesla organ") wasn't a
  generated melody; it was a *timbre* the user discovered and
  played melodies into.

This is a discipline, not a limitation. Resist the urge to
write `let melody = [220, 261, 329, ...]` in a patch. If you
need a melody, plug in the keyboard and play one. The patch's
job is to make the instrument so beautiful, and the backing
so alive, that whatever the human plays sounds like more than
the sum of the keystrokes.

When a backing track needs harmonic motion (chord
progressions, bassline walks) and rhythm (drums, percussion),
those ARE in scope — they're orchestra, not melody. Use
arrays of root frequencies indexed by quantized LFOs (chord
progressions), `phasor`-shaped envelopes (drum hits), `prev()`
edge-detection for downbeat events. These tools *set the
stage*; they don't replace the soloist.

For solo aither pieces (no live performer), lean toward
ambient, drone, generative — genres where evolution and
texture ARE the music. The math-driven non-repetition is the
point.

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
  # Additive saw — same character as osc(saw, 55) but band-limited
  # by construction (no aliasing) and the spectrum is directly
  # tunable via the shape function. This is the preferred default.
  let s = additive(55, saw_shape, 8) |> lpf(150 + kEnv * 1500, 0.85) * sc
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
  flow. Sees all file-level lets and `$state` slots. Has an
  engine-controllable gain and a name you can `mute` /
  `solo` / `fade` from the CLI.

The asymmetry is by design: `def` is a *function*, `play`
is a *piece of the file's body with a name*. Think of it
as functions vs. top-level blocks in any language.

## Scope

| Declaration          | Visible                                     | State model                       |
|----------------------|---------------------------------------------|-----------------------------------|
| file-level `let`     | everywhere below                            | computed once per sample          |
| file-level `$state`  | everywhere                                  | persistent, keyed by name         |
| file-level `def`     | everywhere (hoisted)                        | n/a (callable)                    |
| `play` body `let`    | inside that `play` only                     | computed once per sample          |
| `play` body `$state` | file-level by name (not play-local)         | persistent, shared by name        |
| `def` body `let`     | inside that `def` only                      | computed once per call            |
| `def` body `$state`  | per-call-site (each call gets own state)    | persistent per call location      |
| lambda body `let`    | inside that iteration                       | computed once per iteration       |
| lambda body `$state` | per-iteration of unrolled `sum`             | persistent per iteration / sample |

A few consequences worth knowing:

- `let` inside two different `play` blocks can share a name without collision — they're independently scoped.
- `$state` inside a `play` is file-level by name. Write `$kick_count = 0` and `$bass_count = 0` (not `$count` twice) if you want them independent.
- A `def` called from multiple places gets independent state per call site. That's why `osc(sin, 440) + osc(sin, 880)` gives two independent phasors without any ceremony.
- `$state` in a `sum(N, lambda)` body gives every unrolled iteration its own slot, which is how a `sum(8, n => $x = 0; $dx = 0; …)` modal bank works as eight independent oscillators.

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

### Playing from MIDI (polyphonic)

A MIDI keyboard plays polyphonically — pressing three keys
produces three notes simultaneously. To make a polyphonic
instrument, wrap your per-key synthesis in `midi_keyboard`:

```
play piano:
  midi_keyboard((freq, gate) =>
    additive(freq, warm_shape, 8) * adsr(gate, 0.01, 0.2, 0.7, 0.4))
```

The lambda describes what each held key sounds like;
`midi_keyboard` handles voice allocation, voice stealing
(oldest evicted at the 9th note), and summing. CC reads
(`midi_cc(74)`, etc.) are global and belong outside the
lambda — every voice shares the same knob.

For a different voice count, use `poly(N, ...)` directly:

```
play piano:
  poly(16, (freq, gate) => ...)        # 16 voices
```

The mono primitives `midi_freq()` / `midi_gate()` still exist
(they return the most-recent note) for monophonic synth-lead
behaviour.

The deepest layer is `midi_voice_freq(n)` / `midi_voice_gate(n)`
— for hand-rolled allocation logic that doesn't fit `poly`'s
shape.

### Smoothstep (ease 0→1)

`ease(x)` is stdlib — smoothstep with inputs clamped to
[0, 1]. The canonical fade / section crossfade curve.

```
let fadeIn = ease(pos / 10)             # 10-second fade-in from pos=0
let cross  = ease((pos - 60) / 8)       # 8-second crossfade centred at 1 min
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

### Tempo, beats, bars (drop-in helpers)

Most patches re-derive the same tempo math at the top of the
file. These four lines (or a subset) handle 95% of cases:

```
def at_bpm(bpm):    bpm / 60                    # bps from bpm
def tphase(rate):   (t * rate) mod 1            # wall-clock phasor (shared)
def beat(rate):     tphase(rate)                # alias, reads better
def bar(rate, n):   tphase(rate / n)            # n-beat bar position
```

Then a patch begins:

```
let tempo = at_bpm(140)
let bt    = beat(tempo)              # shared beat phase
let br    = bar(tempo, 16)           # 16-beat bar phase
```

These are conventions, not language features — drop them at
the top of any patch. If you find yourself writing them in
every file for a month, promote them to `stdlib.aither`.

**Shared time vs voice-local time.** The helpers above use
`t` (wall clock), so two voices using `beat(tempo)` lock
together regardless of when each loaded. Use `phasor(rate)`
when you want voice-local time — its accumulator is per-call-site
and starts at 0 when the voice starts.

| Need                                  | Use                |
|---------------------------------------|--------------------|
| Two voices in lockstep on the beat    | `tphase(rate)`     |
| Each voice has its own clock          | `phasor(rate)`     |
| Smooth start (no first-sample pop)    | `phasor(rate)`     |
| Shared start, smooth start            | both — see below   |

The `t`-based helpers can pop on first sample because the
first value is wherever wall-clock phase happens to land.
Your existing `let fadeIn = ease(pos / N)` at the top of
each patch hides this. If you need wall-clock sync *and*
sample-zero smoothness without a fade, snapshot the start
phase once and offset a voice-local phasor:

```
$startPhase = -1
$startPhase = if $startPhase < 0 then (t * tempo) mod 1 else $startPhase
let bt = (phasor(tempo) + $startPhase) mod 1
```

The state-migration model preserves `$startPhase` across hot
reloads, so subsequent edits don't re-snap.

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

A bar-as-phasor is more powerful than a bar-as-counter
because it is a *signal*. Things you can do to a continuous
phase that you can't do to an integer:

```
let bar       = phasor(tempo / 16)
let intensity = ease(bar)                # rises across each bar
let lastBeat  = ease((bar - 0.94) * 16)  # rises in the last beat
```

A counter could tell you *which* bar; a phasor tells you
*where in the bar* you are, with full audio-rate resolution.
Crossfade, swell, gate, modulate — all just functions of the
phase.

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

Stdlib has four stereo helpers:

```
# pan a mono signal left↔right (pos in [-1, 1])
let panPos = sin(TAU * pos / 50)             # 50s cycle
additive(110, warm_shape, 8) * 0.3 |> pan(panPos)

# psychoacoustic width via a 1-30 ms delay on one channel
sin(TAU * phasor(220)) * 0.2 |> haas(8)      # right delayed 8 ms

# mid-side width: 0 → mono, 1 → unchanged, >1 → exaggerated stereo
let stereo = [additive(440, saw_shape, 8) * 0.2, additive(441, saw_shape, 8) * 0.2]
stereo |> width(1.6)

# collapse a stereo signal to mono (e.g. for sidechain)
let level = mono(bass)
```

`pan` uses equal-power (`cos`/`sin`) so total loudness
stays constant across the field. The classic manual form
`[centre + motion * cos(ang) * 1.41, centre + motion * sin(ang) * 1.41]`
is still available if you want to compensate for the
single-channel sum; use `* 1.41` (i.e. `√2`) at the
extremes.

## DHO — the universal oscillator

`dho(force, freq, damp)` is one primitive that integrates the
damped harmonic oscillator:

```
ddx = -k*x - c*dx + force      with k = (TAU*freq)^2, c = 2*damp*omega
```

Its parameter regions ARE the synthesis paradigms. Sweep `damp`
or change what you wire into `force` and you walk between named
paradigms without ever changing primitive:

| Region                                  | What you get                                |
|-----------------------------------------|---------------------------------------------|
| `damp = 0, force = 0, init dx ≠ 0`      | free sine — an additive partial             |
| `damp ≈ 0.005, force = impulse`         | struck bell / modal physical instrument     |
| `damp ≈ 0.2, force = audio_in`          | resonant bandpass — subtractive filter      |
| `damp = 1.0, force = step`              | critical-damped slew — envelope shape       |
| `damp ≈ 0, freq = 0.5 Hz, force = 0`    | LFO / sub-audio modulator                   |
| chained at formant Hz, `force = speech` | vowel / wood-box body resonance             |

Three small expressions, same primitive, categorically different
sounds:

```
# Bell strike — long ring at 440 Hz
dho(if t < dt then 100000000 else 0, 440, 0.005)

# Resonant filter — narrow bandpass at 800 Hz, driven by external audio
dho(audio_in * 50000, 800, 0.2)

# Slow envelope — sub-audio critically-damped slew driven by gate
dho(g * 1500, 5, 1.0)
```

For demonstration, `patches/dho_walk.aither` wires K1 to `damp` on
a struck DHO. Holding a key and turning the knob walks audibly
from sustained sine → bell → pluck → thunk → silence — one
oscillator, one parameter, every paradigm.

`dho_v(force, freq, damp)` returns the velocity (`dx`) of the same
equation at its own call site. Useful for chained physics (one
oscillator's velocity drives another's force) and as an audio
output — velocity sounds different from position for resonant
systems (it's a 90°-shifted, frequency-emphasised view).

**Why this is `f(state)`-native.** In a modular synth you pick a
"filter module" or an "FM module"; the paradigm is baked into
the cabling. DHO inverts that: the parameters ARE the paradigm.
A patch that reads `dho(force, freq, damp)` lets the user — and
the running knob — slide continuously between the paradigms
without re-routing. This is what `f(state) → sample` looks like
when one equation generalises a half-dozen named techniques.

**Force is in acceleration units.** `force` adds directly to
`ddx`, so a constant force settles to `x = force / k`. At audio
frequencies `k = (TAU*freq)^2` is huge — a 440 Hz oscillator has
`k ≈ 7.6M`, so impulse-style strikes need force on the order of
`1e7`–`1e8` to ring at unit amplitude. For LFO/envelope use at
sub-audio `freq`, `k` is small and modest force values work
directly. Wrap with a stdlib def if you want a normalised
interface for one specific use; the raw primitive keeps the math
visible at the call site.

DHO does NOT replace `additive` for hundred-partial drones —
`sin(TAU * phasor(f))` stays the cheap workhorse for sums of
many sines. DHO is for *expressive single voices* where one knob
should walk continuously between named timbres.

## Pair operations — operations from complex algebra

aither has no complex-number TYPE, but it has the OPERATIONS that
make complex algebra useful: rotate, multiply, magnitude, phase,
Hilbert transform, frequency shift. They take and return ordinary
floats — the "pair" is just two scalars, no special type. The
pair-returning functions reuse aither's existing `[L, R]` plumbing,
so a result is destructured the same way you index a stereo pan
output: `out[0]` for the real part, `out[1]` for the imaginary.

Scalar-in/scalar-out:

| call                       | does                              |
|----------------------------|-----------------------------------|
| `magnitude(re, im)`        | `sqrt(re² + im²)`                 |
| `phase(re, im)`            | `atan2(im, re)`                   |
| `freq_shift(signal, hz)`   | shift signal up by `hz` (negative shifts down) |

Pair-in/pair-out (return a 2-element value; bind with `let p = …`,
then use `p[0]`, `p[1]`):

| call                           | does                                          |
|--------------------------------|-----------------------------------------------|
| `cmul(a_re, a_im, b_re, b_im)` | complex multiplication: `(ac − bd, ad + bc)`  |
| `cdiv(a_re, a_im, b_re, b_im)` | complex division                              |
| `cscale(s, re, im)`            | scalar × pair                                 |
| `rotate(re, im, omega)`        | rotate by `omega` radians                     |
| `analytic(signal)`             | `(signal, hilbert(signal))` — Hilbert pair    |

`+` and `*` are NOT overloaded for complex. `(re, im) * (re', im')`
does componentwise multiplication (Hadamard product, NOT `cmul`).
Reach for the named functions when you mean the algebra.

### `freq_shift` — the headline move

A frequency shifter slides every spectral component by the same Hz
offset (not by the same ratio). A 220 Hz fundamental + 440 + 660
becomes 320 + 540 + 760 with a +100 Hz shift — partials that used
to be integer multiples are no longer harmonically related, so the
result reads as inharmonic but coherent. No additive or FM recipe
can do this because they all act on ratios.

```
let warm = additive(220, warm_shape, 8)
freq_shift(warm, midi_cc(74) * 200 - 100)
```

`patches/freq_shifter.aither` is the live demo: a held drone with K1
sweeping shift Hz across ±200, walking smoothly from harmonic →
inharmonic → enharmonic territory.

### Mandelbrot iteration as a sound source

`cmul + add` is enough to write a Mandelbrot voice in three lines:

```
$zr = 0.0
$zi = 0.0
let zsq = cmul($zr, $zi, $zr, $zi)
$zr = zsq[0] + cReal
$zi = zsq[1] + cImag
$zr      # the audio
```

`patches/mandelbrot_voice.aither` adds a periodic reset and a
magnitude clamp, but the engine is one line of complex arithmetic.
Sweep `(cReal, cImag)` and the timbre walks across the Mandelbrot
plane: inside the cardioid → silent (fixed point); just past the
boundary → chaotic but bounded → the aether-shimmer zone; far
outside → escape and silence.

### Cost notes

- Pair ops are 4 mults + 2 adds at most (`cmul`, `rotate`). Trivial.
- `analytic` and `freq_shift` carry a 32-slot Hilbert pair (4-stage
  IIR allpass per branch). ~30 multiply-adds per sample. Fine for
  one or two voices; don't put one inside a `sum(N, …)` over many
  partials.

## State that holds an array

`$state` cells hold scalar `float64` by default — fast, flat, fixed
shape. For the cases where a voice needs *structured* state — a
running history, a mutable scale, an evolving wavetable — `$state`
can also hold an array handle.

The seven operations:

| call                              | does                                                                                |
|-----------------------------------|-------------------------------------------------------------------------------------|
| `array_make(N)`                   | reserve a per-call-site array (length=N, capacity=N, all zeros). Returns a handle.  |
| `array_get(arr, i)`               | read; out-of-bounds returns 0 (no crash)                                            |
| `array_set(arr, i, v)`            | write; out-of-bounds is a no-op                                                     |
| `array_len(arr)`                  | current length                                                                      |
| `array_push(arr, v)` / `pop(arr)` | grow / shrink length within capacity                                                |
| `array_resize(arr, n)`            | truncate or pad with zeros                                                          |

`array_make(N)` is per-call-site: the same call site returns the
same handle every tick, the array's contents persist. So bind once
at the top of the patch and read the handle as a regular `double`:

```
let scale = array_make(8)
$inited = 0
let _seed = sum(8, n =>
  if $inited > 0.5 then 0.0 else array_set(scale, n - 1, 110 * pow(2, (n - 1) / 12)))
$inited = 1
```

Then read from downstream:

```
sum(8, n => additive(array_get(scale, n - 1), warm_shape, 6))
```

### When an array beats a scalar `$state`

When the patch needs to remember more than ~3-4 values (a pitch
history, an evolving scale, a per-step velocity table), an array
is one allocation instead of a forest of `$slot_0`, `$slot_1`, …
cells. Plus the index can be computed at runtime — useful for
"every bar mutate one of N entries" patterns where the entry isn't
known until the trigger fires.

`patches/adaptive_scale.aither` keeps an 8-element chord in an
array; every K1 seconds, one slot is rewritten toward the held MIDI
pitch (with K2 controlling pull-strength). Hold a key for a minute
and the chord migrates toward your input — the patch "learns" you.

### Don'ts

- Capacity is fixed at codegen time. `array_make(N)` requires `N` to
  be a numeric literal; you can't size by a runtime value.
- Not the same thing as the constant arrays you pass to `wave()`
  (`let notes = [220, 247.5, …]`). Those are immutable, hoisted to
  static const data, and indexed for compile-time scale lookup. The
  `array_*` family is mutable, lives in the voice pool, and is the
  right tool when you want runtime mutation.
- Don't put `array_make` inside a function called from many sites:
  each call site allocates its own region. Allocate once at top
  level, pass the handle around.

## Timbre choice: which paradigm for which sound

Aither's contract is `f(state) → sample`. Synthesis paradigms
are different ways of WRITING that function. Pick whichever
fits the sound you're after; the engine doesn't care.

| When you want…                                              | Reach for                              |
|-------------------------------------------------------------|----------------------------------------|
| A musical sound from its spectrum (pads, leads, vocal-like) | `additive(f, shape, N)`                |
| Bells, plates, stiff strings — non-integer partials         | `inharmonic(f, ratio, amp, N)`         |
| Plucked / bowed / struck physical instruments               | `pluck_string`, `bowed_string`, `struck_bar`, `tuning_fork` (stdlib — **WIP, sounds naive vs additive**) |
| A vibrating physical object — transients, decay built-in    | `$x; $dx; ẍ + 2γẋ + ω²x = F(t)` (raw physics) |
| Sidebanded / FM grit — aliasing as character                | inline `sin(carrier + sin(modf) * depth)` |
| Chiptune / lo-fi / digital-sounding                         | `osc(saw, f)`, `osc(sqr, f)`           |

These are recipes, not language commitments. The stdlib ships
starter kits for the most common ones (additive, inharmonic);
the rest you can write inline with `$state`, `phasor`, `noise`,
and a few cycles of math.

### Physical instruments (work in progress)

The stdlib ships four physical-instrument defs as starter sketches.
**They sound naive compared to the additive recipes above** — reach
for `additive` / `inharmonic` when you want a sound that feels like
an instrument. Use these as templates you'd tune yourself, or wait
until they're polished. Live test on 2026-04-25 confirmed each one
produces an "approximately correct" sound (a recognizable pluck, a
recognizable strike), not yet "good enough to want to play."

For sounds where
excitation-response physics is the discriminating character:

- `tuning_fork(strike, freq)` — single damped HO, the canonical
  inline-physics example. Read its body to learn the pattern.
- `pluck_string(strike, freq, brightness)` — Karplus-Strong: a
  delay-line + LP-feedback loop. The LP runs every iteration so
  the sound darkens as it rings out.
- `bowed_string(bow, freq)` — modal bank with continuous noisy
  excitation. The bow is whatever signal you pass in; try
  `noise() * pressure + sin(TAU * phasor(freq) * vib_factor)`.
- `struck_bar(strike, freq)` — modal bank with `bar_partials`
  ratios and mode-dependent damping (high modes decay faster
  than low ones — the metal-bar bright→dark evolution).

Additive does NOT cover these well. A plucked string with a
bolted-on envelope sounds synthetic; the same sound via
Karplus-Strong is unmistakable. The `tuning_fork` body shows
the integration literally inline (`$x = 0; $dx = 0; $dx = $dx +
… * dt; $x = $x + $dx * dt`). The modal banks
(`bowed_string`, `struck_bar`) iterate the same equation
per-mode by writing the integration *inside* `sum(K, n => …)`
— each unrolled iteration claims its own per-mode `$x` /
`$dx` slots. Read `tuning_fork` once and the modal banks
read like the same code N times over.

```
play strings:
  midi_keyboard((freq, gate) =>
    pluck_string(noise() * impulse(0.5) * gate, freq, 0.7) * 0.3)
```

**Hot-reload caveat.** These defs use `$state` slots for state. On reload
with parameter changes, the system can transiently re-equilibrate
(an audible click on big jumps). For knob-driven parameters
(`midi_cc`), no issue — knobs change smoothly. For code edits,
large parameter swings on a ringing instrument may pop. This is
acceptable for live coding; it's the same situation as any
stateful filter with a large cutoff jump.

Rules of thumb for picking:

- **Additive (`additive`/`inharmonic`) is the most hot-reload-
  friendly.** Sums of sines have no conserved quantities, so
  parameter changes during reload are musically smooth. Best
  default for live-coded music.
- **Physics (`$state`-based damped HOs, Karplus, etc.) gets you
  transients and decay shape from the equation itself.** No
  manual envelopes. But a parameter change during reload can
  cause a transient as the system re-equilibrates — wrap
  parameters in `slew()` if it bothers you.
- **`osc(saw)` and FM** are honest about being aliased
  paradigms. Reach for them when the aliasing IS the character
  (chiptune, broken-radio, DX7 grit). Otherwise additive gives
  you what you actually wanted, cleaner.
- **All paradigms compose freely.** A patch can mix additive
  pads, a `$state`-based bell, a Karplus pluck, and a saw bass —
  they're all just `f(state) → sample` and the engine sums
  them.

Shape functions for `additive` cover the common moods:
`warm_shape` for pads, `bright_shape` for edgy leads,
`vowel_ee`/`vowel_ah` for vocal pads, `cello_shape` for bowed
textures, `saw_shape` for a clean band-limited saw. `N=8` for
pads, `16` for leads, up to `24` for characterful features.
CPU is linear in N — `N=8` is essentially free, `N=24` is fine
for one voice, push past 32 only with reason.

Inharmonic ratio functions: `bar_partials` (struck metal),
`plate_partials` (struck plate), `phi_partials` (φ-spaced
shimmer), `stiff_string` (piano-like), `stiff_cello` (cello
body). Each is a `n → frequency_multiplier` def — write your
own for unusual tunings.

All three compose with envelopes, effects, pans the same way.
Wrapped in `midi_keyboard` so chords work:

```
play voice:
  midi_keyboard((freq, gate) =>
    additive(freq, cello_shape, 16) * swell(gate, 0.1, 0.4) * 0.2) |> reverb(2.5, 0.15)
```

Nothing else in the patch cares which synthesis style you
picked — swap `osc(saw, freq)` ↔ `additive(freq, saw_shape, 16)`
↔ `inharmonic(freq, stiff_string, soft_decay, 16)` freely.

### Tabular ratio / shape functions

A ratio fn that maps `n → multiplier` from a fixed table
(e.g. `bar_partials` with its five modes) is just a
let-bound array and a lookup:

```
def bar_partials(n):
  let p = [1.0, 2.756, 5.404, 8.933, 13.345]
  p[n - 1]
```

The stdlib ships this form. `else if` chains also parse
if you prefer them, but the array form reads better and
lets you sketch a custom tuning by editing one line.
Out-of-range indices wrap (`p[5]` reads `p[0]`), so either
size your table to `max_n` or clamp inside the fn.

## Live-performance knob design — the acid_complex recipe

`patches/acid_complex.aither` works as a live-set patch because
every per-instrument knob does something audibly radical while
the music stays musical. Three knobs per instrument:

1. **PARADIGM crossfade** — three categorically different synthesis
   recipes computed in parallel, weighted by triangular zones across
   the knob. Same notes, three sound worlds in one sweep.
2. **CHARACTER** — one knob that controls multiple related parameters
   together (sweep depth + body damp combined; cutoff + resonance
   combined). Cleaner than two knobs that each do half a thing.
3. **freq_shift** — the complex-algebra knob. ±400 Hz spectrum shift
   on the instrument's output. Walks harmonic→inharmonic without
   leaving the groove.

Plus volume sliders per instrument so the performer mixes by hand.

### The paradigm-crossfade pattern (the workhorse)

```
let paradigm = midi_cc(74)

# Compute all three paradigms every sample. Cheap; the cost is
# fixed regardless of knob position. Total CPU is the budget for
# choosing freely between them.
let A = ...   # paradigm A — recipe 1
let B = ...   # paradigm B — recipe 2
let C = ...   # paradigm C — recipe 3

# Triangular crossfade weights, equal-power across three zones.
let wA = max(0, 1 - paradigm * 2)
let wB = (if paradigm < 0.5
          then paradigm * 2
          else 1 - (paradigm - 0.5) * 2)
let wC = max(0, (paradigm - 0.5) * 2)
let mix = A * wA + B * wB + C * wC
```

Three paradigms is the sweet spot — gives two distinct transitions
in one knob throw. Each paradigm should be a real synthesis recipe
(additive, struck DHO, noise+filter, FM), not three variations of
the same idea. The cost-per-sample is fixed regardless of knob
position, so the user gets free crossfade with no glitches.

### The kick recipe — three paradigms in parallel

From `patches/acid_complex.aither`:

```
# A — 808 sub: pitch-swept sine
let sweepDepth = 60 + kickPunch * 180
let pSweep   = 35 + discharge(kTrig, 30) * sweepDepth
let kickA    = sin(TAU * phasor(pSweep)) * kEnv

# B — DHO body strike: damp tightens with K2
let bodyDamp = 0.04 - kickPunch * 0.025
let kickB    = dho(kTrig * 50000000.0, 200, bodyDamp) * 0.5

# C — FM-distorted boom: sine modulating sine, deeper with K2
let fmDepth  = 0.5 + kickPunch * 4.0
let modSig   = sin(TAU * phasor(pSweep * 1.5)) * fmDepth
let kickC    = sin(TAU * phasor(pSweep) + modSig) * kEnv * 0.7

# Triangular crossfade
let kwA = max(0, 1 - kickParadigm * 2)
let kwB = (if kickParadigm < 0.5
           then kickParadigm * 2
           else 1 - (kickParadigm - 0.5) * 2)
let kwC = max(0, (kickParadigm - 0.5) * 2)
let kickClick = noise() * discharge(kTrig, 200) * 0.3
let kickRaw   = kickA * kwA + kickB * kwB + kickC * kwC + kickClick

# Per-instrument freq_shift — the complex-algebra knob
let kickShifted = freq_shift(kickRaw, kickShift)
let kickSig     = kickShifted * kickVol * 0.5
```

Three kick paradigms fundamentally different in synthesis method
(time-domain pitch sweep / physical model / FM). The PUNCH knob
controls multiple aspects of each (sweep depth + body damp + FM
depth) so you only need one character knob, not three. The
freq_shift on the OUTPUT lets the kick walk into metallic-ping or
subterranean-thud territory without changing the recipe.

### The acid bass recipe — paradigm + density + shift

```
# A — Acid: saw → DHO resonant LPF
let saw   = additive(bFreq, saw_shape, 10)
let cutA  = 600 + bEnv * 3200 * (0.5 + bAcc * 0.5)
let bassA = dho(saw * 80000.0, cutA, 0.025) * bEnv * (0.4 + bAcc * 0.6)

# B — Bell-bass: struck DHO at the note pitch (force scaled by
# frequency so peak amplitude stays consistent across notes)
let bForceB = 22000.0 * bFreq
let bassB   = dho(bTrig * bForceB, bFreq, 0.008) * bEnv * (0.5 + bAcc * 0.5) * 0.4

# C — Noise-formant: white noise into resonant DHO BPF at 3rd harmonic
let nAmp  = bEnv * (0.3 + bAcc * 0.7) * 6000000.0
let bassC = dho(noise() * nAmp, bFreq * 3, 0.025) * 6.0

# Same triangular crossfade
let bwA = max(0, 1 - bassParadigm * 2)
let bwB = (if bassParadigm < 0.5
           then bassParadigm * 2
           else 1 - (bassParadigm - 0.5) * 2)
let bwC = max(0, (bassParadigm - 0.5) * 2)
let bassMix = bassA * bwA + bassB * bwB + bassC * bwC

# K5 DENSITY — sawtooth harmonic voices on octave/fifth/seventh
let envH  = bEnv * (0.4 + bAcc * 0.6)
let h_oct = additive(bFreq * 2.0,  saw_shape, 6) * envH
let h_fth = additive(bFreq * 3.0,  saw_shape, 5) * envH
let h_7th = additive(bFreq * 3.56, saw_shape, 4) * envH
let w_oct = clamp(bassDensity / 0.33,          0, 1)
let w_fth = clamp((bassDensity - 0.33) / 0.33, 0, 1)
let w_7th = clamp((bassDensity - 0.66) / 0.34, 0, 1)
let bassStack = (bassMix
             + h_oct * w_oct * 0.36
             + h_fth * w_fth * 0.30
             + h_7th * w_7th * 0.26)

let bassShifted = freq_shift(bassStack, bassShift)
let bassSig     = bassShifted * bassVol * 0.5
```

Same pattern as kick, but the second knob (DENSITY) crossfades in
harmonic voices rather than collapsing parameters. The harmony
voices use the SAME `bFreq` so they always hit the right notes
regardless of which pattern step is playing.

### Why the amplitude normalization matters

Different synthesis recipes naturally produce wildly different
peak amplitudes:

- Saw → DHO LPF: peaks around 0.5 (filtered)
- Struck DHO at low freq: peak ≈ `force / (sr × ω)`, which scales
  with frequency — for `bForceB = 22000 × bFreq`, peak ≈ 0.7
  regardless of which note plays
- Noise → narrow DHO BPF: very quiet from broadband averaging,
  needs large gain (`* 6.0`) to match the others

If you don't normalize, one paradigm dominates the crossfade and
the other knob positions sound dead. Always set each paradigm to
the same target peak (≈0.4-0.5) before applying weights.

### Adapting this to a new instrument

Copy the structure:

1. Pick three categorically different recipes (additive / struck
   DHO / noise-driven; or saw / FM / wavetable; or any triple).
2. Normalize their peaks to the same level.
3. Crossfade with triangular weights.
4. Add ONE character knob that controls multiple related params.
5. Add `freq_shift` on the OUTPUT for the complex-magic move.
6. Wrap with a volume control (slider).

Three knobs per instrument, eight instruments worth of expressivity
in one patch. See `patches/acid_complex.aither` for the full kick +
bass + didge + flute + pads layout.

## Drums that sound like a player, not a sequencer

The trick: don't trigger drums at constant velocity from `impulse(rate)`.
Use a **velocity array** indexed by a step phasor, multiply the trigger
by the velocity, and you get instant per-step dynamics that read as a
real player's intent — accents on downbeats, ghost notes on off-beats,
the whole "feel" of human playing — without per-note envelopes or
event sequencing.

The pattern (from `patches/gaelic_fairy.aither`'s bodhrán):

```
# 1. Velocity arrays of equal length per playing "style"
let bodSlow = [1.0, 0,   0,   0,   0,   0,   0.7, 0,   0,   0,   0,   0]
let bodJig  = [1.0, 0,   0.4, 0.5, 0,   0.4, 0.7, 0,   0.4, 0.5, 0,   0.4]
let bodReel = [1.0, 0.4, 0.6, 0.4, 0.7, 0.4, 0.5, 0.4, 0.7, 0.4, 0.6, 0.4]

play bodhran:
  # 2. Step phasor + step trigger at the step rate
  let stepRate = 6.0
  let stepPh   = phasor(stepRate / 12)
  let idx      = int(stepPh * 12)
  let trig     = impulse(stepRate)

  # 3. Triangular crossfade between three velocity arrays — one knob
  #    morphs continuously from "slow heartbeat" through "6/8 jig"
  #    to "driving 4/4 reel". Same rate, completely different feel.
  let vSlow = bodSlow[idx]
  let vJig  = bodJig[idx]
  let vReel = bodReel[idx]
  let pwA = max(0, 1 - bodPattern * 2)
  let pwB = (if bodPattern < 0.5 then bodPattern * 2
             else 1 - (bodPattern - 0.5) * 2)
  let pwC = max(0, (bodPattern - 0.5) * 2)
  let vel = vSlow * pwA + vJig * pwB + vReel * pwC

  # 4. velTrig = trig × velocity. The drum hears a strike whose
  #    intensity is the array's value at this step. Soft hits =
  #    muffled tap, loud hits = full boom — zero per-note envelopes.
  let velTrig = trig * vel

  # 5. Two-layer drum body — pitched sub-thump (boom) + filtered
  #    noise burst (tap), BOTH scaled by velocity. This split is
  #    how every real percussion synth (TR-808, hand pan, kalimba)
  #    models a struck membrane: pitched body + noise transient.
  let pSweep  = 50 + discharge(velTrig, 35) * 50
  let boomEnv = discharge(velTrig, 18)
  let boom    = sin(TAU * phasor(pSweep)) * boomEnv * 0.7

  let tapEnv  = discharge(velTrig, 80) * 0.5
  let tap     = (noise() |> hpf(2000, 0.5)) * tapEnv

  # 6. Drive the mix for wooden saturation character
  let raw    = boom + tap
  let driven = drive(raw, 1 + bodDrive * 4)
  ...
```

### Why this sounds like a real drummer

Every step gets its own velocity. Downbeats are `1.0`, the "and"s
are `0.4`, the ghost notes might be `0.5` — the array IS the
drummer's accent map. Multiplying `trig * vel` shapes both the
boom amplitude AND the tap brightness in one move (because both
are proportional to the velocity-scaled trigger).

A drum machine that hits at constant velocity sounds robotic.
A drum machine that has per-step accents sounds programmed.
This pattern sounds *played*, because the velocity contour is
itself a continuous signal — and crossfading between contours is
the drummer changing feel mid-piece.

### Adapting to other drum types

The same skeleton works for kick, snare, hi-hat, conga, tabla,
djembe — anything struck. Change three things:

- **The body sound**: kick = pitch-swept low sine; snare = noise +
  bandpass at ~200 Hz; hi-hat = high-passed noise burst; tabla =
  modal physical model (DHO chain).
- **The velocity arrays**: 16-step for 4/4, 12-step for 6/8, 7-step
  for 7/8, etc. Three contrasting velocity contours per knob zone.
- **The crossfade weights**: the triangular three-zone shape is the
  proven default. For two styles use a simple linear crossfade.

### When to reach for this

- Any auto-rhythmic instrument (drums, plucked patterns, arpeggios).
- When a normal `impulse + envelope` drum sounds too mechanical.
- When you want one knob to morph between musical styles
  (jig ↔ reel, march ↔ shuffle, four-on-floor ↔ broken-beat) on the
  same underlying rate.

The whole pattern is ~12 lines of aither for any drum. No event
list, no scheduler, no per-step state — just a phasor, an array
lookup, and a multiplication.

## Versioning while you compose

Aither has no built-in undo or rollback. The patch file is
the source of truth; the engine holds runtime state but
not history. That's deliberate — git is the time machine.

A composition session looks like:

```bash
git init && git add . && git commit -m "session start"
# ...edit, send, listen, edit, send...
git add piece.aither && git commit -m "verse arc working"
# ...try a bigger structural change...
git diff piece.aither           # what did I just change?
git checkout piece.aither       # nope — restore last commit
```

Two habits that pay off across long composition sessions:

- **Commit at every milestone**, however small. The piece
  arrives at a working drop, an interesting breakdown, a
  good crossfade — commit it. `git rebase -i` cleans up
  later. The cost of the commit is zero; the cost of
  losing a sweet spot you can't recreate is hours.
- **`git stash` before a bigger experiment.** `git stash`,
  rewrite half the file, send. If the rewrite is better,
  commit. If it's worse, `git stash pop` and you're back
  to a known-good state in one keystroke.

A failed `aither send` (compile error) does NOT lose the
voice — the engine returns `ERR` and keeps the prior
version running. The actual failure mode worth guarding
against is "patch compiled, but the audio is wrong"
(silence, NaN-induced reset, accidental gain blowout, the
piece structurally broken). That's exactly what
`git checkout` is for.

The aither workflow assumes git underneath. Treat your
patches directory as a git repo from day one.

## Iterating with the analysis CLI

When designing a sound, render and audit before bothering
anyone with playback. `./aither audit patches/foo.aither 2`
prints a spectral summary — top peaks, centroid, RMS,
fundamental estimate — that lets you verify the patch is
producing what you intended (correct fundamental, harmonic
series falloff, no aliased peaks above Nyquist, expected
brightness from the centroid). Offline: no engine required,
~100 ms turnaround.

```
$ ./aither audit patches/cello.aither 2
audit: patches/cello.aither (2.0s @ 48000 Hz)
  RMS:        -18.4 dB    Peak: -3.2 dB
  Fundamental: 220.0 Hz
  Centroid:   1842 Hz
  ZCR:        2114 / sec
  Top peaks:
     1.   220.1 Hz  0.0 dB
     2.   440.3 Hz  -8.1 dB
     ...
```

For live voices (engine running, MIDI playing in),
`./aither spectrum [voice]` does the same against the
engine's last ~0.5s of buffered output. With no argument it
analyses the master mix; with a voice name it analyses just
that voice's contribution. Use it to confirm a patch sounds
the way you think when MIDI input is involved (e.g. a
keyboard-driven additive lead — offline render sees no
midi_freq and produces silence).

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
Helpers: `gain`, `fold`, `tremolo`, `slew`, `prev`, `ease`.
Envelopes: `discharge`, `pluck`, `swell`, `adsr`.
Character: `drive`, `wrap`, `bitcrush`, `downsample`, `dropout`.
Stereo: `pan`, `haas`, `width`, `mono`.
Spectrum: `sum(N, fn)` (engine), `additive(f, shape, N)`,
`inharmonic(f, ratio, amp, N)`.
Shape fns: `saw_shape`, `sqr_shape`, `tri_shape`, `warm_shape`,
`bright_shape`, `bowed_shape`, `vowel_ah`, `vowel_ee`,
`cello_shape`.
Ratio fns: `stiff_string`, `stiff_cello`, `bar_partials`,
`plate_partials`, `phi_partials`.
Amp fns: `soft_decay`, `bell_decay`, `bright_decay`.
Physical instruments: `tuning_fork`, `pluck_string`,
`bowed_string`, `struck_bar`.

`prev(x)` returns the previous sample's value of any expression.
Useful for forward cross-play feedback:

```
play a: osc(sin, 440) * 0.2
play b:
  let echo = prev(a[0]) * 0.7     # one-sample-delayed a
  ...
```

Self-feedback is more naturally written inline with `$state`:
`$last = 0.0; $last = <new signal using $last>; $last`.

Always read `stdlib.aither` before inventing a function —
it is short and probably already has what you need.

