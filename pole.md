# pole(state, drive) — a regime-switching resonator primitive

A design note for a proposed aither primitive. The idea comes from
the monopole/multipole framing in `bachPolyphase.md`: same physical
entity, two regimes, distinguished only by whether the underlying
state is isolated or shared.

## The contract

```
pole(state, drive, freq, damp) -> sample
```

A second-order resonator (DHO-shaped) with one twist: its behaviour
depends on whether `state` is **private** to this call site or
**shared** across multiple call sites.

- **Monopole regime** — `state` is unique to this call. The pole
  behaves as a damped resonator driven by `drive`. Energy injected
  by `drive` rings out and decays. This is `dho` as we have it
  today.

- **Multipole regime** — the same `state` slot is used by N pole
  call sites at compatible frequencies. The bank locks into a
  coupled system. Drive energy circulates among the participants;
  divergence at any one site approaches zero; the only loss is
  the bank's collective damping.

Same primitive, two regimes, distinguished only by the topology of
the state cell.

## Why this is new

aither today has:
- `dho(state, drive, freq, damp)` — always isolated. State is
  per-call-site.
- Manual feedback patterns where one voice writes to a state cell
  that another voice reads — but this is bus-style, with one
  sample of delay, and requires hand-coded coupling logic.

What's missing:
- A primitive where coupling is **structural** rather than
  hand-coded. You declare two `pole`s sharing the same state and
  the engine handles the bidirectional simultaneous update.
- A primitive that **changes its own behaviour** based on whether
  it has neighbours. Today every primitive has one fixed regime.

## What it would sound like

The interesting cases are the boundary behaviours.

### Single pole (monopole only)

Identical to `dho`. Pluck it with `drive = trig`, hear a damped
ring at `freq`. No surprise.

### Two poles sharing state at the same frequency

Two voices feeding into the same resonator field. Driving either
one excites the shared resonance; the energy sloshes between them
based on how they're coupled. If they're driven in phase, the
resonance compounds (constructive). If out of phase, it cancels.

This is acoustically what happens with two strings tuned to the
same pitch on a sympathetic resonator (sitar, hardanger fiddle).
Hard to fake convincingly with stock additive synthesis; trivial
with shared-state poles.

### N poles at related frequencies sharing state

A whole resonant chamber. Drive any one and the harmonically related
poles ring along; drive a non-related frequency and only that pole
responds. The field does the harmonic filtering for free.

This is the "Tesla resonant transmission" mode from
bachPolyphase.md — energy transfers only between systems at
matching frequency, because the field IS the coupling mechanism.

### Slowly retuning one pole through a bank

Sweep one pole's `freq` past the others. Each time it crosses a
neighbour's frequency, the two lock briefly, energy shuttles,
then they detune. This is mode-locking made audible — and it's
how real coupled oscillators behave (pendulum clocks on a shared
wall, neurons firing in sync, etc.).

Stock synths can imitate this with envelope tricks. Shared-state
poles produce it as emergent behaviour from the coupling itself.
That's the difference.

## The hard part — engine semantics

The contract requires **mutual read/write within a single sample
tick**. Today's `$state` semantics handle read-then-write within
one call site fine, but cross-voice ordering when N poles share
state needs to be specified.

Three options, in increasing order of cost and accuracy:

### 1. Two-pass — all read first, then all write

Within a sample, all poles sharing state read the field's current
value. Then all poles compute their contribution. Then all poles
write. Coupling is one sample delayed but order-independent.

**Pro:** simple to implement; deterministic; no convergence loop.
**Con:** the 1-sample delay is small at 44.1k but isn't zero.
Sharp transients may smear at high frequencies.

### 2. Sequential — declaration-order single pass

Each pole reads the field as updated by previous poles in the same
sample. Order-dependent results.

**Pro:** zero delay; cheap.
**Con:** the order in the source file changes the sound. That's a
weird semantic for a language that otherwise treats order
indifferently.

### 3. Iterative — fixed-point per sample

Iterate the coupled system within a sample until the field
converges (e.g. delta below threshold). Most physically accurate.

**Pro:** matches real coupled-oscillator math.
**Con:** unbounded cost per sample; may not converge for some
parameter combinations; CPU-heavy.

**Recommendation:** start with option 1 (two-pass). The 1-sample
delay is inaudible for the sustained-resonance use cases that make
this primitive interesting in the first place. Promote to option 3
only if a specific patch needs it.

