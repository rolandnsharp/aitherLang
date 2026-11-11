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

## Two layers (decided 2026-11-11)

Roland asked whether this is two projects: aither grew as a language that runs
a function of `t` and of state, sample by sample, line after line, so any
signal-processing paradigm fits it; a field theory is a system of equations
with no order, which a solver holds as modes, delay lines or a grid and
couples exactly. Most of this week's bugs lived where the second was lowered
into the first (`imports.c`, about 1600 lines rewriting the syntax tree).

Not two projects, and nothing deleted: the live engine, `send`, rollback,
plays, MIDI, the pad, `scope`, `tv`, export, rhythm and effects are what a
field-theory bush doof is played with, and none of them is field theory. Two
layers in one project:

1. **The field core, a module of its own** (`medium/`). In: the laws, `∇`,
   `Δ`, `δ`, the domains (`ring`, `line`, `sheet`, a point), as a system of
   equations. Out: a compiled solver, each medium held as it is best computed
   (modes for a linear one, delay lines for a uniform moving string, a grid
   for a nonlinear law or a `matched` end, its places chosen for accuracy and
   the same at every rate), stepped as one moment a sample, with what couples
   inside it coupled exactly. Its own suite of known answers and convergence
   from the first day. It never goes through aither's line-by-line lowering.
2. **aither, the live environment**, hands a patch's field system to the core
   whole. Its signals drive the core as forcing; reads come back. **The
   boundary is one sample of memory**, said in SPEC with its size; whatever
   must push back exactly (a wake, lips) is written as a law, inside the
   medium, on the exact side of it.
3. `imports.c`'s field lowering goes, replaced by the core. Patches keep
   their syntax: `string_keys` reads the same and sounds correct.

**Types across it.** The output stays one pair, `[l, r]`: two channels, one
complex number, what the scope draws; the least a stereo instrument can be.
The core works in pairs, as a mode is a pair turned exactly each sample, and in
pairs of pairs only for a medium with a direction at each place
(`aether_ring`). What crosses the boundary is a read, a number or a pair, and
a force back. A bush doof patch can be numbers to its `[l, r]` and never meet
a pair of pairs.

**Where it is built.** On the branch `field-core`, in its own worktree
(`~/aitherLang-core`), so `main` stays playable and the other session's
checkout isn't switched under it. `main` takes it when it beats the lowering
on the suite and plays every field patch; only then does the old code go.

## Milestone 1: the numerics, proved (2026-11-11)

A prototype of the core in C (`medium/modes.c`, on `field-core`): a line held
as modes, each a pair stepped exactly (its 2x2 exponential worked out once),
and everything else coupled to it by **Strang splitting**: each medium turns
half a step on its own, exactly; the forces between them (and any law on
plain state, by RK4) act as one kick; each medium turns the other half.

| case | today's engine | the prototype |
|---|---|---|
| struck string, 12 partials (`string_proto`) | 9.68 cents off, partials 6 up gone | 0.0000 cents, within 0.011 dB, 218 modes at 48 kHz |
| two springs, lossless (`springs_proto split`) | `dho` pair blows up | energy flat to 0.0000 dB, modes within 0.003 cents |
| wake on the string, 48 to 384 kHz (`wake_proto kick`) | 335.3 to 329.8 Hz: 28.8 cents | 329.757 Hz at every rate: 0.01 cents |

**What the wake's trouble really was.** The prototype with aither's step for a
law on plain state (velocity, then place: `wake_proto lag-euler`) gives 335.28,
334.43, 329.81, 329.79 Hz, aither's own numbers to the hertz; with RK4 and the
same one-sample lag, 0.03 cents. So the lock was lost to a first-order step on
a nonlinear law, not to the sample between the wake and the string. The lag
does matter for a lossless linear pair (two exact springs, lagged, gain 79 dB
in 2 s at 48 kHz); split, they hold. The core needs both: a high-order step
for nonlinear laws, and the splitting for what couples.

Cost: a mode is about six multiplies a step; the 100-mode loop ran at 0.02 of
a core. The wake's RK4 is four evaluations of its law a sample.

## The boundary: how a patch reaches the core (a design, 2026-11-11)

The prototype is C on its own; this is how a patch's laws get to it and how its
step runs inside a voice, so that `grid_string.aither` and `grid_wake.aither`
themselves pass. Nothing here changes what a patch says.

### 1. The system is found, not marked

The compiler takes the patch apart into two sides, from what reads what:

- **Inside, the system**: every field (`ring`, `line`, `sheet`) and every law,
  on a field or on plain state, and every expression a law uses that reads
  the system's state, directly or through `let`s. `let moving = $v(0.37)`,
  read by the wake's law, is inside: it is coupling, and the core evaluates it
  where its step needs it. So is the wake's `$q` in the string's δ term.
- **Outside, everything else**: what a law uses that doesn't read the system
  (`blow`, a key's gate, `phasor`, MIDI, a `dho` that isn't a law). It is
  forcing, worked out once a sample before the step.

