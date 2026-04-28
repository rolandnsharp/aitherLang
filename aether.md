# aether.md — the canonical doc for the aether convention

A working document. Names what the aether is in aither today, is honest
about how thin that is compared to what the project's name implies,
and stakes out the trajectory for closing the gap.

## What this doc is, and what it isn't

This is the canonical reference for the aether convention in
aitherLang. It defines the convention, names its rules, points at
the empirical results that justify it, diagnoses where it's
substantively shallow versus what the classical-field-theory
tradition (Tesla, Steinmetz, Wheeler, Faraday-in-the-lines-of-force-
mode, Dollard) means by "aether," and proposes specific extensions
to close that gap.

It is not a tutorial. It is not a celebration. The honest fact about
the aether convention as currently implemented is that it is a
single `$state` cell with a documented usage discipline, and that
fact undersells what the project's name promises. This doc takes
that gap seriously rather than glossing it.

## The aether as currently implemented

```
$aether = 0.0           # the medium
$aether = $aether * decayRate + drive * writeAmount  # any voice writes
let influence = $aether * coupling                    # any voice reads
```

That's it. A single scalar `$state` cell. A leaky integrator with
two parameters (decay rate and write amount). Voices write *drive
signals* (broadband disturbances — impulses, noise bursts, transients)
into it and read it back as *ambient drive* that they couple into
their own input. The convention has two cardinal rules:

1. **Voices write drives, never resonator outputs.** What goes into
   the aether is the *cause* of a voice's behaviour (the strike, the
   broadband excitation, the transient that kicks the resonator).
   What does NOT go in is the resonator's filtered response. This
   distinction is load-bearing — see the empirical section below.
2. **The aether has no modes of its own.** It does not resonate. It
   does not filter. It does not have spatial extent. It is a
   broadband, neutral, dimensionless carrier. Whatever spectral
   content it has comes from what voices wrote into it.

That second rule is what makes the aether work. Earlier failed
attempts (`sympathetic_chord.aither`, `sympathetic_field.aither`,
preserved as documented evidence) tried to make the field a
resonator (scalar leaky integrator that filtered to whatever was
loudest, then a multi-mode DHO bank with structured frequencies).
Both failed because narrow-band media cannot carry cross-frequency
excitation between resonators. The neutral broadband aether works
because it doesn't impose its own spectrum on what passes through
it.

## What the convention has empirically established

Three results have been audit-verified and are reproducible:

1. **Sympathetic resonance works** (`patches/aether_sympathy.aither`,
   commit `a0384a7`). Three DHO voices tuned A / C# / E. Voice 1 is
   struck with a broadband impulse + noise burst written into the
   aether. Voices 2 and 3 NEVER receive a direct strike — they only
   read `$aether * coupling`. They ring at their own frequencies,
   producing a clearly audible chord. The falsification check
   passes definitively (with `v2v3Couple = 0`, the difference signal
   drops to −240 dB machine zero — voices 2 and 3 are silent without
   the aether coupling). Sympathy peaks 13–15 dB below the struck
   note in the full mix — louder than piano, comparable to a sitar.
   Robust across coupling 0.1 → 1000 (60 dB range), no NaN at any
   setting, because the aether is open-loop by construction (voices
   only read from it; voices that write are themselves struck from
   outside).

2. **Cybernetic stabilisation works** (`patches/aether_muqabala.aither`,
   commit `ff9a0aa`). Same substrate, closed-loop topology. A carrier
   reads the aether, processes nonlinearly (fold + drive), writes its
   nonlinear output back into the aether. The closed loop finds an
   attractor and locks the system into it. Falsification check
   passes (with the loop broken, the dominant peak σ goes from 0.2 Hz
   to 260 Hz — the loop is doing real stabilisation work).

