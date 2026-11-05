# The nested pair: a pair may hold pairs

**Status (2026-11-05): built, stages 1 to 3.** Nested literals and
destructuring; pairs of pairs through lets, defs, lambdas, `if`, `sum`
and plays; side-by-side arithmetic; `turn`, `conj`, `unturn`,
`amplitude`, `phase` and the new `axis` by Hamilton's operations;
`$state` and arrays of them; functions once a number on them; `dho`
given a pair a doublet; `i`, `j`, `k`, `about`, `apply`, `hopf` in the
stdlib (tests/lang/quads.aither, SPEC "Pairs of pairs", COMPOSING
"Pairs of pairs"). The four demos rewritten and measured against their
plain versions: `sympathetic_bells` within 2e-13, `tumble`, `spinfield`
and `stereo_spin` identical sample for sample. Both open questions went
to errors that say they are stand-ins. Not yet built, refused the same
way: the voice loop and `def rec`. It follows
[spinors-2026-11-05.md](spinors-2026-11-05.md), which made way 3 (the
spinor as aither's third value) the destination and way 2 (defs that
return small arrays of pairs) the path. This note proposes skipping way
2: it would package the halves the demos keep writing, and keep a
spinor as two flat pairs and a rotation as plane turns stitched
together, which is the planar thinking the physics doesn't have
(Roland: "I'm worried it will get us stuck in 2-D thinking"). Instead,
way 3 in its simplest form: no new kind of value, one rule.

## The rule

**A pair may hold pairs.** A pair of numbers is a complex number; a pair
of pairs is a quaternion (Cayley–Dickson: a quaternion is a pair of
complex numbers, z1 + z2 j). `turn`, `conj` and `unturn` follow the
doubling formula when given pairs of pairs; everything else works side
by side, as it does on pairs today.

    number            a line
    pair of numbers   turns in a plane         (aither today)
    pair of pairs     turns every way round    (a rotation, a spinor)

**It stops there of itself.** One more doubling gives the octonions,
whose product isn't associative: rotations made of them don't compose.
A pair of pairs of pairs is an error, by design, not by limit.

The lineage word: Hamilton named unit quaternions versors, Steinmetz's
word for his turning complex numbers. A pair of pairs is a versor that
turns in any plane, not only the page's.

## The classical vocabulary

The words follow Hamilton, whose quaternions these are; Maxwell's
*Treatise* used his notation, and Tait developed it. For each, aither's
word where it already has one:

| Hamilton | what it is | in aither |
|---|---|---|
| i, j, k | the units: i² = j² = k² = ijk = −1, so ij = k, jk = i, ki = j, and ji = −k | `i`, `j`, `k` in the stdlib (they shadow nothing) |
| S q, V q | the scalar and vector parts | destructuring: `let [[w, x], [y, z]] = q`, w the scalar and [[0, x], [y, z]] the vector |
| K q | the conjugate | `conj(q)` |
| T q | the tensor: how big | `amplitude(q)`, aither's word for a pair's size |
| U q | the versor: the unit, q over its tensor | `q / amplitude(q)` |
| ∠ q | the angle: q = T q (cos θ + ε sin θ), θ from 0 to π | `phase(q)`, as for a pair, which is a versor in its plane |
| Ax q | the axis ε, a unit vector | `axis(q)`, new |

So `phase` of a pair of pairs is Hamilton's angle, and the rotation
q( )q⁻¹ turns a vector by twice it, about the axis (checked: a
quaternion of angle 22.9° about k turns i by 45.8°). A pair's phase is
the same thing in its plane, signed by which way round (−π..π), where a
pair of pairs carries the way round in its axis instead (0..π).

The complex rung is written the way the electrical engineers wrote it:
Steinmetz's j is the plane's own unit. aither's pairs have no named
unit (a pair is `[0, 1]`), so nothing clashes; the quaternion units are
Hamilton's, whose they are.

