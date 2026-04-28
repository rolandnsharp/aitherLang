# ken.md — Wheeler videos, running notes

A working notebook of concrete things worth taking from the
Ken Wheeler / Theoria Apophasis video series. Updated as we
watch more.

## What this doc is and isn't

Wheeler is in the same intellectual lineage the project draws from —
Steinmetz, Tesla, Faraday-in-the-lines-of-force-mode, Russell,
Dollard. He's a leading repository of that lineage's
*classical-field-theory* tradition, and for our purposes — building
a CFT programming language — that's the relevant question. Whether
his framings would be defensible at a mainstream physics conference
is a different question and not ours to fight.

This doc is a *springboard*, not a bible. The aim is to extract the
operationally useful claims that bear on aither's design and
discard the rhetorical scaffolding around them. When a claim has
direct implications for how aither should work, it goes in the
"Concrete takeaways" section at the bottom. When a claim is
suggestive but not yet operational, it stays in the per-video
notes.

When something Wheeler says is overreach in a way that matters for
our design hygiene, we note that too — not as a credential check
but so we don't accidentally build on a false claim. The
interesting parts are the parts that engage with the actual
classical-field tradition; the overreaches are the parts where
philosophical confidence outruns the math.

## Video 1 — "The Plane of Inertia" (chalkboard magnet demo)

### Core claims

- **A magnet has no poles in the substance sense.** "North" and
  "south" are labels on the diverging field directions; they're
  *epiphenomena of the geometry*, not entities. Cutting a magnet in
  half doesn't isolate the poles — you get two new magnets, each
  with its own pair of "poles" and its own equator. Reductio: keep
  cutting, keep getting more poles. The labels are not the substance.
- **The substance is the plane of inertia and the conjugate
  geometries.** Toroidal divergence (the centrifugal magnetic
  loops) and hyperboloidal convergence (the centripetal dielectric
  pull-in along the axis) are the two faces of one structure. They
  share an equator at 45° / 90° — the plane of inertia.
- **Life lives at the plane of inertia.** The neutral zone where
  divergence and convergence balance is where structured behaviour
  emerges. Galactic spiral arms, planetary equatorial life
  concentrations, the equator of any toroidal system. The interesting
  things happen at the dynamic balance, not at the extremes.

### What's transferable

The pole-as-label-not-substance reading is the most useful piece.
It maps onto our retraction of pole-as-primitive: we kept reifying
labels (a "pole," its "frequency," its "amplitude") that were
properly geometric features of a continuous structure. The aether
reframing is the geometric version of the same epistemic move —
model the field, don't reify the labels.

The torus/hyperboloid conjugacy maps onto our existing rigid-ℂ
algebra in a clean way:
- `phasor_pair(rate)` traces the toroidal geometry — a circle in
  the (cos, sin) plane, energy diverging outward from a centre.
- `cmul(p, p)` doubles the angle — the hyperboloidal face. Energy
  concentrating, focusing, sharpening.
- `freq_shift` and `analytic` operate at the boundary between them.

The "life at the plane of inertia" framing translates to: the
interesting musical territory is the *dynamic balance* between
broadband divergence (full chaos / white noise) and sharp
convergence (single resonance / infinite ringing). Patches at the
equator of these two tendencies are where the cybernetic-synthesis
tradition lives.

### What's overreach

Wheeler's confidence in "this is the geometry of the entire
universe" outruns the demonstration. The claims that "95% of life
is on the equator" and that the right-hand rule is "just pressure
mediation" are unjustified. These don't undermine the geometric
core of the video; they're rhetorical extensions worth filtering.

## Video 2 — "Steinmetz, Dielectricity, and the Conjugate Field"

### Core claims

- **Electricity is not a substance. It is the *consubstantiality* of
  the magnetic and dielectric fields at 90°.** Direct quote from
  Steinmetz (*Electrical Discharges, Waves and Impulses*, 1917):
  *"the magnetic and the dielectric field of the conductors both
  are included as meaning the term electrical field and are the two
  components of the electric field of the conductor."* This is the
  engineer who designed the AC power grid; it is not a fringe view
  in the engineering tradition.
- **The right-hand rule is the geometric expression of the
  three-component identity.** Given any two of {magnetic, dielectric,
  electric-current-direction}, the third is determined. They're
  three orthogonal axes of the same field, not three separate
  things.
- **Light is a longitudinal disturbance of the aether.** Tesla
  quote: *"Light cannot be anything but a longitudinal disturbance
  of the aether involving alternating compressions and refractions.
  In other words, light can be nothing but a soundwave of the
  aether."* This is the longitudinal-medium framing — light works
  the way sound works through air, just at higher frequency through
  a different medium.
- **Nothing emits; everything disturbs.** A speaker doesn't emit
  sound, it disturbs air. A candle doesn't emit photons, it disturbs
  the aether. An AC generator doesn't generate electricity, it
  disturbs the aether such that the magnetic and dielectric
  components manifest at 90°. The "emission" framing is wrong;
  the "disturbance of a medium" framing is right.
