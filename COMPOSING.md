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
# 1. helpers (reusable, isolated; see constants, not the file's signals)
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
  sees what it receives and the file's constants, never its signals. A note that
  needs the file's knobs is written where it plays, as a lambda
  in the `play`: `midi_keyboard((freq, gate, vel) => ...)` sees
  every file-level let. Good for filters, envelopes,
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

Two clocks, each with one job:

- `t` is the one clock, seconds since the engine started, the same
  for every voice: `sin(TAU * 440 * t)` is a wave of the one time,
  and two voices agree on what now is.
- `sweep` is this piece's own time, seconds along its timeline. Picture
  an oscilloscope: the beam is *swept* across the screen at a steady
  speed, and where it is says what time it is on that screen. Lay the
  piece out left to right, 0:00 at the left edge; `sweep` is where the
  piece's beam is.

```
let fade  = ease(sweep / 20)                   # fade in over the first 20 seconds
let b     = ease((sweep - 55) / 10)            # cross to section B around 1:00
let swell = exp(-pow((sweep - 150) / 40, 2))   # a climax peaking at 2:30
```

The engine holds the sweep, as an oscilloscope's sweep is triggered
from outside: sending a new voice starts it at 0, sending an edit
lets it run on, and you can move it while it plays:

```
aither sweep cathedral          # where its beam is: 143.2
aither sweep cathedral 150      # jump to the bells at 2:30
aither sweep cathedral 0        # from the top
```

It is `t` less the moment the beam was last set, so it is exact, and
moving it costs nothing.

**Waves from `t` or `phasor`, arrangement from `sweep`.** Both are
sound as a function of time, at two scales, as a studio has house sync
and each tape's timecode. A line usually uses both:

```
osc(saw, 55) * ease(sweep / 20)          # the wave from the clock, the fade from the sweep
let beat = phasor(124 / 60)              # shared and continuous
let section = if sweep < 60 then a else b
```

It goes wrong when one does the other's job:

- **A wave from `sweep`** (`sin(TAU * 440 * sweep)`) clicks when you move
  the sweep, because its phase leaps, and two pieces' beats built on
  their sweeps drift apart, each having started at its own moment.
- **An arrangement from `t`** depends on how long the engine has been
  up, and `aither sweep` can't move it.

**Time as lived state.** Sometimes a piece wants a time a jump
shouldn't touch: how long this voice has really been playing, so a
slow drift carries on however you move around the piece. That is state,
an integral the piece lives through sample by sample:

```
$elapsed = 0
$elapsed = $elapsed + dt     # one sample's time more, every sample
```

`send` keeps it, as any state, and no `sweep` moves it. Name it for
what it means in the piece; some names, and the picture each carries:

| name | the picture |
|------|-------------|
| `$elapsed` | the plain word: time gone by since the start |
| `$ramp` | the waveform it is: a sawtooth that never wraps, `phasor`'s sibling |
| `$timebase` | an analogue computer's independent variable: an integrator fed a steady voltage from the moment the machine goes to *operate* |
| `$age` | how long the voice has lived |
| `$proper` | physics' proper time: the clock carried *with* the voice, where `t` is the time everyone shares |

Adding `dt` drifts by microseconds over an hour, nothing you can hear;
where it must be exact, count samples (`$n = $n + 1`, and `$n * dt`).

`sweep` (often bound as `let pos = sweep`) is your composition
timeline. Shape the piece as a function of it:

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
| "sequence of events"               | clocks, ratios, a contour held at strikes |
| "quantized grid"                   | only quantize if the *music* wants it   |
| "bar counter variable"             | a slow phasor *is* the bar position     |
| "shift note offsets for swing"     | warp the phase: `p + sin(p*π) * swing`  |
| "velocity table lookup"            | velocity is an amplitude oscillator     |
| "record a clip and loop it"        | tape the gesture, a head reads it back  |

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

The mono primitives `midi_freq()` / `midi_gate()` / `midi_vel()`
still exist (they return the most-recent note) for monophonic
synth-lead behaviour.

The voice function gets `(freq, gate, vel, key)` and names what it
uses. `gate` is 1 while held, so it drives envelopes at any
velocity; `vel` scales loudness or brightness: `swell(gate, a, r) *
vel` is the velocity-sensitive envelope, release tail included.

The deepest layer is `midi_voice_freq(n)` / `_gate` / `_vel` /
`_key` — for hand-rolled allocation logic that doesn't fit
`poly`'s shape.

### Looping your playing (tape)

