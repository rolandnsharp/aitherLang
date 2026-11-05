# Shapes reported, and `match` on shape

**Status (2026-11-05): a design note, nothing built.** Roland: "1 and 2
sound good". Two ideas from the ML family (OCaml, Haskell), taken at
aither's size, after pairs of pairs (nested-pair-2026-11-05.md) gave
values three shapes.

## Why now

A value is a number, a pair or a pair of pairs, and the compiler already
knows which at every point: it decides from them whether a def runs once,
once a side or once a number, takes an argument whole, or stops with a
stand-in error. None of that is visible to the patch. Mixing shapes is
now the commonest mistake, and the stand-in errors are where the rules
aren't settled yet.

## 1. Shapes reported back (inference, no annotations)

What OCaml and Haskell give by type inference, kept to aither's three
shapes and never written by hand:

- **In errors:** a def's shape beside its name, e.g.
  `about : pair of pairs, number -> pair of pairs`, and what it was given,
  so "gets a pair of pairs and a pair" says which argument and why.
- **In `aither check`:** each def and each top-level let with its shape,
  and where a def splits (runs once a side or once a number) rather than
  taking an argument whole. The split is today silent; it decides how
  many copies of the state there are.
- **Shapes depend on the call:** a def is shape-polymorphic (`resonator`
  given a number, a pair, a pair of pairs), so a def is reported as the
  rule it follows (`number -> number, run once a number`), a call as
  what it made.

Cheap: the compiler has every fact already; this is reporting.

## 2. `match` on shape (OCaml's `match`)

Where the compiler's default reading is unclear, the patch says which:

```
def lift(v): match v with
  | [[w, x], [y, z]] -> ...        # a pair of pairs
  | [a, b] -> ...                  # a pair
  | n -> ...                       # a number
```

- Resolved when the patch compiles: shapes are known then, so a `match`
  costs nothing at run time and only the arm taken is compiled.
- Arms are destructuring patterns, as `let` has; `_` for what isn't read.
- It is the replacement for the stand-in errors: a pair meeting a pair
  of pairs, a pair of pairs reaching the output. A patch writes the
  reading it means, and the error can point to it.
- No arm for the shape given is a compile error naming the shape.

## Order

1. Shapes in errors and `aither check` first.
2. `match`, once the stand-in errors have shown which readings get used:
   those decide whether some become defaults instead.
3. SPEC and COMPOSING with each, tests in tests/lang.

## Not taken

Lisp macros (`array`, `sum`, `each` generate already, and macros read
badly on stage), lazy evaluation (wrong for a per-sample engine), and
functions or closures held in state (they break the fixed-shape,
compiled-to-C model that makes aither fast and hot-reloadable). Forth
fits beside aither, a small kernel on a microcontroller, not inside it.
