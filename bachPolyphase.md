# Bach Polyphony as Polyphase Mathematics

A speculation on the deepest unification of music theory and electrical
engineering, and what it could mean for aither.

This doc is **not a roadmap**. It's a notebook entry from a late-night
conversation about Eric Dollard's lifelong claim — that the polyphony
of Bach and the polyphase structure of three-phase electrical power are
the same mathematical process. The territory is largely unexplored in
audio synthesis.

## The claim

> "Polyphase music and the polyphase electrical system are based on
> the similar type of process."  — Eric Dollard

In three-phase electrical power:
- Three sinusoidal voltages, each phase-shifted by 120° (i.e. 2π/3).
- The phases sum to zero at every instant — perfect cancellation
  arithmetic that two-phase systems can't achieve.
- Power transmission is more efficient because the rotational pattern
  is genuinely 2D (not collapsing to ±).
- The `+/-` duality of single-phase math vanishes; you need three
  symmetric components (positive sequence, negative sequence, zero
  sequence — the Fortescue method).

In Bach's counterpoint:
- Multiple voices, each entering at a phase offset from the others
  (the canonical fugue: subject enters in voice 1, then voice 2 enters
  with the same subject delayed and transposed, etc.).
- Voice-leading creates harmonic resolution at specific phase
  alignments — the "cadence" is a moment when the rotating phases
  arrive at a consonant interval relationship.
- The deep math is intervallic ratios: 3:2 (perfect fifth), 5:4
  (major third), etc. Pythagoras's Lambdoma is the matrix of these
  ratios across all voices.

Both systems require multiple "phases" cooperating in a rotational,
phase-locked way. Both lose their natural simplicity when collapsed
to two voices (mono melody + bass = two-phase = lossy projection
of the actual structure).

## Why aither hasn't touched this

We've stayed in **two-phase land**:
- Stereo (L, R) — two channels.
- Complex pairs (real, imag) — two components.
- Polarities (positive, negative).

Every aither-native pair operation we've shipped (`cmul`, `freq_shift`,
`analytic`, `rotate`) operates on **2-component values**. That's the
electrical-engineering 2-phase equivalent.

Three-phase, five-phase, seven-phase mathematical structures don't
appear in our toolkit. Yet they are exactly where Dollard claims the
mathematics gets interesting — where Pythagorean ratios start to
mean something operationally.

## What "polyphase music" would look like in aither

Three concrete experiments worth keeping in mind:

### 1. True 3-phase voice rotation

Three independent voices computed in parallel, each at a 2π/3 phase
offset from the others. Like:

```
let phase = phasor(0.25)        # one cycle per 4 seconds
let v1 = synth(phase)
let v2 = synth((phase + 1/3) mod 1)
let v3 = synth((phase + 2/3) mod 1)
```

But the synth function is something where the phase OFFSET creates
musical interplay — e.g. each voice is at a different note in a chord,
or a different rhythmic position, and as the master phasor sweeps
they rotate through their relative positions.

The cadences happen at moments when the three voices land on
consonant intervals — and that's predictable from the phase
relationship, not from a hand-written sequence.