3. **Cybernetic attractor-jumping does NOT work** (same patch,
   diagnosed in `pole.md`'s "Aether enables cybernetic stabilisation
   but not full al-Mukabala dynamics" section). The patch produces
   five qualitatively distinct attractors across parameter space but
   stays in one per setting. The rich attractor-jumping motion the
   analog cybernetic-synthesis tradition is famous for does not
   emerge. The diagnosis: aither's `fold` and `drive` are stateless;
   the loop has nowhere to store multiple attractors. The aether is
   a coupling MEDIUM, not an active processing element. It transmits
   what's written; it does not store competing attractors.

The pattern these results map out: **the aether enables coupling
between elements that have their own state, and is silent / passive
where its partners are stateless.** Sympathy works because DHO
voices are stateful resonators. Cybernetic stabilisation works
because the carrier+nonlinear-loop has a single equilibrium it can
find. Cybernetic attractor-jumping fails because no element in the
loop carries multi-stable memory.

## How thin is the current scalar, really?

Honestly: pretty thin. A single floating-point cell with a leak rate
and a write convention. As a *programming tool* it earns its keep —
the empirical results above prove that — but as a model of the
classical-field-theory aether it is a bare-bones approximation
missing most of what the lineage means by the term.

What the actual classical-field-theory aether (per Tesla, Steinmetz,
Wheeler, Faraday in the lines-of-force mode) is supposed to be:

- **A dual-component substrate carrying magnetic and dielectric
  energy in fixed quadrature.** Steinmetz: *"the magnetic and the
  dielectric field of the conductors are both included in the term
  electric field and are the two components of the electric field
  of the conductor."* The aether doesn't carry "a number" — it
  carries two simultaneous physical quantities at 90° to each other.
- **A medium with its own internal dynamics** — characteristic
  impedance, phase-shift-on-readback, capacitance scaling with
  compression. What you write into the aether comes back to readers
  with the medium's own signature applied.
- **A unifier between participants, not just a passive bus.** Voices
  in a real classical field are *boundary conditions on the field*,
  not sources injecting content into a wire (Wheeler's
  power-lines-vs-magnet observation: identical conjugate-field
  geometry whether the cause is current in two wires or polarised
  mass in a magnet).
- **Always-on participation, not opt-in.** Every physical thing is
  in the field by default; isolation is the special case (a thing
  in a *quiet* field, not a thing in *no* field).
- **A carrier of longitudinal disturbance, distinct from the
  Hertzian observables.** What we measure (cycles, frequencies,
  spectra) are the medium's *response* to longitudinal compression;
  the longitudinal part is the cause; the Hertzian part is the
  observable epiphenomenon. The aether carries the cause; the
  resonators / outputs are the response.
- **The "possibility-zero" of the system.** What appears empty (the
  unmanifest, the medium itself, rest) is where the actual causation
  lives; what appears manifest (the audible cycles, the visible
  field) is the projection. Standard intuition is inverted.

The current scalar `$aether` correctly implements *some* of these:
it carries longitudinal disturbance, it's broadband, it's
dimensionless, it's the place causation happens. It does not
implement: the dual-component (dielectric, magnetic) structure, any
internal dynamics beyond simple leak, default participation (voices
must opt in), interference handling.

This is the gap.

## Specific gaps, ordered by structural cost

Five distinct gaps, each with a different cost-to-close and a
different gain. None require new aither primitives — all live at
the convention layer.

### Gap 1: Voices opt INTO aether participation (should be the inverse)

**Currently:** a voice that doesn't reference `$aether` doesn't
participate in the medium. The aether is opt-in.

**What it should be:** every voice is in the aether by default;
isolation requires explicit opt-out. This matches Wheeler's
"voices are boundary conditions on the field" framing — there is
no "isolated voice"; there's a voice in a quiet aether (no other
voices writing) or a voice that has explicitly chosen to ignore
the medium (read it as zero).

**Cost to close:** documentation + style change. No code change.
A pattern in COMPOSING.md that says "every voice should reference
`$aether` somewhere; voices that don't are explicitly isolated
and should comment why."

**Payoff:** small but real. Makes patches more aether-aligned by
default. Composers reach for the medium reflexively rather than
remembering to opt in. Probably revealing in long patches where
the question "should this voice couple to the others?" gets
answered automatically.

### Gap 2: The aether has no internal dynamics

**Currently:** `$aether = $aether * 0.95 + strike * 0.1`. A
linear leaky integrator. The medium responds passively — what
you write is what comes back, attenuated by decay.

**What it should be:** the medium has its own characteristic
behaviour. What you write comes back with the aether's signature
applied. Wheeler's "smaller-space-higher-capacitance" claim
suggests at least one specific nonlinearity: the aether's
responsiveness scales with how excited it already is. A highly
excited aether couples voices more strongly than a quiet one.

