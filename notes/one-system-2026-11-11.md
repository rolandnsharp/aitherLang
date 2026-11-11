# One system: what correct means for a field language

**Status (2026-11-11): a design, for discussion, with two experiments run.
Nothing built.** Roland: "do we need to do a higher order refactor to simplify
before we go down rabbit holes trying to fix bugs that might be occurring
because we are not thinking about this field first simple and elegant"; "this
is a scientific language for simulation. we have to be correct"; and "is the
code that isnt the law holding us back?" Patches needn't stay bit-identical to
what they were, but they must be right.

aither is becoming a field-theory lab for audio, visual and radio
experiments, changed while they run, and still the language a field-theory
bush doof is live-coded in. Both ask the same of it: an equation means one
thing, and the sound is that equation's, not an accident of how it was typed.

## What went wrong

Almost every field bug of 2026-11-07 to 11-11 came from one rule (SPEC
"Fields"): *a block's laws are one system, stepped where the last of them
stands; a line before it reads the fields as they were, a line after it reads
them stepped.* That gives a field a position in the file:

- whether a read sees this sample or the last depends on which side of the
  last law it is written;
- sources had to be moved after the last law (`settle_sources`, parser.c) or
  the order laws were written in showed (`laws_have_no_order` caught it);
- two calls of one def came out a sample apart, because the first call's read
  stood above the second call's laws (556c1cd fixed it by telling calls apart
  by the number at the end of their written-out names: it works, and it is
  fragile);
- `+=` on rings, the first thing changed in this line of work, was the same.

The same equations giving different numbers by where a line stands is wrong
in a simulation language, not a style question.

## Where the error really is: memory outside the laws

Code that isn't a law is of three kinds, and only one does harm:

1. **No memory**: arithmetic, `tanh`, `let`, `if`. A function of the moment,
   with no time in it. Harmless anywhere.
2. **One-way inputs**: MIDI, envelopes of `t`, `noise`, `input()`, a phasor
   driving something. The equations' forcing terms; nothing feeds back into
   them. Harmless.
3. **Memory**: `dho`, `resonator`, `lp1`, `hpf`/`bpf`, `slew`, `discharge`,
   `delay`, `reverb`. Each is a differential equation with its own private
   step. In a feedback loop with laws (the didge's lips are a `dho` reading the
   bore and pushing it), the loop is cut in two, each half stepped its own way,
   a sample of state between them. A delay in a feedback loop is a phase
   error, and a phase error in a loop feeds energy in or drains it.

## Two experiments (2026-11-11, today's engine)

The patches and scripts are in `notes/one-system/`: `python3 score.py
laws.aither dhos.aither` and `python3 pitch.py didge_laws.aither` (the `dho`
form is `patches/field_didge.aither` as of 556c1cd).

**Two coupled springs**, 440 Hz each, coupled so the second mode is 466.16 Hz,
lossless, spring 1 kicked: the exact answer is known. Each mode's pitch error
and its level over 2 s:

| Written as | 48 kHz | 96 kHz | 192 kHz |
|---|---|---|---|
| laws (four lines) | +0.24 cents, level flat | +0.06 cents, flat | +0.015 cents, flat |
| two `dho`, coupled through `$state` | grows, muted by 0.5 s | grows 42 dB a second | grows 21 dB a second |

The laws converge as their method should, the error a quarter for each halving
of the step, and hold energy. The two exact oscillators, each perfect alone,
gain energy through the sample between them, at a rate that halves as the step
halves: a lossless system that blows up is the boundary, measured.