On stereo speakers you'd hear the sum (v1 + v2 + v3) in mono, but
the rotational structure is in the math. With proper multichannel
output (which aither doesn't have today) you could route each phase
to its own speaker for genuine 3-phase spatial sound.

### 2. Fortescue-style sequence decomposition

The Fortescue method decomposes any 3-phase signal into three
"symmetrical components": positive sequence (rotates one way),
negative sequence (rotates the other way), and zero sequence (no
rotation, in-phase across all three). This is to 3-phase what the
Hilbert transform's analytic decomposition is to 1-phase.

If aither had a `fortescue3(a, b, c) → (pos, neg, zero)` operation,
you could decompose any 3-voice arrangement into its rotational
components, manipulate the components independently, then recompose.
This is harmonic analysis at the level of phase rotation, not
frequency content.

### 3. Pythagorean Lambdoma as a chord-progression engine

Pythagoras's Lambdoma is a matrix of intervallic ratios:

```
       1    2    3    4    5
1:1   1/1  2/1  3/1  4/1  5/1     unison, 8va, 8va+5th, 2 8va, ...
1:2   1/2  2/2  3/2  4/2  5/2     5th, unison, 5th, 2 8va, ...
1:3   1/3  2/3  3/3  4/3  5/3     ...
1:4   ...
1:5   ...
```

Every cell is a ratio that defines a musical interval. The diagonals,
columns, and rows have specific musical-theoretic meanings (e.g. the
1:n column is the harmonic series; the n:n diagonal is unison; the
2:n column is everything an octave up).

A `lambdoma(numerator, denominator)` function could generate
intervallic relationships procedurally for chord progressions or
modal scale design — not "look up D minor pentatonic in an array,"
but "walk the Lambdoma matrix in this pattern."

This is the mathematical structure under both Indian raga theory
and Bach's modulation between keys.

## The bigger frame

If aither's `f(state) → sample` contract is taken literally, then
the deep gap we haven't crossed is:

> **The state vector can carry phase relationships across N>2 voices
> as first-class structure, with operations that act on the rotational
> group rather than on individual values.**

Today: state is a flat list of scalars; pair operations treat 2 of
them as a complex value; everything else is point-wise arithmetic.

The polyphase frontier: state contains phase-related groups (3-tuples,
5-tuples), and operations include rotation-group transforms (Fortescue
sequence decomposition, n-phase rotation, modulus-n cyclic operations).

This is **axis 3 territory** in the language of
`philosophicalSpeculations.md` — structured state with group-theoretic
operations rather than scalar arithmetic.

## What's worth holding open

- **2-phase audio is what we have today** (stereo, complex pairs).
  The 3-phase extension is mostly speculative because real-world
  speakers are 1- or 2-channel.
- **Multichannel output** would make true polyphase audible. Until
  then, we're simulating in mono — and mono summing of polyphase is
  literally Steinmetz's "permanent" representation: a single value
  that is the projection of a rotating multi-component process.
- **The Pythagorean Lambdoma is an interval generator**, not a
  synthesis primitive. It belongs in COMPOSING.md if it ever lands
  — as a way to generate scales and chord progressions
  procedurally.
- **Bach's fugue is the existence proof** that human ears can hear
  3+ voices in phase relationship as MUSIC, not as noise. The
  rotational structure is perceptible. We're not designing for
  electrical efficiency; we're designing for human cognition. Both
  domains share the same math.

## Open questions

- Is there a useful `sequence3(in, mode)` operation that decomposes
  a 3-voice signal into Fortescue components and recomposes after
  some transform on the components?
- Can the Lambdoma be implemented as `lambdoma(p, q) → freq` and
  used to procedurally generate chord progressions that are musically
  coherent because their underlying ratios are mathematically
  related?
- Does aither's lack of multichannel output kill polyphase audio for
  now, or can interesting 3-phase math still produce interesting
  mono-summable results (the way Bach is great even on a mono speaker)?
- Is there an audible difference between 3 voices computed at proper
  phase offsets vs 3 voices started at random times? Bach claims
  yes — the phase offset is structural, not random.

## The Tesla / longitudinal paradigm — voices coupled through a shared field

Tesla's lifelong work centred on **longitudinal** electric waves —
pressure pulses propagating through the medium of the aether at
near-instantaneous speed, as opposed to Hertzian transverse EM waves
travelling at light-speed across space. In his framework: instantaneous
action at a distance, resonant transmission (only systems tuned to the
right frequency receive), and energy that compounds rather than
disperses.

Every audio synthesis paradigm we know is **transverse**:
- Voices each compute their own waveform sample-by-sample.
- They sum at the master mix — that's the only place they "meet."
- Cross-coupling exists but it's READ-AHEAD: one voice reads another's
  already-computed previous sample via state.

Modular synths and DAWs structurally CANNOT do longitudinal coupling
because they're directed acyclic graphs (DAGs) with no real cycles.
You can route voice A's output back to voice B's input, but only with
a sample of delay — never instantly.

aither's `f(state) → sample` contract has no such restriction. The
state vector is one shared space; every operation in the file reads
and writes the same vector at the same sample. Three Tesla-paradigm
moves are reachable:

### 1. Shared scalar pressure field

A `$state` slot visible to all voices. Each voice both READS the
current pressure (uses it to modulate its own synthesis) and WRITES
its contribution back. The result is a coupled feedback system where
every voice's identity depends on what the others are doing — at the
SAME sample, not delayed.

```
$pressureField = 0.0
let p = $pressureField                  # read current pressure
let mySig = synth(...) * (1 + p * 0.3)  # field modulates my own synth
$pressureField = p * 0.99 + mySig * 0.1 # write contribution back
```

Every voice doing this with the same `$pressureField` produces
multi-voice resonant feedback. Bach's counterpoint where the voices
LITERALLY HEAR each other and adjust mid-note.

### 2. Resonant subscription via a DHO bank field

Instead of a scalar field, use an array of resonant DHOs each tuned
to a scale-degree frequency. Voices write force INTO the field at
their fundamental; voices read the field's resonant ringing back.

Two voices tuned to the same pitch cross-couple strongly through the
field; voices off-tune don't excite the corresponding resonator and
therefore don't hear each other. That's exactly Tesla's claimed
"resonant transmission" — energy transferred only between systems at
matching frequency.

This makes the FIELD do the harmonic filtering. The patch becomes
one coupled physical system (instruments in a resonant chamber,
mathematically) rather than a sum of independent voices.

### 3. Pressure scaling — phase-coherent compounding

When voices align in phase + frequency, the field amplitude COMPOUNDS
(constructive interference). When they're random, contributions
average out. So voices that play TOGETHER (phase-locked) produce
more energy than the sum of their parts — physically true, and
computationally true if the field is a real summation.

This means the patch automatically rewards harmonic alignment: voices
that resolve into consonance produce richer output than voices in
dissonance, without any explicit harmonic logic. The MATH does the
musical theory.

### Why this is genuinely aither-native

- **Per-sample mutual state**: every voice reads and writes the same
  state vector at the same sample. No bus latency, no DAG ordering.
- **State as field, not as memory**: `$pressureField` isn't holding a
  past value for one voice; it's holding the CURRENT field value
  shared among all voices.
- **Operations across voices, not just within**: today voices interact
  only via the master mix sum; this lets them interact via the field
  at synthesis time.

The transverse-wave model gives you sound. The longitudinal-wave model
gives you *resonant interaction*. Bach's polyphony works because the
human ear is the field that couples the voices in real time. In aither
the field can be explicit — a state cell, a DHO bank, anything that
all voices share with bidirectional read-write access.

This is the substrate that would make true 3-phase polyphase audio
musically alive rather than mathematically nice. Without the field,
3 voices at 120° phase offsets are just three independent oscillators
that happen to be related. WITH the field, they become a coupled
system that resolves and tensions and breathes the way real ensemble
playing does.

### Open question

The longitudinal paradigm requires `$state` cells with **mutual
read/write within a single sample tick**. aither's `$state` semantics
support read-then-write within a call site, but the cross-voice
ordering of reads and writes within a single sample needs to be
specified. Options:
- All voices read the field FIRST, then all write contributions LAST
  (1-sample delay between read and write across voices).
- Voices process in declaration order, each seeing the field as
  updated by previous voices in the same sample (no delay, but
  order-dependent results).
- Iterate the field within a sample to convergence (expensive but
  most physically accurate).

The right choice probably depends on what kind of resonant feedback
behaviour produces musical results. This is an aither engine question
worth thinking about before the first patch tries to use it.

## Footnote: longitudinal vs transverse waves

Dollard's physical demonstrations (rope/slinky, stone-in-water) emphasise
that any oscillating medium carries TWO simultaneous wave types,
orthogonal to each other:

- **Transverse** — observable surface motion, propagates slowly along
  the medium (the visible ripples on water).
- **Longitudinal** — compression/pressure, propagates near-instantly
  through the medium (the pressure pulse that reaches the bottom of
  the water column the moment the stone breaks the surface).

In Steinmetz's electrical framework: transverse maps to the dielectric
(radial/potential) field; longitudinal maps to the magnetic
(axial/kinetic) field. Both are always present in any electrical
phenomenon — they are the two incommensurate components.

The aither analogue (loose mapping):
- **Time-domain operations** (filters, delays, envelopes) propagate
  ALONG the signal flow sample by sample — transverse.
- **Frequency-domain operations** (`freq_shift`, `analytic`, `cmul`)
  affect ALL frequencies simultaneously at the same sample —
  longitudinal.

The deepest aither moves likely live in the COUPLING between time and
frequency domain — a time-domain parameter read from a frequency-domain
analysis of the same signal, or vice versa. That's analogous to how a
real transformer couples primary and secondary through the simultaneous
existence of both wave types.

This strengthens the polyphase argument: 3-phase electrical power
transmits both transverse and longitudinal components in a coupled
rotational structure. That coupling is the source of its mathematical
richness, and may be what the Bach-polyphony equivalent would unlock
in audio.

## Maxwell's equations as monopoles vs multipoles

A FractalWoman video reframes the first two of Maxwell's equations
in a way that, if right, dissolves the long-standing asymmetry between
the electric and magnetic field laws.

The textbook framing:
- **Gauss's law for electricity** — the divergence of E equals the
  enclosed charge density. Electric field lines start and end on
  charges. Sources and sinks exist.
- **Gauss's law for magnetism** — the divergence of B equals zero.
  Magnetic field lines have no start or end; they always close.
  Magnetic monopoles do not exist.

Her proposal: stop naming these laws after Gauss. Rename them by what
they describe geometrically.

- The first becomes the **law of monopoles**. A free charge IS a
  magnetic monopole — an isolated source/sink whose field has nonzero
  divergence.
- The second becomes the **law of multipoles**. When charges are bound
  together in a body (atoms, dipoles, current loops, magnets), the
  field they collectively produce has zero divergence — it circulates
  in closed loops. Multipoles.

The deeper claim: the electric and magnetic fields **might be the
same entity**, distinguished only by whether the underlying charges
are free or bound. A free charge looks "electric" (divergent field).
The same charges bound in proximity look "magnetic" (circulating
field). One field, two regimes.

If you accept the renaming, the asymmetry that bothered physicists
for a century — "why does electricity have monopoles but magnetism
doesn't?" — disappears. The answer becomes: nobody would say there's
no such thing as a "mono multipole." The categories were the bug.

### Why this matters for aither

The monopole / multipole distinction maps onto a clean structural
choice in synthesis:

- **Monopole behaviour** — isolated sources and sinks; energy
  diverges from a point or converges into one. This is what
  envelopes, triggers, and per-voice excitation already do.
  `pluck(trig, k)` is a monopole — energy injected at a point in
  time, dispersing.
- **Multipole behaviour** — closed-loop circulation; no net
  divergence; energy moves around a structure without leaking out.
  This is what feedback loops, resonant DHO banks, and circulating
  delays do. The Tesla "shared field" section above is multipole
  behaviour by construction — every voice writes back what it
  reads, so the field has no net source.

What's NOT in aither today: a primitive that **switches between
the two regimes** based on coupling. A single oscillator that
behaves as a divergent source when isolated, but joins a closed-loop
multipole structure when its state cell is shared with sibling
oscillators.

### A possible primitive

Imagine a `pole(state, drive)` operation with the contract:
- If `state` is unique to this call site, it acts as a monopole —
  drive energy injected, dissipates through the local DHO.
- If `state` is shared with N other `pole` call sites at compatible
  frequencies, the bank locks into a multipole — energy circulates
  among the participants, divergence at any one site goes to zero,
  and the only loss is the bank's collective damping.

The same primitive, two regimes, distinguished only by whether the
state is private or shared. This is exactly the shape of FractalWoman's
claim about the field: free charge vs bound charge — same charge,
different topology.

It also lines up with the Steinmetz dual-energy framework. A free
oscillator carries dielectric potential AND magnetic kinetic energy
into the void. A bound oscillator bank exchanges those two
energies internally — potential turns into kinetic, kinetic feeds
back to potential, and the multipole closes. Steinmetz's permanent
representation is the multipole's stable state; the transient
solution is the monopole regime.

### Connection to the longitudinal/transverse split

The monopole/multipole distinction may BE the longitudinal/transverse
distinction, viewed from a different angle:

- **Longitudinal / monopole** — pressure, scalar, divergence-bearing.
  A source pushes energy outward radially; the field at any point
  has a definite "amount" that can be summed.
- **Transverse / multipole** — circulation, rotational, divergence-free.
  Energy moves perpendicular to the propagation direction; closed
  loops; no net source.

If this mapping holds, then Tesla's "longitudinal waves are
instantaneous" becomes a statement about monopole coupling: when
multiple sources share the same scalar field, a change at any one
source is felt at all others within the same instant — because the
field IS the coupling. There is no propagation delay because there is
no spatial transmission; there's just the simultaneous update of a
shared state cell.

This is the strongest argument yet that aither's `f(state) → sample`
contract — with shared mutable state across voices — is the right
substrate for whatever the longitudinal/monopole paradigm turns out
to mean operationally.

### Open question

If the proposal is correct and the electric and magnetic fields are
two regimes of the same entity, is there a corresponding ONE PRIMITIVE
in aither that subsumes both `pluck` (monopole, dispersive) and `dho`
(multipole when arrayed, dispersive when isolated)? Or is the duality
already there — `pluck` is the open-loop limit of `dho` — and we just
haven't named it that way?

The renaming, if nothing else, is a useful diagnostic. When designing
a new primitive, ask: does this thing have a divergence (it sources or
sinks energy at a point) or is it divergence-free (it just circulates
what's already there)? The answer tells you whether you're in the
monopole regime or the multipole regime, and what coupling structure
the patch needs to use it well.

## The yin-yang as schematic — vortex self-similarity across scales

A separate FractalWoman video lays out her "ohm particle cosmology":
the universe as a Bose-Einstein condensate whose bulk hosts a sea of
plank-scaled vortices. Each vortex is a yin-yang — a spinning
two-lobed structure whose tangential velocity is c and whose
circumference is the Compton wavelength of whatever scale it lives
at (electron, proton, plank).

The cosmology's specific physics claims aren't what's interesting for
aither. The interesting move is the **schematic**:

- One geometric primitive — a spinning two-lobed vortex.
- Self-similar across scales — same shape at the plank, proton, and
  electron scales; only the spin rate and the circumference change.
- Built-in handedness — clockwise vs counterclockwise is part of the
  symbol, not bolted on afterward.
- Built-in duality — yin and yang are not two things glued together;
  they are one rotation viewed as two.

This is a sharper way of saying what the pair operations
(`cmul`, `analytic`, `freq_shift`) already do. A complex pair is a
yin-yang: two real numbers that ARE one rotating thing. The handedness
is the sign of the imaginary part. The "spin rate" is the angular
frequency. We've been writing yin-yang operations all along; the
diagram makes it explicit.

### What this could mean for aither

A `vortex(rate, scale)` primitive that exposes the same shape at
every level it's used:
- At the sample-rate scale, it's an oscillator (rate = audio freq).
- At the control-rate scale, it's an LFO (rate = modulation freq).
- At the structural scale, it's a phasor driving section transitions
  (rate = once per minute).

Same primitive, three scales, self-similar. Today aither has
`phasor`, `sin`, `dho`, `lfo`-style patterns — but they're
typed/named by their use, not unified by their shape. The yin-yang
schematic suggests they could be one thing parameterised by scale.

The clock-frequency framing in the video is also a useful reminder.
She visualises the plank vortex as the universal computer's clock at
~10^42 Hz; everything else runs as a divider off that clock. aither's
sample rate (44.1 kHz) is the analogue — every frequency in a patch
is a divider off the sample clock, and treating the sample clock as
the "universal vortex" of the patch is closer to the truth than
treating it as a quantisation artefact.

### Footnote on H vs H̄

The video's main technical claim is that physicists should use H
(non-reduced Planck constant, cycles/sec) rather than H̄ (reduced,
radians/sec) in Planck unit calculations, because mixing the two in
one equation introduces a factor-of-2 error and obscures the
geometric meaning (4π r² as the surface area of a vortex).

This is the same complaint, in physics, as the one Steinmetz made in
electrical engineering when he insisted that the natural unit was
the cycle, not the radian. aither already uses cycles natively
(`phasor` is normalised 0..1, `TAU` is applied at the sine call).
That choice — cycles as the natural unit — is the right one for the
same reason: it keeps the geometry honest. Multiplying by 2π is the
coupling to the sine; it's not part of the rate.

## Sources

This document was prompted by a 2021 conversation between FractalWoman
and Eric Dollard (audio transcribed in session) where Dollard discusses
his lifelong project to find the connection between J.S. Bach and
3-phase electrical power. Reference: Dollard's "Symbolic Representation
of the Generalized Electric Wave" and "A Method of Symmetrical
Coordinates Applied to the Solution of Polyphase Networks" (a sequel
to Charles Fortescue's paper of the same title), available at
emediapress.com.

Pythagoras's Lambdoma is documented in Hans Kayser's work on
harmonic analysis (German, mid-20th century).

The Steinmetz dual-energy framework (magnetic kinetic + dielectric
potential, treated as permanent quantities via complex numbers) is
the immediate mathematical predecessor of Fortescue's polyphase
sequence components.

The monopole/multipole renaming of Maxwell's first two equations,
and the proposal that the electric and magnetic fields are two
regimes of the same entity, comes from a separate FractalWoman video
discussing Maxwell's equations and analogies to vortex/antivortex
pair structures in Bose-Einstein condensates.