The human plays the melody; a tape lets them play it back to
themselves. Loop the *gesture*, not the sound: record the pair
`[midi_freq(), midi_gate()]` on a `tape` and feed what it plays to a
voice. The loop then goes through the patch live, so you can edit the
voice while your phrase keeps playing through it, slow the loop down
without changing its key (the gestures slow, the pitch is the
voice's), or send it to a second voice as an answer to the first.

A loop pedal is a def over the tape's heads, so how it behaves is
yours to change, as a pedal's is not. This one: the first press
records, the second closes the loop and plays it, the third stops.

```
def looper(press):
  $mode = 0                     # 0 waiting, 1 recording, 2 looping
  $start = 0.0
  $len = 1.0
  let m = $mode
  $start = if press > 0 and m == 0 then t else $start
  $len = if press > 0 and m == 1 then t - $start else $len
  $mode = if press > 0 then (if m == 2 then 0 else m + 1) else m
  [$start + (t - $start) mod $len, $mode]      # the read head, the mode

def voice(f, g): osc(saw, f) * slew(g, 0.01) |> lpf(1200, 0.3)

let down = if midi_cc(64) > 0.5 then 1 else 0  # the sustain pedal
let press = if down > prev(down) then 1 else 0
let lp = looper(press)
let fg = tape([midi_freq(), midi_gate()], if lp[1] == 1 then 1 else 0, t, lp[0], 30)
let looping = if lp[1] == 2 then 1 else 0

voice(midi_freq(), midi_gate()) + voice(fg[0], fg[1]) * looping
```

The loop comes back exact, sample for sample. The press can be
anything that is a signal: a key's `midi_trig`, a CC, a phasor
(`bar > 0.99`, a loop closed every 32 bars by itself), the mic's
loudness. And the heads are signals too, so the deck's other moves
are a line each: the looper's last line reading `$start + ((t -
$start) * 0.5) mod $len` plays the loop at half speed, two loops of unequal length (17 s and 23 s,
Eno's way) drifting against each other, `keep = 0.8` to overdub with
the old passes fading.

Loop sound instead (a mic, a whole patch) by taping the signal
itself: then a slow head bends the pitch, as on a deck.

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

### Melody from a contour (signal-native melody)

A tune is a line that rises, falls and answers itself, and a line is
a few slow waves summed. Hold it at each strike and put it on a
scale. Nothing is listed: change a wave's phase and the tune changes
everywhere at once.

```
let phrase = phasor(tempo / 16)                 # four bars of four
let line = (sin(TAU * phrase) + 0.6 * sin(TAU * 2 * phrase + 1.3) +
            0.35 * sin(TAU * 3 * phrase + 2.1)) * 4     # in scale degrees

play tune:
  let hit = impulse(tempo * 2)
  $degree = 0.0
  $degree = if hit > 0 then floor(line + 0.5) else $degree    # held at each strike
  let f = scale($degree, 7, 293.66)
  ...
```

The scale is a formula too. `steps` even divisions of the octave,
rounded to the nearest semitone: seven of them are exactly the Dorian
mode (0 2 3 5 7 9 10), five a pentatonic (0 2 5 7 10). One number
picks the mode, and a knob can move it.

```
def scale(degree, steps, root):
  let octave = floor(degree / steps)
  let within = degree - octave * steps
  root * pow(2, octave + floor(within * 12 / steps + 0.5) / 12)
```

For a melody that breathes rather than steps, drop the hold: read
`line` every sample, `slew` it, and the voice glides between the
degrees it passes.

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
from it by math — no scheduler, no event list, no table. A
gate is a phasor crossing a threshold. An envelope is a
function of the phasor. An accent is a wave over the bar.

```
let beat    = phasor(tempo)                   # 0..1 across each beat
let eighths = phasor(tempo * 2)               # eighth-note phasor
let bar     = phasor(tempo / 4)               # a bar of four
let accent  = 0.5 + 0.5 * max(0, cos(TAU * 2 * bar))   # strong on 1 and 3
```

No special "sequencer," no "bar counter" variable.

### Rhythm by ratio

Where the strikes fall is a ratio. Run a grid of `n` pulses a bar:
the pulses where `i * k / n` crosses a whole number are `k` strikes
spread as evenly as the grid allows. That is the Euclidean rhythm
E(k, n), and most traditional rhythms are such: E(3, 8) is the Cuban
tresillo, E(5, 8) the cinquillo, E(5, 12) a bodhrán's jig, E(7, 12)
the West African bell (Toussaint, "The Euclidean Algorithm Generates
Traditional Musical Rhythms", BRIDGES 2005,
http://jeff.cs.mcgill.ca/~godfried/publications/banff.pdf).

```
# k strikes on an n-pulse grid: floor(i * k / n) steps up exactly
# where a strike goes.
def euclid(k, n, pulse, bar):
  let i = floor(bar * n + 0.001)
  pulse * (floor(i * k / n) - floor((i - 1) * k / n))

let bar   = phasor(tempo / 8)
let pulse = impulse(tempo)                    # eight pulses a bar
let clave = euclid(3, 8, pulse, bar)          # the tresillo
```

Several drums on the same grid with different `k` interlock, as
players do. A polyrhythm needs no grid at all: two clocks at rates in
a ratio (`impulse(tempo)` against `impulse(tempo * 3 / 2)`), or three
at rates that never line up (orrery's bells, every 7, 11 and 13 s,
meet once in 1001 s).

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

`dho(force, freq, damp)` is one def, the damped harmonic oscillator,
and returns its phasor `[x, q]`: `x` is the position, the sound, `q`
the position a quarter turn behind. Each sample the phasor turns and
shrinks and the force pushes it, which is the spring's equation solved
exactly (PHASORS.md says why it is written as a turn):

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
| `damp ≈ 0, freq = 0.5 Hz, force = 0`    | LFO / sub-audio modulator                   |
| chained at formant Hz, `force = speech` | vowel / wood-box body resonance             |

Three small expressions, same primitive, categorically different
sounds:

```
# Bell strike — long ring at 440 Hz, its pair doing the work
let hit = impulse(0.5)
let strike = (discharge(hit, 1 / 0.001) - discharge(hit, 1 / 0.0003)) / 0.0007 * 2765   # a push of area 1, times w
let bell = dho(strike, 440, 0.005)
let ring = amplitude(bell)             # how hard it's still ringing: an envelope, at every sample
let beat = turn(bell, spin(1.5))       # the whole bell 1.5 Hz higher: an even beat against itself
(bell + beat) * 0.5 + noise() * ring * 0.02   # and a breath that dies with it