**Cost to close:** one line. Replace the linear write with a
nonlinear one:

```
# Linear (current)
$aether = $aether * 0.95 + strike * 0.1

# Nonlinear — capacitance scales with compression
let capacitance = 1.0 + abs($aether) * 0.5
$aether = $aether * 0.95 + strike * 0.1 / capacitance
```

That's the simplest version. Alternatively, phase-shift-on-readback
(an impedance response) using a second state cell:

```
$aetherInstant = $aetherInstant * 0.95 + strike * 0.1
$aetherDelayed = $aetherDelayed * 0.99 + $aetherInstant * 0.01
let aetherSeen = $aetherInstant + $aetherDelayed * lagAmount
```

Two state cells instead of one. Voices read `aetherSeen`, which
has both an immediate component and a slower-following component
— more like the actual impedance response of a physical medium.

**Payoff:** substantial in principle; needs experimental
verification. The capacitance-scaling version makes the aether
feel "alive" — its responsiveness depends on its own state, which
is what real physical media do. The impedance version gives the
aether structural memory — voices reading it see not just current
disturbance but recent history.

**Risk:** nonlinear self-response could destabilise feedback
loops. The current scalar aether's open-loop discipline (only
strikes write in, in the sympathy patch) is what made it robust
across 60 dB of coupling. Adding nonlinear self-response weakens
that guarantee. Worth testing on the sympathy patch first to see
if the broadband-coupling result still holds with internal
dynamics added.

### Gap 3: Scalar instead of (dielectric, magnetic) pair

**Currently:** `$aether` is one float. It carries an instantaneous
amplitude. Two voices reading it at the same time read the same
number.

**What it should be:** the aether carries a Steinmetz-form
(dielectric, magnetic) pair — two real components at 90°
quadrature, where the dielectric component represents the
*potential-storage* aspect (instantaneous structural state) and
the magnetic component represents the *kinetic-storage* aspect
(time-varying flow). The pair the language already uses for
`cmul`, `phasor_pair`, `analytic` — applied to the medium itself
rather than to individual signals.

This isn't just "two scalars instead of one." It's two physically
named energy-storage modes that voices write into and read from
*as a pair*, with the language's existing pair operations doing
the right thing on the medium's state.

**Cost to close:** moderate. Two state cells instead of one. The
write convention becomes "voices write a (dielectric, magnetic)
pair into the aether" — which means voices have to generate both
components. For a strike, the dielectric component is the impulse
itself (the potential-jump) and the magnetic component is the
impulse's first derivative (the current that flows in response).

```
$aetherDie = 0.0   # dielectric component — potential storage
$aetherMag = 0.0   # magnetic component — kinetic storage

# Voice writing: a strike is a dielectric impulse + its derivative
$prevStrike = 0.0
let dieDrive = strike
let magDrive = strike - $prevStrike   # impulse derivative ~ current
$prevStrike = strike
$aetherDie = $aetherDie * 0.95 + dieDrive * 0.1
$aetherMag = $aetherMag * 0.95 + magDrive * 0.1

# Voice reading: gets both components, can use either or rotate them
let sense = $aetherDie * coupling   # dielectric tap (default)
# or: let sense = $aetherMag * coupling   # magnetic tap
# or: let pair = rotate($aetherDie, $aetherMag, theta)   # rotated read
```

**Payoff:** large. This is the structural extension that closes
the most gap with the most leverage:

- Maps onto the project's existing rigid-conception complex
  algebra (`cmul`, `rotate`, `analytic`) — the substrate carries
  what the language already operates on
- Gives cybernetic patches the state-bearing-loop-partner they
  needed (the magnetic component IS state; voices reading it see
  the system's recent flow history, not just instantaneous
  amplitude)
- Lets voices with phase coherence write into the aether
  *coherently* — phase-locked voices reinforcing each other in
  ways scalar writes can't express
- Connects directly to Steinmetz's dual-energy framework, which
  the manifesto already cites

