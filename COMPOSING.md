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
  Spectral synthesis (additive, inharmonic, FM-per-partial)
  is the default — `osc(saw, f)` is the exception you reach
  for when you specifically want chiptune / lo-fi / aliased
  character. For anything you want to sound *musical*, build
  the spectrum directly: `additive(f, warm_shape, 8)` for
  pads, `additive(f, bright_shape, 16)` for leads, `inharmonic`
  for bells/plates/strings. Plus signal-native textures and
  dynamics (LFO-modulated everything, polyrhythm via products
  of LFOs, slow morphing via incommensurate rates), reactive
  backing tracks designed for live performance.
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
var startPhase = -1
startPhase = if startPhase < 0 then (t * tempo) mod 1 else startPhase
let bt = (phasor(tempo) + startPhase) mod 1
```

The state-migration model preserves `startPhase` across hot
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

## Timbre choice: prefer additive

`additive(f, shape, N)` is the default oscillator for almost
everything. Build sounds out of partial-amp functions; the
spectrum *is* the timbre. `osc(saw/sqr, f)` is the exception,
not the rule.

| When you want…                                    | Use                                    |
|---------------------------------------------------|----------------------------------------|
| Anything musical (pads, leads, bass, vocal-like)  | `additive(f, shape, N)` — DEFAULT      |
| Bells, plates, stiff strings, non-integer partials | `inharmonic(f, ratio, amp, N)`        |
| Deliberately chiptune / lo-fi / aliased character | `osc(saw, f)`, `osc(sqr, f)`           |

Rules of thumb:

- **`additive(f, shape, N)`** is the default. Pick any shape
  fn that matches your mood: `warm_shape` for pads,
  `bright_shape` for edgy leads, `vowel_ee`/`vowel_ah` for
  vocal pads, `cello_shape` for bowed textures, `saw_shape`
  for a clean band-limited saw. `N=8` for pads, `16` for
  leads, up to `24` for characterful features. CPU is linear
  in N — `N=8` is essentially free, `N=24` is fine for one
  voice, push past 32 only with reason.
- **`inharmonic(f, ratio, amp, N)`** when the spectrum
  deliberately departs from integer multiples. Strike a
  bar? `bar_partials`. Bowed string with body resonance?
  `stiff_cello` + `cello_shape`. Dreamy non-tonal texture?
  `phi_partials`. Bell hits? `bar_partials` + `bell_decay`.
- **`osc(saw/sqr, f)`** is for chiptune, lo-fi, "broken hardware"
  character, sidechain test signals, kick body sweep — anywhere
  you specifically want the digital aliased sound or you don't
  care about the spectrum at all. NOT the default. Aliases above
  ~1 kHz; that's a feature when you want it, a bug otherwise.

The reason additive is the default isn't aesthetic — it's that
aither's signal-native model and `sum(N, fn)` primitive are
*built* for spectral construction. Reaching for `osc(saw)`
leaves most of aither's unique value on the table. The only
patch where I'd default to `osc` is one that aims at chiptune
or breakbeat character on purpose.

All three compose with envelopes, effects, pans the same way:

```
play voice:
  let env = swell(midi_gate(), 0.1, 0.4)
  additive(midi_freq(), cello_shape, 16) * env * 0.2 |> reverb(2.5, 0.15)

voice
```

Nothing else in the patch cares which synthesis style you
picked — swap `osc(saw, midi_freq())` ↔ `additive(midi_freq(),
saw_shape, 16)` ↔ `inharmonic(midi_freq(), stiff_string,
soft_decay, 16)` freely.

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

`prev(x)` returns the previous sample's value of any expression.
Useful for forward cross-play feedback:

```
play a: osc(sin, 440) * 0.2
play b:
  let echo = prev(a[0]) * 0.7     # one-sample-delayed a
  ...
```

Self-feedback is more naturally written inline with `var`:
`var last = 0.0; last = <new signal using last>; last`.

Always read `stdlib.aither` before inventing a function —
it is short and probably already has what you need.
