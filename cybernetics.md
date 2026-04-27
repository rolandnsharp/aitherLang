# Cybernetics & aither

A running notebook on the cybernetic-synthesis tradition (David Tudor,
the "new uses for old circuits" school, the al-Mukabala / al-Jabar
patching practice) and what it means for aither's design.

This is a parked-thinking doc. Not a roadmap. Notes accumulate as we
watch / read / listen, and the design implications crystallise over
time.

## The core thesis of cybernetic synthesis

The musician is not the composer. **The system is the composer.** The
musician's job is to set up a self-organising network whose dynamics
produce sound, then ride parameters to *guide* that system rather
than to *specify* the music it makes.

This is cybernetics in the Wiener / Beer sense — control via feedback,
emergence via coupled nonlinearities, regulation via observation of
the system's own behaviour. The musician gives up the "illusion of
control" (the host's exact phrase) in exchange for genuine
*organisation* — sounds that have a life the patcher couldn't have
specified directly.

## The worldview behind the practice

The channel (*Les Sons Humains* / "Los Sons Humane" by ear) opens
with a manifesto episode — the host's musical name is Gunnar Haslam
in one mode, Sky Hobbsbalm in another, Emil Zenner for the
explicitly cybernetic work — that lays out the philosophical frame
for the whole project. It's worth holding onto because it answers
*why* cybernetic synthesis matters, not just *how* to do it.

Three definitions of cybernetics get cited:

