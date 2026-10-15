# The output is a pair: sound, scope and radio from one language

Decided 2026-10-15 (Roland, with Claude on the uConsole). This settles
how aither grows from audio to the oscilloscope and radio without a
second language, and what that means for the `scope` branch merge.

## The decision

A program is a function of state whose output is its final expression:
a float or a pair. That is the whole interface. What the two numbers
mean is decided by the *loop* the program is sent to, never by the
program:

| loop  | sent with            | reads the pair as         | reads a float as          |
|-------|----------------------|---------------------------|---------------------------|
| sound | `aither send`        | left, right               | mono (both ears)          |
| scope | `aither scope`       | x, y: a point in the plane| a real number, `[s, 0]`   |
| radio | `aither radio --rate`| I, Q: complex baseband    | a real signal, `[s, 0]`   |

This is PHILOSOPHY.md's "two cells become a complex number when an
operation reads them as one" taken literally. The speaker reads the
pair as two channels, the scope as the Argand diagram (visualising a
complex signal *is* drawing it), the radio as baseband, where `rotate`
is a frequency shift, `magnitude` the AM envelope and `phase` the FM.
Same two numbers, same operations, three meanings, and the language
names none of them.

Mono stays. A float at the output is read as a real number by every
loop, and as mono by the speaker, because that is what mono means. The
implicit reading hides nothing (a float to a speaker can mean only one
thing) and keeps `sin(TAU * phasor(220)) * 0.2` a complete program.
Making the pair mandatory would save ~20 lines of codegen and cost a
bracket on every mono patch and on the first line a learner writes.

Inside a program floats stay floats. Oscillators, envelopes, filters
and physics are real-valued; `play` parts and `let`s may be either.
`mono(s)` remains: an operation that reads a pair as a real.

## Rates

Programs are written in seconds and hertz, never in samples (`phasor`,
`delay`, envelopes, `t`; only `sr` names the rate, and code that uses
it is correct at any rate). So the rate belongs to the loop, not the
program: sound at 48 kHz, scope at the sound rate, radio at whatever
the SDR wants. The same text renders at 160 kHz for the bat lure.

**One program, one rate.** Several rates inside one file (this part is
audio, that part is radio) would need rate inference across the graph.
That is a research feature and exactly the abstraction layer the
language refuses. A program runs at one rate like a circuit runs at
one clock; programs at different rates talk through the engine.

## Names cross programs

Every named signal in a running program (`play` parts, top-level
`let`s) is readable by name from any other program: `didge.drone`,
`didge.bar_phase`, `didge.lip_pressure`. The engine already evaluates
them into variables each sample; keeping the last value is free. The
engine's meters are names too: `didge.sticks.rms`, `master.peak`,
`master.low` (one cheap filter each), so today's `low` / `mid` / `hit`
globals become ordinary names defined once.

Two kinds of reader, both the engine's business, outside the language:

- a **stream** consumer takes every sample: the speaker, the scope
  display, the SDR, a WAV file, and any program in the *same* loop
  (same tick, so `trace.aither` reads this sample's `didge.drone`,
  sample-accurate);
- a **sampler** takes the latest value across rates: sample-and-hold
  going slow to fast (as `midi_cc` does now), linear interpolation
  going up to a much faster rate (audio into radio).

## Visuals

The beam is the visual. `trace.aither`, sent to the scope loop, emits
a pair and reads the music by name:

```
let s = didge.drone
let k = didge.sticks
[s + 0.3 * k, s - 0.3 * k]
```

Mono draws the classic 45° line; a real signal lies on the real axis;
the picture and the maths agree without a special case. The trace is
its own live-codable file: swap it without touching the sound, and a
broken trace never silences the didge. Beam brightness is not a third
channel: it is a named signal the display reads, or blanking by moving
the beam off-screen, as real scopes do.

**The raster look is the display's job.** Winamp AVS was a pair
language: the superscope emitted x, y per point and everything else
(feedback, blur, kaleidoscope, colour map) was an effect chain with
parameters. The didge_scope scenes already have this shape: `beam(p)`
draws the trace, then feedback, rotate, the six-wedge fold and the
palette are effects on top. So the display keeps an effect chain whose
parameters are live and read named signals (`didge.sticks.rms`,
`didge.bar_phase`); the `.paint` scenes are that display's presets,
with their own small compiler in the web client, as today. They are
not aither and the language never needs a colour. A local GUI (SDL +
GLES on the uConsole's Mali) runs the same effect chain; the preset
format is small enough that a second implementation is a renderer
detail, not a language fork.

What this gives up: arbitrary per-pixel math written live. What it
keeps: trails, folds, feedback, palette, and the live-coded geometry,
which is where the AVS look came from.

## Arrays (settled 2026-10-14/15, restated)

Arrays are data: tables, `wave` lists, buffers, and the returned pair.
Nothing maps a function over an array; write the repetition out with
`sum` / `each` and a table. A function that wants a table reads a
table whatever its length (`wave(1, [0.25, 0.75])` works, commit
8407443); arithmetic is element-wise on pairs and broadcasts a float;
any other array in arithmetic is a compile error that says to index it.

## What this means for the `scope` merge

Take the engine and language work on `scope` (state arrays, `each`,
`len`, `import` and `watch`, fractional `delay`, independent `noise()`,
`park`, migration keyed by def, `render`, meters and `status`, `%`,
negative literals, exponents, lp1/hp1 in hertz, haas and reverb taking
pairs, the 16 tests). Then:

- retire the magic `scope` / `beam` part names: the beam becomes a
  program of its own emitting a pair;
- drop the third intensity channel; the 5-channel pinned output becomes
  two pair streams (sound, beam);
- replace the `low` / `mid` / `hit` globals with engine names;
- keep `stream/` as the display (web client, presets, effect chain);
  drop the server-side C paint renderer from the roadmap: it is a
  backend for one deployment, not the design;
- convert the one `lp1` call in `edc/11_physics.aither` to hertz
  (written on main after the fork; every other patch was converted on
  `scope`);
- resolve the SPEC.md conflict by keeping main's arrays text and
  adding scope's state-array / `each` sections.

## Rejected on the way

- **N-channel outputs** (colour as `[r, g, b]`): 378 of codegen's 2070
  lines are the pair machinery, every site would become a loop over
  channels, and only colour would use it.
- **An `out` keyword** naming destinations in the file: the only case
  needing two destinations (sound + beam) is better as two programs.
- **Magic part names** (`play scope:`): meaning by name is what the
  philosophy says not to do.
- **A `stereo(l, r)` constructor**: the pair as an array is the point.
- **Mandatory pair at the output**: see above.
- **Multi-rate inside one file**: see Rates.

## Still open

- Syntax for cross-program names: dotted identifiers (`didge.drone`)
  read best; `input("didge.drone")` needs no parser change.
- The radio loop itself (SDR output, rate, decimation) is unbuilt;
  `~/aitherRadio` is empty. The shape and operations need nothing new.
- A local display: same effect chain as the web client, in GLES.
