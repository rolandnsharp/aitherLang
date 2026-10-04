# Sound frontiers — what to chase

Additive synthesis is sixty years old. The technique is settled. What's
new in aither isn't *spectral synthesis itself* — it's the **interface
to it**: lambdas + literal-propagation + sample-rate hot reload mean
you can type a spectrum as math and hear it in 100 ms. Sixty years of
additive failed to explore most of its territory because the interfaces
punished exploration: 256-slider hardware synths, batch-render score
files, resynthesis tools that import samples instead of inviting math.

The aither sound — if there is one — lives in the regions of additive
that no prior interface let people reach. Three of them, none requiring
new primitives.

## Region 1: partial-index-modulated detuning

Each partial drifts at a rate that depends on its own index `n`.
Aggregated across 24+ partials, this produces sounds that aren't
pitched, aren't noisy, aren't chordal — they *swarm internally*.

```
sum(32, n =>
  sin(TAU * phasor(n*f * (1 + 0.005 * sin(t * n / 7)))) / n)
```

`t * n / 7` is the load-bearing line — each partial is detuned at a
rate proportional to `n`, so the spectrum can't move as a coherent
whole. The result is a single voice with built-in shimmer that no
chorus pedal can produce.

No commercial synth exposes "LFO rate proportional to partial index"
because the GUI for it is hopeless. As code, it's six characters.

## Region 2: non-monotonic ratio functions

Aither's stock ratio fns (`stiff_string`, `bar_partials`, `phi_partials`)
are monotonic. The interesting territory is **non-monotonic** —
spectra with no natural ordering, no clear fundamental.

```
def jagged(n):  1 + sin(n * 1.7) * n
def prime_ratio(n):  prime(n) / 7        # if we had a prime fn
def chaotic(n):  n * (1 + 0.4 * sin(n * 2.39))
```

These are *xenharmonic spectra* — the music-theory term for "partials
not in integer ratios to a fundamental." They're playable, they're
deterministic, they're tunable, and almost nobody composes with them
because there's no instrument that lets you specify them. Aither
makes it a one-line def.

Mind the perceptual risk: too jagged sounds like noise; too smooth
collapses to "detuned chord." The sweet spot is narrow but real.

## Region 3: time-varying spectra

Replace the constant `shape` or `ratio` with a function of `t`. A
single sustained voice that *evolves* over 30 seconds — the way a
granular pad does, but with full mathematical determinism rather
than random buffer playback.

```
def morph_shape(n, pf):
  let walk = (sin(t * 0.13) + 1) * 0.5      # 0..1 over ~12s
  let from = saw_shape(n, pf)
  let to   = vowel_ah(n, pf)
  from * (1 - walk) + to * walk
```

Wavetable synths approximate this by interpolating between pre-computed
spectra. Aither lets the morphing function be arbitrary math — the
shape can morph faster on high partials than low ones, can crossfade
between four shapes on a Lissajous figure, can lock to a `phasor` or
to MIDI velocity. None of that is buildable on a wavetable engine.

## Combining regions

The strongest aither-native sounds are likely **combinations** of two
or three regions. Region 1 (swarm) + Region 3 (time-morphing shape)
produces a held tone whose timbre is alive on two timescales: fast
chorus-like motion from the per-partial detuning, slow morphing from
the shape function. Plus a bow envelope and reverb for shape, and the
whole thing is a single `sum(N, n => ...)`.

The first attempt at this is `patches/aura.aither` (this session,
pre-bed). Subsequent sessions should do many more — fail fast, move
on, the cost of trying a new spectrum is fifteen seconds.

## How to evaluate a candidate sound

Aiming criteria, ordered:

1. **Unmistakably wrong on every other synth.** If the first reaction
   from a synth player is "what plug-in is that," it's still inside
   the bounds of what they know. Aim for "that's not a synth, what
   is it?"
2. **Coherent.** Random isn't interesting. The math should be
   deterministic and the sound should feel intentional, not like a
   bug.
3. **Tunable.** A great aither sound should respond to one or two
   knobs in a way that takes it through clearly different territories
   without ever sounding broken.
4. **Holds attention for 30 seconds.** If a single key-press can be
   held for thirty seconds and stays interesting the whole time, the
   sound has internal life and doesn't need a sequencer to feel alive.

## What this means for polyphony

Polyphony adds *quantity* of voices, not *quality* of any one voice.
Until at least one aither sound passes the four criteria above, more
voices is just more cellos. The post-polyphony goal would be
*chordal swarm* — eight voices each with their own internal motion,
played as a chord, producing harmony that's stable in pitch but
unstable in spectrum. That's a sound regime nobody has heard yet
either, but it depends on the per-voice sound being interesting in
the first place.