- **Pynchon** (*Gravity's Rainbow*): "If you want the truth, you
  must look into the technology of these matters, even into the
  hearts of certain molecules. It is they, after all, which dictate
  temperatures, pressures, rates of flow, costs, profits, the
  shapes of towers." The two questions Pynchon (via Walter Rathenau)
  poses are exactly the cybernetic questions: **what is the real
  nature of synthesis, and what is the real nature of control?**
- **Wiener** (*Cybernetics*, 1948, subtitle "Control and
  Communication in the Animal and the Machine"): the science of
  systems that use feedback to adapt to their environment. The
  founding text. Wiener's later book, *The Human Use of Human
  Beings*, extends the framing to society — the warning that the
  second industrial revolution devalues human cognition the way
  the first devalued human muscle, and the call for "a society
  based on human values other than buying or selling."
- **Roland Kayn** (in the liner notes to his 1970s breakthrough
  *Tektra* / *Symbolton*): cybernetic music is *the suspension of
  the dichotomy between automatic/dead and anthropogenic/live
  systems*. It's neither pure improvisation (live) nor pure
  algorithm (dead) — it's a dialectic between them.

The host extends Kayn's framing into a sharp political claim that
matters for what cybernetic synthesis is *for*:

- **Machine learning, in the way it's used today, is a dead system.**
  It's driven by stochastic-gradient-descent optimisation toward a
  metric. There's no feedback adaptation in the cybernetic sense —
  there's just objective minimisation. Spotify's algorithmic
  playlists are dead systems in this sense; they replicate what came
  before, optimised against engagement metrics.
- **Capitalism is a dead system in the same way.** Marx in
  *Capital* describes how the capitalist, in the moment they enter
  production, dons "the robe of the capitalist" — their humanity is
  put aside and they become driven by the algorithmic logic of
  surplus-value extraction.
- **Cybernetic synthesis is a counter-move.** It's a way of
  imbuing autonomous systems with the *liveliness found in nature*
  — the kind of nonlinear feedback that biological systems use, and
  that analog computers (which are making a recent comeback in
  neuromorphic engineering) replicate better than digital ones can.

The aesthetic claim: cybernetic music is the *dialectic between
free improvisation and autonomous algorithmic generation*. Pure
improv is human; pure algorithm is dead. The cybernetic system is
the conversation between them — a self-running process that the
musician *guides* rather than commands. The musician's analytic
brain steps back; the sounds come to the musician rather than being
specified by the musician.

This frame matters for aither in two specific ways:

1. **It explains why the "set up dynamics, ride knobs" model is
   philosophically respectable, not just convenient.** The reason a
   live patch with two big knobs feels more *alive* than a fully
   sequenced composition isn't sentimental — it's that the live
   patch is in the cybernetic dialectic, and the sequenced one is
   on the dead-algorithm side of the dichotomy. The live patcher's
   role is to be the human in the human-machine loop, exactly as
   Wiener and Kayn describe.

2. **It anchors the political stance behind aither's design
   choices.** The choice to emit human-readable C from a small
   live-coding language (rather than wrap a heavy framework or
   defer to a black-box ML model) is not just an engineering
   preference. It's a position on what kind of relationship between
   musician and machine is worth building. Aither is a tool for
   the cybernetic dialectic, not for the dead-algorithm model. The
   `f(state) → sample` contract makes the system *legible* — the
   musician can see what they're guiding, not just consume what an
   optimisation found.

The host's closing thesis — that the channel is also about turning
the analytic brain *off* and letting the sounds *come to you*, the
"humane" in *Les Sons Humains* — is a complement to aither's usual
analytic mode. We spend most of our time being precise about
codegen, semantics, primitives. The cybernetic-synthesis tradition
is reminding us that the *point* of all that precision is to
produce systems whose behaviour is then taken in *un*-analytically.
Build the rules with care; let the system surprise you with what
those rules produce.

## The two operative concepts

The "new uses for old circuits" channel keeps coming back to a
specific Arabic-mathematics framing borrowed from Al-Khwarizmi (9th
century, Baghdad — the man who gave us "algebra" itself). His book on
algebra used two complementary terms:

- **Al-Jabar** (الجبر) — "reunion / completion." Grouping like terms,
  combining opposing ones, completing the equation. *Building the
  shape.*
- **Al-Muqabala** (المقابلة) — "balancing / confrontation." The
  balancing-act side of solving. *Keeping the shape stable as it
  evolves.*

These map directly onto two complementary cybernetic moves the patches
deploy:

- **Al-Jabar move:** add an inverted copy of the audio back into the
  feedback path, attempting phase cancellation against the parts of
  the signal that have become regular. If the signal has settled into
  a periodic orbit, the inverted copy cancels it. If the signal is in
  a noisy / chaotic regime, cancellation can't get a grip and the
  signal continues. Result: the system is biased AWAY from regularity,
  TOWARD chaotic exploration. Audio-path tame.
- **Al-Muqabala move:** observe the system (envelope follower,
  derivative, etc.) and use the observation to pull back on the
  *driving* control voltage when the system approaches instability.
  Result: the system is bounded — it can explore chaos but won't
  blow up. Control-path tame.

These are the same idea applied at different abstraction levels: one
acts on the signal directly, one acts on the signal's drivers. Both
are negative-feedback regulators in the cybernetic sense; they differ
only in *what* they're regulating.

## The Al-Muqabala patch (where the framing originated)

Episode 8 of the series introduces the patch the framing was named
after. It's the simplest expression of the cybernetic-synthesis
practice — and worth holding as the canonical reference because every
later, more elaborate patch (al-Jabar, neural synthesis) is a
variation on this skeleton.

**The skeleton:**

1. **Heart** — a feedback loop with two strong nonlinearities
   in series (the host uses two sections of an analog wave-folder /
   rectifier; the abstract ingredient is "two distinct nonlinear
   functions in a closed loop").
2. **Driver** — a slow cycling LFO that walks the bias / parameter of
   the nonlinearities through their range. This is the system's
   exploration energy. Without it, the loop settles into one
   attractor and stays. With it, the loop is constantly being pushed
   toward new attractors.
3. **Observer** — an envelope follower watching the loop's overall
   energy. This is the cybernetic eye.
4. **Regulator** — invert the envelope-follower output and sum it
   into the driver's control voltage. When the loop gets loud (it
   has hit an attractor with high energy), the regulator pulls the
   driver back. When the loop is quiet, the driver is allowed to
   push forward. This is the negative-feedback regulator that
   defines the al-Muqabala move.

**The honest disclaimer the host gives:** "the regulator doesn't
really *work* in the sense of stably parking the system at a
quiet point. But it makes the system *much more interesting* —
the constant push-and-pull between the driver and the observer
produces a richly dynamic motion that you can't get from either
alone."

That last point is the deep insight. Cybernetic regulators in
synthesis aren't there to *achieve* a goal — they're there to
*generate motion* that wouldn't exist without the negative-feedback
coupling. The system never settles; it perpetually *almost* settles,
and that perpetual almost-settling is what produces musical
interest.

**Optional extension** — episode 8 also adds a comparator-triggered
secondary envelope generator. When the observer's envelope passes a
threshold, fire a separate slope into the regulator stack. This is a
gated cybernetic event — the system only reacts to a specific kind
of activity, not to all activity. In aither terms: a `discharge` or
`pluck` triggered by `if envelope > threshold then 1 else 0`,
summed into the driver-modulation chain.

**The "Borges" framing the host gives the patch is worth keeping.**
The patch is a labyrinth — a vast sonic terrain that the driving
voltage tries to map, with the regulator catching it at inflection
points and pulling it back. The musician's job is to ride the
labyrinth. This framing is also why the patch is interesting
musically rather than just sonically: it has *direction* (the
driver), *resistance* (the regulator), and *territory* (the
nonlinear feedback's attractor landscape). Three ingredients minimum
for what feels like motion-with-meaning, not just chaos.

**The aither version** — three voices that match the four roles:

```
$loop = 0.0                                # heart's state
play almuqabala:
  let driver = (sin(TAU * phasor(0.07)) + 1) * 0.5
                                            # 1 cycle per ~14s, 0..1
  $obs = lp1(abs($loop), 5.0)              # envelope follower
  let regulator = max(0, 1 - $obs * 4)     # pull back when loud
  let bias = driver * regulator * 2 - 1    # driven, regulated, ±1

  let stage1 = wavefold($loop * 1.5 + bias)
  let stage2 = tanh(stage1 * 3.0 + bias * 0.5)
  $loop = stage2 * 0.95                    # tiny decay so it doesn't run away

  let s = $loop * 0.3
  [s, s]
```

That's the whole thing. Twelve lines. Two nonlinearities (`wavefold`,
`tanh`) in a closed loop. One driver (the slow `phasor(0.07)`-based
LFO). One observer (`lp1` envelope follower on `abs($loop)`). One
regulator (the inverse-envelope expression). The 0.95 multiplier on
the feedback path is the analog-equivalent of the leakage that any
real circuit has — without it, the loop accumulates DC offsets.

This patch isn't in the repo. Building it as a verified-working aither
demo would be the obvious follow-on experiment to the cybernetic
notes — it's smaller than the Tudor 3-neuron network and proves the
core cybernetic-synthesis idea in the aither idiom before scaling up.

## The logistic map — chaos from a single number

Episode 6 of the series is the foundational chaos episode the rest
of the series rests on. The argument: feed *a single number* back
through a nonlinear transform, and you get the full bifurcation
cascade — periodic orbit, period-doubling, then deterministic chaos
— from one equation, no randomness anywhere.

The equation is the **logistic map**, popularised by mathematical
biologist Robert May in the 1970s as a population-dynamics model:

```
x_{n+1} = λ · x_n · (1 - x_n)
```

`x_n` is normalised to [0, 1] (the "fraction of carrying capacity"
in May's framing — the bacteria-in-a-pond population this year).
`λ` is a feedback-gain parameter. The behaviour as you sweep λ:

- **λ < 3** — single fixed point. The system settles to one value.
- **3 < λ < 3.45** — period-2 orbit. The system alternates between
  two values forever.
- **3.45 < λ < 3.54** — period-4 orbit. Doubled again.
- **3.54 ... 3.57** — successive period-doublings, faster and
  faster. The doubling is itself a self-similar pattern (Feigenbaum
  found a universal constant governing how fast the doubling
  accelerates: ~4.6692, the same for every quadratic map).
- **λ ≈ 3.57 onward** — full chaos. The system never settles. The
  sequence of x values is deterministic but has no period; it visits
  the unit interval in a pattern that never repeats.
- **Above λ ≈ 4** — escape; values blow up.

The deep claim — what made the logistic map a celebrity in 1970s
mathematical biology — is that **a one-line iteration with a
quadratic nonlinearity exhibits the entire menagerie of dynamical
behaviours**. The complexity isn't built in; it emerges from the
iteration of a simple rule.

### What the analog-computing patch is doing

The host patches the logistic map as an analog computation on a
modular synth. The components are exactly what you'd need on a
1960s-era analog computer: addition / subtraction (a CV processor
with offset and inversion), multiplication (an active processor
crossfader, used as a four-quadrant linear multiplier), squaring
(multiplication of a signal by itself), sample-and-hold (the SSG —
the discretising element that makes the loop iterative rather than
continuous), and a clock (to drive the sample-and-hold).

The patch uses the algebraically-equivalent form
`λ · (x_n - x_n²)` because squaring + subtraction is easier to wire
than `x · (1 - x)` (the latter requires creating `1 - x` as a
separate intermediate). Both forms are mathematically identical;
the patcher picks the one that fits the available modules.

The host's honest disclaimer: synths are NOT precise analog
computers. The voltages drift, the multipliers aren't perfectly
linear, the sample-and-holds droop as their capacitors leak. But
**this imprecision doesn't matter for music** — your ear hears a
fluctuating 2.5V offset as a single steady note, not as a series
of slightly-different notes. The patch is "good enough" to exhibit
the qualitative behaviour (period-doubling cascade, chaos) even if
the quantitative numbers don't match a Mathematica simulation.

### What aither already has from this episode

The logistic map is **trivial in aither**. We have addition,
subtraction, multiplication, conditional state updates, and the
`impulse(rate)` clock. The five-line version:

```
$x = 0.5
play logistic:
  let lambda = 1.0 + midi_cc(74) * 3.0   # 1.0..4.0 sweep
  let trig = impulse(8.0)                # 8 Hz iteration clock
  $x = if trig > 0.5 then lambda * $x * (1 - $x) else $x
  let pitch = 100 + $x * 1000            # use the chaos as a parameter
  let s = sin(TAU * phasor(pitch)) * 0.2
  [s, s]
```

Sweep K1 (CC 74) and you walk through the same bifurcation cascade
the host shows. At low λ, single pitch. Past midway, the pitch
alternates between two values per clock tick. Higher, between four.
Past about λ=3.57 (CC 74 ≈ 0.86), full chaos — every clock tick
produces a new pitch in a deterministic but seemingly-random
sequence.

This connects to several things already in aither:

- **The Mandelbrot voice** (`patches/mandelbrot_voice.aither`) is
  the *complex-number* version of the same idea: `z → z² + c`. The
  logistic map is `x → λ · x · (1 − x)`. Both are deterministic
  iterations in a normed space whose long-term behaviour ranges
  from convergence to bounded chaos based on a control parameter.
  The Mandelbrot is just the logistic map's spiritual cousin in ℂ.
- **The Tudor 3-neuron experiment** sketched earlier in this doc is
  the *coupled* version: instead of one chaotic iteration, three
  coupled iterations whose attractors depend on the cross-couplings.
  Same family.
- **The pole(state, drive) proposal** is the *physical* version: a
  resonator iteration that exhibits its own period-doubling and
  chaos when driven hard enough.

Naming this in COMPOSING.md as a documented pattern would be cheap
and high-value. The "use λ as a knob to walk through the
bifurcation cascade" technique is something composers can apply
anywhere they want a single parameter that morphs from stable to
periodic to chaotic. It's the cheapest cybernetic-synthesis
primitive — single state cell, one iteration, one knob, the entire
attractor zoo of nonlinear dynamics.

### The deeper point — synths as analog computers

The host's framing at the end of episode 6 is worth holding onto:

> "What we've got here is a region where the scientist /
> mathematician part of you can be in dialogue with the artistic
> part of you. And through that dialogue, create something really
> exciting and meaningful."

This rhymes exactly with the classical-field-theory-programming-
environment framing in the blog post. Aither *is* this region — a
programming environment where mathematical / physical thinking and
musical / artistic thinking happen in the same medium, with the
same primitives, in the same patches. The host's "use the synth as
an analog computer to model the logistic map" is the same move as
"use aither to model a coupled-resonator chamber" or "use aither
to express Steinmetz's polyphase math." The substrate is sound; the
substance is mathematics.

The episode also closes a circle with the channel's broader thesis:
**chaos in synthesis isn't randomness, it's deterministic complexity**.
A randomly-modulated oscillator produces noise; a logistically-
modulated oscillator produces *structured* noise that the ear can
follow. The structure is in the period-doublings, the windows of
brief order within chaos, the fact that turning the knob slowly
produces a *predictable* path through the bifurcation diagram.
Composers can navigate this terrain meaningfully, in a way that
true randomness doesn't permit.

This is also what aither's `f(state) → sample` contract gives us
naturally that node-based synths have to fight for. The logistic
map needs a single state cell and an arithmetic expression. In a
modular environment it requires a sample-and-hold module, a clock,
an active processor for multiplication, a CV processor for
subtraction, and a feedback patch cable. In aither it's one line.
That ratio (one line vs five modules) is the practical payoff of
the language model.

## Tracking systems vs regulation systems

Episode 7 of the series introduces a cybernetic loop that's
*structurally different* from the al-Muqabala / al-Jabar regulators
above. The al-Muqabala pattern observes a chaotic system and pulls
its drivers back when it gets unstable — a *regulation* loop, where
the goal is "stay bounded." The episode-7 patch is a *tracking* loop,
where the goal is "follow an external target." Same cybernetic
machinery (sense, compare, correct), different objective.

This is Wiener's actual canonical example of cybernetics, more than
the thermostat. *Cybernetics* (1948) was rooted in his WWII work on
anti-aircraft artillery — a missile interceptor that observes an
incoming rocket, predicts where it will be, and steers itself to
intercept. The host's framing is honest about how grim that
genealogy is: synthesizer technology comes downstream of war
surplus (magnetic tape from Nazi battlefields, military-grade
op-amps in the first Moogs and Buchlas), and the cybernetic ideas
we're using for sound came from the same source. The redemptive
move is to take those tools and "redirect them toward something as
full of life as music."

### The tracking loop

The patch in episode 7 is a discrete-time first-order tracker:

```
new_state = current_state + lambda * (target - current_state)
```

`lambda` is the gain — how aggressively the tracker chases the
target. A clock samples the loop at a fixed rate; between samples,
the state holds. Result: a state variable that lags behind the
target, asymptotically catching up. This is a one-pole low-pass
filter expressed as a discrete cybernetic loop.

The musical use the host shows: drive an oscillator's pitch with the
tracker output and have a melodic-sequence target. The tracker
approximates the sequence with adjustable lag — at low `lambda` it
glides between notes (portamento), at high `lambda` it locks tight,
at intermediate `lambda` it produces an interesting "approximating"
voice that follows the leader imperfectly.

### The predictive (PD) tracker

Episode 7 then upgrades to a predictive tracker by adding a velocity
estimate:

```
velocity     = current_state - previous_state
predicted    = current_state + velocity
error        = target - predicted
new_state    = current_state + lambda * error
```

This is the standard Proportional-Derivative (PD) controller from
control theory. It tries to anticipate where the target will be next
based on its current trajectory, and corrects against the
prediction rather than the current value.

In music: a predictive tracker that follows a melody can OVERSHOOT
on big jumps (because it predicts the trajectory will continue) and
RING around stable target values (because it perpetually
under/over-corrects). Both are musically interesting — the
overshoot adds a "chasing" character, and the high-gain ringing
produces a chaotic regime that the host explicitly calls out as a
useful "happy bug."

### What this gives us in aither

Both the simple and predictive trackers are tiny — a few state cells
and a few arithmetic ops. They're not currently primitives; they
should probably stay as patterns in COMPOSING.md rather than become
codegen builtins. Sketches:

```
# P-tracker — first-order asymptotic follower
$state = 0.0
let target = midi_voice_freq()
$state = $state + lambda * (target - $state)
let pitch = $state          # use as a pitch input
```

```
# PD-tracker — predicts forward, can overshoot
$prev  = 0.0
$state = 0.0
let target = midi_voice_freq()
let velocity  = $state - $prev
let predicted = $state + velocity
let error     = target - predicted
$prev  = $state
$state = $state + lambda * error
```

The interesting compositional move here is using `lambda` as a
performance knob. Two voices, one playing the sequence directly and
the other through a PD-tracker with adjustable gain, gives you a
"shadow" voice whose relationship to the leader sweeps from glided
follower (low lambda) to tight unison (right-sized lambda) to
chaotic over-shooter (high lambda) — all on one knob.

This also gives us a natural cybernetic version of glide/portamento.
What synth marketing calls "portamento speed" is essentially the
`lambda` of a P-tracker on the pitch input. Naming it as such
clarifies the relationship between standard synth parameters and the
cybernetic tradition: most "expressive" controls are first-order
trackers in disguise.

### The deeper point — different cybernetic objectives

The episode-7 patch and the episode-8/9 patches are both cybernetic
in Wiener's sense (sense, compare, correct), but they have
*different objectives*:

- **Tracking** — the system has a goal STATE it's trying to match.
  The error is `target - actual`. When the system is at the goal,
  it does nothing. This is the thermostat / missile-interceptor
  family.
- **Regulation of chaos** — the system has a goal RANGE of behavior
  it's trying to keep within. The error is "how unstable are we?"
  not "how far from a target are we?" When the system is well-
  behaved, the regulator does nothing. This is the al-Muqabala
  family.
- **Generation via cancellation** — the system uses negative-
  feedback to bias AWAY from regularity, producing noise / chaos
  on purpose. This is the al-Jabar family (audio-path inversion).

All three are first-class cybernetic patterns, and aither could
support all three with patterns documented in COMPOSING.md and a
small handful of primitives. The tracker is the simplest and most
universally useful — almost every musical idiom needs *something*
that approximates a target with adjustable closeness, and naming it
as a cybernetic tracker rather than as "portamento" or "smooth"
makes the design space more explorable.

## The neural-network reading (David Tudor / "neural synthesis")

Tudor's late work explicitly used neural-network terminology:

- A **neuron** = three things: dendrites (inputs), soma (mixer / sum),
  axon (nonlinearity).
- A **network** = N neurons whose axons feed back into each other's
  somas via a matrix of weighted connections.
- A **matrix mixer** = the weight matrix made physical — N inputs ×
  M outputs, every input has a knob controlling its contribution to
  every output. This is the analog hardware that makes Tudor-style
  patching ergonomic instead of nightmarish.

Slow envelope generators (slope generators acting as one-pole low-pass
filters) tap the network at one point and feed the smoothed signal
back into a *parameter* (a bias voltage, a filter cutoff, a mixer
gain). This is the cybernetic regulator layered on top of the feedback
network — it doesn't disrupt the audio, it just modulates the
parameters that shape the audio's evolution.

## What aither already has from this tradition

Surprisingly much:

- **`f(state) -> sample` with mutable shared state** is *exactly* the
  substrate Tudor's tabletops simulate. State at sample t determines
  state at sample t+1; voices share state cells; nonlinearities
  inserted into feedback paths produce self-organising orbits.
- **Nonlinearities in feedback paths.** `drive`, `wavefold`, `tanh`,
  `discharge`, `pluck`, `pole(state, drive)` (when it lands) are all
  nonlinear functions that, placed in a feedback loop, produce the
  same chaos / order / lock dynamics Tudor was after.
- **Slow LFOs / envelope followers.** `lp1(input, slow_cut)` is a
  slope generator. We just don't typically use it as the cybernetic
  regulator — we use it as a slow modulator of audio parameters.
  Cybernetic use is the same shape with a different role.
- **Phase-cancellation primitives.** Subtraction is just `-`. Inverted
  feedback is `$state = sig - $state * weight`. We have everything;
  we don't have idioms documented.

## What aither does NOT have (and could add)

These are the gaps where adding something small unlocks the
Tudor / al-Jabar style at the language level rather than the patch
level.

### Matrix mixer as a primitive

A `mix_matrix(inputs, weights)` primitive that takes N input signals
and an N×M weights array, and emits M output signals as the matrix
product. Today every Tudor-style 3-voice cross-coupled network has
to be hand-written as 9 multiplications. With a matrix-mixer
primitive, it would be one line:

```
let net_out = mix_matrix([n1, n2, n3], weights)
```

The N inputs are the axon outputs of the neurons; the N outputs are
the soma inputs of the neurons; the weights matrix is the
connection topology. Live-coding a Tudor patch becomes editing the
weights array.

This would be a small codegen change — single primitive, fixed-N at
codegen time (like `additive` or `sum`), unrolls to N*M multiplies
inline. Compiles to the same C as hand-written multiplications, just
with the user-facing concept made explicit.

### A cybernetic-regulator idiom in COMPOSING.md

The al-Jabar pattern (audio-path inversion) and the al-Muqabala
pattern (control-path regulation) are both writable in current aither
but neither is documented. The pattern is small enough to live as
two paragraphs in COMPOSING.md with a code example each.

```
# al-Jabar — audio-path inversion taming
$loop = 0.0
let raw = nonlinear($loop)
let inv = -raw                                 # the inversion
let inv_strength = midi_cc(74)                 # how much to mix in
$loop = raw * 0.7 + inv * inv_strength * 0.3
```

```
# al-Muqabala — control-path observation taming
$loop = 0.0
let raw = nonlinear($loop * drive_cv)
$loop = raw
$obs = lp1(abs(raw), 5.0)                      # envelope follow
let regulated_drive = drive_cv * (1.0 - $obs * 0.4)  # pull back when loud
```

Naming them after the Arabic terms keeps the cybernetic-tradition
provenance visible and gives composers a vocabulary for talking
about what they're doing.

### Derivative / rate-of-change operator

The al-Jabar video computes the derivative of the feedback loop the
analog way: peak detector → low-pass filter → subtract. The result is
a high-pass filtered version of the signal that fires when the
*rate of change* spikes, not when the average level spikes.

Aither could have a `derivative(signal)` primitive that does
`signal - lp1(signal, slow_cut)` in one call. Not strictly necessary
— the user can write the two-line version — but it makes the
cybernetic-regulator idiom one step shorter, and naming the
operation reinforces what it's *for*.

### Preamp-in-the-loop pattern

Tudor inserts a heavy-gain preamp into the feedback path and the
whole network changes character. Aither has `drive(sig, amount)`;
using it inside a feedback loop with the gain pushed past 1.0 to
deliberately destabilise the system is a deliberate
bug-as-feature pattern that should be in COMPOSING.md as a
documented technique.

## The connection to existing design notes

These cybernetic ideas don't need new philosophical scaffolding —
they fit cleanly into what's already in the project's design notes:

- **`pole.md`** — `pole(state, drive)` IS a Tudor neuron. Resonator
  body (soma + axon), shared state cell (matrix-mixer connection
  topology). Multiple `pole` calls sharing state would self-couple
  in real-time, Tudor-style. The monopole-vs-multipole framing is
  asking exactly Tudor's question: when does a resonator behave as
  an isolated voice vs as part of a self-organising network?
- **`bachPolyphase.md` Tesla section** — the "voices coupled through
  a shared field" proposal is a Tudor feedback bus with the
  per-sample mutual-update semantics that analog hardware can't
  quite achieve (the analog version has propagation-time delay;
  aither could have zero-delay or controlled iterative
  convergence).
- **`yingyang.md`** — nested rotations at multiple scales are what
  Tudor's slow-envelope-modulating-fast-feedback produces, but
  organised geometrically. `spin` composed at three rates would
  produce something structurally similar to the long-form
  evolutions in the videos.
- **The blog post's classical-field-theory framing** — Tudor's
  neural networks ARE classical-field-theory devices (nonlinear
  differential equations with feedback couplings settling into
  attractors). Steinmetz's circuits are classical-field-theory
  devices (linear differential equations with energy-storage
  couplings settling into resonant orbits). Aither built right
  expresses both — the rigid conception of ℂ handles Steinmetz,
  the `f(state) -> sample` contract with shared mutable state
  handles Tudor.

The cybernetic-synthesis tradition is one of the strongest existing
musical demonstrations of what a "classical-field-theory programming
environment" is *for*. Tudor wasn't writing compositions; he was
*cultivating field-theoretic systems whose attractors happened to be
musical*. That's the same thing aither patches could be doing.

## A first concrete experiment

3-voice cross-coupled feedback network using only existing primitives:

```
$v1 = 0.0
$v2 = 0.0
$v3 = 0.0

play tudor:
  let mix1 = $v1 * 0.7 + $v2 * 0.4 + $v3 * -0.3
  let mix2 = $v1 * -0.2 + $v2 * 0.6 + $v3 * 0.5
  let mix3 = $v1 * 0.5 + $v2 * -0.4 + $v3 * 0.6

  let n1 = tanh(mix1 * 3.0) |> lpf(800, 4)
  let n2 = wavefold(mix2 * 1.5)
  let n3 = drive(mix3, 8.0)

  $v1 = n1
  $v2 = n2
  $v3 = n3

  let s = (n1 + n2 + n3) * 0.2
  [s, s]
```

Twelve lines. Three nonlinear "neurons." Nine cross-coupling weights.
Live-control any of the weights and the network finds different
attractors. Add slow `lp1` taps that modulate the weights themselves
and you have Tudor's slope-generator cybernetics. Add a couple of
inverted-feedback paths and you have the al-Jabar move. Add envelope
followers feeding back as drive-bias modulators and you have the
al-Muqabala move.

Build this as a patch first. If it produces interesting sound, the
matrix-mixer primitive becomes obviously worth adding because
hand-writing the 9 multiplications gets tedious past N=3.

## The other classical-field-theory programming environment — `resonance-ocaml`

Outside of aither but inside the same head, the user is building
**`resonance-ocaml`** (github.com/rolandnsharp/resonance-ocaml) — a
neural network architecture grounded in classical-field-theory
physics, with three implementations (OCaml, Python, Nim) and custom
CUDA kernels. The architecture is built around a *single* equation:

```
ẍ + 2γ(t)·ω·ẋ + ω²·x  =  β(t)·F(t)
```

That is **the exact same equation aither's `dho` primitive
implements** — the damped harmonic oscillator with time-varying
damping γ(t) and time-varying drive coupling β(t). Steinmetz's
dual-energy circuit. The universal second-order resonator.

Aither uses it to make bells, strings, plates, organ pipes.
Resonance uses banks of them to do sequence modeling — currently
character-level prediction on Shakespeare, with a stated path to
audio + sensor data + multimodal. The stated current results are
BPC 2.72 on Shakespeare with 627K params, training in ~4 minutes
on an RTX 3060.

**The same physics primitive, two domains.** Resonance isn't
"aither's cousin." It is *the same engine in a different application
context*.

### What the resonance architecture actually does

- **State per oscillator**: a 2-component pair `(pos, vel)` that
  rotates over time according to the damped-harmonic-oscillator
  equation. Phase encodes temporal distance. Damping γ(t) and drive
  coupling β(t) are input-dependent (selective).
- **Per-layer structure**: bank → prism. The bank is FFT-based
  oscillator dynamics; the prism is a recombination matrix `W_mix`
  that mixes oscillator outputs back into oscillator inputs.
- **Predictions**: come from "ringing patterns" of the oscillator
  bank — strike (token in), let the bank ring, read the spectral
  signature (token out).
- **The bottleneck**: `W_mix` at dim×dim is O(n²). The oscillators
  themselves are cheap. The research frontier is finding a
  wave-native O(n log n) replacement for `W_mix`.
- **The deep claim** (from resonance's own RESEARCH.md): *interference
  IS attention*. `|A + B|²` produces the cross-terms `A × B` that
  ARE the pairwise associations attention computes. The dense
  W_mix isn't a compromise — it's the physics of wave interference.

### Resonance's own PHILOSOPHY.md cites aither

This isn't a connection we're inventing in retrospect. The user's
own `PHILOSOPHY.md` in the resonance repo has a section literally
titled **"What might be missing — the aither insight"**, which
identifies what resonance has (oscillator bank, rotation) and what
it lacks compared to aither's audio synthesis: envelopes,
cross-modulation, feedback routing, multiple parallel voices. The
acknowledged gap: *resonance's synthesis chain is poorer than
aither's*. The cross-pollination is already a stated direction.

### Both projects make the same philosophical move

The blog post on complex numbers argues aither commits to the
**rigid/coordinate conception of ℂ** because audio is
classical-field-theory output and the mathematical commitment has
to match the physics. Resonance is doing the *same move for
cognition*. Its state rotates `(pos, vel)` as a 2-component
classical pair (resonance's own VISION.md spells this out: "the
state ROTATES instead of decaying. Phase encodes temporal
distance"). That's the rigid conception of ℂ applied to sequence
prediction.

Compare:
- **Mamba** uses *diagonal decay* — state dimensions are independent,
  no canonical phase relationship. That is the algebraic conception
  of ℂ (no canonical "i," Galois-symmetric components).
- **Resonance** uses *rotation* — pos and vel are coupled in a
  definite phase relationship; the rotation has a direction; the
  state has a phase that's measurable and meaningful. That is the
  rigid conception of ℂ.

This is why resonance is interpretable in ways Mamba isn't —
frequency, phase, and damping are all directly readable from the
state, because the rigid conception preserves the things the
algebraic conception throws away.

**Aither and resonance are both classical-field-theory programming
environments.** Aither's observable is sound; resonance's
observable is next-token probability. The substrate (DHO equation),
the philosophy (rigid ℂ, operations not types), the
interpretability claim (frequency / phase / damping are real
quantities), the rejection of the type-theoretic abstraction — all
shared. Different surface, same engine.

### aither → resonance — what aither offers as cross-pollination

Specific things from aither's design notes that map directly onto
resonance's stated open problems:

- **`pole(state, drive)` from `pole.md`** — the monopole-vs-multipole
  framing IS what resonance's PHILOSOPHY.md identifies as missing.
  Each oscillator in a resonance layer is currently a *monopole*
  (isolated, dispersive). The proposed `pole` primitive would let
  oscillators sharing state become a *multipole* (coupled, energy
  circulating among the bank within a single timestep). This maps
  exactly onto what resonance's PHILOSOPHY.md calls "cross-
  modulation: oscillator k's output modulating oscillator j's
  frequency." It's the multipole regime made operational.

- **Tesla shared-field section in `bachPolyphase.md`** — voices
  coupled through a shared scalar pressure field with per-sample
  bidirectional updates. This is the "feedback routing" resonance
  identified as missing. Aither's `f(state) → sample` contract
  with mutable shared state across voices is precisely the
  substrate for this kind of coupling.

- **`phasor_pair(rate)` and SSB heterodyne via `cmul`** —
  `cmul(phasor_pair_a, phasor_pair_b)` produces a clean
  sum-frequency rotation with the lower sideband suppressed by
  construction. In an attention-replacement context this is a
  content-dependent frequency shift that's mathematically clean
  and trivially cheap. Possibly a candidate for the "wave-native
  O(n log n) replacement for `W_mix`" resonance's SCALING.md
  flags as the research frontier.

- **Per-partial drift / `analytic` / Hilbert pair** — aither's
  per-partial slow drift technique (used in the Tesla-organ FM
  swarm and the cello-max patch) produces "alive" timbres because
  each partial has its own slowly-evolving phase relationship to
  the others. Same primitive could give resonance an interpretable
  drift mechanism — each oscillator's phase wandering by a
  learned-but-bounded amount, providing temporal expressivity
  without breaking the rotation invariants.

- **Cybernetic regulators (the Al-Muqabala / Al-Jabar patterns
  documented above in this doc)** — observation-and-feedback loops
  to bound the system's behaviour. In ML terms these are
  normalisation layers, but with cybernetic structure: the
  regulator pulls the system back when its energy crosses a
  threshold, not at every step. Could give a more
  physics-grounded alternative to LayerNorm.

- **The whole COMPOSING.md "drums that sound like a player"
  pattern** — the velocity-array crossfade with sample-and-hold
  produces structured-but-organic sequences from a deterministic
  iteration. Same pattern in resonance terms: a deterministic
  oscillator bank whose damping/drive parameters follow a
  structured velocity-array morph could produce text with
  rhythm-like structure (sentence-level breath, paragraph-level
  density) that pure attention can't easily express because it
  has no native concept of *recurrent timbre*.

### resonance → aither — what resonance offers back

The cross-pollination runs both ways. Resonance has solved, or is
in the process of solving, things aither would benefit from:

- **FFT bank as O(n log n) oscillator update.** Aither's current
  `additive(f, shape, n)` and `sum(N, n => sin(TAU * phasor(n*f)))`
  are O(N) trig calls per sample — fine for N ≤ 16 partials,
  expensive past that. Resonance's FFT-bank update is O(N log N).
  Importing this would let aither do additive synthesis at N=512
  partials with the same audio cost as N=16 today. That's a 32×
  improvement in spectral richness without changing the language
  surface.

- **Learnable damping γ(t) and drive β(t) as time-varying functions
  of the input.** Aither has this in spirit — you can pass any
  expression into `dho`'s damp and drive args — but resonance's
  framing makes it explicit as a *learnable control surface*. A
  patch could be a tiny recurrent network whose parameters are
  learned by gradient descent against an audio objective. That
  would be **aither-as-trainable-model**: write a patch with
  learnable parameters, target a sound, and have the engine fit
  the parameters. A new mode of composition.

- **The "interference IS attention" claim as a design principle.**
  When two voices share state and their `cmul`-products
  cross-couple, you get interference patterns that ARE pairwise
  associations between voices. The `pole.md` shared-field design
  is the audio version of resonance's `W_mix` operating in the
  multipole regime. Naming this in aither's design vocabulary —
  "interference between coupled voices is the audible analogue of
  attention between tokens" — gives composers a frame for thinking
  about coupled-voice patches that they don't currently have.

- **Quantization-friendly bounded amplitudes.** Resonance's
  architecture is designed so all values stay bounded in (0, 1)
  ranges — fixed-point feasible, no FPU required, runs on $5
  RISC-V chips. Aither doesn't currently care about this because
  it targets desktops with FPUs, but the design discipline is
  worth importing for two reasons: (a) aither patches running on
  microcontrollers / embedded hardware would be a real market for
  embedded music devices, and (b) bounded-amplitude discipline
  prevents the clipping regression we've been chasing in recent
  patches.

- **Multi-implementation discipline.** Resonance has OCaml, Python,
  and Nim implementations all targeting the same equation. That
  redundancy *forces* the spec to be portable and the equation to
  be the source of truth. Aither has only the Nim implementation.
  An OCaml or Rust port written against the same `dho` equation
  would similarly force aither's contract to be expressible
  without Nim-specific commitments. The discipline travels.

### The cybernetic-synthesis videos cover both

This is the clean part. The Tudor / Al-Muqabala / Al-Jabar
tradition documented in this whole doc is *exactly* what resonance
is doing in ML. Tudor coupled banks of nonlinear oscillators with
feedback and let the system organize itself into musical
attractors. Resonance couples banks of damped oscillators with
learned cross-mixing and lets the system organize itself into
sequence-prediction attractors. The cybernetic frame from the
manifesto episode — *"the suspension of the dichotomy between
automatic/dead and anthropogenic/live systems"* — applies to
resonance directly:

- **Transformers are dead ML** in the cybernetic sense.
  Stochastic-gradient-descent toward a metric. No internal
  feedback dynamics. Stateless attention with KV cache as
  retrieval, not as oscillation. Optimisation, not regulation.
- **Resonance is live ML.** Internal feedback dynamics (the
  damped rotation IS the feedback loop). The W_mix coupling is
  literally Tudor's matrix mixer at scale. The bank IS the matrix
  of coupled neurons. The rotation IS the cybernetic regulator
  bounding each oscillator's amplitude.

**Resonance is neural synthesis applied to NLP.** That's the
single sentence to remember. The cybernetic-synthesis tradition
that gave us Tudor's tabletops and Roland Kayn's Tektra finds its
ML expression in resonance.

### The practical convergence question

There's a real question here about whether these should be ONE
project at the infrastructure level. Both have Nim implementations.
Both use the same equation as the core primitive. Both want
low-FPU / quantization-friendly behaviour. Both want interpretable
state. The case for separation:

- They have different real-time constraints. Audio = 48 kHz hard
  real-time, no GC, no allocation. ML = batch training, throughput-
  bounded, allocation OK between forward passes.
- They have different user surfaces. aitherLang is a live-coded
  DSL with hot reload; resonance is a model architecture with
  CUDA kernels and gradient-descent training loops.
- They optimise for different hardware. aither targets a laptop
  CPU running TCC-compiled C; resonance targets either a desktop
  GPU for training or a $5 RISC-V chip for inference.

The case for unification:

- aitherLang's codegen, state management, and `dho` primitive
  *could be* the substrate for resonance's Nim implementation. The
  language already does what resonance needs at the inner-loop
  level: per-call-site state, inlined arithmetic, fixed-N unrolling
  via `sum`.
- Resonance's CUDA kernels and FFT-bank tricks *could become*
  aither performance primitives. A `dho_bank(N, freqs, damps,
  drives)` primitive compiling to FFT-based update would serve
  both projects (richer additive synthesis for aither; explicit
  bank primitive for resonance).
- The blog post's classical-field-theory framing already covers
  both. Anyone reading "Complex Numbers Aren't Imaginary" gets a
  philosophical foundation that applies to either project. Splitting
  them at the philosophical level would be misleading; they're the
  same idea applied to different observables.

The most likely truth: **the engine is the thing both projects
need**, and the surface APIs (live coding for sound, gradient
descent for sequences) are downstream applications. A future
unification would extract a "classical-field-theory programming
substrate" library — DHO equation, pair operations, mutable shared
state, FFT-based bank updates, quantization-friendly arithmetic —
and have both aitherLang and resonance import from it.

This is parked thinking, not a roadmap. But it's worth flagging:
**there may be ONE engine and TWO applications, not two separate
projects**. The coherence is more than analogy.

### The symmetry that closes a loop

This whole `cybernetics.md` doc has been about *importing*
musical-tradition ideas (Tudor, Roland Kayn, Wiener, the
al-Khwarizmi framing, the analog-computer chaos tradition) *into*
aither. Resonance is the case where the *export* runs the other
way — aither's own ideas (DHO as universal primitive, rigid ℂ
conception, classical-field-theory framing, `f(state) → sample`
with shared mutable state) get *exported* into a different domain
(machine learning) where they prove useful for different reasons.

That symmetry is real evidence the substrate is doing real work.
A primitive that turns out to be useful in two different domains
isn't an accident of one application; it's a structural feature
of the math. The DHO equation is the most general classical
two-energy-storage system. It shows up in audio because audio is
that physics. It shows up in resonance because cognition might
also be that physics — at least, at the level of phase-locking,
oscillatory binding, and interference patterns that the brain is
known to use.

If both projects keep going, they will probably converge — not
necessarily into one codebase, but into one shared *substrate
library*. The cybernetics tradition was built for hardware that
doesn't exist anymore (Buchla 200, Serge, custom Tudor boxes).
The next iteration of that tradition — applied to sound, to
language, to sensor data, to whatever oscillatory observable
matters next — will be built on shared software infrastructure.
That infrastructure may already exist in nascent form across
aitherLang and resonance-ocaml. Treating it as such is a useful
frame for what to build next.

## Things to watch / read

- David Tudor's "Neural Synthesis" series (the actual pieces) —
  Wesleyan University holds the David Tudor archive; recordings
  exist on Mode Records and Lovely Music.
- Norbert Wiener, *Cybernetics: Or Control and Communication in the
  Animal and the Machine* (1948). The founding text.
- Stafford Beer, *Brain of the Firm* (1972). Cybernetics applied to
  organisations; the "viable system model" is structurally similar
  to nested feedback regulators in synthesis.
- The "new uses for old circuits" YouTube series (Phil Stearns?
  whoever the host is) — episodes on al-Mukabala, al-Jabar, neural
  synthesis are particularly relevant.
- Eric Dollard's polyphase work — already on the radar from
  bachPolyphase.md, but worth re-watching with the
  cybernetic-regulator lens applied. Dollard's "negative resistance"
  / "self-balancing" arguments map onto al-Muqabala-style regulation.
- **`resonance-ocaml`** (github.com/rolandnsharp/resonance-ocaml) —
  the user's own classical-field-theory neural network. Built around
  the same DHO equation as aither's `dho` primitive. Directly
  relevant; cross-pollination opportunities documented in the
  section above. Read PHILOSOPHY.md, VISION.md, and SCALING.md for
  the architectural context; RESEARCH.md for the experimental log.

## Open questions

- Does a 3-neuron network actually find musically interesting
  attractors at audio rate, or does it just produce noise? The
  experiment above will answer this.
- Is the matrix-mixer primitive worth the engine cost (small but
  nonzero) given that hand-rolling N=3 is fine? Probably worth it
  at N≥5; below that, idiom in COMPOSING.md is enough.
- Do the al-Jabar and al-Muqabala patterns have audible signatures
  the ear can recognise, or do they just produce different chaotic
  textures that the ear lumps together? The patches in the videos
  suggest the former; need to verify in aither.
- Can aither's per-sample mutual-update semantics improve on the
  analog tradition's propagation-time-limited feedback, or does
  the latency actually contribute musically? Possibly an
  uncomfortable answer either way.
