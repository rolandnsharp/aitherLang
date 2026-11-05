# Step first: no clock, no hertz, only turning

**Status (2026-11-05): a design note, nothing built.** Roland: "to move
away from dimensions, time, hz and get into the aither way of thinking.
simplicity is divinity." He also said "perhaps we have both during the
transition", so `t` stays until rewritten patches show the way without it.

## Where aither started, and where it is going

aither began with the oscilloscope: sine waves written as functions of
`t`, the x-t trace. As it leaned into field theory, the physical part of
the language stopped reading the clock at all. `dho`, `resonator`,
`phasor`, `vdp` and every `$z = turn($z, step)` keep a state and turn it
by a step each sample. That is turning, not time: what is called time is
the count of turns. The step is already the primitive, and `t` sits
beside it.

## The lineage

- **Tesla** read the medium as carrying compression and rarefaction
  along the direction of travel, as sound does in a gas, and called his
  system non-Hertzian, against Hertz's transverse waves. He wrote in
  resonance, wavelength and the natural period of a circuit or the
  earth, not in a clock ticking under a sine. (Tesla, "The True
  Wireless", *Electrical Experimenter*, May 1919; *Colorado Springs
  Notes 1899–1900*, Nolit, Belgrade, 1978. These are popular press and
  notebooks and aren't in the literature index used for references
  here. Check them against the originals before quoting.)
- **The hertz as a unit** is later: named in 1930 by the IEC, after
  Hertz. Before that, frequency was "cycles per second", a count of
  turns. *(Not checked against a primary source yet.)*
- **Steinmetz** made the turning complex number the engineer's tool
  (1893): a phasor whose time is only implied by how far it has turned.
  *(The 1893 paper is not checked yet.)*
- **Hamilton's versors** (the pair of pairs, `nested-pair-2026-11-05.md`)
  are turning in every direction, with no time in them.

A compression wave is a field handing a push along, each part pressing on
the next. That is a step rule (state turned, shrunk and pushed by its
neighbour), not a function of `t`.

## The idea

**Each sample is one step of the field.** Everything that changes is
state being turned (spin), shrunk (damping) or pushed (force). With pairs
of pairs, turning covers every direction, so those three are the whole of
the dynamics. No outside parameter runs underneath.

| today, by the clock | step first |
|---|---|
| `sin(TAU * f * t)` | a phasor turned by its step: `spin`, `dho` (pitch = how far it turns a step) |
| an envelope of `t` | a size shrunk each step (damping, which `dho` already is) |
| `since(x)`: `t - $changed_at` (stdlib) | steps counted since `x` changed |
| `t mod 40`, sections, "strike every 8 s" | a slow phasor: each time it comes round; a slow pair of pairs for a whole form, sections where it crosses a pole |
| harmony as Hz | ratios between turnings, as the bell's partials and the sympathetic bells already are |
| `pixel_index`, `frame_number`, `every = M` (stdlib, codegen) | counts of steps, which they already are underneath (`floor(t * sr + 0.5)`) |

## What it gains

- **Nothing grows without bound.** `t` loses precision over a long set,
  sooner in single precision (a microcontroller, the C export). Turning
  state stays bounded forever.
- **The engine gets smaller.** Today it owns a clock (`aither_t`, carried
  across cores as `XB_T`), `sweep` and its origin (`s->sweep_origin`,
  12 uses), and the times a voice and a reload start. Step first, the
  engine gives each patch only the step. Everything else is state in the
  patch, and state already migrates on hot reload, so a reload keeps the
  music's place with nothing to line up.
- **One kind of change**, at every rung: number, pair, pair of pairs.

## What it costs, honestly

- **Pitch still needs a step length,** as long as it is written in Hz: a
  hertz is turns per step times steps per second. Either `sr`/`dt` stay
  as the one bridge to the outside, or pitch becomes ratios to one master
  turning (a patch says "this turns 3/2 of that"). Ratios are the aither
  way; Hz is how a MIDI note, an SDR's centre frequency and a listener's
  ear arrive. Probably both: the outside speaks Hz at the edge, and the
  patch thinks in turnings.
- **Some things read naturally in seconds:** "strike every 8 s", a
  render's length. As slow phasors they work, but are one step less
  direct to write.
- **A large migration:** 63 of 115 patches use `t`; the stdlib uses it on
  7 lines.

## The transition: both

