# Places and values: what aither is, as a field language

**Status (2026-11-06): steps 2 and 3 of the order of work built** (the
compiler's values are width-generic; `( , )` works on the ladder beside
today's `[a, b]`, with whole powers only, SPEC "Values on the ladder").
The rest is a design note. Agreed with Roland in discussion, as the
destination. It takes the ladder
([ladder-2026-11-06.md](ladder-2026-11-06.md)) for what a value is, and
replaces that note's answer for stereo (`both`, and the dotted operators
considered after it). Past patches don't decide this; they are changed
to it.

> **Round brackets are one value. Square brackets are many places.**

## The idea

A field theory has three parts, and aither should say them plainly:

1. **A value at a place.** The ladder: real, complex, Hamilton's. Each
   rung is the one below doubled, and multiplying is turning.
2. **A field: a value at every place.** Arithmetic happens at each place.
3. **How places affect their neighbours.** Hamilton's ∇, which Maxwell's
   *Treatise* used; with step-first's turn, shrink and push, the whole of
   the dynamics. A destination, not part of this work (below).

The pair was two different things that looked alike because both are two
numbers. **A complex number is one place holding two numbers. Stereo is
two places holding a number each.** Arithmetic couldn't know which it
had, which is what every rule proposed so far was trying to get round.
Told apart, there is nothing left to guess.

## The rule

> Arithmetic is the algebra at each place, and a value with one place is
> everywhere.

Two steps, always in this order:

- **Places.** Fields of the same number of places go place by place. One
  place (a plain value) is the same at every place: a uniform field.
  Different numbers of places (2 against 3) are an error.
- **Values.** At a place, the ladder: the narrower sits inside the wider,
  the rest zero, and `+ - * / ^` are the algebra's.

```
let z = (0.6, 0.8)                    # one complex number
let q = ((w, x), (y, z))              # one of Hamilton's
let stereo = [left, right]            # two places
stereo * [0.8, 0.6]                   # a gain at each place
stereo + voice                        # one place, so everywhere: both sides
stereo * 0.5                          # the same
z * w                                 # turning; z + 1 moves the real part
z ^ 2                                 # z * z, turned twice: spin(220) ^ 2 is spin(440)
stereo * spin(50)                     # each side turned: two places of complex
```