- **Wave phenomena require a medium.** No exceptions. There is no
  such thing as a wave without something to wave in. The "speed of
  light" is the maximum rate at which the aether can be disturbed —
  the lag value of the permeability and permittivity of the
  medium against itself.

### What's transferable

This video has the most direct implications for aither's design
that we've encountered. Three concrete things:

1. **Pair components as (dielectric, magnetic), not (real, imag).**
   The two real components of every pair in aither aren't arbitrary
   axes labelled "real" and "imaginary." They're the two
   *physically named* energy-storage modes of the substrate that
   exist simultaneously and at 90°. When `phasor_pair(rate)` returns
   `(cos, sin)`, the cosine is the dielectric-like (potential,
   voltage-equivalent) component and the sine is the magnetic-like
   (kinetic, current-equivalent) component. They're not the same
   thing rotated by 90°; they're two different physical quantities
   in fixed quadrature.

2. **Voices disturb the aether; they don't emit.** The discipline
   we settled on for the broadband-aether convention — voices write
   *drive signals* (the disturbances they cause) into the aether,
   not their *outputs* (what they themselves are doing in response)
   — is exactly the Wheeler/Tesla framing. The deeper implication:
   in a Wheeler aether-physics model, **there is no such thing as
   an isolated voice**. Every voice is a disturbance of the same
   medium. "Isolation" is just *a voice that hasn't given the
   medium any disturbance to carry to other voices*. This inverts
   the default: voices should opt **out** of aether participation,
   not in.

3. **Longitudinal-medium framing for what the aether carries.** The
   broadband drive signals in our experiment are exactly Tesla-style
   longitudinal disturbances. The dimensionless single-cell
   `$aether` is the right shape; the experiment we're running is a
   literal test of "voices coupled through a longitudinal medium"
   in the Tesla sense. Confirms the framing we already locked in.

### What's overreach

The claim "all free neutrons spontaneously become protons,
therefore neutrons are just protons in a different phase modality"
is a category error — empirical decay is not the same as
ontological identity. By the same logic, all radioactive isotopes
decay and are therefore not really separate things. We can use
Wheeler's *observations* (the dual-component electricity, the
longitudinal-medium framing) without inheriting his particle-
ontology overreaches.

The Michelson-Gale 1925 claim is contested — both ether-drift and
general-relativistic interpretations fit the data. Worth knowing as
the experimental basis Wheeler cites; not load-bearing for our
project.

## Concrete takeaways for aither's design

A running list, aggregated as we watch more videos. Each item
should connect to a specific design choice in the project.

### From Video 2 (most actionable)

1. **The complex-pair-aether extension should commit to (dielectric,
   magnetic) component identity**, not generic (magnitude, phase) or
   (real, imaginary). Tie it explicitly to Steinmetz's 1917
   framework. Small docs change for `pole.md` when the broadband-
   aether experiment validates and we write `aether.md`.

2. **Default voices to participating in the aether; isolation is the
   special case.** When `aether.md` lands as a canonical pattern
   doc, the discipline should be: every voice writes its drive
   into `$aether`; voices that *don't* are the explicit isolated
   case (a voice in an empty room). This inverts the natural
   programming-language framing where opt-in is default.

3. **The longitudinal-disturbance framing is the right model for
   what the aether carries.** Confirms the experiment design. The
   single dimensionless cell carrying broadband drive over time is
   a Tesla-style longitudinal medium, and the cross-frequency
   coupling test we're running is a literal test of whether the
   substrate hosts that physics.

### From Video 1 (more conceptual)

4. **Don't reify labels.** A `dho` doesn't have intrinsic
   "pole-ness" or "frequency-ness" or "amplitude-ness" — it has
   a configuration of magnitude-and-phase relationships that
   surrounding operations *interpret* as those properties. The pole
   primitive failed because it tried to make a label into an object.
   Future primitive proposals should pass the test "is this an
   actual structure, or am I reifying a property of a structure?"

5. **The torus/hyperboloid conjugacy is the geometric reading of
   our existing pair operations.** `phasor_pair` traces toroidal;
   `cmul(p, p)` traces hyperbolic angle-doubling. Worth keeping in
   mind when the complex-pair-aether (Level A) extension is
   designed — the medium itself can be visualised as the
   conjugate-geometry pair, and writes/reads as the boundary
   between divergent and convergent flows.

6. **Aim patches at the dynamic balance, not the extremes.** The
   plane-of-inertia framing translates to: the interesting musical
   territory is the equator between broadband chaos and sharp
   convergence. This is what the cybernetic-synthesis tradition has
   always been about, and it's why patches at fixed parameters
   sound dead — they're sitting at one of the extremes rather than
   navigating the boundary.

## How to use this doc

When we start designing `aether.md`, this doc is the source for the
philosophical framing. When we propose a new primitive, the
"Don't reify labels" test (item 4) should be applied. When we
extend the aether to carry complex pairs, the (dielectric, magnetic)
identity (item 1) should be the explicit commitment. When patches
go quiet or repetitive, the dynamic-balance framing (item 6)
suggests where to look.

The doc grows as we watch. Each new video adds its core claims,
its transferable parts, its overreaches, and its contributions to
the running concrete-takeaways list at the bottom.