1. `t` stops being the engine's: a stdlib name derived from a step count
   the engine keeps. The patches don't change; the engine loses its
   clock. Measure that the corpus is unchanged.
2. `since` counts steps; `pixel_index`, `frame_number` and `every` read
   the count.
3. Rewrite three patches without `t` and compare them with the originals:
   one tonal, one arranged over minutes, and `hopf_split` (`t mod 40`,
   `t * passes`, `TAU * t / 37`). The same sounds and pictures, and which
   reads more simply.
4. Decide whether `t` stays as a convenience or goes. Then decide
   whether Hz stays at the edges or gives way to ratios.

## Pitch, the way Tesla tuned

As his writing describes it (not checked here as physics), Tesla tuned by
resonance: a circuit's period came from what it was made of, its capacity
and self-induction, and he tuned circuits to each other, and in his
wireless work to what he took to be the earth's own period. A frequency
was relational, a property of the resonator; the second came in only when
someone measured against a clock.

So in aither: **pitch lives in the resonator and in its ratio to other
resonators; the step is the medium's grain, not a clock.** The one place
a second is real is the edge, where the patch meets the outside (the
sound card, an SDR's centre frequency, a MIDI note, the ear). A sketch:

- A patch has one reference turning, its keynote. Only that one takes a
  rate from outside, in Hz, at the edge.
- Everything else turns in ratios to it: 3/2 of that, a bell's partials,
  the sympathetic bells' octave as 2.
- A resonator may instead carry its own make (a stiffness and a mass, as
  Tesla had coil and condenser); its pitch is what they give, in turns a
  step, with no Hz at all.

Moving the keynote retunes the patch; the same patch runs at 48 kHz for
sound and 192 kHz for radio, only the edge knowing the rate; `sr` leaves
patches.

## Each time a turning comes round

`impulse(freq)` already is this, for its own hidden phasor: a 1 on the
step it wraps. What is missing is the word for a turning the patch
already has. Proposed, in the stdlib, written in aither, in the
classical words (below): **`transit(z)`**, 1 on the step a turning
crosses its starting line, else 0:

- a pair (`spin`, `dho`, a phasor pair): its phase crossing 0 upward;
- a phasor's 0..1: its wrap;
- a pair of pairs: its angle coming back through 0, a versor's whole turn
  (the double cover: a spinor transits once each two turns of what it
  turns).

Form is then the slow turnings and their transits:
`since(transit(slow))` counts steps into a section; `impulse(1 / 8)`
becomes `transit(slow)` with `slow` one revolution in eight seconds'
worth of keynote turns.

**The classical words** (Roland: "we should use classical language"):

- **revolution**: one whole turn, the astronomers' word since Copernicus
  (*De revolutionibus*, 1543). For a count of whole turns.
- **transit**: in astronomy, a body's passage across the meridian, its
  reference line. That is exactly a turning crossing its start, so it
  names the event.
- **period**: the length of one revolution, Tesla's word for a circuit's
  natural time and Maxwell's for a vibration. It is a length, not an
  event.
- **phase** and **epoch**: in the mechanics of harmonic motion (Thomson
  and Tait, *Treatise on Natural Philosophy*, 1867), the phase is how far
  round a motion is, and the epoch where it stood at the reckoning's
  start. aither has `phase`; the epoch is a turning's first value.

So the event is `transit`, and if a count of whole turns is wanted,
`revolutions(z)`. (Usages are as remembered and not yet checked against
the originals: check Thomson and Tait's definitions of phase and epoch,
and Tesla's use of period, before SPEC quotes them.)

## sweep, a voice's own count

Yes. Today `sweep` is `t` less the moment it was last set, in seconds:
a double that grows, set from outside with `aither sweep <voice>
<seconds>`. Step first, it is **the voice's count of steps since it was
last set**, kept by the engine as an integer (exact for ever, where a
double second count is not), set from outside at the edge (in seconds
there, or turns of the keynote), and read inside as a count. `t` is then
the same kind of thing: the engine's count, the sweep of everything. Two
counts, one kind, no clock; and a poly voice's own age is `since` its
note began, already a count.

## Open questions

1. Pitch: settled above as a direction (ratios inside, Hz at the edge);
   open is how a patch names its keynote, and whether a resonator's own
   make (stiffness and mass) is a stdlib form.
2. `transit` and `revolutions`: confirm the classical usages from the
   sources.
3. Whether `t` stays as a name at all once `sweep` and `since` are
   counts.