Hamilton's rotation of a vector is q v q⁻¹ (for a unit q, q v K q):
that is `apply` below. The Hopf map is later (1931) and has no classical
spelling; it is written with Hamilton's operations, from the axis k,
the pole the spinor's θ is measured from.

## Why the demos say so

Four patches were written the plain way (`spinfield`, `stereo_spin`,
`tumble`, `sympathetic_bells`). Across all of them the spinors and
rotations needed exactly these, and nothing else:

- `turn` (compose two rotations; turn a spinor by a rotation),
- `conj` (undo a rotation),
- `+`, `-` and scaling by a number,
- the Hopf map (which way a spinor points).

and every one of them paid for the value being two pairs: `turned_xi`
with `turned_eta`, `comp_a` with `comp_b` (four arguments each), four
calls to turn "ahead" in `sympathetic_bells`. The nested pair is that
list, with the halves gone.

## Writing it

Literals nest: `[[1, 0], [0, 0]]` is the rotation that does nothing.
The quaternion units, as nested pairs (z1 = w + x i, z2 = y + z i):

    one  [[1, 0], [0, 0]]     i  [[0, 1], [0, 0]]
    j    [[0, 0], [1, 0]]     k  [[0, 0], [0, 1]]

**Destructuring, as everywhere in aither** (SPEC, "Pairs": names of
the halves' meaning where the patch reads them, not `[0]`, `[1]`):

```
let [xi, eta] = s                       # a spinor's two halves, each a pair
let [a, b] = r                          # a rotation's
let [[w, x], [y, z]] = q                # all four numbers, named
let [[_, x], [y, z]] = hopf(s)          # a direction: its first number is 0 by construction
```

**Both sides where practical**, as SPEC's "Read the whole pair" asks of
pairs: what is wanted from a rotation or a spinor is mostly read whole,
by an operation (`turn` to apply or compose, `conj` to undo,
`amplitude` for its size, `hopf` for its direction), not by taking a
half. A half is taken by name only where a formula needs it, and `_` only
where one number is all there is room for.

## The operations

**`turn(p, q)`** on two pairs of pairs is the quaternion product, the
doubling of complex multiplication:

    (a, b)(c, d) = (a c − b conj(d),  a d + b conj(c))

It is associative and the sizes multiply (checked numerically). It is
the one product in aither where order matters: `turn(r2, r1)` is r1
then r2. On pairs whose second halves are 0 it is complex `turn`, so the
rung below sits inside it. As today, `turn` is strict: two pairs, or two
pairs of pairs, nothing mixed (a pair isn't silently made [p, [0, 0]]
or [p, p]: a patch says which).

**`conj(q)`** is (conj(a), −b): the rotation undone, the spinor turning
the other way. **`unturn(q, r)`** is `turn(q, conj(r))` over the size of
r squared.

**`amplitude(q)`** is the size, all four numbers (Hamilton's tensor).
**`phase(q)`** is Hamilton's angle, 0..π, and **`axis(q)`** his axis, a
unit vector [[0, x], [y, z]] (the classical vocabulary, above).

**Side by side**: `+`, `-`, `*`, `/` on two pairs of pairs act on the
four numbers; a number scales all four. A pair against a pair of pairs
is an error until a meaning is chosen (broadcast to both halves, or the
embedding [p, [0, 0]]: the two disagree, so the patch says which).

**Stateful functions** (filters, delays, `resonator`, `dho`, `phasor`)
run once per number, four slots each, as they run once a side on a pair.
This gives a meaning to what is an error today: aither's rule is that a
function given a pair runs once a side, so `dho([440, 442], …)` would be
two oscillators, each a phasor, a pair of pairs. That is a bell's doublet
as one value:

```
let doublet = dho(push, [f, f * 1.0013], damp)     # [[x1, q1], [x2, q2]]: the two modes of one partial
```

## In the stdlib, written in aither

```
let i = [[0, 1], [0, 0]]                                                         # Hamilton's units
let j = [[0, 0], [1, 0]]
let k = [[0, 0], [0, 1]]
def about(axis, angle): [[cos(angle / 2), 0], [0, 0]] + axis * sin(angle / 2)   # a turn by angle about a unit axis: a versor of half the angle
def apply(r, v): turn(turn(r, v), conj(r))                                       # a rotation applied to a direction
def hopf(s): apply(s, k)                                                          # the direction a spinor points
```

A direction is a pure quaternion, [[0, x], [y, z]]. Turning a spinor by
r is `turn(r, s)`, and its direction turns by r (associativity:
(r s) k conj(r s) = r (s k conj(s)) conj(r); checked numerically). The
double cover is there: r and −r turn every direction alike, and a
spinor turned all the way round comes back negated.

One convention note, checked: the demos' spinor form of the Hopf map
(X + iY = 2 conj(xi) eta, Z = |xi|² − |eta|²) and the quaternion form
`hopf(s)` agree only up to naming the axes; no choice of which half is
which makes them identical. Under the nested pair, the quaternion form
is the one, and directions are read in the quaternion's own axes.

## The demos, rewritten (sketches, to be checked when built)

`sympathetic_bells`, the second bell's facing, four calls and two defs
pairs today:

```
let r = turn(about(k, yaw), about(j, tilt))
let [[_, fx], [fy, fz]] = apply(r, i)
```

`tumble`, the body's turning, two equations in halves today; with the
body's spin w as a pure quaternion, q' = ½ q w:

```
$q = [[1, 0], [0, 0]]
let w = [[0, $w1], [$w2, $w3]]
let moved = $q + turn($q, w) * (0.5 * dt)
$q = moved / amplitude(moved)
```

`spinfield`, an ear: `hopf(turn(head, ear))`.

## At the edges

- **The pipe**: a pair of pairs reaching the output is an error that
  names the choices: its Hopf direction (a projection of it), its first
  half, or a projection the patch writes. `scope` and `tv` take pairs,
  so the same.
- **Arrays**: an array may hold pairs of pairs (a field of spinors, a
  Weyl field: one spinor a place, stepped by `each`). `sum` adds them.
- **State, hot reload, export**: a `$state` of nested pairs is four
  slots; migration matches regions by identity, as now, with the width
  in the region; export writes the same C.
- **MIDI, input**: untouched; they make numbers and pairs.

## What it costs

The compiler's values are one side or two (`V` with `l` and `r` in
codegen.c); everywhere a pair is handled (operands, per-side calls,
state regions, lets, arrays, `if`, the voice loop, hoisting, `prev`,
destructuring) learns a width of four. That is the bulk of the work. The
parser needs nested literals and nested destructuring. Then SPEC and
COMPOSING, tests (the doubling formula, associativity, the double
cover, `dho` given a pair, the pipe's error), and the four demos
rewritten as the first users.

## Open questions, answered for now

1. A pair against a pair of pairs in `+` and `*`: an error, broadcast,
   or the embedding? **An error**, saying it is a stand-in and naming
   the two readings.
2. The pipe: an error, or a default (the Hopf direction's first two
   numbers)? **An error**, the same way (Roland, 2026-11-05: "go with
   errors for both, but explain in the error message that this is just
   a stand in").

Settled (Roland, 2026-11-05, "follow the classical field theories"):
`phase` is Hamilton's angle and `axis` his axis; `i`, `j`, `k` are his
units, in the stdlib; `hopf` measures from k, the pole.

## Order of work

1. Settle the open questions.
2. The parser: nested literals and destructuring.
3. codegen: width four through values, operands, state and arrays;
   `turn`, `conj`, `unturn`, `amplitude` by the doubling formula.
4. Tests, then the four demos rewritten, measured against their plain
   versions (the same pictures and sounds, to the sample where the
   arithmetic is the same).
5. SPEC, COMPOSING, and the spinor note's way 3 marked as taken.
