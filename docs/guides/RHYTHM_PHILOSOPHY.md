# Composing with Signals: The Aither Philosophy of Rhythm

> The Clock is not a special object given to you by the engine. The Clock is
> a signal, created and composed by you. In Aither, rhythm is not a feature;
> it is an emergent property of the system.

Ported from the JavaScript aither (`rolandnsharp/aither`,
`docs/guides/RHYTHM_PHILOSOPHY.md`, 2026-02-22) to aitherLang syntax. The
ideas are unchanged; the examples are rewritten and checked to compile.

## The Core Principle

In most live coding environments, rhythm is based on a central clock or a
timeline. You place notes on a grid, and the engine plays them.

Aither follows the modular synthesis approach:

1. **There is no master clock.** The only universal truth is `t`.
2. **A "clock" is just a low-frequency oscillator.**
3. **Rhythm emerges from composing signals through math.**

You don't ask for a clock; you build one.

---

## The Phasor: The Fundamental Rhythm Primitive

The most important signal for rhythm is the **phasor**: a ramp from 0 to 1
that repeats at a given frequency. It answers the question: *where am I in
the current beat?*

A phasor is the raw phase accumulator that lives inside every oscillator.
All other rhythm concepts (gates, envelopes, triggers, sequences) are
derived from the phasor through simple math.

```
phasor at 2 Hz (120 BPM):

1 |    /|    /|    /|    /|
  |   / |   / |   / |   / |
  |  /  |  /  |  /  |  /  |
  | /   | /   | /   | /   |
0 |/    |/    |/    |/    |
  |-----|-----|-----|-----|
  0    0.5   1.0   1.5   2.0 sec
```

```
# A click at the start of each beat
play click:
  if phasor(130 / 60) < 0.002 then 0.8 else 0
```

---

## From Phasor to Rhythm

Everything is derived from the phasor value with simple math:

```
let bps   = 130 / 60
let gate  = if phasor(bps) < 0.1 then 1 else 0       # on for the first 10% of each beat
let env   = exp(-phasor(bps) * 30)                   # a fast decay every beat
let notes = wave(bps, [220, 261.6, 329.6, 392])      # a sequence is a waveform
let half  = phasor(bps / 2)                          # clock division is frequency division
let eight = phasor(bps * 2)
```

`wave` is a wavetable oscillator: its phase scans an array at a given
frequency, just like `sin` scans a sine curve.

---

## Building a Heartbeat Kick

A kick drum built entirely from a phasor, an envelope, and a sine whose
pitch follows the envelope:

```
play kick:
  let env = exp(-phasor(130 / 60) * 40)        # the transient
  let f   = 60 + 200 * env                     # 260 Hz falling to 60: the thump
  sin(TAU * phasor(f)) * env * 0.8             # phase-continuous body
```

No special kick drum helper. No sample playback. Just a phasor, an
envelope derived from it, and a sine wave whose frequency follows the
envelope. This is synthesis from first principles.

### A Snare on Beats 2 and 4

```
play snare:
  let p   = (phasor(130 / 60 / 2) + 0.5) mod 1     # half speed, offset half a cycle
  let env = exp(-p * 60)
  (sin(TAU * phasor(180)) * 0.5 + noise() * 0.5) * env * 0.6
```

### Hi-Hats on Eighth Notes

```
play hats:
  let env = exp(-phasor(130 / 60 * 2) * 80)
  (noise() |> hpf(7000, 0.5)) * env * 0.3
```

---

## Groove and Swing

Real rhythm has human feel: some beats pushed forward, others laid back.
In Aither, you warp time itself:

```
play swung_hats:
  let p      = phasor(130 / 60 * 2)            # the raw 8th-note phase
  let warped = p + sin(p * PI) * 0.1           # an S-curve warp of time
  let env    = exp(-warped * 80)
  (noise() |> hpf(7000, 0.5)) * env * 0.3
```

You're not shifting note positions on a grid. You're **warping the
envelope's sense of time**. To push the off-beat 16th late, warp
piecewise: within each 8th, the second 16th starts at `h = 0.5 + swing / 2`
instead of 0.5, and each hit's envelope runs on the time since its own
start (the sticks in `patches/didge_scope.aither` do this).

---

## Sequencing Is Oscillation

You might expect a `seq(clock, [values])` helper: a sequencer that steps
through a list. But sequencing in Aither isn't a special concept. It's
just what happens when a wavetable oscillator runs at a low frequency.

`wave(freq, values)` scans an array at the given frequency, exactly like
`sin` scans a sine curve. At audio rate, it produces a custom waveform. At
beat rate, it produces a step sequence. Same mechanism, different
timescale.

```
let bps     = 130 / 60
let vel     = wave(bps * 2, [1, 0.6, 0.8, 0.5, 1, 0.4, 0.9, 0.7])  # a velocity pattern
let cutoffs = wave(bps, [400, 1200, 600, 2000])                    # a filter pattern
play acid:
  osc(saw, wave(bps, [55, 73.4, 82.4, 65.4])) |> lpf(cutoffs, 0.7) |> gain(0.2)
play hit:
  sin(TAU * phasor(440)) * exp(-phasor(bps * 2) * 40) * vel * 0.3
play custom:                                                      # audio rate: a waveform
  wave(440, [0, 0.5, 1, 0.5, 0, -0.3, -1, -0.7]) * 0.1
```

There is no sequencer. There are oscillators, and some of them read from
tables instead of computing math. The distinction between "sequence" and
"waveform" is just frequency.

---

## In a Live Set: One Clock, Many Phases

A hot swap changes this one detail. Each `phasor(...)` or `wave(...)` call
keeps its own phase, and a call site that's new in a send starts at 0 at
that moment, so it lands off the beat. In a set, keep one clock as `$state`
and derive every part's phase from it with plain arithmetic:

```
$bpos = 0.0
$bpos = $bpos + 108 / 60 / 4 * dt              # position in bars
let barPh  = $bpos mod 1                       # where in the bar
let stepPh = (barPh * 16) mod 1                # where in the 16th
let i16    = int(barPh * 16)                   # which 16th: index a velocity table
let phrase = ($bpos / 4) mod 1                 # where in the 4-bar phrase
```

A hit is then a function of that phase too: an envelope of the time since
the step, `stepPh * secondsPer16th`, with no trigger and no `prev()`.
Anything added in a later send is on the beat from its first sample.

---

## Summary

1. **Start with a phasor**: the 0→1 ramp is the fundamental rhythm primitive.
2. **Derive everything from phase**: gates, envelopes and sequences are
   just math on the ramp.
3. **Clock division is frequency division**: halve the frequency for half
   notes, double it for eighth notes.
4. **Warp time for groove**: modulate the phase itself for swing and feel.
5. **Sequencing is oscillation**: `wave` at beat rate is a sequencer; at
   audio rate it is a custom waveform. Same mechanism, different timescale.
6. **Live: one clock, many phases**, so every hot swap lands on the beat.

In Aither, you don't program a drum pattern. You program the physics of a
drum being struck by a clock made of light.