`turn` and `unturn` go: `*` and `/` are them, and the docs keep the word
as the explanation, **multiplying is turning** (the ladder note, "`*` is
`turn`"). `conj`, `amplitude`, `phase` and `axis` read a value at each
place.

Gone with this, with nothing in their place: `both`, `gains`, dotted
operators, a warning for `stereo + mono`, the stand-in errors for a pair
meeting a pair of pairs, and the two-entry `[a, b]` that is sometimes a
pair and sometimes a table.

## Brackets

`[]` already means many things in a row: tables, `$f[i]`. So `[l, r]` is
a field of two places, spelled as patches and SPEC already spell stereo,
and `[220, 330, 440]` one of three. Two entries is nothing special.

`()` is how the mathematics writes a value: the doubling rule is `(a,
b)(c, d) = (ac − b conj(d), ad + b conj(c))`. `(a, b)` is free in the
grammar: a comma in parentheses today is a call, or a lambda, which `=>`
tells apart. `let (a, b) = z` destructures. `{}` isn't used: braces mean
an unordered set.

## Reading part of a value

Destructuring only (Roland, 2026-11-06). `[]` is for places, so `z[0]`
goes: a value has no places to index, and `dho(...)[0]` becomes `let (x,
_) = dho(...)`. One more line where it was an expression, and a value is
read by naming its parts, as aither prefers. No `re(z)`.

## Translating

Nothing is lost between a complex number and stereo. At the edge it is
automatic; inside a patch it is one line either way:

```
let (a, b) = z          # a complex number's two numbers
let s = [a, b]          # now two places: left and right

let [l, r] = stereo     # a stereo sound's two sides
let z = (l, r)          # now one complex number, ready to turn
```

The numbers don't change, only how arithmetic reads them. What goes is
that it happens silently. A word for each direction can come later, if
they get written often.

## The edge

The pipe carries two numbers, and these reach it as they do now:

- two places of numbers: left and right;
- one complex value: its two numbers, so `spin(220)` is a circle on the
  scope and a sound in the ears, and I and Q on a radio;
- one number: everywhere, so both speakers. The ladder note's wart goes:
  that is the place rule, not a convenience. `x + (0, 0)` is a complex
  number and sounds different, as it now should.

Anything else stops and says what it is: more than two places, two
places of complex, a pair of pairs. These stay stand-ins until patches
show what's wanted. **N channels** are simply fields of N places, which
aither has to have anyway; only the output is two, and more speakers
would be a matter for the edge alone.

## Functions and defs

A function runs at each place, and within a value once a number, each
run its own state. Parameters follow the place rule, so `lp1(stereo,
[100, 200])` is a cutoff a side and `lp1(stereo, 100)` the same on both.
A filter given a pair of pairs and a complex parameter stays an error.

**The split goes away.** Today the compiler infers whether a def reads a
pair whole (`pair_params`) and otherwise runs it once a side. On places
and values: a def given a field runs once a place, and a value is always
taken whole. So the inference that decides how many copies of state exist
can go, and `aither check` needs nothing for it.

`dho(push, [f, f * 1.0013], damp)` is then two places of complex: a
bell's doublet, a field of two. The nested-pair note made this a pair of
pairs; it isn't one. A pair of pairs comes from the algebra.

## Surviving trap

`[cos(a), sin(a)]` typed by habit is two places, not a complex number,
and `z * [cos(a), sin(a)]` then runs silently. It is the only one left,
and it is the old spelling, so it bites mainly during migration. In
stereo, nothing is silent any more.

## Open

1. **`^` with a fraction.** `spin(440) ^ 0.5` is half the turn, by the
   standard definition, the principal root; quaternions the same (slerp,
   Shoemake 1985). Whole powers are built; a fraction is an error so far.
2. **What makes stereo.** `pan`, `reverb`, `input()` return fields. Does
   `input()` stay two places for a radio, translated in one line, or does
   a patch say?
3. **Whole-field arithmetic at size.** Two places unroll. `$f + $g` over
   a million elements has to loop; `each` is that loop today.
4. **`mono`** becomes reducing a field; is `sum` over places that word?
5. **A number or a pair put in state** (`$z = 1`), an `if` with a value
   on one branch and a field on the other: by the rule, but to check.

## ∇, later

Once a field is a value in its own right, how places affect their
neighbours can be one operator rather than index arithmetic in `each`.
In Hamilton's and Maxwell's form ∇ applied to a field gives divergence
and curl together. It is what a field physics lab most lacks, and the
reason to do this at all, but it needs its own note: what a neighbour is
on a line, a grid, a ring.

## Cost

This is far bigger than the ladder alone. Every complex `[a, b]` in
patches and stdlib respells, each judged, as a `[a, b]` could be either.
The compiler carries places as well as widths, so width-generic values
inside `codegen.c` come first. The check stays strong: no patch's sound
should change, as every migrated line means what it meant.

## Order of work

Each step keeps the corpus unchanged.

1. Settle the open questions; verify the references.
2. Width-generic values inside the compiler, no change in behaviour.
3. `( , )` as values on the ladder, beside today's `[a, b]`.
4. Migrate complex uses to `()`, stdlib first, then a few patches
   (`sympathetic_bells`, `stereo_spin`, `turning_bells`) to judge how it
   reads, before the rest.
5. When no complex `[a, b]` is left, `[a, b]` becomes two places; fields
   gain arithmetic; the def split goes.
6. SPEC, COMPOSING, PHILOSOPHY and tests in the same commits, with the
   sentence at the top of this note. PHILOSOPHY's "Complex numbers as
   operations, not types" is rewritten: values are now told from places.

## References

- W. R. Hamilton, "On quaternions; or on a new system of imaginaries in
  algebra", *Philosophical Magazine* (1846).
  https://doi.org/10.1080/14786444608645590
- J. H. Conway and D. A. Smith, *On Quaternions and Octonions: Their
  Geometry, Arithmetic, and Symmetry* (2003).
- K. Shoemake, "Animating rotation with quaternion curves", *ACM SIGGRAPH
  Computer Graphics* (1985). https://doi.org/10.1145/325165.325242
- *From memory, not verified; check before SPEC quotes them:* Hamilton's
  complex numbers as ordered couples (1830s); Maxwell's *Treatise* (1873)
  using ∇, and his naming curl; ∇ giving divergence and curl together;
  de Moivre's formula as the precedent for `^`; Steinmetz (1893).