# Resonant filter — narrow bandpass at 800 Hz, driven by external audio
let band = dho(audio_in * 50000, 800, 0.2)
let level = amplitude(band)            # its loudness, as a tuned circuit hears it
```

For demonstration, `patches/dho_walk.aither` wires K1 to `damp` on
a struck DHO. Holding a key and turning the knob walks audibly
from sustained sine → bell → pluck → thud — one oscillator, one
parameter, every paradigm. `damp` stops just under 1: an oscillator
turns, and a spring damped past critical only creeps back, which is an
envelope's job (`swell`, `slew`).

The whole pair is a phasor, a circle at every frequency, so it goes
straight into `turn`, `rotate`, `amplitude`, `phase` or `tv`. `q` is
the velocity only when undamped: damped, the phasor also shrinks as it
turns, so the velocity takes both sides,
`v = -(wd * q + damp * w * x)` with `wd = w * sqrt(1 - damp^2)`, the
rate it turns. Chained physics uses it (a damping force, one
oscillator's velocity driving another); the didge patches show it,
with a comment. A pair given where `dho` takes a number would make a
pair of pairs, which is an error: use two, or `resonator`, which
returns a number.

**Why this is `f(state)`-native.** In a modular synth you pick a
"filter module" or an "FM module"; the paradigm is baked into
the cabling. DHO inverts that: the parameters ARE the paradigm.
A patch that reads `dho(force, freq, damp)` lets the user — and
the running knob — slide continuously between the paradigms
without re-routing. This is what `f(state) → sample` looks like
when one equation generalises a half-dozen named techniques.

**Force is in acceleration units.** `force` adds directly to
`ddx`, so a constant force settles to about `x = force / k` (within
0.03% below 500 Hz; short of it near Nyquist, PHASORS.md "The push"). At audio
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

### An oscillator that sounds by itself: `vdp`

A DHO rings only while something pushes it. `vdp(freq, mu)` keeps
itself alive, as a bowed string, a reed or the lips on a didgeridoo
do: it grows from almost nothing to its own amplitude and holds it.
Its attack is physics, not an envelope, and `mu` is the bow pressure:
0.1 is a pure tone, 3 a buzzing relaxation tone that sags in pitch.

### Voices that lock

Two `vdp`s a little out of tune, each pushed toward the other, pull
into one pitch, as organ pipes side by side and pendulum clocks on one
wall do. The chord tunes itself:

```
$pa = 0.0
$pb = 0.0
let k = 0.1                                   # the spring
let a = vdp(220, push = k * ($pb - $pa))
let b = vdp(223, push = k * ($pa - $pb))
$pa = a[0]
$pb = b[0]
(a[0] + b[0]) * 0.4
```

With `k` 0 they beat at 3 Hz; at 0.03 they pull at each other and
wobble; at 0.1 they sound one pitch, 221.7 Hz, between the two. Let
`k` follow a knob and the beating slows and stops under your hand.

## Coupling: one sample of shared state

Voices that should move each other share a state cell, and both read and
write it. The cell holds what they did last sample; that one sample of
memory is all any feedback loop needs, and it is the only thing stored.

Sympathetic strings are the example. Strings share a bridge: every
string moves it, and the bridge pushes every string back.

```
$board = 0.0                                     # the bridge, as the strings moved it last sample
let bridge = $board * 15000
play struck:                                      # the one string that is played
  sum(4, n => mode(hit, bridge, 220, n, damp))
play sympathetic:                                 # never touched: driven by the bridge alone
  sum(4, n => mode(0, bridge, 440, n, damp)) + sum(4, n => mode(0, bridge, 329.63, n, damp))
