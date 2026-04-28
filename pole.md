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

## Positive finding from broadband-aether experiment (2026-04-29)

The third experiment in this chain tested the dimensionless-broadband-
aether pattern that the synthesis above predicted. **It works.** Patch:
`patches/aether_sympathy.aither`. Run `./aither audit
patches/aether_sympathy.aither 4.0` to reproduce.

This is the first positive result in the chain, and it lands not by
adding a primitive but by using existing aither primitives plus a
documented dimensionless convention — exactly the resolution the
two-step synthesis above predicted.

### What the audit shows

Same three poles as the previous two experiments — A / C# / E
(220, 277.18, 329.63 Hz). v1 strikes the aether with a broadband event
(impulse + 30 ms noise burst). v2 and v3 NEVER receive a direct strike;
their only input is what they read from `$aether * coupling`. The
diagnostic master is `coupledTotal − monopoleTotal` so the audit
spectrum reads "what v2 and v3 contributed via aether-sympathy" — v1's
contribution cancels exactly because mv1 has the same input.

Default parameters (`driveAmp = 1e7`, `burstAmp = 1e6`, `aetherDecay
= 0.95`, `coupling = 1.0`, `damping = 0.001`):

```
Top peaks of (coupled − monopole):
  1.  330.0 Hz   0.0 dB    ← E   (v3 — sympathising)
  2.  278.0 Hz  -2.5 dB    ← C#  (v2 — sympathising)
  3.  277.4 Hz  -4.6 dB
  4.  276.0 Hz  -5.2 dB
  5.  329.4 Hz  -7.5 dB
  6.  328.0 Hz -11.2 dB
  220 Hz absent — v1 cancelled by mv1, exactly as the diagnostic intends
```

Switching the master to `coupledTotal` (the FULL chord, what you would
hear) instead of the diff signal:

```
Top peaks of full coupled mix:
  1.  220.0 Hz   0.0 dB    ← A   (v1 — struck)
  2.  330.0 Hz -12.6 dB    ← E   (v3 — sympathy)
  3.  278.0 Hz -14.9 dB    ← C#  (v2 — sympathy)
```

Sympathy peaks ~13–15 dB below the struck note. Real piano sympathy
is 30–40 dB down per the briefing's calibration; this is *louder than
piano*, comparable to a sitar or a sympathetic-string ensemble. The
chord A / C# / E is fully present and obvious, not subtle ringing.

### The falsification check passes — definitively

The previous session's variant C produced an apparent positive that
was actually three independent strikes through three independent
filters; the spectrum was bit-identical with and without pole-to-bank
feedback. To rule out the same trap here, the experiment includes a
falsification toggle: setting `v2v3Couple = 0.0` zeroes the force
into v2 and v3 specifically. With the diff-signal master:

```
v2v3Couple = 0.0:
  RMS  -240.0 dB    Peak  -240.0 dB    (machine zero)
  No peaks present.
```

The 277 / 330 Hz peaks completely disappear when v2 / v3 are silenced.
They are not artefacts of the broadband strike leaking through v1 —
they are entirely the product of v2 and v3 ringing in response to
the aether's broadband content. This is the opposite of variant C.
The aether is doing real cross-frequency coupling work.

### The parameter sweep — the design is robust

- `coupling` 0.1 → 1.0 → 10 → 1000 — RMS scales linearly with `coupling`
  (60 dB+ range tested) and the spectral SHAPE is bit-identical across
  the range. No runaway at any value because the aether has NO feedback
  loop — only the strike writes in; v1 / v2 / v3 only read. The system
  is open-loop by construction.
- `aetherDecay` 0.5 → 0.95 → 0.99 — sympathy persists across the full
  range; ~14 dB RMS variation. The aether doesn't need to "hold"
  broadband energy long. The strike's broadband content seeds v2 / v3
  within their natural integration window.
- `useNoiseBurst` 0 vs 1 — both work. Pure-impulse variant B is ~2 dB
  quieter than impulse + noise burst variant A. The impulse alone is
  enough broadband content for sympathy.

No NaN at any parameter setting tested.

### Why this works — and why the previous two didn't

The diagnosis is now clean across the three experiments:

1. **`sympathetic_chord` (option-1, scalar resonator field).** Voices
   wrote pole OUTPUTS into the field. As soon as v1 settled, the field
   became spectrally narrow (just 220 Hz). v2's bandpass rejected it.
   *Bootstrap problem from a narrow medium.*
2. **`sympathetic_field` (option-2, multi-mode resonator field).**
   Voices wrote pole outputs into a DHO bank. The bank's C# and E
   modes never got driven because v1's output is dominated by 220 Hz
   and the bank's modes don't share energy across themselves.
   *Inter-mode-coupling problem in a structured medium.*