**Risk:** the derivative-as-magnetic-component is one possible
convention; the lineage suggests it but doesn't dictate it. Other
choices (analytic-signal pair, integrator-as-magnetic) might
work better. Needs experimental verification of which (dielectric,
magnetic) decomposition produces the most musically useful
behaviour.

**Status (2026-04-29):** *partial validation* with the derivative
convention. Three pre-committed falsifiable tests run on
`patches/aether_pair_sympathy.aither` plus six supporting test
patches:

- *Test 1 (phase-coherent sympathetic resonance):* both pair and
  scalar produce peaked phase histograms (σ = 0.130 π and 0.157 π
  respectively). The test's prediction that scalar would give a
  uniform distribution is empirically wrong — periodic strikes give
  deterministic phase regardless of architecture. The pair version
  rotates the mean angle by π/2 (controllable phase rotation) but
  doesn't show the binary peaked-vs-uniform behaviour the test was
  designed to detect.
- *Test 2 (rotational morph as continuous live knob):* centroid
  variation 1.91× (just under 2× criterion); pair stddev 5.2× larger
  than scalar but variation is not smooth — concentrated near
  rotation π/2 where the dielectric is nullified. RMS does modulate
  smoothly across rotation by ~30 dB.
- *Test 3 (in-phase reinforces, out-of-phase cancels):* PASSES
  cleanly. 209 dB RMS difference between in-phase and out-of-phase
  configurations, far exceeding the 6 dB criterion. Scalar control
  is theta-invariant. *Selective listening between multiple writers*
  is verified as a pair-only capability.

The diagnostic finding: the derivative-as-magnetic convention puts
most audio-bandwidth energy in the dielectric (per-sample finite
difference attenuates ~29 dB at 277 Hz), so rotation acts as a
magnetic-attenuator with high-frequency emphasis rather than a
continuous spectral character morph. Steinmetz's framing — equal-
magnitude dielectric and magnetic components in 90° phase quadrature
— is more naturally realised by the **analytic-signal pair**
(`re = signal`, `im = Hilbert(signal)`), which the language already
provides via the `analytic` primitive. The natural next experiment
is to swap the convention and re-run the same three tests; same
primitives, no engine work.

The pair-valued substrate works structurally (Test 3 confirms it).
What's not yet validated — and what the analytic-signal convention
may unlock — is the music-theoretic payoff: rotation as a
continuous timbre / character knob.

See `pole.md`'s "Pair-valued aether: partial result" section for
full audit numbers, the per-test verdicts, and the diagnosis.

### Gap 4: One aether vs many

**Currently:** there is one `$aether` cell shared by all voices
that participate. Every voice that reads it gets the same
content.

**What it should be:** Tesla's framing — *radio waves through the
aether as sound through air* — reads literally as "different
kinds of excitation propagate through different aethers." Aither
could have multiple named aethers (`$aetherAcoustic`,
`$aetherEM`, `$aetherCV`), each carrying a different *kind* of
disturbance. Voices subscribe to whichever applies. This lets
the language eventually encapsulate radio, biosignal, control
voltages, and other domains where the underlying math is the
same but the excitation kind differs.

**Cost to close:** trivial. Just convention — a documented set
of named state cells.

**Payoff:** mostly future-facing. For audio synthesis alone,
multiple aethers don't add much over a single one (you're only
ever generating one audio stream). The payoff lands when aither
grows beyond audio — when patches need to model coupling between
audio and modulation signals, between voices and an abstract
"control field," between physical-acoustic content and
electromagnetic-style content. Worth documenting now so the
trajectory is clear; not worth implementing as a discipline
until there's a use case.

### Gap 5: No interference operations

**Currently:** when two voices write into the aether, the
contributions add. Constructive interference (in-phase writes)
and destructive interference (out-of-phase writes) both happen
naturally as a consequence of the addition. But there's no
operation that *targets* the interference — no way to say "fire
this trigger when the aether content is destructively
cancelling," no way to compute "the difference between what
voice A wrote and what voice B wrote."

**What it should be:** Wheeler's "chasm" framing — interference
nulls as first-class structural objects. The structurally
significant moments in a coupled-pair system are not just where
both components are present; they are also where they cancel. A
patch designed in this framing would deliberately compute where
its silences fall.

