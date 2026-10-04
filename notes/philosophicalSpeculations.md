# Philosophical Speculations

This document is **not** a roadmap. It's not the SPEC. It's not the ARCHITECTURE.
It's a record of design conversations about where aither's `f(state) → sample`
contract could be pushed if we let ourselves think outside the current
implementation envelope. Treat it as a notebook of half-formed bets about which
unexplored territory might compound aither's natural advantages.

The point of writing it down is so future-us doesn't have to re-derive these
ideas, and so when an opportunity appears, we recognise it.

---

## The three-axis model

Across this session we converged on a model of where aither's "uniquely
possible" sounds live:

- **Axis 1 — Spectral construction freedom (proven).**
  Per-partial state with no module wall. The Tesla organ / `fm_swarm` recipe
  vindicated this: 16 partials × per-partial FM × incommensurate drift, all in
  one expression. Modular synths can't afford it; software synths wall the
  inner loop off. aither writes it by hand in 30 lines.

- **Axis 2 — Primitive-level paradigm-transcendence (DHO, in progress).**
  The damped harmonic oscillator's parameter regions ARE the synthesis
  paradigms (additive partial / bandpass / bell / envelope / LFO / formant).
  One expression walks across categorical boundaries via a continuous knob.
  This is what the `dho` primitive ships to make operational.

- **Axis 3 — ?**
  Open. This document is mostly about what Axis 3 could be.

The structural pattern across axes 1 and 2: take a thing that other paradigms
wall off into separate modules / engines / files, and make it ONE freely
composable expression in aither. Axis 3 should follow the same pattern but in
a dimension we haven't yet named.

---

## Candidate 3A — chora (state indexed by position, not just time)

Generalize the contract from `f(state) → sample` to `f(state, position) → field`.
Position becomes a state variable on equal footing with `t` and `freq`. Source
location can be modulated by anything else in the state vector — partial
frequency drives partial position drives listener angle drives reverb send.

In every other audio system, position is a *mixer parameter* applied AFTER the
synthesis math. In aither it would be INSIDE the math, freely cross-coupled.

**Cost reality:** full HRTF + room IR + ambisonic decode is roughly 393M ops/sec
at 8 voices. That blows the per-sample budget. A "chora-lite" subset is
reachable cheaply (Doppler is free, ITD is cheap, distance via lpf+amp is
cheap, head-shadow approximation is cheap-ish). Chora-lite would still be
aither-native in spirit because it puts position into the state vector rather
than the mixer.

This is the strongest *familiar* candidate for axis 3.

---

## Candidate 3B — structured state (state has shape, not just length)

Today, state is a flat vector of `float64` whose shape is fixed at compile
time. `$age = 0.0`, `$x = 0.5` — each cell is one number. The state vector
has known size before the patch runs.

What if a `$state` cell could hold:

- A variable-length array
- A pointer to another voice's state vector
- A small graph: nodes = sub-oscillators, edges = modulation routings, all
  dynamic in count and topology
- A *recursive* reference: this state contains a "child" with its own state

Then `f` could **traverse** state, not just index into it. A patch could be a
tree of DHOs whose topology evolves: branches grow when a feature triggers,
prune when amplitude drops below threshold. The state vector becomes a
**living data structure**, not a flat record.

**Concrete example — the "fractal organ":** a key strike spawns one DHO. After
0.5s, that DHO spawns 3 child DHOs at harmonic ratios. After another 0.5s,
each child spawns its own children, recursing to a depth limit. The sound is
one fundamental that bifurcates into a forest. The state vector grows from 2
floats (one DHO) to ~3^N floats over time, then prunes back as voices die.

Today, impossible — the state vector shape is fixed at compile time.

This is the deepest of the three candidates because it generalizes
`f(state) → sample` to `f(structured_state) → sample` where the state's
*shape itself* evolves. It implies a runtime memory allocator inside the audio
thread, which is the engineering minefield.

---

## Candidate 3C — self-modifying patches (the AST is in $state)

This is the candidate the user's intuition pointed at: **code held in
`$state`, not just values.**