$board = mono(struck) + mono(sympathetic)         # what every string did to it, felt by all next sample
```

(`mode` is one dho: a string's nth mode, struck or pushed by the bridge.)
Now the untouched strings ring only because the struck one moves the
bridge, and the physics decides where: a string sympathises on the
partials it shares with the struck one. In `patches/sympathy.aither`
the octave rings loudest (on the A's own partials), E only on the
660 Hz they share, 32 dB under it, and C# barely. Strike another string
and the sympathy follows. The test that it is coupling: leave the struck
string out of the bridge, and the others are silent.

A feedback loop has a gain, so keep it under 1 at every mode's
resonance (a mode driven at its own frequency swings by
`force / (2 damp w²)`): too much coupling and the strings feed each
other without bound, and the runaway guard steps in. `sympathy`'s
15000 is measured stable over 40 s.

**A shared drive is not coupling.** One signal that several voices
read, written by nobody they make (a strike every string hears, a
sidechain envelope, a common modulator), is just a `let`: it is a
function of its source, and no `$` is needed. Until 2026-10-29 this
section and `sympathy.aither` wrote only the strike into a `$bus` the
strings read, and called that sympathy; deleting the struck string
changed nothing, because no string's motion reached another. The
difference is who writes: a cell is a medium only when the voices write
back into it.

## Pair operations — turning

aither has no complex-number TYPE, but it has the OPERATIONS that
make complex algebra useful, all of them one picture: a pair is a
point that can turn. They take and return ordinary pairs. Use a pair
whole where you can (the operations below, or played as it is); name
its sides where a model needs both: `let [x, q] = dho(...)`.

| call                     | does                                                   |
|--------------------------|--------------------------------------------------------|
| `spin(hz)`               | the unit pair turning at `hz` Hz: `[cos, sin]` of one phase |
| `turn(z, w)`             | `z` turned by `w`'s angle, scaled by its size (complex multiplication) |
| `conj(z)`                | the same pair turning the other way                    |
| `unturn(z, w)`           | `z` turned back by `w`'s angle, divided by its size (complex division: an impedance V / I, a transfer function) |
| `rotate(z, angle)`       | turned by a fixed angle, in radians                    |
| `amplitude(z)`           | how big                                                |
| `phase(z)`               | which way it points                                    |
| `analytic(signal)`       | `[signal, hilbert(signal)]`: the signal as spins       |
| `freq_shift(signal, hz)` | the analytic signal turned by a spin: shifted up `hz` Hz, a pair in quadrature |

The rule the name carries: **turning by a spin adds its speed.** A
frequency shift, a radio tuned down (`turn(iq, conj(spin(f)))`),
single-sideband mixing and octave doubling (`turn(p, p)`) are all it.

`+`, `-`, `*` and `/` stay side by side: `p * q` multiplies each side
(`sound * [0.8, 0.6]` is a gain a side), never the complex product. Reach
for `turn` when you mean the algebra. A number times a pair scales it:
`0.5 * p`.

### Read the whole pair

A pair from an oscillator (`dho`, `vdp`, `spin`) or from `analytic` is a
phasor: two sides a quarter turn apart. Most of what you want from it
reads **both** sides. Taking one side (`let [x, _] = p`) throws half the
information away; when you find yourself writing `_`, look for the
operation that reads the whole pair.

| you want         | read the pair with                                  | instead of                     |
|------------------|-----------------------------------------------------|--------------------------------|
| how loud         | `amplitude(p)`                                      | one side, rectified and smoothed |
| how fast it turns (pitch, FM) | `phase(turn(p, conj(prev(p)))) / (TAU * dt)` | timing zero crossings      |
| move, shift, mix | `turn(p, w)`                                        | one side times a sine          |
| hear it          | the pair itself: the sink decides                   | `let [x, _] = p`               |

The crystal radio shows why. A 1906 crystal set has one side, the
voltage across its tuned circuit, so it needs a diode to keep the top
half of each wave and an earphone to smooth it into the station's
loudness. A `dho` is the tuned circuit (a coil and capacitor is a damped
harmonic oscillator) and gives both sides, so the whole receiver is

```
amplitude(dho(antenna * 2 * damp * w * w, dial, damp))
```

the phasor's length, which is the station's sound at every instant
(`patches/crystal.aither`, with `patches/station.aither` as the
broadcaster: `aither patches/station.aither | aither patches/crystal.aither
| tools/play`). The same reading makes a vocoder's bands
(`patches/listen.aither`) and a pitch follower (`patches/follow.aither`).
The tuned circuit's damping is its selectivity: narrow keeps a
neighbouring station out but blurs the sound, wide is clear but lets it
in.

**Strike what you play whole with a short push.** A push of one sample
changes a `dho`'s velocity at once, so its quarter-turn side jumps from
one sample to the next. Its position doesn't (position is the integral
of velocity), so it never showed while only `[0]` was heard. Played
whole, that step is a click on every strike. Spread the push over a
millisecond or two, rising from nothing as a mallet's felt does: the
difference of a slow and a fast decay,
`(discharge(trig, 1 / 0.001) - discharge(trig, 1 / 0.0003)) / 0.0007`,
is a push of area 1 that swells and dies. A push that switches on at
full force still turns the quarter side with a corner: a quieter tick.
Its swing is `area * force_scale / w`, so scale by `w` for a swing
that doesn't depend on pitch. Patches that play `dho`s
whole and are struck (`bell`, `orrery`) do this.

In order of preference:

1. **Use the pair whole.** An operation reads both sides for you:
   `amplitude(p)`, `phase(p)`, `turn(p, w)`, or the pair played as it
   is. No names, nothing thrown away.
2. **Destructure with both names** when a formula needs each side on
   its own. A damped mode's velocity is the case (the didge patches):
   ```
   let [x, q] = dho(force, f, damp)
   let v = -(TAU * f * sqrt(1 - damp * damp) * q + damp * TAU * f * x)
   ```
3. **One side, with `_`,** only where one number is all that fits.

Destructuring a pair just to rebuild what an operation gives
(`let [x, q] = p` then `sqrt(x * x + q * q)` for `amplitude(p)`) is
the same maths done by hand.

Played, a phasor pair is the same sound with its sides a quarter turn
apart: a little wide, and summed to mono it doesn't cancel (cos + sin
is a sine 3 dB up). So even where a patch wants "one sound", the pair
usually serves. Taking one side is for where only one number fits.

### Pair producers — start with `spin`, not `sin(TAU * phasor(…))`

When the next step in your signal chain is a pair operation (`turn`,
`rotate`, `freq_shift`, or feeding `turn(p, p)` for octave doubling),
**produce the rotation directly with `spin(rate)`** rather than
building a scalar sine and then converting to analytic. Two reasons:

1. **One trig pair instead of two.** `spin(rate)` advances one
   phase accumulator and emits `(cos(TAU*ph), sin(TAU*ph))` from it.
   Writing `sin(TAU * phasor(rate))` and then needing the cosine
   later costs you a second `phasor` call (with its own state slot)
   plus another sine.
2. **Intent reads correctly.** A line that opens with
   `let drone = spin(220)` says "I have a rotating thing at
   220 Hz." A line that opens with `let drone = sin(TAU * phasor(220))`
   says "I have a sine wave at 220 Hz" — and then the reader has to
   do extra work when the next operation treats it as a pair.

The historical `phasor(rate)` (returning a 0..1 ramp) stays. It's
still right when the consumer is a `sin`/`cos` shape, an
`if phasor < width` pulse, an `int(phasor * N)` step counter, or
anything else that needs the angle-as-scalar. Use `spin`
**when the rotation itself is what you want**.

### `turn` by a spin is a single-sideband mixer

`turn(spin(220), spin(50))` produces a clean tone at
**270 Hz only** — the sum frequency. There is no 170 Hz lower
sideband, no DC, nothing else of significance. The lower sideband is
suppressed by construction because both inputs are analytic-signal
rotations, not scalar sines, and the algebra cancels the unwanted
component.

```
let carrier = spin(220)
let modulator = spin(midi_cc(74) * 200)   # K1 sweeps modulator
let mixed = turn(carrier, modulator)
mixed[0] * 0.3
```

Sweep K1 from 0 to 1 and the audible tone slides cleanly from 220 Hz
up to 420 Hz, with no aliasing artifacts and no companion frequencies.
This is what radio engineers call upper-sideband modulation. In scalar
DSP it requires explicit Hilbert filters and careful sideband
cancellation. Here it falls out of the producer-pair × producer-pair
composition.

### Phase-coherent octave stack via `turn(p, p)`

Squaring a `spin` doubles its phase angle by construction —
`(cos θ + i sin θ)² = cos 2θ + i sin 2θ`. That is, mathematically and
not via pitch detection, an octave up. Cube it for a perfect fifth +
octave, take the fourth power for two octaves up. All layers come from
the same phase accumulator, so they NEVER drift out of phase relative
to each other.

```
let p  = spin(220)
let p2 = turn(p, p)        # 440 Hz, phase-locked
let p4 = turn(p2, p2)      # 880 Hz, phase-locked
(p[0] + p2[0] * 0.5 + p4[0] * 0.25) * 0.3
```

This is harmonic stacking that no oscillator-bank approach can match —
two independent 220/440 oscillators always drift slightly out of
phase as their accumulators accumulate floating-point error
differently. The squared-pair stack derives every octave from one
phase, so the relative phase relationships are exact for the lifetime
of the voice. The patch `spin_demo.aither` plays this side-by-
side with the heterodyne and the freq_shift halo so the difference is
audible.

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

`turn` and `+` are enough to write a Mandelbrot voice in three lines:

```
$zr = 0.0
$zi = 0.0
let zsq = turn([$zr, $zi], [$zr, $zi])
$zr = zsq[0] + cReal
$zi = zsq[1] + cImag
$zr      # the audio
```

`patches/mandelbrot_voice.aither` adds a periodic reset and a
magnitude clamp, but the engine is one line of complex arithmetic.
Sweep `(cReal, cImag)` and the timbre walks across the Mandelbrot
plane: inside the cardioid → silent (fixed point); just past the
boundary → chaotic but bounded → a shimmering metallic zone; far
outside → escape and silence.

### Orbit synthesis: the iteration clocked at the pitch

Iterating once a sample makes c choose the timbre *and* the pitch: a
3-cycle at 48 kHz sounds at 16 kHz. Clock the hops instead, q of them
a cycle, and glide between the points they land on:

```
def orbit(f, q, c):
  let step = phasor(f * q)
  $z = [0, 0]
  $was = [0, 0]
  $last = 0
  let hop = step < $last                 # the phasor wrapped: hop
  $last = step
  $was = if hop then $z else $was
  $z = if hop then (if amplitude($z) > 2 then [0, 0] else turn($z, $z) + c) else $z
  let s = step * step * (3 - 2 * step)   # glide from where it was to where it is
  $was + ($z - $was) * s