## Syntax sketch

A few possible surface forms:

### A. Implicit shared state

```
$bank = field(8)          # 8-cell shared field
let v1 = pole($bank, drive1, 220, 0.001)
let v2 = pole($bank, drive2, 330, 0.001)
let v3 = pole($bank, drive3, 440, 0.001)
let mix = (v1 + v2 + v3) * 0.3
```

Anything taking the same `$bank` is in the same coupling group.
Engine routes the field internally.

### B. Explicit field with named slots

```
$bank = field()
let v1 = pole_at($bank, "string1", drive1, 220, 0.001)
let v2 = pole_at($bank, "string2", drive2, 330, 0.001)
```

More explicit; allows poles to selectively couple.

### C. Modal expansion

```
let modes = pole_bank($bank, drive, [220, 330, 440, 550], 0.001)
```

A whole bank from one call. Easier ergonomics for the common case
(modal resonator, sympathetic-string section).

**Recommendation:** A and C. A for the general case, C for the
common modal-resonator pattern. B is only useful if selective
coupling becomes important.

## Cheap experiment before committing

Build the two-pass version in user space using existing `$state`
cells:

```
$field = 0.0
$next_field = 0.0

def coupled_pole(drive, freq, damp):
  let f = $field
  let env = dho(drive + f * 0.3, freq, damp)
  $next_field = $next_field + env * 0.1
  env

# at the end of the sample, before mixdown:
$field = $next_field * 0.99
$next_field = 0.0
```

This isn't quite right (the timing is hand-managed), but it's close
enough to hear whether shared-field coupling produces the
sympathetic-resonance effect we hope for. If yes, promote to a
built-in with proper semantics. If no, drop the idea.

## What I'd actually do next

1. **First**, build the user-space version above and write a patch
   with three coupled poles tuned to a chord. Drive only one of
   them and listen for the other two ringing in sympathy. If that
   works, the primitive is justified.

2. **Second**, do the same with a modal bank tuned to non-harmonic
   frequencies (an inharmonic bell or a stretched-octave piano).
   Coupling between non-harmonic resonators is where real
   instruments get their character; if `pole` can produce that
   without explicit physical modelling, it's a major win.

3. **Third**, try the slowly-retuning-one-pole experiment.
   Mode-locking-as-audible-event is something no current synth does
   well. If aither can do it with two lines, the primitive earns
   its keep.

If all three work in user space, write the engine version. If only
some work, narrow the scope of the primitive accordingly.

## Negative finding from the user-space experiment (2026-04-28)

The user-space proof-of-concept lives in `patches/sympathetic_chord.
aither`. It implements exactly the option-1 two-pass design above:
three `dho` calls share a leaky-integrator scalar field through
top-level `$field` / `$next_field` cells with one-sample delay
between the read and the write. Run `./aither audit
patches/sympathetic_chord.aither 4.0` to reproduce.

**The result is negative.** The shared scalar field does not produce
audible sympathetic resonance.

### What the audit shows

With three poles tuned to A / C# / E (220, 277.18, 329.63 Hz), only
v1 driven by an `impulse(2)` strike, and field-to-pole `coupling`
swept from 0 up to the runaway threshold:

- `coupling = 0` (control) — single peak at exactly 220.0 Hz, FFT
  sidelobes only. v2 and v3 are silent.
- `coupling = 30000` (strong, just under runaway at ~4e4) — peak
  shifts to 218.0 Hz from coupling-induced detuning of v1, but
  there are still no peaks at 277 or 330 Hz. v2 and v3 are silent.
- `coupling = 5e6` — immediate runaway, NaN within ~0.7 s.