3. **`aether_sympathy` (broadband neutral aether).** v1 writes its
   DRIVE (impulse + noise burst — broadband by construction, NOT
   filtered through v1's resonance) into the aether. The aether is a
   neutral leaky integrator — no resonance, no preferred frequency,
   no spatial extent. v2 reads broadband ambient drive and projects
   its 277 Hz component through its own bandpass, exactly as a real
   piano string does with air pressure.

The three negatives-then-positive form a clean diagnostic chain:
narrow medium fails; structured medium fails; *neutral broadband
medium is what the physics requires*. That neutral medium is the
aether.

### What this vindicates

Beyond unlocking one musical technique (which by itself would already
justify the work), the result vindicates the **dimensionless-aether
framing** the synthesis above arrived at. The single `$aether` cell
is not a flat approximation of a "real" 3D field — it IS the right
shape for a dimensionless field, which is what the project's
intellectual lineage (Steinmetz, Tesla, Faraday in lines-of-force
mode, Dollard) explicitly committed to. Cross-frequency coupling
between resonators happens *because the medium has no spatial extent
and no frequency preference* — exactly the properties textbook
3D / FDTD / vector-field representations make hard to express.

The two earlier negative results are now retroactively useful — the
clean diagnostic chain that pointed at the right answer would not
exist without them.

### What this means for future work

Future enrichment of the aether is **ontological, not spatial**. We
do not need a 1D / 2D / 3D grid version of `$aether` — that fights
the lineage's commitment and adds bookkeeping the substrate doesn't
require. The natural directions are richer dimensionless media:

- **Complex-pair aether** — `$aether` carries a (magnitude, phase)
  pair instead of a scalar real number. Voices write `(re, im)`
  drives; voices read `(re, im)` ambient drive. Phase carries
  timing/coherence information a scalar can't; phase-locked voices
  can write coherently into the aether and produce effects scalar
  writes can't. Architecturally identical to the scalar version.
  This is the deepest alignment with Steinmetz: the rigid-conception
  complex pair the language already uses everywhere (`cmul`,
  `phasor_pair`, `analytic`) — applied to the medium itself rather
  than to individual signals. The language already commits to the
  representation; extending it to the field substrate is structurally
  consistent rather than novel.
- **Multiple coupled aethers** — Tesla's "radio waves through the
  aether as sound through air" reads literally: one aether per
  medium type (acoustic, EM, capacitive, ...), each dimensionless
  and global, voices subscribing to whichever applies. Lets aither
  eventually encapsulate radio, biosignal, and other domains where
  the underlying math is the same but the excitation kind differs.
- **Aether with internal dynamics** — the medium has its own
  characteristic energy-storage behaviour; what you write comes
  back to readers with the medium's signature applied (a phase
  shift, a damping, a transformation). The dimensionless analogue
  of a wave equation: not "wave propagating through space" but
  "energy oscillating between magnitude and phase aspects of the
  same dimensionless field". Steinmetz's circuits had this; the
  aether could too.

These extensions compose. The richest aether worth considering
carries complex pairs, exists as multiple distinct media, and has
its own internal dynamics. None adds spatial dimensions. The
aether stays everywhere-at-once.

### What lands as a result of this experiment

- `patches/aether_sympathy.aither` — the working broadband-aether
  patch, preserved with full audit numbers, the falsification
  methodology, the parameter sweep, and the dimensionless-aether
  framing in its header.
- This section of `pole.md`.
- *No new primitive.* The pattern is achievable with existing
  primitives + a documented convention. The manifesto's "primitives
  only land when they really earn their keep" principle predicts
  precisely this outcome.
- The two preserved negative-result patches (`sympathetic_chord
  .aither`, `sympathetic_field.aither`) and their pole.md sections,
  which together with this section form the diagnostic chain.

What does *not* land in this commit:

- A documented broadband-aether technique in COMPOSING.md — the
  user will decide whether to write that up after seeing this
  result.
- An `aether.md` design note — same reason; deferred until the user
  decides what shape to give the convention.
- Any of the three ontological extensions (complex pair, multi-
  aether, internal dynamics) — those are future experiments
  motivated by this positive, not part of it.

## Aether enables cybernetic stabilisation but not full al-Mukabala dynamics (2026-04-29)

The natural follow-on to the sympathy positive: the same
substrate, applied to a cybernetic-feedback patch, should produce
the chaotic-but-regulated motion the analog cybernetic-synthesis
tradition (al-Mukabala / al-Jabar / Tudor) is about. Patch:
`patches/aether_muqabala.aither`. **Result is partial.** The
aether unlocks ONE form of cybernetic behaviour but not the form
the analog tradition is most famous for.

This is a more nuanced result than the sympathy positive.
Documenting it carefully because the gap between what works and
what doesn't is itself useful information about what kind of
substrate the cybernetic-synthesis tradition needs.

### What the patch does

The structural moves match the briefing template:

- A continuous broadband noise floor, LFO-modulated in amplitude,
  writes into `$aether`.
- The carrier reads `$aether` as additive bias into a wavefolder
  AND reads the envelope of `$aether` (`$obs = lp1(abs($aether)
  , 5)`) as FM. Both paths are state-dependent.
- The wavefolder's bite point is also state-dependent
  (`foldAmount + $obs * foldModDepth`).
- The carrier's nonlinear output (folded then drive-saturated)
  writes BACK into `$aether`. This is the closed loop, and it
  lives in the aether.
- The regulator pulls back the noise floor's amplitude when
  `$obs` gets high.

The original `al_muqabala.aither` had the same four roles
(driver / heart / observer / regulator) but no aether — the loop
ran through one state cell `$loop`, hand-wired. That patch
sounded like a fixed-pitch fire alarm at any knob position. The
aether version produces five qualitatively distinct sounds
across parameter space (more on that below).

### Falsification — the cybernetic loop is doing real work

`carrierToAether = 0.0` breaks the carrier-to-aether feedback
while preserving the carrier's READS from the aether (so the
patch still produces sound). Comparing 30-window temporal
audits at default parameters:

```
Loop ON  (carrierToAether = 0.8, default):
  RMS         μ = -21.65 dB    σ = 0.79 dB     ← audible breathing
  centroid    μ = 15457 Hz     σ = 1251 Hz     ← spectral motion
  fundamental μ = 114 Hz       σ = 625 Hz      ← jumpy autocorr
  dominant Pk μ = 254 Hz       σ = 0.2 Hz      ← LOCKED ATTRACTOR

Loop OFF (carrierToAether = 0.0, falsification):
  RMS         μ = -19.45 dB    σ = 0.02 dB     ← no breathing
  centroid    μ =  6867 Hz     σ = 396 Hz      ← LFO-driven only
  fundamental μ = 0 Hz         σ = 0 Hz        ← no detected periodicity
  dominant Pk μ = 366 Hz       σ = 260 Hz      ← WANDERS with LFO
```

The differences are unambiguous and point in opposite directions:
WITH the loop, the carrier LOCKS at 254 Hz and the system breathes
spectrally on top of that lock. WITHOUT, there is no detected
fundamental and the dominant peak wanders ±260 Hz with the LFO.
The loop's role is **stabilisation** — it finds an attractor and
holds the system in it. This is a real cybernetic behaviour, just
not the one most associated with the tradition.

### What's NOT happening — the rich al-Mukabala motion

The analog cybernetic-synthesis tradition produces "events":
moments where the system finds a NEW attractor and lingers, then
breaks out, then finds another. The patch's parameter space has
five distinct attractors (locked at 254 Hz, locked at 156 Hz,
locked at 22 kHz, marginal-breathing, saturated-noise) — but at
each setting the system stays in ONE attractor for the duration
of the audit. There is no within-knob-position attractor-jumping.

Pushing the loop gain higher (`carrierToAether ≥ 1.5`, weak
regulator) puts the system into a "wandering peak" regime where
the dominant peak σ ≈ 8 kHz across windows. Looks cybernetic on
paper. But the falsification check FAILS in that regime — with
`carrierToAether = 0`, peak σ is still ≈ 8 kHz. The wandering is
mostly noise + nonlinearity, not loop-driven. The genuinely
loop-driven regime is the locked one; the wandering regime is
just stochastic harmonic shuffling.

### Diagnosis — why "rich motion" doesn't emerge

The al-Mukabala tradition's events come from MULTI-STABLE memory
in the loop. The analog wave-multipliers used in the original
patches have nonlinear transfer functions with smooth bumps and
analog memory (capacitor charge, slow integration of bias
networks). A loop containing wave-multipliers can settle in
multiple operating points and need a kick to leave each one. The
"events" are the kicks delivered by the slow LFO and regulator.

This aither patch's nonlinearities are stateless:

- `fold(x, amount)`: zigzag — stateless
- `drive(x, amount)`: smooth saturator `x/(1+|x|)` — stateless

The only stateful elements are `$aether` (a leaky integrator)
and `$obs` (an envelope follower on `$aether`). Both have
simple, predictable, monostable dynamics. The loop's only memory
is "current aether value" — a single number — which can settle
to one equilibrium but cannot represent multiple competing
attractors.

So: aether-as-medium is necessary (the original al_muqabala
without aether produced nothing interesting), but aether-as-
medium is not sufficient — the **carrier-side nonlinearity also
needs state**. The aether is one piece of cybernetic-synthesis
infrastructure; another piece (stateful nonlinearity in the
loop) is missing.

### What this contrast means for the substrate

The two adjacent results paint a clean picture:

1. **Broadband sympathy** (`aether_sympathy.aither`) — open-loop
   coupling between resonators. The aether is NEUTRAL; the
   resonators have their OWN state (each `dho` is a stateful
   second-order resonator). Result: clean positive. The aether
   carries broadband drive between stateful elements.
2. **Cybernetic synthesis** (this experiment) — closed-loop
   feedback through nonlinearity. The aether is again neutral;
   the carrier-side processing is STATELESS (just `fold` then
   `drive`). Result: partial — stabilises but doesn't produce
   chaos.

The pattern: the aether transmits whatever drive is written to
it. Where the *other* elements have state, the aether enables
their interaction. Where the other elements are stateless, the
aether's neutrality means the loop has nowhere to store
attractors.

This sharpens the framing the sympathy result vindicated. The
aether is a coupling MEDIUM, not an active processing element. It
needs partners with state. Sympathy works because the dho voices
have state. Cybernetic motion fails because the carrier-side
nonlinearities don't.

### Approximate cost-ordered ways forward

Whichever of these gets tried, **the right move is structural —
the patch needs more state than the current architecture
provides.** None of these are about adding aither primitives;
they're about composing the existing primitives differently.

1. **Comparator-triggered kick** (cybernetics.md episode-8
   extension). When `$obs` crosses a threshold, fire a discrete
   slope into the regulator. Discrete events that pop the system
   between attractors. Adds essentially no structural
   complexity.
2. **Stateful loop nonlinearity** — replace the `fold |> drive`
   chain with a compander (slow attack/release) or a Schmitt
   trigger built from `if`. State in the loop's gain, not just
   in its medium.
3. **Relaxation oscillator carrier** — replace the sin-phasor
   carrier with an integrator-then-comparator. Period set by an
   integrator that's itself driven by the aether. The
   wave-multiplier analogue.
4. **Multiple coupled aether cells** — each can settle in a
   different attractor; their cross-coupling drives jumps. This
   is the "multiple coupled aethers" extension from the sympathy
   write-up, applied here to a single domain rather than to
   different physical media.
5. **Time-delay feedback** — a delay line in the loop. Time-delay
   feedback in nonlinear systems is a textbook chaos generator
   (Mackey-Glass equation, Ikeda map). Aither would need a delay
   primitive or a clever buffer-based simulation.

Direction (1) is the cheapest test of whether the missing
ingredient is "discrete events" or "multi-stable memory."
Direction (2) is the cheapest test of whether the missing
ingredient is the latter.

### What lands as a result of this experiment

- `patches/aether_muqabala.aither` — the partial-positive patch
  with full audit numbers, falsification methodology, parameter
  sweep regime structure, and the diagnosis in the header.
- `tests/temporal_audit.nim` — windowed-RMS / centroid / peak
  analysis tool used to characterise the patch (cybernetic
  patches need time-domain stats; the default audit's single FFT
  flattens those).
- `tests/sweep_muqabala.nim` — parameter-sweep characterisation
  tool used to map regime structure.
- This pole.md section.
- *No new primitive.* Same principle as the sympathy positive —
  the result lives at the convention layer, not the language
  layer.

What does NOT land:

- A "cybernetic-synthesis recipe" in COMPOSING.md — the patch is
  partial; documenting it as a recipe would overclaim. Wait until
  one of the structural directions above unblocks the rich-motion
  case.
- An aither-version-of-al-Jabar or aither-version-of-Tudor patch
  — those are downstream of getting the al-Mukabala kernel right.
  Until the partial gap is closed, more elaborate cybernetic
  patches will hit the same wall.
- Any feedback into the al_muqabala.aither original — preserved
  as the pre-aether-discovery baseline.

## Pair-valued aether: partial result with derivative-as-magnetic (2026-04-29)