Today: a patch is parsed once, compiled to C, run forever (until hot-reload,
which is a full re-parse). The expression graph is static at run time.

What if the patch's **AST itself** were a state variable? A knob doesn't just
modulate a parameter — it rewrites WHICH expression executes:

- `K1=0` → `dho(impulse, 440, 0.01)`
- `K1=0.5` → rewrites to `additive(440, saw_shape, 8)`
- `K1=1.0` → rewrites to `noise() |> bpf(440, 0.5)`

The CODE is reactive. The state vector contains a tree of nodes that f
*interprets* at every sample, and another part of state can rewrite that tree.

This is closely related to 3B (the AST is structured state), but specifically
about **the AST of the patch's own behavior**. It's the most aggressively
self-referential: the function `f` is reading state that includes its own
source.

**Why aither could plausibly do it:** expressions are short and TCC compiles
sub-second. A patch could recompile itself on parameter thresholds. The gnarly
problem is synchronisation — how do you crossfade across a recompile without
glitching? Plus the "interpreter inside the audio thread" cost is real.

This is the speculation the user's question crystallised: **"are we talking
about holding code in `$state` variables not just values?"** Yes. That's the
unifying frame for 3B and 3C.

---

## Candidate 3D — cross-modal / multi-output

Today, `f` produces a sample (or a stereo pair). What if it produced a tuple:
`(sample, midi_event, network_packet, lighting_value, semantic_tag)`?

A patch could SEND a MIDI note when its envelope crosses a threshold. It could
emit "I'm being percussive right now" as a tag another patch reads. It could
talk to other aither processes, lighting rigs, video synths.

The synth/sequencer/orchestrator distinction collapses. A patch is no longer
just a sound generator — it's a participant in a system that can react to
others. The state vector includes inbound signals from peers.

Less "stranger axis" than the others; more "aither becomes a node in a
network." Still aither-native in spirit because it's all `f(state) → outputs`.

---

## The unifying intuition: state can hold more than scalars

The user's question — "code in `$state` not just values" — generalises
beautifully:

> Today `$state` cells hold scalars.
> The third axis is letting them hold **anything an expression can produce**:
> arrays, references to other state, sub-graphs, AST fragments, distributions,
> message queues.

If you take that one design move seriously, **3B and 3C fall out of it**. 3A
falls out of it partially (a position cell is just a 3-vector, which is a
small fixed-shape array; a *moving collection* of position-tagged sources
would need full structured state). 3D requires both this AND a generalisation
of the output side.

So the deepest version of axis 3 is:

> **Generalise `$state` from "named scalar" to "named term."**
> A term can be a number, an array, a graph, an AST node, a closure.
> `f` can traverse, transform, and rewrite terms.

That's a single language change with cascading consequences.

---

## What this would mean for the language

Honest engineering implications:

- **Memory model:** today everything lives in a flat per-voice header. Variable
  state needs a real allocator. Audio-thread allocation is the hard problem;
  arenas + voice lifetime can probably contain it.
- **Type system:** today every cell is `float64`. Terms imply tagged values
  or sum types. Likely small (number / array / ref), not full Hindley-Milner.
- **TCC limits:** TCC compiles flat C readily. Generating C for tree-walking
  interpreters or AST manipulation is plausible but adds a layer.
- **Determinism:** with allocation comes GC or arena reset timing. Per-voice
  arenas reset on note-off; the audio thread never blocks on free.
- **Backwards compatibility:** today's flat-scalar `$state` should remain the
  zero-cost default. Structured state opt-in.

None of this is impossible. All of it is a research project, not a sprint.

---

## Why we're parking it

The current focus is music + MIDI + landing DHO. Speculation should be
written down (this file) but not built until the cheap wins are in:

1. DHO primitive lands and gets used in real patches.
2. Chora-lite (positions in the state vector + cheap binaural) is shipped
   as a probe of axis 3 in its most familiar form.
3. We see whether *users* (i.e. the user) reach for structured state — if
   real patches keep wanting to grow voices, spawn children, mutate ASTs, the
   demand argues for the engineering investment.

