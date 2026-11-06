# ∇ and the medium: how places affect their neighbours

**Status (2026-11-06): a design note, nothing built, and a direction
rather than a decision.** Roland: "im thinking we are onto something
here". This is part 3 of [places-and-values-2026-11-06.md](places-and-values-2026-11-06.md),
which left "∇, later" for a note of its own. It needs places (step 5 of
that note) first.

> **Values, places, `*` and `∇`, with the medium as fields.**

## The question

Does each classical force earn a place in the language: compression,
rarefaction, divergence, centripetal, centrifugal, refraction? The answer
is that nearly all of them are one thing seen from different sides, so
they earn a word and not a primitive.

## One product gives the lot

Hamilton's ∇ is a direction whose parts are differences:

    ∇ = i ∂x + j ∂y + k ∂z

Applied to a field of directions v it is his product, so it needs no new
algebra, and from i i = −1 and i j = k:

    ∇v = −(∂x vx + ∂y vy + ∂z vz)  +  curl v
         scalar part                  vector part

Checked numerically on v = (xy, yz, zx): the scalar part is minus the
divergence and the vector part the curl, exactly. So the sign is worth
noticing: the scalar part is positive where a field **converges**, and
negative where it spreads.

| the word | what it is |
|---|---|
| compression | the scalar part of `∇v`, positive: the field converging at a place |
| rarefaction | the same, negative |
| divergence | the same, with the sign turned |
| curl | the vector part of `∇v`: the field turning at a place |
| slope (gradient) | `∇` on a field of numbers: the push a pressure or potential gives |
| concentration | a place against the mean of its neighbours: `∇∇` of a field, minus the Laplacian (`∇²` is −∂x² − ∂y² − ∂z² in Hamilton's algebra) |

Compression and rotation are the two parts of one product. Tesla's
longitudinal waves and Hertz's transverse ones then aren't rivals in the
algebra: they are the scalar and vector parts of the same `∇v`. This says
nothing about which the medium carries, which is for experiment.

## Already in aither

- **Centripetal** is what keeps a turning turning: the pull towards the
  centre, the `w²` in `dho`'s spring. `*` does it each step.
- **Centrifugal** is that same motion seen from inside the turning:
  multiply by `conj` of the spin. A change of view, not a force to add.
- **Damping** is "shrink", a value of size under 1.
- **Resonance** needs nothing: it emerges, as `sympathetic_bells` and
  `turning_bells` show.

## The medium

- **Refraction** is the rate of handing a push along varying from place
  to place. So it belongs to a field of stiffness or density, the
  medium's own make (Tesla's capacity and self-induction, in the
  step-first note), and needs no operator.
- **Reflection** is what happens at the edge of a field, so it is the
  same question as what a neighbour is at the end of a line.

## Earlier work this meets

[fields-2026-10-29.md](fields-2026-10-29.md) weighed this already. Its
candidate **V**, "a field is a value; arithmetic is at every place at
once; neighbours are whole shifted fields, so ∇² is a def on fields", with
`[ … ]` meaning only a field, is close to where places-and-values
arrived by another road. That note chose **M** (a field as its modes)
and **X** (coordinates, like `t`) instead.

V was turned down for simplicity. "The limitation" says why: a program
in aither is one sample at one place, a field is many places at one
moment, and every way of naming all the places had cost that simplicity,
"a second way of thinking", with every operator gaining a second
meaning. Read against this design:

- **Every patch changing** was the price of V for one row of the ladder.
  It is being paid now for the ladder as a whole, and stereo keeps its
  spelling.
- **A second meaning for operators** mostly doesn't arise. A number
  spreading over `[l, r]` is already how aither works; places take the
  same rule to N.
- **A second way of thinking** is answered only if every line still reads
  as what happens at one place, a def running once a place, with `∇` the
  one reach to the neighbours. Then `[ ]` says where a program runs, not
  a second kind of program. This is S's "one place, one moment" without
  its construct. It has to be held to: whole-field operations written in
  a patch would bring the objection back.