**Cost to close:** higher than the others. Would need either a
new primitive (`interference_null(a, b)` returning 1 when a + b
is near zero) or a documented pattern of using existing
primitives in a specific way. Probably not worth opening this
gap until cybernetic patches start needing event-triggers from
medium-state — at which point the comparator-kick experiment
from `pole.md`'s diagnosis would naturally introduce the
pattern.

**Payoff:** speculative. Possible compositional technique for
patches that explicitly compute their silences as structure.
Worth holding in mind, not worth pursuing now.

## Recommended trajectory

The honest answer to "how do we close the gap" is to do Gaps 1
and 2 immediately and Gap 3 after experimental verification.
Gaps 4 and 5 stay parked.

**Immediate (no code, just discipline):** Gap 1 (default
participation). Update COMPOSING.md to document the convention
that voices participate in the aether by default; isolated
voices are the special case and should comment why. Costs
nothing; aligns the project's framing with the lineage.

**Next experiment (one line of code):** Gap 2 (internal
dynamics). Test the capacitance-scaling version of the aether
write rule on the sympathy patch. Does cross-frequency coupling
still work? Does it work better? Does it break the falsification
check? If the result is "still works, still robust, possibly
richer-sounding," it lands as the new default aether write
discipline.

**Larger experiment (multi-cell pattern, no engine work):** Gap 3
(pair-valued aether). Build a user-space patch using
`(dielectric, magnetic)` aether cells — strike-as-impulse-and-
derivative writing, voice reads of either component or rotated
combination. Test whether this gives the cybernetic_muqabala
patch the state-bearing loop partner it was missing. If yes, the
pair-valued convention becomes the canonical aether shape going
forward and the scalar version is documented as the simpler
fallback. *Status 2026-04-29:* partial — Test 3 (reinforce/cancel)
passes; Tests 1 and 2 reveal that the derivative convention puts
most audio-bandwidth energy in the dielectric, limiting rotation's
musical reach. Natural next experiment: re-run the same three
tests with the analytic-signal pair convention. See Gap 3 above
for details.

The principle the manifesto names — *aither should already be
complete; new primitives only land when they really earn their
keep* — applies to all of these. None of the proposed extensions
require codegen changes. All live at the convention layer with
documentation, patches, and design notes as the deliverable. The
language already has the substrate; the gap is in how we use it.

## How to actually use the aether (today, with the scalar version)

Until the proposed extensions land, the working aether convention
is the scalar one. The working pattern:

```
# At top of patch — declare the medium
$aether = 0.0

# Tunable parameters
let aetherDecay = 0.95          # per-sample leak; smaller = shorter memory
let aetherWrite = 0.1           # how strongly strikes write into the aether
let coupling    = 1.0           # how strongly voices couple back from it

play sourceVoice:
  # Generate a broadband strike — impulse + brief noise burst
  let trig    = midi_trig(60)
  let burst   = pluck(trig, 30)             # 30 ms envelope
  let strike  = trig * driveAmp + noise() * burst * burstAmp
  $aether     = $aether * aetherDecay + strike * aetherWrite

  # Audible voice — a resonator excited by the strike
  let v = dho(strike + $aether * coupling, 220.0, 0.001)
  let s = v * 0.15
  [s, s]

play receiverVoice:
  # Only reads from the aether — never directly struck
  let v = dho($aether * coupling, 277.18, 0.001)
  let s = v * 0.15
  [s, s]

# Master mix
sourceVoice + receiverVoice
```

The two cardinal rules:

1. **Voices write drives, never resonator outputs.** What goes
   into the aether is the *cause* — impulses, noise bursts,
   broadband transients. What does NOT go in is `$aether =
   $aether + dho(...)` (writing the resonator output). That's
   the failure mode of the scalar-field experiments (sympathy_
   chord) and the DHO-bank-field experiments (sympathy_field).
2. **The aether is a medium, not a resonator.** Don't put it
   through a filter. Don't make it ring. It is broadband and
   neutral; whatever spectrum it carries comes from what voices
   wrote into it.

Common pitfalls:

- **Writing the resonator output back in.** Will fail to produce
  cross-frequency coupling. Diagnosed as the "spectral
  narrowness" failure mode in the sympathetic_chord patch.