```

Now pitch and timbre come apart. The pitch is `f`. The timbre is the
shape of the orbit, which c decides, and the Mandelbrot set is a map
of timbres:

- **Inside the main cardioid** z spirals into a fixed point. The tone
  rings down like a plucked string, turning at an angle set by c.
- **In the bulb at angle p/q round the cardioid** z settles into a
  cycle of q points, a q-pointed star traced round and round. Clocked
  at q hops a cycle, that is a steady tone at exactly `f`, and the
  star is its waveform: left the real part, right the imaginary, so
  `tools/scope` draws the star itself.

The point at angle θ round the cardioid is `rotate([0.5, 0], θ) -
rotate([0.25, 0], 2 * θ)`. A bulb p/q sits there at θ = 2π·p/q and is
about sin(πp/q)/q² across, so a c a little way out from the edge
along that direction lands in it. A chord is then a set of pitches
*and* rotation numbers (1/2, 1/3, 1/4 …). Moving c from one bulb to
the next through the cardioid collapses one star, rings down, and
locks into the next one: a change of chord heard as mode-locking.
`patches/orbits.aither` plays four such chords, three voices each.

### Cost notes

- Pair ops are 4 mults + 2 adds at most (`turn`, `rotate`). Trivial.
- `analytic` and `freq_shift` carry a 32-slot Hilbert pair (4-stage
  IIR allpass per branch). ~30 multiply-adds per sample. Fine for
  one or two voices; don't put one inside a `sum(N, …)` over many
  partials.

## Polyphase: phases are turns of one phasor

aither has pairs, not triples, and that is enough for polyphase: it was
Steinmetz's point that the phasor algebra *is* the polyphase calculus. A
balanced three-phase set has two degrees of freedom (the phases sum to
zero), which is one turning pair (the Clarke transform), and phase k is
that pair turned back by k thirds of a turn. Nothing is stored or
indexed; every phase is a function of one phasor.

```
let field = spin(rate)                                   # the rotating field
def coil(k): rotate([1, 0], TAU * k / 3)                 # three coils a third of a turn apart
def seen(field, k): turn(field, conj(coil(k)))           # the field as coil k sees it: phase k
sum(3, n => seen(field, n - 1)[0])                       # the three drives: always 0
```

An unbalanced set (one coil stronger, one weaker) is Fortescue's
symmetrical components: a positive sequence turning with the field, a
negative sequence turning against it, and a zero sequence that doesn't
turn. Each is a sum of turns: the phases turned back by their own
thirds, forward by them, or not at all, and averaged.

```
let positive = sum(3, n => rotate(v(n - 1), TAU * (n - 1) / 3)) / 3
let negative = sum(3, n => rotate(v(n - 1), -TAU * (n - 1) / 3)) / 3
let zero     = sum(3, n => v(n - 1)) / 3
```

For N phases, `TAU * k / N`: the symmetrical components are the discrete
Fourier transform over the phase index, written as a sum of rotations.
`patches/three_phase.aither` turns a chord with a rotating field (each
coil sounds one note while the field faces it) and sounds the negative
sequence as an unbalanced transformer's hum. Balanced, the negative
sequence is exactly 0 and the positive has length 1; unbalanced by half,
the negative rises to 0.29.

### Fields: waves first, stored fields last

Most fields are written as waves, and should be. Reach for these, in
this order, before storing anything per place:

| the field | write it as |
|---|---|
| a medium rung at its modes (a plate, a drum, a room) | a sum of its modes, each a `dho` (demos/chladni) |
| a medium carrying waves (a string, a tube, echoes) | travelling waves as delays: `delay` is the field |
| oscillators all coupled to all (a bridge, a soundboard, Kuramoto) | one shared cell, the mean field, every copy reading and writing it (sympathy, "Coupling: one sample of shared state") |
| polyphase, a rotating field | turns of one phasor (above) |
| oscillators coupled to their neighbours, nonlinear | a stored field: a state array, one value per place, stepped by `each` |

All-to-all coupling is the shared cell, with no array: Kuramoto's own
model, twelve detuned oscillators each pulled toward their mean, locks
into one pitch at a coupling of 0.002:

```
$mean = [0, 0]                                     # the field's mean, as the oscillators left it
let ring = sum(12, k =>
  $z = [1, 0]
  let moved = turn($z, step(k)) + $mean * couple
  $z = moved / amplitude(moved)
  $z)
