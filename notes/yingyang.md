# The yin-yang lens for aither

A parked design note. Not a roadmap. Captures a way of seeing the
existing pair operations and a small primitive extension that might
follow from it.

## The lens

A complex pair `(re, im)` is not two numbers. It is **one rotating
thing seen as two reals**. The yin-yang diagram is the picture: two
lobes that are inseparable from a single rotation. Handedness
(clockwise vs counterclockwise) is the sign of the imaginary part.
Spin rate is the angular frequency.

This is what `cmul`, `analytic`, `freq_shift`, and `rotate` already
do under the hood. The schematic just makes the shape explicit.

## Why this is interesting beyond renaming

Self-similarity across scales. The same yin-yang shape applies at:

- **Audio rate** — an oscillator. One vortex per cycle of the
  fundamental.
- **Control rate** — an LFO. One vortex per cycle of the modulator.
- **Structural rate** — a section phasor. One vortex per cycle of
  the form (chorus / verse / bridge).

Today aither has separate idioms for each (`phasor` + `sin`, LFO
patterns, hand-rolled section counters). The yin-yang frame says
they are **the same thing at different scales**. If a composer
internalises that, the question shifts from "what's oscillating
here?" to "what's spinning, and at what rate?" — which is a richer
question.

## Proposed primitives (not committed)

### `vortex(rate) -> (cos, sin)`

A built-in that returns a normalised 2-component rotation at the
given rate. Equivalent to `analytic(sin(TAU * phasor(rate)))` but
one call. Pure sugar; the value is the name, not the math.

### `spin(pair, rate) -> pair`

Rotate an existing 2-pair by `rate` cycles per second. This is the
composable verb. `cmul` rotates a pair by *another pair*, which is
the right primitive when both rotations are signals. `spin` is the
right primitive when one rotation is a clock — "keep this thing
turning at this rate."

The interesting use is **nesting**:

```
let v_audio   = vortex(440)              # spinning at audio rate
let v_lfo     = spin(v_audio, 0.5)       # whole thing precesses at 0.5 Hz
let v_section = spin(v_lfo, 0.01)        # the precession itself precesses
                                         # at 0.01 Hz (once per 100s)
let out = re(v_section)                  # project to audio
```

Three vortices at three scales, composed. The result is structurally
different from nested *modulation* (FM, AM) because it is nested
*rotation* — every level preserves the geometric relationship of the
levels below it.

## Will it produce novel sounds

Honest: not from `vortex` alone. That is sugar.

`spin` nested at multiple scales is the open question. Nested
rotation might produce textures that nested modulation does not,
because:

- FM at low rates becomes vibrato; at high rates becomes timbre. The
  transition is the gap.
- Nested rotation has no such transition — it is the same operation
  at every scale, and the audible result depends on how the rates
  interact (resonance? beating? slow precession of timbre across
  minutes?).

Plausible novel territory:
- **Slow timbral precession** — a sound whose harmonic content
  rotates around a fixed centre over tens of seconds, producing
  inhuman-but-not-random evolution.
- **Phase-locked section transitions** — a section change that is a
  rotation in the same field as the audio, so the transition feels
  *continuous with* the sound rather than imposed on it.
- **Self-similar drift** — drift at three scales that all derive
  from the same vortex shape, so the patch breathes the same way at
  every timescale.

Plausible NOT novel territory:
- Single-vortex use. That's just `analytic(sin(...))`.
- Two-vortex composition. That's just `freq_shift` or AM, depending
  on how the rotations combine.

The novelty, if it exists, lives at three or more nested scales.

## Why parked

1. Adding two primitives for a "maybe novel" outcome is poor ROI
   compared to the monopole/multipole `pole(state, drive)` idea
   from `bachPolyphase.md`, which adds a regime we don't have.
2. The schematic is already useful as **documentation and
   composition guidance** without code changes. Composers can think
   in vortices using `analytic` + `cmul` today.
3. The composability of `spin` depends on whether nested rotation
   actually produces interesting audible results, which is unknown.
   Cheap to test once but risky to commit primitives around.

## Cheap experiments before committing

Before writing `vortex`/`spin` into the engine, build the same shape
using existing primitives:

```
def vortex(rate):
  let p = phasor(rate)
  let c = cos(TAU * p)
  let s = sin(TAU * p)
  pair(c, s)

def spin(v, rate):
  let p = phasor(rate)
  let r = pair(cos(TAU * p), sin(TAU * p))
  cmul(v, r)
```

Then write a patch that nests three of them and listen for whether
the structural-scale spin actually produces something the ear can
follow. If yes, promote to built-ins. If no, the lens stays a
documentation tool.

## Connection to other docs

- `bachPolyphase.md` — the polyphase / Tesla / monopole-multipole
  framing. The yin-yang is the **shape** of one phase; polyphase is
  what you get when you have N of them rotating in coordinated
  offset. 3-phase electrical power is three yin-yangs at 120°
  apart.
- `philosophicalSpeculations.md` — axis 3 (structured state with
  group-theoretic operations). `spin` is a group-theoretic
  operation; nested `spin` is a chain of rotations in the rotation
  group, which composes.
- `PHILOSOPHY.md` — "complex numbers as operations, not types." The
  yin-yang is the diagrammatic version of that principle.

## Open questions

- Does `spin` belong as a primitive, or is `cmul(v, vortex(rate))`
  ergonomic enough that the extra name doesn't earn its keep?
- Is there a polar-form version (`vortex_polar(magnitude, phase) ->
  pair`) that would unlock anything `cmul` can't?
- Can the multi-scale self-similarity be made structural — e.g. a
  `vortex_chain([rate1, rate2, rate3])` that returns the composed
  rotation directly, so the nesting is a single declaration rather
  than three lines?
- Does the framing change what composers reach for, or do they
  ignore it and keep writing `sin(TAU * phasor(...))`? The answer
  determines whether the rename is worth it.