Nobody writes the line between them; it falls out of the reads. A `let` can
be inside for one law and outside for another only if it reads nothing of the
system, so a `let` is on one side.

### 2. One moment a sample, and one place it happens

Each sample the voice runs, in order the core sets and the patch can't
change:

1. **Outside, before**: the forcing (outside expressions laws read), with every
   read of a field or of a law's state seeing **the start of the moment**.
2. **The step**, the whole system together, Strang split:
   - each medium turns half a sample on its own, exactly (modes; or delay
     lines, or a grid's own step, below);
   - the inside expressions are evaluated on that midpoint state, the laws on
     plain state step by RK4 (exactly, when linear with fixed coefficients),
     and every force between them lands as one kick;
   - each medium turns the other half.
3. **Outside, after**: everything else, the output among it, again reading
   the start of the moment.

So everything outside the system sees one state, the moment's start, wherever
its line stands. The sound is the system a sample after it was made, which no
ear or test can hear. Where outside code has memory and closes a loop through
the system (today's didge: an `lp1` reading the bore, a `dho` lips pushing it),
that loop has one sample of lag, and SPEC says so, with what it costs (a
lagged lossless pair gains energy, 79 dB in 2 s at 48 kHz in the prototype;
a damped one shifts its pitch). The cure is to write that memory as a law:
then it is inside.

### 3. What the core makes of a medium

For each medium the core reads its laws and picks:

| the law | ends | held as |
|---|---|---|
| linear in the fields, coefficients from outside | `fixed` or `free`, a `ring`, a rectangular `sheet` | **modes**, exact |
| the same, `width` set (a horn) | as above | modes, worked out numerically once (Sturm-Liouville) |
| a uniform pair whose period moves | any but `matched` | **delay lines**, as now (exact, cheaper than modes that retune) |
| nonlinear in the fields (`$x ^ 2`), or a `matched` end | any | **a grid**, its places chosen for the accuracy asked and the same at every rate |

"Linear" is checked on the tree: each term is an outside factor times a field,
a slope of one (`∇`, `Δ`, `ΔΔ`), or a δ term. A coefficient that changes in time
(a key's felt changing the loss) keeps modes: the loss factor is one `exp` a
sample shared by all modes, the turn precomputed. A period that moves goes to
delay lines. The engine can say which it chose (`aither check`), and the patch
never has to know.

How many modes: those under Nyquist, at most the cap a patch can afford
(`modes = n` on the domain, a default from cost). Modes left out are said, not
hidden: they are the part of the spectrum not simulated.

### 4. How it is built

- A new pass, `system_pass`, replaces `fields_pass`, `plain_laws` and the
  def write-out's stepping. It splits the program as in 1, builds the system
  as data (media, laws, inside expressions, sources, reads), and hands it to
  `medium/`.
- `medium/` decides the representations (3) and returns **one block**: the
  step, as aither tree with calls into native helpers (`modes_step`,
  `modes_kick`, `modes_at`, RK4's stages written out). It goes in one place,
  between the outside-before and outside-after code. Positions in the user's
  file stop meaning anything to it.
- The natives live in `medium/`, claim their state from the voice's pool as
  `dsp.c`'s do, and name their regions (`modes:string:v`), so a hot reload
  keeps a string ringing when it is edited: modes carry over by index, places
  by position, live coding as it is.
- Codegen doesn't change: it compiles the block like any other aither.

### 5. What stays out of milestone 1

Delay lines and grids stay as they are (the old lowering, behind the new
pass); defs holding fields stay written out; sheets and bars wait. Milestone 1
is: a `line` under a linear law, as modes, and laws on plain state, coupled as
in 2, so the suite's `string_partials` and `wake_on_grid` pass from the patches.

### Decisions this asks for

1. Everything outside the system reads the start of the moment, the output
   included (a sample later than today).
2. The inside/outside line falls out of the reads; nothing marks it.
3. The core returns an aither block with native helpers, rather than raw C,
   so codegen, hot reload and export keep working as they are.
4. A mode cap per medium, said when it bites.

## Plan

1. **The suite**: known answers and convergence, run on today's engine first,
   so what fails is on record. Its first cases are these experiments: coupled
   springs (laws, and `dho` through `$state`), a struck string's partials, the
   didge across rates, the wake's lock across rates.
2. **The core, first milestone**: one linear `line` held as modes, and one law
   on plain state coupled to it (the wake), passing the suite's string and
   wake cases: exact partials, and the lock the same at 48 kHz as at 384.
3. **Into aither**: a patch's field system handed to the core, the boundary as
   said; the field patches moved over and checked by the suite and by ear.
4. **The rest of the media**: delay lines for moving strings, grids for
   nonlinear laws and `matched` ends, sheets, bars.
5. **Memory as laws**: `dho`, `lp1` and the filters in stdlib as laws, stepped
   exactly; the coupled-`dho` springs then hold their energy.
6. **Fields of a call**, then `string_keys` on `midi_keyboard`.

Each step a commit with SPEC and COMPOSING.