- **Choosing a leak rate too fast.** If `aetherDecay < 0.5`, the
  aether's memory is too short to support sympathy across
  reasonable damping values. Default ~0.95 works at 48 kHz.
- **Choosing a leak rate too slow / no leak.** DC accumulates;
  amplitude grows unboundedly; eventually NaN.
- **Writing without a broadband strike pattern.** A pure sine
  wave written into the aether is band-limited; receiver voices
  at off-frequencies see nothing.
- **Forgetting the falsification check.** When testing whether a
  patch genuinely uses the aether for coupling vs is just routing
  through independent paths, build a parallel "monopole" version
  with the aether-coupling-coefficient zeroed and subtract; the
  difference signal tells you what the aether is contributing.

## Connection to other docs

- `pole.md` — the design exploration that arrived at the aether.
  Five experiments (`sympathetic_chord` negative, `sympathetic_
  field` negative, `aether_sympathy` positive, `aether_muqabala`
  partial, `aether_pair_sympathy` partial) form the diagnostic
  chain. The synthesis sections ("pole was the wrong abstraction,"
  "the aether is dimensionless," "broadband-aether positive
  finding," "aether enables stabilisation but not full al-Mukabala
  dynamics," "pair-valued aether: partial result") are the working
  record of how this convention was arrived at.
- `bachPolyphase.md` — the Tesla longitudinal section frames the
  aether as the substrate the project's intellectual lineage
  always assumed; the dimensionless-field clarification is the
  same insight from the polyphase-physics angle.
- `cybernetics.md` — situates the aether work in the broader
  cybernetic-synthesis tradition. The aether is the substrate
  Tudor-style coupled-resonator patches need; aither's version
  unlocks one form of cybernetic behaviour (stabilisation via
  the closed-loop coupling) but not yet the multi-stable
  attractor-jumping the analog tradition is most known for.
- `ken.md` — running notes from the Wheeler / Theoria Apophasis
  videos that informed the framing. 23 takeaways aggregated;
  most directly relevant to the aether convention are items 1
  ((dielectric, magnetic) component identity), 2 (voices
  disturb don't emit), 3 (longitudinal medium), 11
  (longitudinal-AND-Hertzian environment), 15 (voices as
  boundary conditions), 16 (shape the principle), 17
  (asymmetric pair speculation), 19 (aether as unifier), 22
  (two-zeros), 23 (unconnected-antenna analogue).
- `MANIFESTO.md` — the project-level claim that aither is a
  classical-field-theory programming environment. The aether
  convention is what makes that claim operational rather than
  rhetorical.

## What this doc does NOT cover

- A defence of the dimensionless / no-spatial-extent commitment.
  That argument lives in `pole.md` and `bachPolyphase.md`.
- The history of the failed pole-as-primitive proposal. Lives in
  `pole.md`'s synthesis sections.
- The Wheeler / Steinmetz / Tesla / Faraday lineage in detail.
  Lives in `ken.md` and the relevant manifesto / blog post
  paragraphs.
- A list of musical patches built on the aether. The current
  `patches/aether_sympathy.aither` and `patches/aether_muqabala
  .aither` are *diagnostic* patches — they prove the substrate
  works rather than make music. Real musical patches are
  forthcoming; once they exist, they belong in `sounds/` and
  documented in `COMPOSING.md`.

## Status as of this writing

The aether convention is empirically validated for sympathetic
resonance and for cybernetic stabilisation. It is honestly thin
relative to what the lineage means by aether. The trajectory for
closing the gap is:

1. **Default participation** — small, just docs, do now
2. **Internal dynamics** — small, one-line code change, test on
   sympathy patch
3. **Pair-valued (dielectric, magnetic) aether** — partial as of
   2026-04-29 with the derivative-as-magnetic convention. Test 3
   (reinforce/cancel) passes; Tests 1 and 2 reveal a convention
   weakness, not a substrate failure. Natural next experiment:
   the analytic-signal pair convention, same primitives, same
   tests
4. **Multi-aether** — parked until a use case appears
5. **Interference operations** — parked until cybernetic
   experiments need event-triggers from medium state

The aether is necessary for what aither claims to be; the current
scalar version proves the substrate works; the extensions above
make the substrate more aether-like in ways the lineage would
recognise. The work is incremental, the cost is small, the
trajectory is clear.