$mean = ring / 12
```

Only the last row needs one value per place: what a sum can't say is one
copy reading another, oscillator i pulled toward i - 1 and i + 1 as they
were last sample. Write the medium's law as a def, the field as a state
array of phasors (an array's elements are what its initial value is), and
the step as `each`, which moves every place from the same moment:

```
def law(me, left, right, step, couple):            # what a phasor does, given itself and its neighbours
  let moved = turn(me, step) + (left + right - me * 2) * couple
  moved / amplitude(moved)

$z = array(n, [1, 0])                              # the field
each(n, i => $z[i] = law($z[i], $z[(i + n - 1) mod n], $z[(i + 1) mod n], step(i), couple))
```

`patches/phasor_ring.aither` is that ring: apart, a swarm; coupled, one
pitch with a travelling wave of phase round it. A medium with ends needs
no `mod`: past either end reads `[0, 0]`, which is a string with fixed
ends.

## State that holds an array

`$state` holds one number by default. When a voice needs to remember
more — a mutable scale, a history, a medium a wave travels along —
a state array holds N of them, kept from sample to sample and across
hot reloads (SPEC.md, "State arrays"):

```
let seedFreqs = [110.0, 138.59, 164.81, 220.0, 277.18, 329.63, 440.0, 554.37]
$scale = array(8)
$inited = 0.0
each(8, i => $scale[i] = if $inited > 0.5 then $scale[i] else seedFreqs[i])
$inited = 1.0

$scale[$cursor] = if trig > 0.5 then newF else $scale[$cursor]   # rewrite one
sum(8, n => additive($scale[n - 1], warm_shape, 6))               # read them all
```

The index can be worked out as it runs, which suits "every bar
mutate one of N entries" when the entry isn't known until the
trigger fires. `patches/adaptive_scale.aither` keeps an 8-note chord
this way; one slot at a time is rewritten toward the held MIDI pitch,
so hold a key for a minute and the chord migrates toward you.

A state array is not the constant table you pass to `wave()`
(`let notes = [220, 247.5, …]`): that one is fixed and hoisted to
static data; a state array lives in the voice's pool and changes.
Its size is fixed when the patch compiles.

## Timbre choice: which paradigm for which sound

Aither's contract is `f(state) → sample`. Synthesis paradigms
are different ways of WRITING that function. Pick whichever
fits the sound you're after; the engine doesn't care.

| When you want…                                              | Reach for                              |
|-------------------------------------------------------------|----------------------------------------|
| A musical sound from its spectrum (pads, leads, vocal-like) | `additive(f, shape, N)`                |
| Bells, plates, stiff strings — non-integer partials         | `inharmonic(f, ratio, amp, N)`         |
| Plucked / bowed / struck physical instruments               | `pluck_string`, `bowed_string`, `struck_bar` (stdlib — **WIP, sounds naive vs additive**) |
| A vibrating physical object — transients, decay built-in    | `dho(force, freq, damp)`: ẍ + 2·damp·ωẋ + ω²x = F(t), turned exactly |
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
Karplus-Strong is unmistakable. The modal banks (`bowed_string`,
`struck_bar`) are a `resonator` per mode inside `sum(K, n => …)`;
each unrolled iteration claims its own state, so each mode rings
by itself. A resonance written by hand as `$dx = $dx + … * dt;
$x = $x + $dx * dt` runs sharp at high pitch (13 cents at 3 kHz,
57 at 6 kHz, at 44.1 kHz): `dho` and `resonator` turn the spring
exactly instead.

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
let [body, _] = dho(kTrig * 50000000.0, 200, bodyDamp)   # one side: the low end stays centred
let kickB    = body * 0.5

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
let [acid, _] = dho(saw * 80000.0, cutA, 0.025)          # one side: bass stays centred
let bassA = acid * bEnv * (0.4 + bAcc * 0.6)

# B — Bell-bass: struck DHO at the note pitch (force scaled by
# frequency so peak amplitude stays consistent across notes)
let bForceB = 22000.0 * bFreq
let [bellBass, _] = dho(bTrig * bForceB, bFreq, 0.008)
let bassB   = bellBass * bEnv * (0.5 + bAcc * 0.5) * 0.4

# C — Noise-formant: white noise into resonant DHO BPF at 3rd harmonic
let nAmp  = bEnv * (0.3 + bAcc * 0.7) * 6000000.0
let [formant, _] = dho(noise() * nAmp, bFreq * 3, 0.025)
let bassC = formant * 6.0

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

## Spectral morph: moving the partials, not mixing sounds

The paradigm crossfade above mixes finished sounds by volume, so half
way through the knob you hear two instruments at once. A spectral
morph moves every partial's **frequency** instead. One knob takes a
note from the harmonic series (1, 2, 3, 4 ...) through a metal bar's
modes (1, 2.778, 5.444, 9 ...) to golden-ratio steps (1, 1.618,
2.618, 4.236 ...), and the note dissolves into metal as one sound.

This is rare in live coding. Typical live-coding setups sequence notes
and samples into fixed synths, where a knob reaches a filter or an
envelope, not the position of each partial. Moving partials live
needs per-partial frequency control at audio rate, a spectrum you can
rewrite as a line of math, and a hot reload that keeps the phases
running. aither has all three, so a MIDI knob (or the gamepad) can
reshape what a note *is* while it sounds.

`patches/spectra.aither` is the instrument: one held note, morph on
A, plus stretch, jitter, brightness, pitch, strike, rate and space on
the other seven gamepad knobs. The pattern:

```
# the ratio for partial n is a function; three of them to morph between
#   string  n                 bar  ((2n + 1) / 3)^2       phi  1.618^(n - 1)
let u = clamp(midi_cc(20) * 2, 0, 1)        # string -> bar, first half
let w = clamp(midi_cc(20) * 2 - 1, 0, 1)    # bar -> phi, second half

