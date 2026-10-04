# Game audio roadmap (parked — not the current focus)

This document captures everything we've thought about aither as a
**procedural game audio engine**, so the IP isn't lost while
attention stays on the current focus: live-coding music and
instrument design for a MIDI keyboard.

**Status: PARKED.** Don't pursue this work until the music-side
investment plateaus or an external collaborator wants to. Future
sessions reading this doc cold should NOT take it as a roadmap
to start executing — it's a thinking artefact for the day this
becomes the right thing to chase.

## Why game audio matters

Andy Farnell's '90s critique of game audio (sample triggering, GBs
of WAV assets, no behavioural depth, no scaling to sandbox worlds)
mostly STILL applies in 2026. The industry made graphics fully
procedural (real-time CGI rendering). Audio is overwhelmingly stuck
on sample playback with parametric tweaks. **Aither's spectral
synthesis at sample rate with hot reload is decades overdue for
this market.**

Two structural arguments:

1. **Combinatorial explosion of assets.** N interactive objects
   produce N²/2 interaction sounds. Sample-based audio can't scale
   to a sandbox with hundreds of objects. Aither's parametric model
   is the only answer that scales: one `collide(material1, material2,
   velocity)` def replaces hundreds of WAV samples.
2. **CPU is no longer the constraint.** Past 2006/2007, the
   "haven't got the CPU" mantra became false. Aither's `sum(N,
   lambda)` unrolling 32 sines per voice is essentially free by
   2026 standards. The 2026 question is "what should we compute
   with all this CPU lying idle" — procedural game audio is one
   of the highest-value answers.

## The reframing — aither is a computational audio renderer

Andy Farnell's framing borrowed: aither is to sample-triggered audio
what CGI is to pre-rendered cutscenes. Declarative scene description
(the patch) → frame-by-frame audio output. The same primitives that
make live-coded music work also make procedural game audio work.

Live-coded music is the **proving ground and cultural seed**, not
the endgame. The fm_swarm "gothic Tesla organ" patch validated that
aither's spectral primitives produce world-class instruments. Same
primitives serve game audio.

## What aither already has that fits

- Sample-rate synthesis from minimal code (a 5-line `cello_shape`
  def replaces hundreds of MB of cello samples).
- Hot reload (matches game-development iteration cycles).
- C-via-TCC backend (compiles into native code, embeddable).
- Math-driven non-repetition (perfect for ambient game audio that
  shouldn't loop audibly).
- Spectral primitives no commercial game audio engine exposes
  (`additive`, `inharmonic`, FM-per-partial, sum-of-lambdas).
- No state machines by construction — every patch is one continuous
  expression. Andy's "state-based → continuous" reframe is enforced
  by the language.
- Composition discipline (modularity, scope, fractal reuse via
  `def`/`play`).
- Reproducibility — every sound is a file in git. Solves Andy's
  "ineffable knowledge" problem (sound designers who can't reproduce
  yesterday's settings).

## What's missing for the game audio use case

Listed roughly in dependency order. None are blockers; all are
extensions.

### 1. Synthesis-method coverage

Andy's five synthesis categories: sample-based, additive,
subtractive, modulation/distortion, physical modeling. Aither
covers 1.5 of the 4 math categories (additive ✓, subtractive
basic, FM partial, physical modeling weak; sample-based deliberately
out of scope).

To serve game audio, add named primitives:

- `fm(carrier, modulator, ratio, depth)` — FM as first-class
  primitive (currently doable inline but verbose, see fm_swarm).
- `karplus(freq, decay, brightness)` — Karplus-Strong plucked-string
  family. ~10 lines using `prev` + delay.
- `waveguide(freq, dispersion, damping)` — digital waveguide for
  bowed/blown instruments.
- `chebyshev(sig, order)` — Chebyshev waveshaper (controlled
  harmonic distortion).
- `phase_distort(sig, amount)` — Casio CZ-style phase distortion.

Terminology note: don't call this "physical modeling." Andy
explicitly refuses the term — classical PM means mass-spring-damper-
tensor finite-element stuff. What we'd be doing is **phenomenological
synthesis** (capture the perceptual signature, skip the underlying
physics). Better term: "behavioural synthesis" or "procedural
synthesis."

### 2. Behaviour-driven envelope library

Andy's insight: **the envelope IS the object.** A motor isn't
characterised by its rotor sound — it's defined by `1 - exp(-t/tau)`
spin-up and linear spin-down. Same for ballistic projectiles
(parabolic), liquid pouring (gravity-driven), explosions (pressure
decay), wind gusts (Perlin-noise modulated).

Each is ~5 lines of aither but unlocks a whole interaction-mode
category. Stdlib additions:

- `motor_envelope(target_speed, ramp_tau)` — exponential up, linear
  down.
- `ballistic_envelope(initial_v, gravity)` — projectile arcs.
- `pressure_decay(impulse, vessel_size)` — explosion / pop.
- `pour_envelope(flow_rate, vessel_geometry)` — liquid filling.

### 3. Interaction-mode catalogue (the procedural-audio stdlib)

Named primitives for the physical interactions Andy enumerates.
Each is a stdlib def composing existing primitives (~10-30 lines):

- `collide(material1, material2, velocity)` — impact event.
- `scrape(material1, material2, pressure, velocity)` — sliding contact.
- `friction(material, normal_force, tangential_velocity)` — stick-slip.
- `roll(material, velocity, surface_roughness)` — rolling contact.
- `cavity(geometry_volume, exciter)` — acoustic resonator.
- `fragment(material, energy)` — breakage event.
- `footstep(grf_phase, surface, footwear)` — Andy's worked example.

A `Materials` namespace would parameterise these — `metal`, `wood`,
`glass`, `plastic`, `concrete`, `flesh`, `fabric`. Each a small
def returning a parameter bundle (resonance frequencies, damping,
brightness coefficient).

### 4. Behavioural-depth heuristic

Add to COMPOSING.md when this work begins: "Ask 'how many ways can
this be interacted with?' before deciding how complex the model
needs to be." A static prop has no behavioural depth — noise +
filter + envelope is correct, not lazy. An interactive object has
rich behavioural depth and deserves rich modelling. The complexity
of the synthesis should match the complexity of the interaction.

### 5. Temporal `sum`

Aither's `sum(N, lambda)` unrolls N copies in **spectrum**. There's
no temporal counterpart — `seq(N, n => offset_fn(n), body)` that
emits the body N times at staggered times.

For game audio with mechanical sound (clocks, gun mechanisms, lock
cylinders, spring-loaded anything), this is a real gap. Andy's clock
example: at high zoom, the "tick" is 6+ cog-click micro-events
arranged in time. Could be a stdlib def using `discharge` + `impulse`
shifted by `n * dt`, or a new engine special form.

### 6. Listener decoupling — THE big architectural gap

Andy's "money shot" insight: same code, multiple listener positions.
Jet engine model runs ONCE on the client. Cockpit observer hears
it through cockpit-IR. Ground observer hears it through doppler +
distance. **One model, many tap-offs.**

Currently aither is one source → one stereo output. For game audio:

```
Voice → tick(t) → (raw_signal + metadata)
  per_listener.process(raw_signal, world_state) → (L, R)