The bet: by the time those steps are done, we'll know whether axis 3 is chora,
or structured state, or self-modifying code, or something we haven't named.

This document exists so the option is **available** when the moment comes,
not so we build it now.

---

## Aren't we just reinventing Lisp?

A fair challenge. Lisp solved "code as data" in 1958. Lisp-family live-coding
audio systems (Extempore, Common Music, Overtone via Clojure, Fluxus) have
been holding ASTs in variables and rewriting code at runtime for decades. So
the "code in `$state`" move, considered in isolation, isn't novel. We'd be
applying a 60-year-old idea, not inventing one.

The honest question is **where it's applied, with what constraints, and what
that combination makes possible.** Three places where the aither version
isn't just "Lisp for audio":

**1. Real-time deterministic context.** Lisp's `eval`, GC, and dynamic
dispatch all assume time is fungible. The audio thread does not. Even
Extempore — the most aggressively live-coded Lisp-family audio system —
keeps the actual sample-rate inner loop statically compiled and does the
dynamic work at the orchestration layer above the audio thread. If aither
put "code in `$state`" at sample rate (a knob value recompiles the DSP), we'd
be doing what Lisp deliberately doesn't. Not philosophically deeper — more
operationally constrained, which forces different design choices.

**2. Different atomic unit.** Lisp's atom is the s-expression: everything is
a list of lists, anything goes. aither's atom is `f: state → sample`: every
expression has the SAME type signature — read state, emit a number. That's
much narrower. Any AST fragment held in `$state` would have to also be a
state-reading function, not arbitrary computation. *More* restrictive than
Lisp, not less. The constraint is what enables ahead-of-time compilation to
flat C and per-sample determinism in the first place.

**3. Different starting ontology.** Lisp says "everything is a list." aither
says "everything is a state-reading function." Both are universal-data-structure
claims, but state has properties lists don't:

- State has a temporal dimension built in (evolves sample by sample).
- State is read every sample, not built once and consumed.
- Access must be O(1), so flat layouts beat cons cells.
- Mutable updates are the default (ECS-like) rather than functional.

A structured-state aither wouldn't end up looking like a Lisp. It'd look more
like a **reactive dataflow language with first-class structured state** —
closer to Bret Victor's "world as state vector" than to McCarthy's "world as
tree of symbols."

**The honest frame:** we're not inventing something Lisp doesn't have. We're
stealing Lisp's best move (data = code) and applying it in a domain (real-time
DSP) where Lisp explicitly cannot reach (the inner loop), with a starting
ontology (`state → sample`) that constrains the move differently than
s-expressions do.

That's positioning, not philosophy. The depth, if any, is in whether the
*combination* — homoiconicity + sample-rate determinism + uniform
`state → sample` contract — produces sounds and patches that no Lisp-family
audio system has ever made. We don't know that yet. The bet is that the
constraints aren't a cage; they're a shape that makes some new things easy.

If the bet is wrong, aither becomes "a worse Extempore." If it's right, it
becomes the first audio language where the AST-as-state move is operational
inside the per-sample loop, not above it. The difference between those two
outcomes is what makes the question worth holding open rather than answering
prematurely in either direction.

---

## Open questions to revisit

- Is the right framing "f reads state" (passive) or "f rewrites state"
  (active)? DHO is firmly in the first category. The stranger axes pull
  toward the second.
- How does this relate to existing paradigms — granular synthesis (probably
  3B-shaped), generative composition (3C-shaped), live coding tools like
  TidalCycles (3D-shaped)? Are we just rediscovering known territory under
  different names, or is the f(state) framing genuinely new?
- Is there an axis 4 we're missing because we've fixed `f(state) → sample`
  too tightly? What if `f` itself is part of the state — i.e. `f(f, state)`,
  the function reading its own definition? (This is 3C taken to the limit.)
- Could the language ship a *very small* structured-state primitive — maybe
  just "$state can hold an array of fixed max length, indexable at runtime" —
  as a low-cost toehold into axis 3?