def partial(n, u, w):
  exp(log(n) * (1 - u) + 2 * log((2 * n + 1) / 3) * (u - w)
      + (n - 1) * log(1.618) * w)

play tone:
  sum(20, n =>
    let pf = f0 * partial(n, u, w)
    if pf >= sr * 0.45 then 0
    else sin(TAU * phasor(pf)) / n)
```

Why it is written this way:

- **Geometric, in logs.** `a^(1-u) * b^u` moves each partial at an
  even rate in pitch (log frequency), the way the ear hears it; a
  linear blend moves it at an even rate in hertz. Written as
  `exp(...)` over logs, and since `sum` unrolls `n` as a literal,
  every `log` of `n` is a constant: one `exp` per partial per sample.
  The direct `pow` form cost 112% of an A53 core at 20 partials; this
  costs 62% with all of spectra's knobs.
- **Phase-continuous.** Each partial is its own `phasor`, so moving
  its frequency bends it; nothing restarts or clicks.
- **The band limit is per partial.** Bar and phi spectra climb fast;
  `if pf >= sr * 0.45 then 0` drops a partial as it nears Nyquist
  instead of letting it alias.
- **Any ratio function works.** `stiff_string`, `plate_partials`, a
  tuning from a table, or a fourth target: add a weight and a log.

**On a keyboard.** The morph goes inside `midi_keyboard`, and the
knob reshapes every held note together:

```
play keys:
  midi_keyboard((freq, gate) =>
    sum(8, n =>
      let pf = freq * partial(n, u, w)
      if pf >= sr * 0.45 then 0
      else sin(TAU * phasor(pf)) / n)
    * adsr(gate, 0.005, 0.3, 0.6, 1.2) * 0.1)

keys |> reverb(2.5, 0.25)
```

Eight notes held at eight partials each costs 24% of an A53 core, so
it plays on the uConsole. Play a chord, then sweep the knob: the chord
stays a chord in rhythm and voicing while its timbre walks from organ
to bells to something with no name.

**Inside each note.** The morph can move during a note, too.
`patches/metal_keys.aither` throws each strike toward metal, as far as
its velocity says, and lets it settle back to the base knob, the way a
hard blow on a real bar excites inharmonic modes that die first. A
"bloom" knob does the reverse: a held note drifts toward metal. A
note struck at velocity 0.9 measures 220, 506, 846, 1233 Hz in its
first 60 ms and 220, 440, 660, 880 Hz a second later. Eight notes
sounding cost 36% of an A53 core.

**A tuning that follows the spectrum.** Which intervals sound smooth
depends on the partials (Sethares, *Tuning, Timbre, Spectrum, Scale*):
two tones are rough where their partials sit about a quarter of a
critical band apart. Harmonic partials make 5/4, 3/2 and the octave
consonant. A metal bar's don't, so bar-spectrum chords in ordinary
tuning clash, which is why gamelan tunes to its own metallophones.
`tools/consonance.py` computes each spectrum's dissonance curve and
prints its 7 most consonant degrees and the ratio it repeats at:

| spectrum | degrees (cents) | repeats at |
|---|---|---|
| string | 0 386 498 583 702 884 969 | 2 (the octave) |
| bar | 0 578 695 870 1165 1390 1565 | 2.778, its own 2nd partial: a bar has no consonant octave |
| phi | 0 247* 493 833 1065* 1296 1481* | phi^2 (* even steps: phi has only three dips) |

The string row comes out as just intervals (5/4, 4/3, 7/5, 3/2, 5/3,
7/4), which is the check that the model is right. Because the tables
line up degree by degree, `patches/tuned_keys.aither` morphs the
tuning with the partials (`patches/lib/spectral.aither`,
`spectral_key`): turn the knob and a held chord's intervals bend with
its timbre, so it stays consonant as it turns to metal. White keys
are the 7 degrees, C4 = degree 0. By the same roughness model, chords
of bar-spectrum notes on the same keys are 38-56% smoother in the
bar's tuning than in 12-note equal temperament.

## Drums that sound like a player, not a sequencer

A drum struck at the same force every time sounds like a machine. A
player's feel is a shape over the bar: strong on the one, a lift on
the other beats, ghosts between. That shape repeats every bar, so it
is a periodic wave, and a few harmonics of the bar make it. Changing
feel is crossfading shapes. No velocity table, no steps.

From `patches/contour.aither`'s bodhrán (6/8, twelve pulses a bar):

```
let tempo = 2.2                                # pulses a second
let bar   = phasor(tempo / 12)
let pulse = impulse(tempo)

