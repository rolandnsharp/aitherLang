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
