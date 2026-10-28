# Field: an analogue computing library for aither

Moved here from `~/aitheranalogueos` on 2026-10-28; the libraries are
in `patches/lib/`. Its A/B patches used `dho_v` and were dropped; the
result is in [field-ab.md](field-ab.md).

**A classical-field computing library and bench for aither.** Started
2026-10-26 (Roland, with Claude). Not an OS, despite the name: every
part here is plain aither, imported by any patch and run on the normal
engine.

```
import "lib/field.aither"        # from a patch in patches/

summer(0.5, 0.5)                 # analogue addition: a part, with a speed and rails
ring(440, 0, 0.7)                # two integrators and an inverter: a cosine and a sine
coupled(440, 440.5, 0.002)       # two poles on one spring, trading their energy
```

`a + b` stays exact. When a patch wants a physical part it names it,
so what you read is what runs, and each place chooses: exact where the
music wants grit, a part where it wants one.

## What is here

- **`patches/lib/field.aither`**: analogue parts and oscillators as named defs:
  `rail`, `amplifier`, `summer`, `inverter`, `multiplier`,
  `integrator`; `ring`, `analogue_vdp`, `coupled`. [field-plan.md](field-plan.md) says what comes next
  (locking, parametron logic, memory, learning).
- **`patches/lib/analogue.aither`**: aither's filters and drive as circuits
  (saturating integrators, tolerance, drift, an asymmetric stage), and
  [field-ab.md](field-ab.md), the blind A/B on Roland's didge: the original was preferred,
  "more grungy and raw".
- **Examples:** `patches/field_ring`, `field_vdp`, `field_coupled`, `field_summer`.

```
aither patches/field_coupled.aither | tools/scope | tools/play hw:rockchipes8388c 20
```

## The rule

> **Inexactness comes from dynamics, not decoration.** Every part is a
> process with a speed, limits and neighbours. Noise appears only where
> the physics requires it, explicit and seeded.

A simulation's arithmetic is exact; adding noise would only make it
look analogue. Real analogue inexactness is time (a summer moving
toward its sum at its bandwidth), limits (rails, finite Q) and
coupling (parts pulling on each other). `summer(0.5, 0.5)` settles to
0.976, not from noise but because 1 sits in the amplifier's knee below
its rail. Seeded noise and deterministic physics also mean the same
patch replays the same, imperfections included.

The vocabulary is classical field theory throughout: fields,
waveforms, phasors, resonance, waves adding (aitherLang's
MANIFESTO.md), not the quantum sense of "wave function".

## Where it comes from: the parametron

The parametron computers of 1950s Japan had one logic element, an
oscillator. A bit is a phase: a parametric oscillator pumped at twice
its frequency settles into phase 0 or π. Logic is a sum of waves:
three oscillators feed a fourth, which locks to the majority's phase;
majority and inversion make all of logic. Eiichi Goto, 1954; the
University of Tokyo's PC-1 (1958), the Musasino-1. Literature
(OpenAlex, 2026-10-26):

- Onyshkevych, Kosonocky and Lo (RCA), 1959: the parametric
  phase-locked oscillator "as the sole component in a digital computer
  system". https://doi.org/10.1109/tec.1959.5222684
- Nosan et al., 2019: gate-controlled phase switching in a parametron.
  https://doi.org/10.1103/physreve.99.062205
- Takesue et al., 2022: coherent Ising machines of 100 000 optical
  parametric oscillators. https://doi.org/10.23919/oecc/psc53152.2022.9850000
- Soma and Soma, 2009 and 2014: a ternary parametron, logic as "a
  majority operation for vector sum of input signals".
  https://doi.org/10.1109/ismvl.2009.25
- Chou et al., 2019: an analogue coupled-oscillator Ising machine.
  https://doi.org/10.1038/s41598-019-49699-5
- Todri-Sanial et al., 2025: physical computing with coupled
  oscillators. https://doi.org/10.1109/iedm50572.2025.11353846

aither's pair is a complex number, so a phasor sum is aither's own
arithmetic, and a ternary parametron is three phases of one pair.

## Relation to the other repos

- `~/aitherLang`: the language, used as it is.
- `~/aitheros`: the simulated machine, the tape, the pipeline
  (`tape kbd |> keys.aither |> patch.aither |> (speaker, scope)`).
  Anything here can run through it as a stage.
