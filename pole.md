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
