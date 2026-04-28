# Manifesto for a Classical-Field-Theory Programming Environment

A draft. Subject to revision. Records, in plain language, what this
project is becoming and why it might matter beyond the synthesis-of-
sound it currently does.

## The thesis

There is a coherent intellectual tradition — running from Faraday and
Maxwell through Steinmetz and Dollard, through Hertz and Tesla,
through Tudor and Roland Kayn, through Hamkins on mathematical
pluralism — that treats *classical fields* as the fundamental
substrate of physical reality and *operations on those fields* as the
fundamental computational primitives.

This tradition has never had its own programming environment.

Programming languages, even those used heavily for signal processing
and simulation, have inherited their abstractions from elsewhere:
from the algebraic-quotient view of mathematics, from the discrete
finite-state-machine view of computation, from the type-theoretic
view of values. These abstractions are *not wrong* — they are simply
not native to the classical-field tradition, and they impose costs
when the substrate they describe is fundamentally a continuous
oscillating medium evolving under local rules.

A classical-field-theory programming environment treats:

- **Signals as field values**, not as data.
- **State as field configuration**, not as memory.
- **Evolution as a local update rule**, not as a procedure call.
- **Observation as projection**, not as I/O.
- **Coupling as shared field access**, not as message passing.
- **Composition as superposition + interference**, not as function
  composition over typed inputs.

The unit of computation is the **damped harmonic oscillator** —
Steinmetz's dual-energy circuit, the universal second-order
resonator, the simplest nontrivial classical field — because every
classical-field system that does interesting work reduces to a
network of these oscillators coupled through one or another field.

The arithmetic of the substrate is **the rigid/coordinate conception
of complex numbers** — two real quantities in fixed quadrature,
operated on by rotations rather than reified as a separate type.
This is what Steinmetz operationalised in 1893, what every classical
field theory in physics actually uses, and what every type-theoretic
"Complex" library quietly betrays by hiding the orientation choice
that the physics requires.

## What we have already built (and what it points at)

aitherLang is a probe into what this environment looks like for
**audio synthesis**. It commits to:

- The `f(state) → sample` contract — every signal is a function from
  field configuration to one observable value, evaluated locally,
  iterated indefinitely.
- Pair operations (`cmul`, `rotate`, `analytic`, `freq_shift`,
  `phasor_pair`) as the rigid-conception complex algebra.
- `dho` as the universal oscillator primitive.
- Mutable shared state across voices for genuine inter-field coupling.
- A REPL with hot-reload — every patch is a live, editable artefact
  rather than a compiled binary.
- Codegen that emits direct C arithmetic — zero abstraction overhead,
  the audio loop is a single tight numerical kernel.