# 1. The feel: accents as harmonics of the bar. One knob morphs from
#    a heartbeat (one accent a bar) through a jig (two) to a reel (four).
let feel      = midi_cc(74)
let heartbeat = 0.35 + 0.65 * max(0, cos(TAU * bar))
let jig       = 0.45 + 0.55 * max(0, cos(TAU * 2 * bar))
let reel      = 0.55 + 0.45 * max(0, cos(TAU * 4 * bar))
let accent    = heartbeat * max(0, 1 - feel * 2) +
                jig * (1 - abs(feel * 2 - 1)) + reel * max(0, feel * 2 - 1)

# 2. The strike: a push that rises from nothing (see "Strike what you
#    play whole with a short push").
def push(strike): (discharge(strike, 1 / 0.001) - discharge(strike, 1 / 0.0003)) / 0.0007

play bodhran:
  # 3. Where it strikes (E(5, 12), "Rhythm by ratio"), times how hard.
  let hit = euclid(5, 12, pulse, bar) * accent

  # 4. Two layers, both scaled by the strike: a pitched body (a dho
  #    struck, one side so the low end stays centred) and a noise
  #    transient. Every struck-membrane synth is built so.
  let w = TAU * 75
  let [boom, _] = dho(push(hit) * w * 0.6, 75, 0.12)
  let tap = bpf(noise(), 1800, 0.4) * discharge(hit, 60) * 0.5
  (boom + tap) * 0.5
```

### Why this sounds like a real drummer

The accent wave read at each strike is the drummer's intent: full on
the one, less on the off-beats. Multiplying the strike by it sizes
the push, so a soft hit is both quieter and duller (the dho is driven
less, the tap is quieter) in one move.

A drum machine at constant force sounds robotic; a step table of
accents sounds programmed. The accent here is a continuous signal, so
it can be swung by warping the bar's phase, morphed between feels
mid-piece, and nudged: a harmonic's phase moves its accents earlier or
later, and its weight makes them stronger.

### Adapting to other drum types

The same skeleton works for kick, snare, hi-hat, conga, tabla,
djembe — anything struck. Change three things:

- **The body sound**: kick = a low dho struck hard, or a pitch-swept
  sine; snare = noise + bandpass at ~200 Hz plus a short dho; hi-hat =
  high-passed noise burst; tabla = modal physical model (DHO chain).
- **Where it strikes**: `euclid(k, n, …)` with the grid of the metre
  (16 for 4/4 sixteenths, 12 for 6/8, 7 for 7/8) and `k` for the
  density.
- **The feel**: which harmonics of the bar, how strong. The nth
  harmonic accents n times a bar; a sum of two gives the strong-weak
  pattern of most metres.

### When to reach for this

- Any auto-rhythmic instrument (drums, plucked patterns, arpeggios).
- When `impulse + envelope` sounds too mechanical.
- When one knob should morph between feels (jig ↔ reel, march ↔
  shuffle, four-on-floor ↔ broken beat) at the same tempo.

A clock, a ratio and a few harmonics. No event list, no scheduler,
no per-step state.

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

- **Intermittent kick clicks from `env × sin(TAU * phasor(f))`.**
  An instant-attack envelope multiplied by a continuously-running
  phasor pops whenever the strike lands at non-zero sine phase.
  Symptom: 4-5 clean kicks, then one click, then 3 clean, then
  another click — the offending pattern is beat-locked, hard to
  hear on small speakers, instantly audible on studio headphones.
  Fix: reset the phasor on each trigger via `$state`:
  ```
  $kph = 0.0
  $kph = if trig > 0.5 then 0.0 else ($kph + freq * dt) mod 1.0
  let body = sin(TAU * $kph) * env
  ```
  Doesn't apply to `dho(force, freq, damp)` body strikes — the
  integrator handles transients cleanly. Doesn't apply to broadband
  sources (`noise`, `additive` with high N) — random per-partial
  phase masks the click. Default to phase-resetting any single-sine
  strike voice.

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

- Each patch is compiled to C (by TCC at once, then by cc -O2
  behind it). Even so, four voices with reverb each can hit
  60%+ of one core on the uConsole.
- **Reverb is expensive.** A Schroeder reverb does ~10
  array accesses per sample. One shared reverb beats four
  per-voice reverbs.
- **`wave` and `impulse` are cheap.** `lpf`/`hpf`/`bpf`
  are cheap. `reverb` and `fbdelay` with long buffers are
  not.
- **`tape` is cheap to run and costs memory:** two reads and a
  write a sample, and 8 bytes a sample of length (30 s of a
  pair is about 23 MB). The voice gets what its tapes need.
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
Delays: `delay`, `fbdelay`, `tape`. Reverb: `reverb(signal, rt60, wet)`.
Oscillators: `spin`, `dho`, `vdp`.
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
Physical instruments: `pluck_string`, `bowed_string`,
`struck_bar`.

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