```

Where `world_state` includes listener position, occlusion geometry,
room IR. Multiple listeners can subscribe to the same voice. CPU
per voice is constant regardless of listener count.

This is the biggest single architectural change. Touches engine.nim,
voice.nim, the audio output path. Probably 200-400 lines of new code.
Defers gracefully — voice.tick keeps working as today; listener
processing is opt-in for game contexts.

### 7. Level of audio detail (LAD)

Andy's rain example: close raindrops fully modeled, mid-range modal
hits on objects, distant raindrops just spectrally-shaped noise.
Aither computes every voice at full sample rate regardless of
perceptual position. For game audio, distance-driven detail
reduction would be significant.

Probably easiest to bolt onto listener decoupling (#6) — listener
declares its perceptual budget; voice produces a "best-effort"
signal at the requested complexity tier.

### 8. Spatial audio primitives

Beyond pan / Haas:

- HRTF convolution (head-related transfer function for headphone
  listening — gives a real sense of "the sound is BEHIND me").
- Distance attenuation (inverse-square + air absorption).
- Doppler shift (frequency shift driven by relative velocity).
- Occlusion / obstruction (low-pass filter when a wall is between
  source and listener).
- Room impulse response convolution.

Most of these are existing DSP standards, not aither inventions.
The aither contribution is exposing them as composable primitives.

### 9. Event-driven dispatch + game-state binding

Currently aither voices play continuously. Games trigger sounds on
entity events. Need an event-injection API:

```
aither_event("collision", {material1: "metal", material2: "stone",
                           velocity: 4.2, position: [12.5, 0, -3.1]})