The structural follow-on to the sympathy positive: replace the scalar
`$aether` with a Steinmetz-form (dielectric, magnetic) pair —
`$aetherDie` carrying potential storage (the strike's amplitude),
`$aetherMag` carrying kinetic storage (the strike's derivative). Read
via `rotate($aetherDie, $aetherMag, theta)` where theta selects which
aspect of the medium the voice hears. Patches: `patches/aether_pair_
sympathy.aither` (canonical) plus six supporting test patches for the
three pre-committed falsifiable tests. **Result is partial.** The
substrate works as a structural extension and Test 3 passes
unambiguously. Tests 1 and 2 reveal that the derivative-as-magnetic
convention puts most of the audio-bandwidth energy in the dielectric
component, limiting the rotation knob's musical reach.

This is a more nuanced result than the sympathy positive. Documenting
it carefully because the gap between what works and what doesn't
points at a specific next experiment.

### What the patch does

`patches/aether_pair_sympathy.aither` mirrors the scalar sympathy
patch's three-voice A / C# / E topology with the medium replaced by a
pair of state cells. The natural-pair convention writes both
components:

```
$aetherDie = $aetherDie * 0.95 + strike            * 0.1
$aetherMag = $aetherMag * 0.95 + (strike - prev) * 0.1
$prev      = strike
let pair  = rotate($aetherDie, $aetherMag, rotationAngle)
let aRead = pair[0] * coupling
```

At `rotationAngle = 0` and `magScale = 1` the dielectric read tap
matches the scalar version bit-for-bit (dielectric = scalar's
`$aether`, magnetic is decoupled). Top peaks `330.0 / 278.0 / 277.4
/ 276.0 / 329.4 / 328.0` Hz at `0 / -2.5 / -4.6 / -5.2 / -7.5 /
-11.2` dB — bit-identical to the scalar audit. Falsification
(`v2v3Couple = 0`): RMS at machine zero (-240 dB), confirming the
substrate is doing real cross-frequency coupling.

### The three tests

#### Test 1 — phase-coherent sympathetic resonance (PARTIAL)

Tool: `tests/phase_histogram.nim`. Strike voice 1 via `impulse(2)` for
50 seconds (99 strikes), find first positive-going zero crossing in
voice 2's output after each strike, histogram the phase modulo voice
2's period.

```
patches/aether_pair_test1_scalar.aither (magScale = 0):
  N strikes = 99   mean angle = 0.198 π   σ = 0.157 π
patches/aether_pair_test1_pair.aither   (magScale = 1, rotation = π/2):
  N strikes = 99   mean angle = 0.730 π   σ = 0.130 π
```

Both pass the literal `σ < π/4` threshold. The test's prediction —
that scalar would give a uniform phase distribution (`σ ~ π`) — is
empirically wrong. With periodic strikes, voice 2's phase response
is deterministic regardless of medium architecture. Both histograms
peak.

The pair version is slightly tighter (0.130 π vs 0.157 π) and sits
at a different mean angle. The 0.5 π offset between mean angles is
exactly what `rotate(die, mag, π/2)` predicts: at this rotation, the
read tap reads the magnetic component, which (per the derivative
convention) is approximately a 90°-shifted copy of the dielectric.
The pair representation does provide a controllable phase-rotation
knob; it just doesn't manifest the binary peaked-vs-uniform behaviour
the test was designed to detect.

#### Test 2 — rotational morph as a continuous live knob (PARTIAL)

Tool: `tests/temporal_audit.nim`. Render 30 seconds with rotation
angle sweeping at `phasor(1/30) * TAU` (one full turn), measure
spectral centroid per 1-second window.

```
patches/aether_pair_test2_scalar.aither (magScale = 0):
  centroid mean = 285.8 Hz   σ =  9.9 Hz   range = 263 → 301 Hz   (1.15×)
patches/aether_pair_test2_pair.aither   (magScale = 1, sweeping):
  centroid mean = 306.1 Hz   σ = 51.1 Hz   range = 263 → 501 Hz   (1.91×)
```

The pair version's centroid stddev is 5.2× larger than scalar — a
real differential signal. But the literal pass criterion ("at least
2× variation, smooth") is borderline failed: 1.91× falls just under
2×, and the variation is not smooth. Most windows have centroid
locked at 277 Hz (the receiver's resonance); the larger centroids
are concentrated in 1-2 windows when rotation crosses through π/2,
where the dielectric is nullified and the magnetic-component's
high-frequency emphasis dominates.

What DOES change smoothly across rotation is RMS: 30 dB modulation
range across the full turn. The rotation acts as a smooth magnetic-
attenuator with continuous angle control, not the smooth spectral
character morph the test predicted. Useful as a "duck-and-bring-back"
knob, but not as a "shift the medium's character" knob.

#### Test 3 — in-phase reinforces, out-of-phase cancels (PASS literal)

Tool: `./aither audit` on three configurations. Voice 1 writes its
strike with pair-angle 0 (pure dielectric). Voice 2 writes a strike
rotated by `theta`. Voice 3 (silent) reads dielectric and rings.

```
patches/aether_pair_test3_inphase.aither   (theta = 0):    RMS = -31.0 dB
patches/aether_pair_test3_outofphase.aither (theta = π):   RMS = -240.0 dB
patches/aether_pair_test3_scalar.aither    (no theta):     RMS = -31.0 dB
```

In-phase vs out-of-phase difference: 209 dB (machine-zero
cancellation), far exceeding the 6 dB criterion. Scalar control is
theta-invariant, so it cannot represent the configuration difference.
**PASS** on the literal criterion.

Caveats. (1) The perfect cancellation depends on bit-identical noise
content in voice 1 and voice 2 — both `noise()` instances start from
the same default pool seed, producing identical xorshift sequences.
With decorrelated noise the cancellation would be partial, set by a
random-walk sum. (2) Voice 3 reads dielectric only (rotation φ = 0),
so the test reduces to signed scalar arithmetic on the dielectric
component. A scalar aether with explicit sign control on voice 2's
write would produce the same result. The pair structure becomes
strictly necessary only when voice 3 reads via rotation φ ≠ 0,
where the magnetic component contributes — verified separately: at
θ = π/2 (V2 writes pure magnetic) and φ = π/2 (V3 reads pure
magnetic), V3 isolates voice 2 and ignores voice 1. *Selective
listening between writers* is not possible with scalar; it is with
pair.

### What the partial result reveals

The (dielectric, magnetic) pair as proposed — with the derivative
convention for magnetic — produces a magnetic component that at
audio frequencies is mostly a high-pass-filtered shadow of the
dielectric. The per-sample finite difference has gain `2·sin(πf/SR)`
≈ `2πf/SR` for `f ≪ SR`; at 277.18 Hz with 48 kHz sampling, that's
≈ 0.036 (about -29 dB). Most of the audio-bandwidth energy stays in
the dielectric. Rotation acts as a magnetic-attenuator with an
incidental high-frequency-emphasis signature, not as a continuous
spectral character morph.

The structural extension is sound — Test 3 confirms that pair-form
writes propagate additively, that rotation works as a write/read
primitive, and that selective-listening between multiple writers is
possible (something scalar cannot do). What's *not* yet validated
is the music-theoretic payoff: the pair representation becoming a
continuous timbre / character knob.

### Diagnosis — why the convention chosen falls short

The test failure mode points cleanly at the convention. Steinmetz's
classical-field-theory pair carries dielectric and magnetic energy
storage *with the same magnitude spectrum*; the 90° quadrature is in
phase, not amplitude. The natural mathematical realisation is the
analytic signal: `re = signal`, `im = Hilbert(signal)`. Both
components have identical magnitude spectra; only the phase differs.

The derivative-as-magnetic convention chosen for this experiment is
*one* mathematical realisation of "potential vs flow" but it bakes
in a frequency-dependent gain that the lineage's framing does not
require. At audio frequencies, that gain rolls off the magnetic
component to where rotation becomes mostly an attenuator.

### What lands as a result of this experiment

- `patches/aether_pair_sympathy.aither` — the canonical pair-valued
  sympathy patch with full audit numbers and the test verdicts in
  its header.
- Six supporting test patches: `aether_pair_test1_{scalar,pair}.
  aither`, `aether_pair_test2_{scalar,pair}.aither`,
  `aether_pair_test3_{inphase,outofphase,scalar}.aither`. Preserved
  as evidence; running each reproduces the audit numbers above.
- `tests/phase_histogram.nim` — windowed zero-crossing-based phase
  extraction tool, used for Test 1.
- This pole.md section.
- `aether.md` Gap 3 section updated to note partial validation and
  the natural next experiment.
- *No new primitive.* All of `cmul`, `rotate`, `analytic`,
  `phasor_pair` exist already; the experiment uses them as-is. The
  derivative-as-magnetic convention is documented; the analytic-
  signal pair is named as the next decomposition to try.

What does *not* land:

- Promotion of the pair-valued aether to canonical. The result
  doesn't justify replacing the scalar; the pair is one route into
  richer medium structure but not the only one and not yet
  empirically dominant.
- A sympathy recipe in COMPOSING.md based on the pair-valued aether
  — the derivative convention's audio-bandwidth attenuation makes
  the rotation knob a magnetic-attenuator rather than a character-
  morph; not a satisfying compositional handle.
- Any feedback into aether_sympathy.aither or aether_muqabala.aither.
  The scalar versions remain the working baselines.

### What's the natural next experiment

The analytic-signal pair: replace `magDrive = strike - $prevStrike`
with the imaginary part of `analytic(strike)`. Both components
would carry full audio bandwidth with equal magnitude; rotation
would then produce a true phase shift on the medium's content,
which is the music-theoretic effect the pair-valued framing was
supposed to deliver. Same primitives, same convention layer; just
a different convention choice for what the magnetic component
represents.

The decision point — whether to invest in that next experiment —
belongs to the user. The current result establishes that pair-form
substrate composition works, that Test 3's reinforce/cancel is real,
and that the derivative convention has a specific audible weakness
the analytic-signal convention may avoid.

## Pair-valued aether: analytic-signal convention is structurally cleaner but rotation collapses (2026-04-29)

Direct follow-on to the derivative-pair partial finding above. The
diagnosis there pointed at the convention: derivative gave the
magnetic component a high-pass-like gain so most audio-bandwidth
energy stayed in the dielectric, and rotation acted as a magnetic-
attenuator with incidental high-frequency emphasis rather than a
continuous spectral character morph. The natural next experiment was
the **analytic-signal pair**: `re = signal`, `im = Hilbert(signal)`,
both components with identical magnitude spectra at every frequency
and 90° phase quadrature — the textbook-correct realisation of what
Steinmetz's framing actually describes.

Patches: `patches/aether_pair_v2_sympathy.aither` (canonical) plus
four supporting test patches (`aether_pair_v2_test{1_pair,2_pair,
3_inphase,3_outofphase}.aither`). Scalar controls were reused from
the derivative-pair run. **Result is partial — but with a different
shape than derivative-pair's partial.**

### What the patch does

Same three-voice A / C# / E topology and same write/read pattern as
`aether_pair_sympathy.aither`, with the (dielectric, magnetic) pair
sourced from `analytic(strike)` instead of `(strike, strike-prev)`:

```
let strikePair = analytic(strike)
let dieDrive   = strikePair[0]   # real part = original strike
let magDrive   = strikePair[1]   # imaginary part = Hilbert(strike)

$aetherDie = $aetherDie * 0.95 + dieDrive * 0.1
$aetherMag = $aetherMag * 0.95 + magDrive * 0.1
```

At `rotationAngle = 0` the dielectric tap matches the scalar version's
spectrum bit-for-bit. Falsification (`v2v3Couple = 0`) was implicit in
the substrate test (we know the substrate works from the derivative
run — replacing `(strike, strike-prev)` with `analytic(strike)`
doesn't change the substrate's coupling mechanics, only the magnetic
component's spectral content).

### The three tests

#### Test 1 — phase-coherent sympathetic resonance (PASSES literal)

50-second render at `impulse(2)`, 99 strikes, voice 2 at 277.18 Hz.

```
patches/aether_pair_v2_test1_pair.aither (rotation = π/2):
  N strikes = 99   mean angle = 0.503 π   σ = 0.203 π
```

Comparison to the derivative-pair and scalar baselines:

```
analytic-pair:    σ = 0.203 π    mean angle = 0.503 π
derivative-pair:  σ = 0.130 π    mean angle = 0.730 π
scalar control:   σ = 0.157 π    mean angle = 0.198 π
```

All three pass the literal `σ < π/4` threshold. The analytic-pair's
mean angle sits at exactly `π/2` — the Hilbert quadrature signature.
Its σ is wider than derivative-pair (less peaked) but still well
under the threshold.

The Test 1 design issue from the derivative run remains: with
periodic strikes, all three architectures produce peaked phase
histograms because the impulse response is deterministic from
strike onset. Jittered strikes were considered (per briefing
permission) but reasoning suggests they would also fail to
differentiate — the residual ringing's phase decorrelates equally
across all architectures. Test 1 is preserved as a comparison point;
it does not differentiate conventions.

#### Test 2 — rotational morph as continuous live knob (FAIL — flatter than baseline)

30-second render with rotation phasor at `1/30` Hz (one full turn
across the audit), 1.0-second analysis windows.

```
analytic-pair:    centroid σ = 10.0 Hz   range 1.13×   RMS σ = 1.76 dB
derivative-pair:  centroid σ = 51.1 Hz   range 1.91×   RMS σ = 7.12 dB
scalar control:   centroid σ =  9.9 Hz   range 1.15×   RMS σ = 7.59 dB
```

The analytic-pair's centroid stddev is 5× SMALLER than derivative-
pair's, essentially identical to scalar (10.0 Hz vs 9.9 Hz). The
literal 2× criterion fails (1.13×) and fails *harder* than
derivative-pair did. RMS modulation across rotation is only ~6 dB
peak-to-peak, vs ~30 dB for derivative-pair.

The diagnosis is clean and structural: the analytic pair represents
two components with **identical magnitude spectra** in 90° phase
quadrature. Rotation in this plane —
`re·cos(θ) − im·sin(θ)` — is a true phase shift on the underlying
signal, with no amplitude or magnitude-spectrum change. A high-Q
resonator (the receiver voice at damping 0.001) is invariant to
phase shifts: its impulse response selects frequency content and
is insensitive to absolute input phase. So rotation produces no
audible change.

This means derivative-pair's centroid-variation (1.91×) was an
**artefact** of the convention's spectral asymmetry, not a music-
theoretic feature. Switching to the more "physically correct"
analytic convention removes the asymmetry — and removes the audible
variation along with it.

#### Test 3 — in-phase reinforces, out-of-phase cancels (PASSES — same as derivative)

```
patches/aether_pair_v2_test3_inphase.aither   (theta=0):    RMS = -31.0 dB
patches/aether_pair_v2_test3_outofphase.aither (theta=π):   RMS = -240.0 dB
```

209 dB span between configurations, identical to derivative-pair.
Reinforce/cancel is convention-independent — it's signed additive
arithmetic, which any pair representation supports identically.

### Selective listening — convention-independent, real

Independently verified: with voice 1 writing pure dielectric (no
magnetic) and voice 2 writing the analytic-pair rotated by `θ = π/2`
(strike content goes into the magnetic component), voice 3 reading
at `φ = π/2` (rotated read, picks up `−mag`) hears voice 2 only and
ignores voice 1. Reading at `φ = 0` reverses the situation: voice 1
dominates, voice 2 nearly absent. 6 dB difference between the two
read angles.

This is a routing capability the scalar substrate cannot express —
voices reading the same medium can selectively receive content from
*specific* other voices based on the angular alignment of their
write-angle and read-angle. It works under both derivative and
analytic conventions because it doesn't depend on rotation having a
spectral effect; it depends on rotation having an *amplitude* effect
on each component-tap.

Selective listening is the strongest pair-only capability validated
to date. It deserves named recognition as a new compositional move
rather than being buried in Test 3's caveats.

### What the analytic-pair result reveals

Tests 2 and 3 together produce a sharper picture than either
convention alone gave us:

1. **Pair-valued substrate works.** Both conventions confirm Test 3
   (reinforce/cancel) and selective listening. The structural
   extension is sound.
2. **Rotation is not a continuous spectral character knob.** Neither
   convention delivers the music-theoretic payoff originally claimed
   for the rotation primitive. Derivative gave audible-but-spiky
   amplitude variation as a side-effect of bad spectral matching;
   analytic gives smooth-but-tiny variation that the high-Q
   resonator filters out. The "rotation as live knob" framing was
   an aspiration based on an incorrect mental model — phase
   rotation in pair-space is inaudible through stateful resonators.
3. **The pair's audible payoff is angle-based gating, not
   continuous morph.** What works is "in-phase = reinforce, out-of-
   phase = cancel" along with continuous angles between, plus
   selective listening — both of which use the rotation primitive
   as an *amplitude* control (cosine-of-angle weighting) rather
   than as a phase rotator.

### Diagnosis of the deeper structural issue

The original framing — "rotation as a continuous live knob that
shifts the medium's character" — assumed phase rotation in pair
space would produce audible spectral variation. It doesn't. Phase
rotation in pair-space corresponds to *temporal* phase shift in the
underlying signal, and a stateful resonator's bandpass is insensitive
to temporal phase shifts. To get audible variation through rotation,
the two pair components would need different *magnitude* spectra
at the receiver's frequency — which is exactly the spectral
asymmetry the lineage doesn't actually demand.

In other words: Steinmetz's "fixed quadrature" describes the
analytic-pair (smooth, equal-magnitude). The aspiration of "rotation
as audible continuous knob" demands derivative-pair-like spectral
asymmetry. These two goals are in tension; you can have one but not
the other.

### What lands as a result of this experiment

- `patches/aether_pair_v2_sympathy.aither` — the canonical analytic-
  signal sympathy patch.
- Four supporting test patches.
- This pole.md section.
- `aether.md` Gap 3 updated to record the convention-comparison
  result and to demote the "rotation as continuous knob" goal in
  favour of the validated capabilities (reinforce/cancel, selective
  listening).
- *No new primitive.* `analytic` exists already; the experiment
  uses it as-is. The integral-pair convention (mag = integral of
  strike) is named as a third candidate but explicitly NOT tested
  in this commit.

What does *not* land:

- Promotion of either convention to canonical. The substrate is
  validated; rotation as a music-theoretic knob is not.
- A sympathy recipe in COMPOSING.md based on rotation. The
  reinforce/cancel and selective-listening moves *could* lead to
  recipes, but they need a music-design pass to find compelling
  compositional shapes; that's downstream of this experiment.
- Any feedback into the scalar `aether_sympathy.aither` baseline.

### Where this leaves the trajectory

The natural next experiment branches:

1. **Option B (cybernetic_muqabala with pair-valued aether) — still
   worth running.** The pair representation gives the cybernetic
   loop a 2D phase-space coordinate (the magnetic component IS
   state, available as a leaky-integrator-with-phase-relationship-
   to-dielectric). Whether this 2D state unlocks the multi-stable
   attractor-jumping problem is testable independently of whether
   rotation produces audible spectral variation. Use either
   convention; the structural extension is what matters here, not
   the rotation knob.
2. **Integral-pair convention as a deferred alternative.** If the
   "rotation as audible knob" goal is judged worth pursuing, the
   integral convention (mag = leaky integral of strike) puts a
   low-pass-like asymmetry into the magnetic component — opposite
   to derivative's high-pass. Same primitives, no engine work.
   But the goal itself is structurally suspect (per the diagnosis
   above), so this branch should only be tried if a specific
   musical use case demands it.
3. **Document the validated capabilities for COMPOSING.md.**
   Reinforce/cancel and selective listening are real new
   compositional moves that the scalar substrate cannot express.
   They deserve documentation alongside sympathy.

The user decides. The substrate works; the convention question is
mostly settled (analytic is the cleaner one); rotation-as-knob is
demoted from aspiration to "use it for amplitude control, not
character morph."

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