**The didge**: the lips (a `dho`) and the pressure across them (an `lp1`) as
they are, against both written as laws (still stepped apart from the bore's
field laws, which today's engine steps separately). The note it sounds:

| Lips as | 48 kHz | 96 kHz | 192 kHz |
|---|---|---|---|
| `dho` and `lp1` | 73.867 Hz | 74.015 Hz | 74.252 Hz |
| laws | 73.926 Hz | 74.044 Hz | 74.252 Hz |

The lips' form moves it about 1.4 cents at 48 kHz and nothing at 192: small
here, where the lips are well damped. **But the bore itself moves 9 cents
between 48 and 192 kHz, and the steps grow rather than shrink**: the field
isn't converging on one answer as the rate rises. SPEC says a patch "has the
same motion at any rate"; for this one it doesn't, quite. That is a separate
bug, in how a fine field picks its places, and it comes first.

**The bore's drift** is its grid. A field too short for a place a sample is
given about 32 places at any rate, and rounding makes it coarser as the rate
rises (42, 35, 30 places at 48, 96, 192 kHz), so the error grows with the rate.
With a minimum of 128 places the didge sounds 73.826 Hz at 48 and 96 kHz alike:
today's grid is 1 cent sharp at 48 kHz, 4 at 96 and 10 at 192.

## Modes: a third experiment

A string, fixed ends, 110 Hz, struck at 0.3 for 0.5 ms, heard at 0.13, against
the exact answer (partials at n · 110 Hz, levels from the strike and the
pickup's places): today's grid string, and the same string as 100 modes, each
a `dho` struck and read by its own shape, sin(nπp) (`grid_string.aither`,
`modal_string.aither`, `python3 partials.py ...`).

| | partials' pitch | partials' level against exact | 48 / 96 / 192 kHz |
|---|---|---|---|
| grid | flat by up to 9.7 cents (the 5th) | from the 6th up 68 to 140 dB under: gone | much the same |
| modes | exact, 0.00 cents | within 0.01 dB | the same |

On its 32-odd places the grid string at 110 Hz has about five partials. The
modal string is the exact answer at every rate. Cost: the grid 0.09 of a core,
the modes as written in aither 0.40 (each `dho` works out its turn every
sample); turned natively, a complex multiply each, 100 modes are 0.02 of a core
(a C loop on the uConsole).

**A wake coupled to each** (the harp's van der Pol, near the 3rd harmonic, at
0.37; `grid_wake.aither`, `modal_wake.aither`, `python3 lock.py ...`). No exact
answer, so the question is whether the answer settles as the step shrinks:

| | 48 kHz | 96 kHz | 192 kHz | 384 kHz |
|---|---|---|---|---|
| grid | free, 335.0 Hz | locked, 329.1 | locked, 329.3 | free, 335.4, string near silent |
| modes | free, 335.3 | free, 335.0 | locked, 329.80 | locked, 329.78 |

The modal string settles: locked at 329.8 Hz, within a cent of its exact 3rd
partial. At 48 and 96 kHz it misses the lock, from the one sample of lag
between wake and string, about 2.5 degrees at 330 Hz at 48 kHz. The grid
doesn't settle at all. So the representation and the coupling are both
correctness, separately: modes make the medium exact, and the coupling has to
be made exact too, or the physics depends on the sample rate.

## The design

1. **All of a patch's laws are one system**, on fields and on plain state,
   stepped together each sample by one integrator, with no order in the file.
   Coupling between laws is then inside the step, as the terms within a field
   are, not across a sample.
2. **Anything with memory is a law.** `dho`, `lp1`, the filters, `slew`,
   `discharge` are written in stdlib as the equations they are (a `dho` is
   `$v' = force - $x * w ^ 2 - $v * 2 * damp * w`, `$x' = $v`), so a patch is
   one system of equations with memoryless code between and inputs driving it.
3. **The engine chooses how to solve each law**, as it already chooses how to
   hold a field:
   - a linear medium is held as its modes, each a pair turned exactly (the
     same thing as stepping a linear law by its matrix exponential): exact
     partials, exact δ and reads at points, loss by pitch per mode, nothing
     above Nyquist, at any rate, for about a fifth of a grid's cost;
   - a linear law with fixed coefficients on plain state (a spring, a filter)
     is stepped exactly the same way, as `dho` is today, so nothing is lost;
   - a nonlinear law (a wake, a stiffening string) by the stable method it has
     now, velocity before position (symplectic Euler);
   - a delay, a reverb, a moving string: delay lines, a known medium computed
     the cheap way.
4. **Correct is measured against physics, not old output.** A suite of known
   answers (coupled springs, a string's partials, a bar's modes, energy held
   with no loss) and convergence: every field patch at 48, 96 and 192 kHz,
   approaching one answer at the rate its method promises. A patch that
   doesn't converge is a bug, whatever it sounds like.

What remains a sample of memory: code with memory that isn't (yet) a law, and
any loop through memoryless code, which needs one. SPEC says so, with its size.

For live coding nothing in the patch changes but what it means: `dho`,
`resonator` and the filters read the same, cost about the same, and a change
sent mid-set lands in a system whose answer is the equations'.

## Fields that belong to a call

**A call of a def has its fields, as it has its `$state`.** Instead of copying
the def's text out, each call gets its own instance, laid out as a def's state
already is per call site and as `poly` lays out a voice's. Then a def holding
a field works anywhere a def does, `midi_keyboard` voices included, and needs
no stepping position of its own. Memory is the cost: a moving string holds two
delay buffers of 131072 cells, 2 MB, so eight voices are 16 MB (buffers could
be sized to the lowest note a call reaches).

## Open questions

1. Arrays assigned bare (`$x = $x + 1`) are per-place lines, not laws, with
   position by design. Keep them outside the system, said so, or retire them?
2. A law could read a `let` written below it, since everything it reads is the
   moment's. Allow it, for laws only?
3. `reverb` and `delay` are media too; are they laws now, or later?

## Plan, each step a commit with SPEC and COMPOSING

1. **The test suite of known answers and convergence**, run on today's engine
   first, so what fails is on record before anything changes: these
   experiments are its first cases.
2. **Modes for linear media**: `line`, `ring` and `sheet` under a linear law
   held as modes by the engine; the grid kept for nonlinear laws and
   `matched` ends, with its places chosen for accuracy and held the same at
   every rate. Then **the coupling**: a field and a law on plain state that
   push each other stepped so the answer doesn't depend on the rate, measured
   by the wake settling at 48 kHz where it settles at 384.
3. **One system**: laws on fields and plain state stepped together; the
   position rules, `settle_sources` and the per-call grouping go. Expected:
   reads below the laws shift a sample, and the suite says whether answers
   got closer to the physics.
4. **Memory as laws**: `dho`, `lp1` and the filters in stdlib as laws, with
   exact stepping for linear ones; the coupled-`dho` springs then hold
   their energy.
5. **Fields of a call**, then `string_keys` on `midi_keyboard`.
