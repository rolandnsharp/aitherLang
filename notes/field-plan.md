# The plan

**Rewritten 2026-10-26.** The first plan made this a separate machine
with its own OS and a front end that turned `+` into a summing
junction. Both went: everything it computed can be aither library code
on the normal engine, and analogue addition is a named def,
`summer(a, b)`, chosen where a patch wants it. What remains is a
library, a bench and experiments.

## What it is for

What physical systems do well, and a library can explore:

1. **An instrument:** computing you can hear. The adder chirping into
   its answer, an optimiser settling like a slowing bell, a memory
   recalled as a chord locking, harmony forming as detuned oscillators
   pull into tune.
2. **An ear** for Zara: a resonator bank is how a cochlea works.
3. **Associative memory:** a phrase, a hum or a voice recalled by a
   field synchronising to its nearest stored pattern.
4. **Optimisation by settling:** small constraint problems as an
   oscillator Ising machine.
5. **A research bench** for classical-field computing and Vidya:
   oscillator models on their native substrate, watched and heard.
6. **Teaching** how computing comes out of physics.

Not exact work: that is aither's `+`, and aitherOS.

## Built

`field.aither`, checked by rendering:

- `summer(0.5, 0.5)` settles to 0.976, in the amplifier's knee;
  `summer(1, 1)` meets the rail. With a slow amplifier the rise is
  seen.
- `ring(440, 0, 0.7)`: 440.1 Hz, a cosine and a sine, a circle on the
  scope.
- `vdp(220, 0.05)`: spirals out from a tiny start to its own circle.
- `coupled(f1, f2, k)`, rendered and measured per 50 ms:
  - 440 and 440.5 Hz, `k` 0.002: the energy passes fully across and
    back about once a second.
  - 440 and 443, `k` 0.002: detuned past the spring, the second only
    borrows a little.
  - 440 and 443, `k` 0.02: a stronger spring, and partial exchange
    returns on a slower cycle.

`analogue.aither` and its A/B ([field-ab.md](field-ab.md)): blind, the original
didge was preferred; the circuits softened its grit, at 80% more CPU.

## Next

| step | builds | done when |
|---|---|---|
| 1 | **locking**: two `vdp`s on a spring | detuned self-sustained oscillators pull into one pitch: heard as the beat slowing to nothing, seen as the xy figure stopping still |
| 2 | **the bench**: `meter` (a needle with inertia, the exact number under it), `sweep` (the scope in time mode, triggered), `poles` (the pole plane, computed from the wiring) beside `tools/scope` | `summer(0.5, 0.5)` swings the needle and sits; `coupled` shows its two poles splitting as `k` rises |
| 3 | **parametron logic**: `parametron(pump, inputs)` from a pumped oscillator; `majority`, NOT, AND/OR; a full adder; a 4-bit `adder` | `adder(1, 1)` settles to 2, heard as the bits chirp into their phases; error rate and time per add measured against noise and Q |
| 4 | **memory**: a pumped parametron register; a recirculating delay line that fades unless refreshed | a value held, and forgotten when let go |
| 5 | **fuzzy**: phasor degrees, deciding late | fuzzy and binary compared on one task (a noisy key, recognised by its sound) |
| 6 | **learning**, in order: a reservoir with a trained readout (the ear for Zara); plasticity (couplings that strengthen when oscillators lock: associative memory); equilibrium propagation (settle, nudge, settle, update) | each an experiment on experiments.blah.dev, pre-registered before compute is spent |

Later, only if it earns its place: a front end that makes every
operator a physical part, for when a claim must be "this could run on
real oscillator hardware".

## Expectations

- **Cost.** N oscillators cost N times the sample rate. A parametron
  bit at a 1 kHz carrier settles in about 20 ms: a gate decides some
  50 times a second, every decision audible. Fine for an instrument
  and a bench; the uConsole on battery limits how many.
- **Small learning.** Physical neural networks are "at present
  confined to small-scale laboratory demonstrations" (Momeni et al.,
  Nature 2025, https://doi.org/10.1038/s41586-025-09384-2). Here:
  vowels, keywords, hundreds of patterns. Vidya at scale stays on
  GPUs; this is where oscillator learning is understood and heard.
- **Fuzzy chains drift.** Parametron majority restores levels, which
  is why Goto's machines worked; where to restore and where to stay
  fuzzy is the design question.

## Literature for learning

- Seliger, Young and Tsimring, 2002: plasticity in coupled phase
  oscillators. https://doi.org/10.1103/physreve.65.041906
- Hoppensteadt and Izhikevich: associative memory of weakly connected
  oscillators. https://doi.org/10.1109/icnn.1997.616190
- Wang, Wanjura and Marquardt, 2024: coupled phase oscillators trained
  with equilibrium propagation. https://doi.org/10.1088/2634-4386/ad752b
- Rageau and Grollier, 2025: training and synchronising oscillator
  networks with equilibrium propagation.
  https://doi.org/10.1088/2634-4386/adebaa
- Wright et al., Nature 2022: deep physical neural networks trained
  with physics-aware backpropagation.
  https://doi.org/10.1038/s41586-021-04223-6
- Momeni et al., Nature 2025: training of physical neural networks, a
  review. https://doi.org/10.1038/s41586-025-09384-2