```

Then a patch responds:

```
play impacts:
  on_event "collision":
    collide(materials[event.material1], materials[event.material2],
            event.velocity)
```

(That on_event syntax is illustrative — actual binding TBD.)

### 10. Engine packaging

Currently aither is a standalone process with an ALSA audio thread.
For game-audio embedding, package as:

- A C API exposed via shared library — game engines link against
  `libaither.so`, call `aither_load_patch(...)` and `aither_render(...)`
  per audio frame.
- A Unity native plugin (C API + Unity wrapper).
- A Godot extension (GDExtension).

The existing `voice.nim` + `parser.nim` + `codegen.nim` core is
already structured for this — `engine.nim` is the only file that
assumes "we own the audio thread."

## Roadmap (when this becomes the focus)

Bounded phases, each useful even alone:

1. **Synthesis-categories work** (covers gap 1). Music-side wins
   too — fm_swarm becomes a one-liner instead of inline FM math.
2. **Procedural-audio stdlib** (covers gaps 2 + 3). Extends the
   existing additive/inharmonic library with behaviour envelopes
   and interaction modes. Pure aither code in stdlib, no engine
   changes.
3. **Behavioural-depth doc** (covers gap 4). One COMPOSING.md
   subsection or a new GAME_AUDIO.md.
4. **Temporal `sum`** (covers gap 5). Engine special form,
   ~100 lines.
5. **Listener decoupling** (covers gap 6). The big architectural
   change. Should ship before spatial primitives so they have
   somewhere to live.
6. **Spatial primitives** (covers gaps 7 + 8). Implement on top of
   listener decoupling.
7. **Event dispatch** (covers gap 9). Adds the game-binding layer.
8. **Engine packaging** (covers gap 10). C API + Unity/Godot
   bindings. The "ship it as a product" step.

Total estimate: probably 10-20 sessions of focused work over months.
End state: aither is a serious procedural game-audio engine, with
live-coded music as the proven validation case.

## Principles and lessons (from Andy Farnell's lectures)

These belong in COMPOSING.md or PHILOSOPHY.md when this work begins,
or in a future GAME_AUDIO.md companion doc.

- **"Realism is a behavioural quality, not a sonic quality."**
  Players said the procedural jet engine sounded MORE realistic
  than samples — because it RESPONDED to their input. The sound
  was less photo-accurate, but interactivity made it feel alive.
- **"Most people think they want realism, they don't."** Sound
  design is smoke and mirrors, emotion, metaphor. Aither's `cello_shape`
  is three Gaussian peaks, not a finite-element model of a cello
  body. This is the right approach.
- **Phenomenal vs essentialist.** Don't model the thing in itself;
  model its perception. Aither sits in the phenomenal camp.
- **Behavioural depth determines synthesis complexity.** Static
  props get tiny models. Interactive objects get rich ones.
- **Find the signature process, parameterise around it.** For a
  footstep, it's the GRF (ground response force). For a motor, it's
  the speed envelope. For a collision, it's the impulse spectrum.
  Then everything else is a modulation of that core.
- **Procedural objects produce CLASSES of sound, not specific
  sounds.** A clock model becomes a watch / alarm clock / grandfather
  clock by parameterisation. A motor model becomes a drill / fan /
  helicopter rotor. Design for the class, not the instance.
- **Composite systems via shared LFOs / shared phase clocks.**
  Aircon's motor and fan share a shaft phase. Helicopter's main
  rotor and tail rotor are coupled via the engine. Express coupling
  in aither via top-level `let` bindings shared across plays.

## Trigger conditions for un-parking this

This doc gets actively pursued when ANY of these become true:

- Live-coded music side is "done enough" — the user has stopped
  finding new sounds in fm_swarm-style explorations and the
  language no longer feels like the bottleneck for music
  composition.
- A specific game project becomes a target (collaborator, jam,
  commission, personal game).
- An external collaborator with game-audio context joins and wants
  to drive this direction.
- A specific gap from above becomes clearly valuable for music too
  (e.g., behaviour envelopes for instruments responding to MIDI
  expression).

Until then: **stay focused on the music side.** Every win there
sharpens the case for game audio later because the synthesis
primitives are shared. The music-side wins are not detours; they
are the foundation.