resonance-ocaml is a probe into what this environment looks like for
**sequence prediction / cognition**. It uses the same DHO equation,
the same pair-state representation (pos, vel), the same commitment
to bounded amplitudes and physical interpretability. Its working
artefact, the Causal Oscillator LM (parameter-golf submission #1061),
achieves competitive bits-per-byte on FineWeb at 14.8 M parameters
compressible to 11.2 MB — and the same architecture transfers to
audio without modification (26.4 dB causal speech continuation from
oscillator states, no tokeniser).

These are two probes into the same substrate. They share an equation,
a philosophy, a coordinate convention, and a stance against the
type-theoretic / DAG-based / batch-stateless abstractions that
dominate elsewhere. They differ in surface (live coding vs gradient
descent) and in deployment target (audio thread vs CUDA), but the
substrate underneath is one substrate.

## The next move — name the substrate

The substrate has been implicit. Naming it makes it possible for:

- *Other projects in the same lineage to recognise themselves.* People
  doing oscillator-bank ML, modal-synthesis audio, neuromorphic
  computing, control-systems simulation, EEG / biosignal processing,
  classical electromagnetic modelling, and other field-theoretic
  computation are working on the same substrate without knowing it.
- *Shared infrastructure to be built once.* If the substrate has a
  name and a canonical primitive set, libraries that implement it
  in different host languages (Nim, OCaml, Python, Rust, future
  hardware-specific assembly) can be interoperable rather than
  reinvented.
- *A school of thought to coalesce.* Programming-language schools
  exist (functional, logic, array, concatenative) and they shape
  what gets built. There is no current school for classical-field-
  theory programming. There should be.

The working name, until something better arrives: **CFT** —
Classical-Field-Theory programming. Pronounced as letters, treated
as both noun and adjective. *"This is a CFT environment." "This
primitive is CFT-native." "The CFT substrate handles the audio
synthesis directly."*

## What the substrate library would contain

Concretely — not as a roadmap, but as a sketch of what *should* be
expressible by anything calling itself CFT-native:

**Core primitives** (the irreducible operations):
- `dho(state, drive, freq, damp) → output` — the universal damped
  harmonic oscillator. Multiple discretisation forms (continuous-time,
  rotation-form, FFT-bank-form) producing equivalent math at
  different costs.
- `pole(state, drive)` (when it lands) — regime-switching DHO that
  behaves as a monopole when state is isolated and as part of a
  multipole when state is shared.
- `pair_ops` — `cmul`, `cdiv`, `cscale`, `rotate`, `analytic`,
  `freq_shift`, `phasor_pair`, `magnitude`, `phase`. All operate on
  pair-shaped argument lists; no type, no boxing.
- `state_arena` — per-call-site mutable state cells with shared
  regions for inter-voice coupling.

**Field operations** (combinations the substrate treats as natural):
- `field_couple` — bidirectional state sharing within a sample tick,
  with a chosen update semantics (two-pass, sequential, iterative).
- `superpose` — explicit named addition (the substrate emphasises
  that addition IS interference).
- `project` — extract a one-dimensional observable from a field
  configuration (audio output, prediction, control voltage).

**Spectral operations** (the FFT-bank class):
- `fft_bank(N, freqs, damps)` — N-pole resonator bank computed via
  one FFT, equivalent to N separate `dho` calls but O(N log N) in
  cost. Used in resonance and would unlock high-N additive
  synthesis in aither.
- `monarch(input, factors)` — Monarch-style block-diagonal mixing
  matrix, O(N^{3/2}) factorisation of dense matmul. Aither's matrix-
  mixer primitive proposal would land here.
- `butterfly(input, angles)` — O(N log N) butterfly rotation network,
  the deployment-side complement to Monarch.

**Stability operations** (numerical-discipline primitives):
- `clamp_logits` — bounded saturation for cumulative multiplications.
- `xavier_init` — the right initialisation discipline for bounded
  cumulative loops.
- `rmsnorm` — energy normalisation for layer chaining.

This is enough to express both aither's audio synthesis and
resonance's sequence modelling — and, crucially, anything else in
the same lineage. Each primitive has multiple implementations
(reference C, optimised SIMD, CUDA, RISC-V fixed-point) selected
by the host environment.

## Could this be an OS?

The question sounds extreme. It isn't.

A REPL turns a runtime into an environment. An environment that owns
its compute primitives top-to-bottom turns into a system. A system
that is the only thing the user runs is, operationally, an OS. This
is the path Lisp Machines, Smalltalk, Plan 9, and TempleOS all
followed — and the path Forth followed before all of them on
embedded hardware.

The CFT environment has the prerequisite shape:

- **One language top to bottom.** The same primitives express audio
  synthesis, signal analysis, ML inference, ML training, control,
  simulation. No "engine in C, scripting in Lua, training in
  Python."
- **Hot-reload as the interaction model.** Every artefact is live-
  editable. There is no edit-compile-run cycle; there is only
  edit-and-listen. A neural network trained at 3am can be tweaked
  at 4am without restarting anything.
- **Image-based state.** A running CFT system's state is the
  field configuration. Snapshot the field, restore the field,
  resume from where you were. The Lisp Machine's "save world,
  restore world" property is structurally available because the
  substrate IS state.
- **Direct hardware access.** Codegen emits inline arithmetic with
  no framework overhead. The shortest path from "user wrote this"
  to "the speaker / GPU / RISC-V chip executed this" is what the
  language was built around.

What would a CFT operating system *be*? A boot environment whose
shell is a REPL, whose data model is field configurations, whose
applications are patches (some of which happen to be trained ML
models, some of which happen to be audio synthesisers, some of which
happen to be control loops for physical hardware). One mental model.
One way of working. Top to bottom.

The hardware to try this costs $12 (Pi RP2350 with 8MB PSRAM — see
the existing Forth9 work in this neighbourhood). The CFT runtime
fits in tens of kilobytes. Patches are short enough to type from a
prompt. **The economics are no longer wrong.**

## The analog-computer endgame

The most striking thing about all of this is *which way the hardware
is moving*.

For the last forty years, "computing" has meant digital von Neumann
architectures. But:

- **Mythic AI** ships analog matrix-multiply chips for ML inference.
  The matmul is performed by literal analog currents through a
  resistor crossbar — not digital arithmetic at all.
- **Neuromorphic chips** (Intel Loihi, IBM TrueNorth, BrainChip
  Akida) implement spiking neural networks in event-driven analog
  silicon. The "computation" is the dynamics of physical
  oscillators on chip.
- **Optical neural networks** (Lightmatter, Lightelligence) do
  matmul with photons — the computation IS interference, the
  primitive IS the wave equation.
- **Memristor crossbars** (HP, Knowm) use the physical hysteresis
  of memristor devices to store and compute simultaneously.

What all of these have in common: **they are physical systems
evolving under classical field equations, used as computers**. They
are analog computers in everything but name. And they need software
substrates that match their physics.

A typical ML framework (PyTorch, JAX, TensorFlow) was designed for
digital von Neumann hardware. Compiling a PyTorch model to a Mythic
analog chip requires a translation layer that fights both ends — the
Python abstractions don't map naturally to analog crossbars, and the
analog crossbars don't naturally execute Python's dynamic semantics.

A CFT environment was designed for *exactly the kind of physics
these chips embody*. The DHO primitive maps to a real oscillator.
The pair-state representation maps to two physical voltages in
quadrature. The FFT-bank primitive maps to a parallel optical
interferometer. The Monarch matrix maps to a butterfly photonic
mesh. **The substrate that aither and resonance share is the
substrate analog hardware needs.**

This is the most ambitious version of the project. Not "a programming
language for sound." Not "a programming language for ML." A
programming language for *the analog computers we are starting to
build again*, with the existing aither and resonance work as proofs
that the substrate is real and useful even on conventional digital
hardware.

When the analog hardware is more widely available — five years out,
maybe ten — the language that natively expresses what those chips
do will be the language people reach for. That language doesn't
exist yet. The closest existing thing is what this project is
building.

**Aither was a synthesizer. Resonance is a neural network. The CFT
substrate that underlies them both is the foundation for the
analog-computer renaissance, and the manifesto's job is to make
that legible before the moment arrives.**

## What this manifesto changes

For the active projects:

- **aither** continues as the audio probe. It demonstrates that the
  CFT primitives work end-to-end in a live-coded environment with
  zero-allocation hard-real-time constraints.
- **resonance-ocaml** continues as the cognitive probe. It
  demonstrates that the same primitives produce competitive ML
  results when properly trained.
- **The Causal Oscillator LM submission** is the proof that the
  substrate works in a production benchmark with measurable
  numbers.
- **The proposed `pole(state, drive)` primitive** is the next
  test — if it works, it's the strongest evidence yet that one
  primitive can serve both probes through different state
  topologies.
- **A future shared substrate library** would unify the engineering
  without forcing the projects to merge organisationally. Both
  import from it; each surface stays distinct.

For people who haven't yet recognised they're doing CFT:

- Modal-synthesis audio researchers. (The DHO bank IS modal
  synthesis.)
- Oscillator-bank ML practitioners (S4, Mamba, the
  state-space-model community). (Their state-space models ARE
  damped resonators in disguise.)
- Neuromorphic-hardware software people. (Their chips ARE CFT
  substrates.)
- Geometric-algebra users. (Their formalism IS the rigid-ℂ
  conception generalised.)
- Cybernetic-synthesis musicians. (Their patches ARE field-coupled
  systems with regulators.)

All of these share more than they currently know. A document that
names the substrate and lists what depends on it is what makes the
recognition possible. This is the modest version of what this
manifesto is for.

The ambitious version — the analog-computer programming environment
for the next thirty years — is the long bet. It might not happen
this decade. It might never happen. But the substrate is real, the
hardware is starting to arrive, and the first project to name what's
underneath all of this gets to shape how the school of thought
develops.

That's the bet. That's the project. That's what aither and resonance
have been quietly building toward without saying so.

This document says it.

---

*Working draft, 2026-04-28. Names, framings, and emphases subject to
revision. The substrate is the durable claim; everything else is
prose around it.*