Subtracting the zero-coupling output from the strong-coupling output
(coupled − monopole, computed in the same patch by stripping the
field from one bank but keeping v1's strike identical) yields a
residual at -128 dB RMS. Whatever v2 and v3 are doing in response
to the field is below the audible floor by ~100 dB.

### Why it doesn't work

Two compounding obstacles, both rooted in `dho`'s force-input
semantics rather than in the two-pass design itself.

**1. The bootstrap problem.** v1 settles into pure 220 Hz ringing
within a few samples; thereafter the field carries only 220 Hz
energy. v2 (tuned to 277 Hz) is a sharp band-pass at 277 with
~50 dB rejection at 220. Without 277 Hz drive in the field, v2
stays silent. Nothing in a scalar field converts v1's 220 Hz
energy into a 277 Hz drive for v2.

**2. The unit problem.** `dho`'s `force` is in acceleration units;
the steady-state position-from-force gain at resonance is
`1 / (2 * damp * omega²)` ≈ 2.6e-4 for `damp = 0.001` and
`freq = 220`. To get v2 to ring at v1's amplitude purely from
field-coupling, the loop gain `coupling × fieldGain × position-
gain × fieldGain` has to approach 1 — that requires
`coupling × fieldGain ≈ 4e4`. At that scale same-frequency banks
mode-split (peak shifts off-centre and disperses) but off-frequency
sympathetic excitation still doesn't happen, and any further push
hits runaway.

The mode-splitting at unison frequencies IS a real coupled-oscillator
phenomenon — and arguably the only audible thing the design produces.
But it's not the sympathetic-resonance effect the patch is meant to
demonstrate, and it's already cheap in two lines without a new
primitive (two `dho` calls reading and writing the same `$bus` cell).

### What would actually work

Three options for someone picking this up later:

- **Broadband-resonance field — `option 2` from `bachPolyphase.md`.**
  The field is not a scalar leaky integrator but a multi-mode
  resonator (or a literal DHO bank tuned to relevant frequencies)
  that can carry energy at all the chord frequencies simultaneously.
  Then v2's 277 Hz force input is non-zero whenever any pole is
  ringing. This is structural redesign, not a tweak — the field
  becomes the central object and the poles become "taps" into it.

- **Drive all poles broadband.** Hit every pole with the same
  broadband impulse (verified to work in the experiment — the
  spectrum then shows a clean A / C# / E chord). But this isn't
  sympathetic resonance — it's three independent strikes plus a
  small detuning effect from the field. The "shared field" buys
  nothing musically.

- **Non-DHO state representation.** A pole carrying a different
  state shape — e.g., already in normalised position units, or
  a (cos, sin) pair like `phasor_pair` — sidesteps the unit
  problem because there's no force-to-position scaling. Whether
  such a pole still has the band-pass character that makes
  sympathetic resonance interesting in the first place is an
  open question.

### Recommendation

**Do not promote the option-1 shared-scalar-field design to a
primitive.** The audible behaviour it produces is
mode-splitting at unison frequencies — a niche effect achievable
in two lines with shared `$state` and not worth a primitive.
The interesting cases (sympathetic resonance, modal-bank
coupling, mode-locking-as-audible-event) all require a different
field topology than option 1 specifies.

If someone wants to revive this design, do the broadband-field
variant first as a user-space patch (a top-level `dho` driven by
the sum of the per-pole outputs, then read by each pole). That
costs two extra `dho` call sites and changes the field's
spectral character — measure whether it produces audible
sympathy before investing in a primitive.

The patch `sympathetic_chord.aither` is preserved as the
documented evidence.

## Negative finding from broadband-field experiment (2026-04-29)

Following the option-1 negative finding above, this session tested
the option-2 broadband-resonance-field topology proposed in
`bachPolyphase.md` and recommended at the end of the previous
session as the next experiment. Patch:
`patches/sympathetic_field.aither` (preserved as evidence). Run
`./aither audit patches/sympathetic_field.aither 4.0` to reproduce.

**The result is negative again.** The broadband field DOES carry
energy at all chord frequencies simultaneously — that's the design
goal, and it works — but the bootstrap problem isn't solved by it.

### What the audit shows

Three variants tested with three poles tuned A / C# / E
(220, 277.18, 329.63 Hz) and a 3-DHO bank tuned to the same
frequencies. Each pole has its own DHO state; each bank-mode has
its own DHO state. Coupling is the field-to-pole gain; bankDamp is
the bank's per-sample damping. All numbers below are from the
diagnostic difference signal `coupled - monopole` (subtracting an
isolated v1 hit by the same impulse, so the spectrum reads "what
the field contributed").

**Variant A** — broadband coupling: each pole reads the SUM of all
bank outputs. Impulse goes into v1. Coupling=1e9, bankDamp=0.0001:
- 220 Hz: 0.0 dB (residue from v1 mode-splitting against bankAout)
- 218 Hz: −1.6 dB (mode-split sideband)
- 222 Hz: −7.8 dB (mode-split sideband)
- 276 Hz: **−37.2 dB** (C# — well below the −30 dB audibility
  threshold the prior session set; spectral-coloration territory)
- 330 Hz: not in top 8 (E — below ~−50 dB)

The 220 Hz mode-splitting cluster grows much faster with coupling
than the off-frequency sympathetic peaks. Sweeping coupling from
1e5 to 3e10 shows the C# peak rising from below noise to ~−37 dB
at best, while same-frequency mode-splitting reaches 0 dB and
broadens dramatically. No NaN was hit even at coupling=3e10.

**Variant B** — tuned coupling: each pole reads ONLY its own freq's
bank output (v2 reads `$bankCsout` not the sum). Impulse → v1.
Coupling=1e8, bankDamp=0.0001:
- 220 Hz: 0.0 dB
- 222 Hz: −38.8 dB / 218 Hz: −38.8 dB (less mode-splitting than A,
  because v1 only feedbacks via bankAout, not the sum)
- 278 Hz: −54.1 dB (C# — visible)
- 330 Hz: −54.9 dB (E   — visible)

Variant B is the "cleaner" demonstration of the topology: all three
chord peaks appear in the top 8 at roughly equal levels, showing
the design is doing sympathy in shape — but at ~25 dB below
audibility. Pushing coupling higher produces the same mode-split
runaway as variant A.

**Variant C** — broadband seed: impulse goes into the BANK directly,
each pole reads its own freq mode (tuned coupling). Coupling=10000,
bankDamp=0.0001:
- 220 Hz: 0.0 dB
- 330 Hz: **−16.5 dB** (E — clearly audible)
- 277 Hz: **−20.5 dB** (C# — clearly audible)

This LOOKS like genuine sympathetic resonance — clean A C# E chord,
peaks well above audible. But setting `fieldGain = 0.0` (so the
poles do not contribute back into the bank) yields a
**bit-identical** spectrum. The chord comes entirely from the
impulse exciting all three bank modes through the bank's own
filter shape; pole-to-bank feedback contributes literally nothing.
This is exactly the prompt's "drive all poles broadband" trick:
the shared field buys nothing, just three independent strikes
through three independent filters.

### Why it doesn't work

The bootstrap problem returns in a new form. The scalar-field
experiment failed because the field was spectrally narrow — once
v1 settled, the field carried only 220 Hz and v2's 277 Hz band-pass
rejected it. The broadband field IS spectrally wide, so this part
is fixed. But there's still no mechanism to convert v1's narrow-band
220 Hz output into 277 Hz drive for v2.

The DHO bank's modes are independent — three parallel band-pass
filters with no cross-mode coupling. The bank's C# mode only sees
the (small) 277 Hz spectral content of `(v1+v2+v3)`, which is
dominated by v1's 220 Hz output. The 277 Hz content of v1 is
suppressed by ~17x relative to its 220 Hz peak (DHO transfer
function 1/((omega²−omega_f²)² + (2·damp·omega·omega_f)²) for
omega=2π·220, omega_f=2π·277, damp=0.001). v2 thus sees a 277 Hz
drive that's 25-30 dB weaker than v1's drive at 220 Hz, which
means even at unity loop gain at 277 Hz (coupling ≈ 1.8e8) the
sympathetic ring saturates ~30 dB below the same-frequency mode-
splitting at 220 Hz.

In short: replacing the scalar field with a multi-mode resonator
fixes the spectral-narrowness problem but exposes the **inter-mode
coupling problem**. Independent DHOs in the bank do not share
energy across modes. v1's 220 Hz energy stays in the bank's 220 Hz
mode; v2's 277 Hz mode never gets a strong drive.

### What would actually work

Of the three options the previous session listed, the remaining
candidate is now **non-DHO state representation**.

The fundamental issue with both option-1 and option-2 is that DHO
poles act as narrow-band filters when they contribute back to the
field — v1's contribution to the field is its band-passed output,
which is dominated by 220 Hz. To get cross-frequency coupling, the
pole's contribution to the field must NOT be band-passed.

Concrete shapes worth trying:
- A pole whose feedback to the field is the DRIVE signal it sees,
  not its band-passed output. v1's drive contains the impulse
  (broadband by construction), so it would seed all bank modes
  immediately at full impulse amplitude. Architecturally this means
  `pole` exposes two outputs: the audible band-passed signal, and a
  "field contribution" tap that's broadband.
- A pole with state in normalised (cos, sin) phasor pair form
  (`phasor_pair`-like). The pair carries phase information; the
  field contribution is the pair itself, not a position projection.
  Cross-mode coupling becomes a phase-rotation/projection question
  rather than a band-pass filter question.
- Bank with off-diagonal terms: instead of three independent DHOs
  in the bank, use a 3×3 coupled-resonator matrix where 220 Hz
  energy can leak into the 277 Hz mode through deliberate coupling
  in the field itself. This is structural redesign of the FIELD, not
  the poles, and arguably the most physically faithful to real
  acoustic instruments.

The first option is the cheapest to test next as a user-space
patch. The third is the most interesting if it works (it would
make the field itself the source of cross-frequency coupling
rather than relying on accidental spectral leakage).

### Recommendation

**Do not promote the broadband-field design to a primitive either.**
Both options from pole.md and bachPolyphase.md (option-1 scalar
field, option-2 multi-mode resonator field) fail the bootstrap
test. The interesting cases (sympathetic resonance from a single
strike, non-harmonic modal-bank coupling) remain inaccessible
through field-mediated coupling between standard DHOs.

If someone wants to revive this design, do the **drive-tap pole**
variant first as a user-space patch (the pole's contribution to
the field is its drive input rather than its position output). If
that produces audible sympathy at off-frequency modes, the
primitive design changes shape — it's no longer "pole(state, drive,
freq, damp) → sample" but "pole(...) → (sample, fieldContribution)".

The patches `sympathetic_chord.aither` (option-1) and
`sympathetic_field.aither` (option-2) are preserved as documented
evidence. Both audit results are reproducible.

## Synthesis (2026-04-28) — pole was the wrong abstraction; aether is the right one

After both negative findings landed, the honest read is that
*pole-as-primitive* doesn't earn its keep. The capability we
wanted (coupled resonators, sympathetic resonance, modal-bank
behaviour) is real and worth having. The proposal in this doc —
a regime-switching `pole(state, drive)` primitive whose behaviour
flips between monopole and multipole based on state topology — is
not the right shape for that capability.

### What the experiments actually revealed

Three different things had been bundled under the name "pole"
without us noticing:

1. **A more controllable resonator primitive.** Engineering — a
   better `dho`. Plausible standalone goal but not what the
   experiments tested.
2. **A coupled-oscillator system that produces sympathetic
   resonance.** The system property the experiments actually
   targeted. Failed under both proposed field topologies.
3. **The operationalisation of the Maxwell monopole/multipole
   philosophical insight.** A claim about a hidden mathematical
   unity. Beautiful framing; not, by itself, a justification for
   a primitive.

The experiments tested goal #2 while the design notes argued
goal #3. Both negative findings (scalar field's spectral
narrowness; DHO-bank field's inter-mode-coupling problem) are
diagnoses about *the medium between resonators*, not about the
resonators themselves. The actual physics of sympathetic
resonance — strings in a piano sharing air pressure and bridge
mechanics — requires **narrow-band resonators + a broadband
medium**. Both proposed field topologies built narrow-band media,
which is why both failed.

### The realisation

The thing we kept reaching for and missing has a name in the
classical-field-theory tradition: **the aether**. Not "the
substrate," not "a bus," not "a shared field that's also a
resonator" — the aether is the *medium that carries excitation
between resonators without itself being a resonator*. It
transmits broadband energy. It has no preferred frequency. It is
not a thing that rings; it is the thing that lets other things
ring at each other.

In Maxwell's framing it's the carrier of the EM field. In
acoustics it's the air pressure between strings. In Steinmetz's
circuits it's the conductor between coupled inductors. **In
aither it should be a state cell that voices write their drive
signals (broadband — impulses, noise bursts, transients) into,
and read back as ambient drive.** Not their outputs (band-passed
through their own resonance). Their drives.

### Why this isn't a third way to do resonance

The proliferation worry is real — if we add aether as a third
resonance primitive alongside isolated `dho` and the
proposed-then-retracted `pole`, the language gets harder to
explain, not easier. But the aether reframing dissolves the
proliferation problem rather than adding to it:

- A standalone `dho` is a resonator participating in an aether
  that no other resonator reads or writes. The aether is still
  there; it's just empty. The voice is alone in the room. This
  is the *empty-aether* configuration.
- The pole-style coupled bank we tried to build was a resonator
  participating in an aether that the bank itself defined the
  structure of. Wrong shape — the aether shouldn't have its own
  modes. Both option-1 and option-2 are *structured-aether*
  configurations, and both fail because the aether shouldn't be
  structured.
- The actual aether — broadband medium where voices read and
  write drive signals at all frequencies — is the *full-aether*
  configuration. This is where coupled-oscillator behaviour
  falls out naturally.

There's one substrate (aether-mediated coupling) with three
configurations (empty, structured, full). `dho` is the resonator
that lives in it. The configuration is what changes; the model
doesn't. That's *one way to do resonance*, with the apparent
"three ways" being three positions on a single spectrum.

### What this means for the proposal

`pole(state, drive)` as proposed in this doc is **withdrawn**.
The capability the proposal aimed at is achievable in current
aither using a documented pattern, not a new primitive:

```
$aether = 0.0           # the medium — broadband drive carrier

play voice1:
  let trig = midi_trig(60)
  # broadband strike: impulse + brief noise burst
  let strike = trig * driveAmp + noise() * pluck(trig, 30) * burstAmp
  $aether = $aether * 0.95 + strike * 0.1   # write drive into aether
  let v = dho($aether * coupling, 220.0, 0.001)
  ...

play voice2:
  let v = dho($aether * coupling, 277.18, 0.001)   # only reads
  ...
```

Voice1 writes a broadband drive into `$aether`. Voice2 reads the
aether and resonates at its own frequency, picking up the 277 Hz
spectral content of voice1's broadband strike. Cross-frequency
sympathetic resonance happens *because the aether carries
broadband drive*, not because voice1's filtered output ever
reached voice2.

This is testable in a 10-line patch with no engine work. The
next experiment is exactly that test.

### What earns its keep from this exploration

The two preserved patches and the diagnoses they produced are not
wasted work — they are *what taught us where the right
abstraction lives*. Specifically:

- The scalar-field failure taught us that a single-number field
  is spectrally narrow.
- The DHO-bank failure taught us that a structured (resonant)
  field doesn't share energy across modes.
- Both failures together pointed at *what the medium has to be*:
  broadband, drive-carrying, neutral. That is the aether.

Without those two negative findings we wouldn't have arrived at
the clean diagnosis. The doc is preserved for the same reason
the patches are preserved — *the path to the right answer is
itself the useful artefact*.

### What replaces this proposal

A new design note (probably `aether.md`, to be written after the
broadband-aether experiment validates or invalidates the
pattern) documenting:

- The aether as a named convention in aither (not a primitive).
- The discipline: write *drive signals* in, read *ambient drive*
  out, never write *resonator outputs* in.
- The empty / structured / full configurations as a unified
  framework for thinking about coupled resonance.
- The patches `sympathetic_chord.aither` and `sympathetic_field
  .aither` as documented evidence of what the aether *isn't*,
  and the next experiment's patch as documented evidence of what
  it *is*.

If the pattern works, it lands as a section in COMPOSING.md and
a short reference doc, not as engine work. If it doesn't, the
diagnosis sharpens further and the next iteration of pole-or-not
follows from there.

The principle the manifesto names — *aither should already be
complete; new primitives only land when they really earn their
keep* — applies fully here. Neither pole nor aether-as-primitive
earns it. The aether-as-convention does, because it captures the
substrate the language is named for in a way the language
already supports.

### The aether is dimensionless — and that's a feature, not a limitation

A subtle correction to an earlier draft of this synthesis: the
single `$aether` cell is not a *flat approximation* of a more
"correct" 3D field. It is **the right shape for a dimensionless
field** — which is what the project's intellectual lineage
(Steinmetz, Tesla, Faraday in the lines-of-force mode, Dollard)
explicitly commits to.

Dimensions are a man-made bookkeeping convention, useful for
making certain calculations tractable on paper but not
ontologically primary. Steinmetz's phasors don't have spatial
coordinates — they have **magnitude and phase**, and that's
sufficient to describe the entire AC-power system without any
appeal to (x, y, z). Tesla's longitudinal-wave framing was
explicitly incompatible with three-dimensional propagation;
"instantaneous action at a distance" is the natural behaviour of
a field that doesn't have spatial extent to propagate across.
Faraday's lines of force were patterns of magnitude-and-direction-
of-influence between charges, not vector fields evaluated at
coordinates. The textbook Cartesian / FDTD / manifold
representations came later and are *one bookkeeping convention
among several*.

So the natural extensions of the aether in aither are NOT
spatial. We're not heading toward a 1D / 2D / 3D grid version of
`$aether`. We're heading toward **richer dimensionless media**:

- **Complex-pair aether** (Steinmetz extension). The aether
  carries `(magnitude, phase)` — a complex pair — instead of a
  single real number. Voices write `(re, im)` drives; voices
  read `(re, im)` ambient drive. This is the same rigid-conception
  complex algebra the language already uses for `cmul`,
  `phasor_pair`, and `analytic` — but applied to the field
  substrate rather than to individual signals. Phase carries
  timing/coherence information a scalar can't; phase-locked
  voices can write coherently into the aether and produce
  effects scalar writes can't. Architecturally identical to the
  scalar version; ships when the scalar version validates.
- **Multiple coupled aethers** (Tesla "radio-through-aether
  like-sound-through-air" extension). One aether carrying
  acoustic excitation, a different aether carrying
  electromagnetic excitation, a third for capacitive coupling,
  etc. Each is dimensionless, each is global, but they're
  distinct media with distinct read/write semantics. Voices
  choose which aether(s) to participate in. Lets aither-the-
  language eventually encapsulate radio, biosignal, and other
  domains where the underlying math is the same but the
  excitation kind is different.
- **Aether with internal dynamics** (impedance extension). The
  aether isn't passive; it has its own characteristic energy-
  storage behaviour — what you write comes back to readers with
  the medium's own signature applied (a phase shift, a damping,
  a transformation). This is the dimensionless analogue of the
  wave equation: not "wave propagating through space" but
  "energy oscillating between magnitude and phase aspects of
  the same dimensionless field." Steinmetz's circuits had this;
  the aether could too.

These three extensions compose. The richest aether worth
considering carries complex pairs, exists as multiple distinct
media, and has its own internal dynamics. None of them adds
spatial dimensions. The aether stays everywhere-at-once.

This also clarifies what the third experiment is testing.
**Level 0 (single scalar `$aether`) is not a calibration probe
for higher-fidelity levels — it IS the correct minimal model of
a dimensionless field.** A clean positive result vindicates the
dimensionless-field framing as the right substrate for cross-
frequency coupling, which is a substantial vindication of the
classical-field-theory lineage the project draws from. The
extensions above (complex pair, multi-aether, internal dynamics)
are then the natural directions for ontological enrichment of
the same model — not corrections to a flat approximation but
**additional properties the medium can be given without ever
acquiring coordinates**.

The deepest alignment: the rigid-conception complex pair
the language uses everywhere (cmul, phasor_pair, analytic) IS
the Steinmetz magnitude-and-phase representation applied to
single signals. The aether-with-phase extension is *the same
idea applied to the medium itself*. The language already
commits to the representation; extending it to the field
substrate is structurally consistent rather than novel.

## Connection to other docs

- `bachPolyphase.md` — the monopole/multipole framing. `pole` is the
  primitive that makes the regime switch operational.
- `bachPolyphase.md` Tesla section — `pole` with a shared field IS
  the longitudinal-coupling substrate. The two design notes describe
  the same thing from different angles.
- `COMPOSING.md` — once `pole` works, the sympathetic-resonance
  technique and the modal-bank pattern belong in COMPOSING as
  patterns alongside the velocity-array crossfade.

## Open questions

- Does the field need damping of its own, or does the bank's
  collective damping suffice? Probably needs explicit damping, or
  the field will accumulate DC offsets.
- Should the field be a scalar or a complex pair? Pair would carry
  phase information and enable phase-coherent compounding (the third
  Tesla move from bachPolyphase.md). Probably pair.
- How does this interact with polyphony? Each MIDI voice already
  has its own state arena; a "shared field" needs to span voices,
  which crosses the arena boundary. Engine question.
- Is the right damping exponential per-sample, or should it be
  frequency-dependent (high-Q resonators decay slowly, low-Q
  decay fast)? The latter is more physical but harder to control.