- **Cost is not answered.** A patch runs per sample, and on `tv` it is
  one place per sample. Every place every sample is out of reach for
  large fields, so large fields need the scheduling the note lists
  (Halide's split of what from when; `every` is a first case). Two
  places unroll; a raster doesn't.
- **The law apart from its stepping**, the note's other real question,
  stays open: in aither the integrator is written into the law by hand.
  Yee's scheme also needs a *present* read next door, which bears on
  question 3 below.

M and X stand beside ∇, not under it: a bell is better as its modes.

## Prior art

Others have gone field first, each in one corner. Diderot and the field
calculus were read from their papers; the rest from abstracts, with
anything from memory marked. None of them is for sound, and no language
found has fields, Hamilton's ladder and live reload together. That rests
on a short search: "not found", not "there is none".

**Diderot** (Chiw, Kindlmann, Reppy, Samuels, Seltzer, 2012), scientific
visualization. The closest in spirit. Fields are continuous and
first-class, `∇` is written as `∇`, and a field can be *probed* at any
point, `F(x)`, between its samples. The computation is **strands**: a
small program, one per ray or particle, each with parameters, mutable
state and an `update` method, run bulk-synchronously, as with a CUDA
kernel. Globals are immutable, and pointers are left out so the compiler
can analyse it. A strand may `stabilize`, ceasing to be updated. Types
carry a field's continuity (`field#2`), so a field is only differentiated
as often as it can be.

**The field calculus and Proto** (Beal, Viroli, Damiani, Audrito and
others, 2011 onward), swarms and sensor networks. The word is theirs:
"computational fields". The core is a tiny calculus, two constructs doing
the work: **`rep`** for time evolution and **`nbr`** for reaching the
neighbourhood. `nbr { e }` gives a *neighbouring field*, which built-ins
then summarise (`min-hood`, `fold-hood`). Most important for aither: they
state the two views outright, each device's and the whole network's, and
prove them the same: full abstraction and adequacy between the aggregate
denotational semantics and the per-device operational one (Viroli et al.
2016).

**ZPL and regions** (Chamberlain, Lewis, Lin, Snyder, 1998–2000), parallel
arrays. A region is an index set that is named and "syntactically
separated from array references", replacing subscripts, so where a
statement runs is said apart from what it does. They report fewer errors
and more reuse, and that it exposes locality to the compiler. Neighbours
by named direction (`A@north`) is from memory.

**Halide** (2016 and earlier papers), image pipelines. "What to compute"
in the language, "when and where" in a separate scheduling co-language,
so one algorithm has many schedules; schedules can also be generated
automatically (Mullapudi et al. 2016).

**Liszt** (DeVito, Hanrahan and others, 2011), PDEs on meshes. Restricts
the class of programs on purpose: analysis of its mesh statements exposes
parallelism, locality and synchronisation. "Designing a DSL requires a
careful balance" between what the compiler can see and what the user can
say. **MeshTaichi** (2022) does similar with "reference-style
neighborhood queries", and keeps optimisation apart from the computation.

**Devito** (2019), seismology. PDEs written symbolically, finite
differences generated from them: the law apart from its stepping.

**Scanned synthesis** (Tubb et al. 2012 on it; the original, Verplank,
Mathews and Shaw, from memory), sound. Not a language, but the nearest
thing in audio: a mass-spring network evolves slowly, and a wavetable
read from it is **scanned** at audio rate. Block-based physical modelling
compilers (Karjalainen et al.; Petrausch et al.) and WaveCore are tools
and hardware for models, not languages of fields.

GPU shaders are the most used of all (from memory): a program for one
pixel, with `dFdx` as a neighbour derivative.

### What aither takes from them

Ideas, not vocabulary (Roland, 2026-11-06: "im prefering our classical
field theory language"). `nbr`, strands, hoods, probes and schedules
stay in this section. aither's words stay classical: Hamilton's and
Maxwell's, turn, shrink, push, and whatever the sources confirm.

1. **`nbr` is the discipline, already designed.** Neighbours aren't a
   value a patch holds: they arrive as something only a reduction can
   turn back into a value at this place. Make `∇` and its relatives those
   reductions, and "every line reads as one place" holds by construction,
   not by good manners. And aither already has `rep`: it is `$`.
2. **The two views can be proved the same.** The objection in "The
   limitation" was a second way of thinking. The field calculus shows the
   place view and the whole-field view can be one semantics. aither's
   test: a law run per place gives bit for bit what the whole field does.
3. **Question 3 is answered by prior art:** bulk-synchronous, every
   place's update reading the old field, stores landing together, which
   is what `each` and `every` already do. Yee's present read stays open.
4. **Scanning answers cost, musically.** Let the field evolve slowly and
   read it at audio rate along a path. That is `every` and Diderot's probe
   together, `field(x)` between places, and it needs nothing to run at
   every place every sample. This is the most promising single idea here.
5. **The schedule apart from the law**, from Halide: when a field steps
   and how often belongs apart from what it does. `every` is the first
   word of that.
6. **`stabilize`:** a place that has settled stops costing anything, as a
   poly voice retires. A struck plate is mostly quiet.
7. **Restrict on purpose,** from Liszt: the one-place discipline is also
   what lets the compiler see a stencil at all.
8. **Continuity in the type** is worth remembering, but Diderot's fields
   are continuous and reconstructed; aither's are stepped. Not for now.

## Tried (2026-11-06)

`∇` needs no language to try: along a line it is `i * (next - this)`, a
plain def. On `scanned_ring` (masses moving one way) the concentration so
written, `∇` twice with its sign from `i i = -1`, renders byte for byte as
the hand-written line, at about 10% more cost. There it is honest but
ceremonial: `i` goes in and comes straight out.

`patches/aether_ring.aither` is where it does work. Each mass is a
direction, and the scalar and vector parts of one product take their own
stiffness: 0.593 and 0.254 places a step measured, against 0.592 and 0.245
from sqrt(stiffness). A strike along the ring makes no twist, and across
it no squeeze. Found on the way:

- **Where differences live.** It took a step ahead and a step behind, one
  each side of a place. From what is known of the numerics, not tested
  here, one centred difference twice would reach two places and split
  the ring into odd and even masses. So `∇∇` needs a choice about this.
- **"Here" is missing.** The note's `concentration($x)` had to be
  `concentration($x, k)`. If `∇` is to be the only reach to a neighbour,
  a line has to know its place.
- **Nothing holds the ring.** Without a spring at each place a strike's
  momentum carried the whole ring off, the medium needing its own hold.

## The abstraction, and the code it replaces

**Built (2026-11-06), simpler than proposed here.** The block below,
`play ring[128]`, was dropped before building: `[128]` read as "place 128",
the same brackets meaning contents, reading and size. Instead a field is
just an array assigned bare, the stereo rule at N places, with no block at
all (Roland: "im liking the array and stereo rule"). See SPEC "Fields". It
reverses "Arrays are data, not a map" for those arrays, and is candidate V
of fields-2026-10-29.md, turned down then as a second way of thinking; it
reads now as stereo already does, which is what changed. Both rings are
fields: `scanned_ring` 32 lines to 24, `aether_ring` 46 to 31, with the
measured speeds holding (0.582 and 0.252 against 0.592 and 0.245). They
cost about 20% more than by hand. Since then `ring(period = T)` took the
size out of the patch and a law, `$x' = $v`, the stepping out of the patch
(field-by-its-period-2026-11-06.md): the open question "the law apart from
its stepping" is answered for a ring. Not the same to the bit: a strike
became a write head sweeping a stretch, within 0.01 of the radius at each
check. The sketches below are kept as how it got here.

The proposal, written against real patches so
the simplification can be judged line by line (Roland, 2026-11-06: "we
should document this simplification and the code it replaces so that we
can understand the abstraction").

### What a field is, written by hand

`scanned_ring` and `aether_ring` work today, and every line of this is
about something other than the physics:

```
let k = pixel_index(N, 1)                          # which place this sample is
$x = array(N, every = N)                           # state, by hand, per place
$v = array(N, every = N)                           # ... and the step, by hand
let me = $x[k]                                     # reading here
let left = $x[(k + N - 1) mod N]                   # reaching a neighbour,
let right = $x[(k + 1) mod N]                      # the ring's wrap written out
let far = abs(k - place)                           # how far from a strike,
let apart = min(far, N - far)                      # the ring's wrap written again
$v[k] = moved                                      # writing here
$x[k] = me + moved
let at = floor(round)                              # reading between places,
let was = $x[at mod N]                             # the wrap a third time
let height = was + ($x[(at + 1) mod N] - was) * (round - at)
```

Four things are being said by hand, over and over:

| What is meant | Written as | Knows about |
|---|---|---|
| this is a field of N places | `array(N, every = N)` on each state | the stepping trick |
| this line is at one place | `k`, and `[k]` on every read and write | the index |
| reach a neighbour | `(k + 1) mod N` | the ring's size and wrap |
| read the field at a point | floor, mod, interpolate | the same again |

### The same field, as the abstraction

```
play ring[128]:                                    # a play at each place
  $x = 0                                           # plain state: here
  $v = 0
  let push = -concentration($x) * stiff - $x * held
  $v = ($v + push) * (1 - loss)
  $x = $x + $v

ring.v[where] = blow                               # the outside pushes in
let height = ring.x[phasor(note)]                  # the field read at a point
spin(note) * (0.45 + height)
```

Line for line:

| Today | Becomes | Why it goes |
|---|---|---|
| `let k = pixel_index(N, 1)` | nothing | the block runs at each place, so there is no index |
| `$x = array(N, every = N)` | `$x = 0` | state in a field is one per place, stepped together |
| `let me = $x[k]`, `$v[k] = moved` | `$x`, `$v = ...` | a bare name is here, as it is in any play |
| `$x[(k + 1) mod N] + ... - me * 2` | `concentration($x)` | the only reach to a neighbour |
| `abs(k - place)`, `min(far, N - far)` | `ring.v[where] = blow` | the outside writes at a point; the law knows no distance |
| floor, mod, interpolate | `ring.x[p]` | `[ ]` reads a place, and between places |

`aether_ring`'s law, today and as the abstraction:

```
# today
def ahead(f, n): i * (f[(n + 1) mod len(f)] - f[n])
def behind(f, n): i * (f[n] - f[(n + len(f) - 1) mod len(f)])
def S(q):
  let ((w, _), (_, _)) = q
  w
def V(q): q - S(q)
let n = pixel_index(N, 1)
let up = ahead($d, n)
let down = behind($d, n)
let push = -(i * ((S(up) - S(down)) * squeeze + (V(up) - V(down)) * twist))

# the abstraction
let push = -slope(convergence($d)) * squeeze - curl(curl($d)) * twist
```

Ten lines become one line of physics: a mass is pushed down the slope of
the squeeze, and back against the curl of the curl. Both signs are the
working law's: `slope` of the convergence is `i * ΔS`, and `curl(curl)` is
`i * ΔV`, with the minus outside. The words are the classical ones, and
each is a stdlib def over one primitive.

### What the abstraction is

- **One rule, already aither's:** a block runs at each place. It is what a
  def given `[l, r]` does for two places, for N.
- **It keeps "every line reads as one place".** Inside the block nothing
  names the field as a whole, which answers "The limitation" of
  fields-2026-10-29.md by construction. The patch holds no neighbours,
  only what a reduction gives back.
- **Stepping is the block's job, not the patch's.** One place a sample,
  stores landing together: `scanned_ring`'s cost, 0.5 s and 0.44 s for 128
  and 8192 masses, is then the default, and "the schedule apart from the
  law" comes for free.

## No here, no now

> **Inside a field there is no here and no now, only what changes and what
> it touches. Where and when exist only at the edge.**

Roland, 2026-11-06: "there is no time only change in field theory". An
earlier draft of this note had a `here` word, and that was the same mistake
about space as `t` is about time: a coordinate standing outside the field.
A place doesn't know where it is. It knows itself and its neighbours, so
the field's geometry *is* its neighbour relation, and open question 1
below is the whole of the space, not a detail of it.

The two things `here` was for don't need it:

- **A stiffness that varies (refraction)** is a medium differing from place
  to place, so it is state, each place holding its own make. The law stays
  the same everywhere.
- **A strike's distance** is the outside touching the field, the mirror of
  reading it.

So a field has two touch points, the same thing in each direction, a write
head and a read head, as `tape` has:

```
play ring[128]:                          # no here, no index, no time: only change
  $x = 0
  $v = 0
  let push = -concentration($x) * stiff - $x * held
  $v = ($v + push) * (1 - loss)
  $x = $x + $v

ring.v[where] = blow                     # the outside pushes in at a point
let height = ring.x[phasor(note)]        # the outside reads out at a point
```

`where` needn't be a coordinate either: on a ring it can be a turning, a
phase, how far round, which is what scanning already reads by. This is
step-first's "Hz only at the edge", for position.

Weighed and set aside: `here` as a word like `t` (it names the index
without removing it), and the `each` lambda's parameter (it runs every
place every sample, the cost `scanned_ring` escaped).

**What it costs.** A broad strike can't be "everywhere within this
distance". It is a write head sweeping a stretch, or a narrow push the
field spreads itself; the second is more physical, and they won't sound
the same as `scanned_ring` does now. A field's starting shape, or a varied
medium, is set from the edge, once. A store from outside has to land with
the step.

### What it leaves to decide

1. **The word.** `play ring[128]` for now (Roland, 2026-11-06: not a fan of
   "medium"). Others can come later, and there is a bigger question under
   it: **does `play` need to exist, or could there be only defs?** A def
   already holds state and runs per call. What `play` adds is a name the
   outside can reach: `mute`, `solo`, levels, parking, hot reload's
   identity, the tasks across cores, and now `ring.x[p]`. Whether those can
   hang off a def, or a let of one, is worth its own look.
2. **The shape of a field.** `[128]` is a line; the edge (a ring, ends
   fixed or free) has to be said: `ring[128]` could wrap by default, the
   bracket a size, the edge the field's. A sheet is `[w, h]`, and which of
   i, j, k runs along it follows from that.
3. **`∇` of something that isn't state.** `slope(convergence($d))` applies
   `∇` to a value worked out at each place, so a place needs its
   neighbour's *result*, not only its state. Either the compiler expands
   both into one stencil (a step ahead then a step behind, as the patches
   do by hand, which is also what keeps odd and even places together), or
   derived fields become two passes. The first is enough for the words
   here, and is what the tests must pin.
4. **Heads.** `ring.x[p]`, `ring.v[p] = ...`: p as 0..1 of the way round,
   so the same line works at any N. Between places, linearly, on reading;
   what a write between places does is open.
5. **Where a field's cost lands.** The default is one place a sample. A
   small field that is the sound, not read by it, may want more, as `every`
   gives now.

### How to judge it

Rewrite `scanned_ring` and `aether_ring` in the abstraction, as a compiler
change, and hold it to the same test as `*` for `turn`: the same sound to
the bit, and clearly fewer ideas on the page.

## Open

1. **What a neighbour is.** A line, a ring, a sheet, a raster on `tv`.
   Which of i, j, k a field has neighbours along, and how a field says so.
2. **The edge.** Fixed, free, wrapped, absorbing: reflection decides which
   sounds right, and it may be the medium's.
3. **Stepping.** Bulk-synchronous, as Diderot's strands and `each` today:
   reads see one step's field and stores land together. Is that enough
   for Yee's present read?
4. **Cost.** A neighbour read over a large field is what `each` loops do;
   whole-field operators must not be slower. Scanning was tried
   (2026-11-06, `patches/scanned_ring.aither`), with no new language:
   `every = N` and `pixel_index(N, 1)` step one place a sample, as `tv`
   draws a pixel. 10 s of sound took 0.5 s at 128 masses and 0.44 s at
   8192, against 10.6 s for 128 stepped every place every sample (but N
   sets the speed as N²: at that stiffness 8192 places take hours to go
   round, so that figure measured a medium nobody could hear move; see
   field-by-its-period-2026-11-06.md). So the
   cost no longer grows with the field, at a price: the field moves only
   every N samples (344 times a second at N = 128), and that shows as a
   buzz at sr / N, about 40 dB under the note, worse as N grows. It suits
   a field that moves slowly against the sound, not a field that is the
   sound.
5. **The names.** Maxwell's convergence, curl, slope and concentration,
   or the modern ones. Whether the words are his has to be checked.
6. **Scheduling large fields**, and the law apart from its stepping, as
   above: the two things "The limitation" leaves unanswered.

## What this doesn't remove

Time and Hz are still in aither: `t` is built from the step count and
pitch is written in Hz. Taking them out is a decision for the step-first
note, not this one.

## References

- W. R. Hamilton, "On quaternions; or on a new system of imaginaries in
  algebra", *Philosophical Magazine* (1846).
  https://doi.org/10.1080/14786444608645590
- A. Gsponer and J.-P. Hurni, "Lanczos's functional theory of
  electrodynamics" (2004): electrodynamics as a biquaternion field theory.
  https://doi.org/10.48550/arxiv.math-ph/0402012 (seen in the literature
  search as a record and abstract, not read in full).
- C. Chiw, G. Kindlmann, J. Reppy, L. Samuels, N. Seltzer, "Diderot: a
  parallel DSL for image analysis and visualization", PLDI (2012).
  https://doi.org/10.1145/2345156.2254079 (read). Kindlmann et al.,
  "Diderot: a domain-specific language for portable parallel scientific
  visualization and image analysis", *IEEE TVCG* (2015).
  https://doi.org/10.1109/tvcg.2015.2467449
- M. Viroli, G. Audrito, F. Damiani and others, "A higher-order calculus
  of computational fields" (2016).
  https://doi.org/10.48550/arxiv.1610.08116 (read). F. Damiani, M. Viroli,
  J. Beal, "A type-sound calculus of computational fields", *Science of
  Computer Programming* (2015). https://doi.org/10.1016/j.scico.2015.11.005
- M. Viroli, J. Beal, M. Casadei, "Core operational semantics of Proto"
  (2011). https://doi.org/10.1145/1982185.1982472
- B. Chamberlain, E. C. Lewis, C. Lin, L. Snyder, "Regions" (1998).
  https://doi.org/10.1145/312627.312713 Chamberlain et al., "ZPL: a machine
  independent programming language for parallel computers", *IEEE TSE*
  (2000). https://doi.org/10.1109/32.842947
- R. T. Mullapudi et al., "Automatically scheduling Halide image processing
  pipelines" (2016). https://doi.org/10.1145/2897824.2925952 T. Denniston
  et al., "Distributed Halide" (2016). https://doi.org/10.1145/3016078.2851157
- Z. DeVito, P. Hanrahan, "Designing the language Liszt for building
  portable mesh-based PDE solvers" (2011).
  https://openalex.org/W2615402527 "MeshTaichi" (2022).
  https://doi.org/10.1145/3550454.3555430
- M. Louboutin et al., "Devito (v3.1.0)" (2019).
  https://doi.org/10.5194/gmd-12-1165-2019
- R. Tubb, A. Klapuri, S. Dixon, "The Wablet: scanned synthesis on a
  multi-touch interface", DAFx (2012). https://openalex.org/W7098604265
- M. Karjalainen, C. Erkut, L. Savioja, "Compilation of unified physical
  models for efficient sound synthesis" (2003).
  https://doi.org/10.1109/icassp.2003.1199999 S. Petrausch, J. Escolano,
  R. Rabenstein, "A general approach to block-based physical modeling"
  (2005). https://doi.org/10.1109/icassp.2005.1415636 M. Verstraelen, F.
  Pfeifle, R. Bader, "Feasibility analysis of real-time physical modeling
  using WaveCore" (2015). https://openalex.org/W2392519124
- *From memory, not verified; the literature search doesn't index them:*
  J. C. Maxwell, "On the mathematical classification of physical
  quantities" (1871), as the source of convergence, curl, slope and
  concentration; his *Treatise* (1873) using ∇; Tesla on longitudinal
  waves (see the step-first note). Check before SPEC quotes them.
