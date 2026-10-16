# Tail recursion

Decided 2026-10-16 (Roland, with Claude on the uConsole). A def marked
`rec` may call itself, and the call repeats the def instead of nesting
it. This is aither's first loop that can stop early.

## Why

`sum` and `each` unroll at compile time: every iteration is written
out, so every pixel of the escape-time zoom paid for all 64 steps,
escaped or not. Stopping when the work is done needs a loop whose
length the data decides. Recursion is how functional languages write
that loop, and it reads as the math:

```
# z -> z^2 + c until it leaves, or n runs out: the steps left
def rec escape(z, c, n):
  if n == 0 or magnitude(z) > 2 then n else escape(cmul(z, z) + c, c, n - 1)
```

What it opens:

- **Pictures:** escape-time fractals for about a third of the cost,
  ray marching (step along a ray until it hits something), Newton
  fractals.
- **Sound,** as a precision tool rather than a new palette:
  - Newton's method for implicit nonlinear models: diode clippers,
    saturating ladder filters, felt hammers, bow friction, reed and lip
    valves. (`didge3` couples its lips and bore a sample late today.)
  - Adaptive-step integration, so a chaotic oscillator stays accurate
    when pushed toward audio rate.
  - Additive that stops at Nyquist instead of paying for `max_n`.
  - Searches: the nearest zero crossing for a loop point, the first dip
    of YIN pitch tracking, continued fractions for snapping an interval
    to a just ratio.
  - Fractals heard: an escape count or an orbit as a waveform.

## The rules

- **`rec` marks a def that loops,** as OCaml's `let rec` does. In
  aither, recursion changes how a def runs: a plain def is an inline
  copy that always finishes, a `rec` def is a loop that shares its
  state and might not. The word shows that where the def is read, and
  a patch without it can't hang. A def that calls itself without `rec`
  is a compile error rather than a loop: the usual way to get there by
  accident is a wrapper calling the stdlib def it replaces, which needs
  a name of its own. `rec` on a def that never calls itself changes
  nothing. OCaml's other half, a plain `f` meaning the previous `f`,
  would let a patch wrap a stdlib def under the same name; that changes
  how aither resolves names, so it is left for later. Clojure's
  `recur`, which marks the call instead, was passed over: the
  recurrence should read like the math.
- **A call repeats when it is the whole result of a branch** (tail
  position): the def's body, either branch of an `if` in tail position,
  the last line of a block. All the new arguments are computed first,
  from the old values; then the parameters take them and the body runs
  again. There is no stack, so there is no depth limit.
- **A call to itself anywhere else is an error.** `fib(n - 1) +
  fib(n - 2)` would need a stack. The error shows the accumulator form,
  which defaults keep tidy at the call site:
  `def rec fib(n, a = 0, b = 1): if n == 0 then a else fib(n - 1, b, a + b)`,
  called as `fib(30)`. General recursion isn't planned.
- **No count limit in the language.** A recursion runs until it
  returns, as in Scheme. The engine guards against one that doesn't
  (below).
- **State belongs to the code, not the iteration.** A stateful call in
  a recursive def has one state, advanced every time it runs: `noise()`
  gives a fresh number each step; an `osc` would advance each step,
  which is rarely wanted. `sum` and `each` are the opposite: copies,
  each with its own state. Copies are space; recursion is time.
- **Only the branch taken runs.** The base case costs nothing until it
  is reached.
- **Numbers and pairs change from step to step; arrays and functions
  don't.** An array or function parameter has to be passed on unchanged.
  A parameter that every repeat passes on unchanged keeps a
  compile-time value, so `each(n, ...)` and `array(n)` still see it.
- **A def that ends in a pair returns a pair,** whether the pair is a
  literal, a pair parameter, a pair `let` or arithmetic on one. A
  recursion's base case is usually `then z`. This holds for every def,
  recursive or not.
- **A parameter that meets a pair in arithmetic is a pair** (the `c` in
  `cmul(z, z) + c`), and so is one a `def rec` passes itself a pair
  for. Before, `c` was only a pair if the body indexed it or handed it
  to a pair operation, so the escape def couldn't take a complex `c`.
  A number passed there still fills both sides. Every patch compiled
  to the same C before and after these two rules (checked 2026-10-16).
- **Calls between defs that come back round (f → g → f) are an error
  for now.** They would compile to one loop with a label per def, and
  `rec` gives them a shape, as OCaml's `let rec f … and g …` does;
  deferred until a patch needs one.

## The guard

Today every sample finishes in a time you can read off the code.
Recursion gives that up, so the engine stands guard:

- Each block of each voice has a deadline: `AITHER_WATCHDOG` seconds
  (0.5 by default; 0 turns it off for heavy offline renders).
- A recursion checks the clock every 65 536 steps. Past the deadline it
  gives up with NaN, and every recursion after it in that block gives
  up at once, so the block ends quickly.
- The engine then treats the voice like one that blew up: within 20 s
  of a send it rolls back to the previous version, otherwise it mutes
  the voice until it is sent again. The log names the def and its line.
- Cost: a counter and a branch per step.

## What it compiles to

Each call site becomes a C loop in the tick:

```c
({ double z_l = ..., z_r = ..., n = 64.0;      /* the call's arguments */
   double r; long k = 0;
   if (s->stuck) r = NAN; else
   for (;;) {
     if ((++k & 65535) == 0 && n_overrun(s, "escape (zoom.aither:2)")) { r = NAN; break; }
     /* the body's lets */
     if (n == 0.0 || ...) { r = n; break; }
     else { double a = ..., b = ..., m = n - 1.0; z_l = a; z_r = b; n = m; continue; }
   }
   r; })
```

## Measured

2026-10-16, on the uConsole on mains: the Seahorse Valley zoom at
128×96 and 60 frames a second (737 280 samples a second), 64 steps.

- The average pixel needs 44.7 of its 64 steps: the view sits on the
  set's boundary, so stopping early saves 30 % of the steps, not the
  two thirds guessed beforehand.
- tcc (offline renders): `each` takes 3.10 s per 5 s of video, `def
  rec` 2.62 s, 1.18× faster. Written with `magnitude(z) > 2` it took
  4.05 s: `magnitude` takes a square root every step, so compare
  `z[0] * z[0] + z[1] * z[1] > 4` instead.
- cc -O2 (live, after a send): both run at 2.1× real time; the patch
  without the escape runs at 4.9×. The unrolled `each` has no
  branches, and -O2 makes its 64 straight-line steps cost the same as
  the loop's 45 steps with a branch on each.
- The -O2 rebuild after a send takes about 14 s for `each(64)` and
  about 1 s for the `def rec`.

So early exit pays when the cap is far above the typical count (a deep
zoom with hundreds of steps, a convergence loop) or too large to
unroll at all. For a 64-step view of the boundary it is a wash once
optimised. The language gain is the rest: loops that no fixed count
can write, and much less code to compile.

## Later

- `check` could warn when an oscillator sits inside a recursion.
- Calls between defs, when something needs them.
