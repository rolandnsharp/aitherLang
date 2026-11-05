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

## Open questions

1. Pitch: Hz at the edges only, ratios inside, or ratios throughout?
2. A slow phasor for form: is there a stdlib word for "each time it comes
   round" (an edge on a phasor's wrap), or is `since` on a phasor enough?
3. `sweep` (a voice's own time from its start): it becomes the voice's
   step count, which a poly voice already has a start for.
