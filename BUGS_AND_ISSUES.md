# Bugs and issues — pick up next session

Captured at the end of the additive synthesis push (commits 2adb710 →
5df01c4). Phase 5 live verification confirmed `additive` and
`inharmonic` produce instrument-grade sound — better than a $400 Casio,
per the user's ears. Real direction unlocked: forget replicating
acoustic instruments, push the sum-of-sines + inharmonic-ratio idiom
toward sounds that have never existed before.

The items below are the loose ends from that session, ordered by how
much they block forward work.

## Session log: 2026-04-25 bug-fix bundle

The 2026-04-25 session resolved every blocking item below plus the
voice-slot-leak issue that surfaced near the end of the additive push.
The new commits (c4dfa6b → 89d52ad) ship together with
`patches/backing.aither` — a no-lead backing track designed for the
human to play `fm_swarm` over.

| Issue                                | Status     | Commit  |
|--------------------------------------|------------|---------|
| 1 · solo stops voices                | RESOLVED   | 7a2c41d |
| 2 · prev() doesn't sample-delay      | RESOLVED   | e763770 |
| 3 · `else if` chaining               | not-a-bug  | c4dfa6b |
| 4 · dangling-binop continuation      | open       | —       |
| 4b · arrays second-class             | RESOLVED   | 6623f9d, 169d598 |
| 4c · MIDI subscription drops silently | RESOLVED  | 0b24948 |
| 5 · already-shipped enablers (docs)  | shipped    | (in 169d598) |
| 6 · forward direction (sound design) | open       | —       |
| voice slot leak on stop              | RESOLVED   | c8944de |

The actual cause of issue 2 turned out to be aither's `and`/`or`
codegen using C's short-circuiting `&&`/`||`, not prev() state-keying.
With short-circuit, `prev(g)` never ran while g was low, so its slot
froze and the second note-on saw stale state. Fix: bitwise `&`/`|`
on the 0/1 comparison results, so both operands always evaluate.

Issue 3 turned out to already work — `parsePrimary` handles `if` as a
fresh primary, so `else if` parses cleanly via the existing parseExpr
recursion. The cascade form was an unnecessary workaround. The
session pinned the behaviour with tests/elif_chain.nim and rewrote
stdlib's `bar_partials` / `plate_partials` to use the cleaner
array-lookup form (issue 4b's fix).

## 1. Solo stops voices instead of muting them

**Symptom**

```
./aither solo bell_verify        # OK
./aither solo cello_verify       # OK
./aither unmute cello_verify     # OK — but cello stays silent
./aither list                    # cello_verify [stopped] gain=0.00
```

`solo` puts other voices into `[stopped]`, which `unmute` cannot
revive (`unmute` only handles `[muted]`). Forces a re-`send` of the
patch to bring the voice back, which loses the running state.

**Expected**

`solo X` should fade other voices to `[muted]`, recoverable via
`unmute Y` or another `solo Y`.

**Fix sketch**

In the solo handler (engine-side), replace the "stop other voices"
branch with the same code path that `mute` uses. The "fade out and
then drop" semantics belong to `stop`, not `solo`.

**Severity**: medium. It's the only safe-to-recover way to A/B
voices live. Workaround for now: use `mute` / `unmute` pairs
manually.

## 2. `prev(midi_gate())` doesn't sample-delay the call result

**Symptom**

The `bell_verify` strike detector

```
let strike = if g > 0.5 and prev(g) < 0.5 then 1.0 else 0.0
```

never fires when `g` is `midi_gate()` directly inline — voice plays
silent regardless of key presses. Replacing with a let-bound
intermediate

```
let g       = midi_gate()
let g_prev  = prev(g)
let strike  = if g > 0.5 and g_prev < 0.5 then 1.0 else 0.0
```

is the workaround the patch already uses (the original commit form
of `bell_verify.aither` already has `let g = midi_gate()` then
`prev(g)` — and that ALSO appears not to fire reliably; needs
re-confirmation). The simpler `inharmonic(...) * pluck(g, 2.0)`
form (no edge detection) does sound, confirming the synth itself
is correct.

**Hypothesis**

`prev` may be specialised on its argument node kind. An inline
`prev(midi_gate())` could be returning the same-tick value because
the expression-tree pass is collapsing or aliasing the two calls.
Worth checking whether `prev` allocates a state slot keyed by
expression identity (which would conflate inline and let-bound
forms) versus by source position.

**Reproduction**

1. `./aither send patches/bell_verify.aither` — silent on key press.
2. `./aither send /tmp/bell_simple.aither` (the gate-only form
   posted in the verification session) — sounds correctly.

**Fix direction**

Inspect `prev`'s codegen. If it's relying on argument-shape for
state-slot identity, switch to source-position. While in there,
write a regression test:

```nim
# prev returns the previous sample of its argument, regardless of
# whether the argument is an identifier, call, or let-bound expr.
test "prev_inline_call":
  ...
```

**Severity**: medium-high. Edge detection on MIDI gates is the
common idiom for percussion / strikes / triggers — broken `prev`
quietly breaks any patch that uses it.

## 3. Language: `else if` chaining

**Symptom**

```aither
def bar_partials(n):
  if n == 1 then 1.0
  else (if n == 2 then 2.756
        else (if n == 3 then 5.404
              else (if n == 4 then 8.933
                    else 13.345)))
```

The paren cascade is the smell. Without `else if`, every alternative
needs its own `else (if ... then ... else ...)`.

**Fix**

Make `if/then/else` right-associative so `else` greedily consumes a
following `if` as the start of a fresh conditional chain:

```aither
def bar_partials(n):
  if n == 1 then 1.0
  else if n == 2 then 2.756
  else if n == 3 then 5.404
  else if n == 4 then 8.933
  else 13.345
```

One-line change in `parseIf` — after the `else` token, recurse into
`parseExpr` (or whichever entry sees `if` again as a fresh primary)
instead of into a tighter atom level. Add a 3-arm chain to whatever
test covers conditionals.

**Severity**: low. Cosmetic — works today, just ugly.

## 4. Language: dangling-binop expression continuation

**Symptom**

```aither
let y = a + b
      + c + d           # parse error: expected expression on prev line
```

Has to collapse to one line or be wrapped in parens.

**Fix**

In the tokenizer, track the *last emitted token kind*. If a newline
arrives and the last token was an infix binary operator
(`+ - * / |> mod and or == != < > <= >= ?`) — i.e. an operator that
*needs* a right-hand side to be a valid expression — swallow the
newline as whitespace.

Subtle: the test must be "operator in infix position," not "operator
character at end of line," so a unary `-` on the next line doesn't
ambiguate. The simplest correct rule is "if the prior token is one
of the known binary infix kinds AND we're not at the start of a
fresh statement, swallow the newline."

**Severity**: low-medium. The pain is real but rare — it cost the
user "another reload" once, not blocking ongoing work.

## 4b. Arrays are second-class — can't be let-bound or indexed in def bodies

**Symptom**

```aither
def prime_ratio(n):
  let p = [2.0, 3.0, 5.0, 7.0, 11.0]
  p[n - 1]
```

Compile error: `array value can't appear here — only numeric
literals for wave() or top-level stereo return`. So tabular partial
data (primes, modal ratios, custom tunings) can't be expressed as
a lookup; you're forced into the parenthesized `if/else` cascade
that issue 3 is trying to kill.

**Why this matters**

The `sum(N, n => ...)` idiom only fully delivers if you can express
the partial-data table inside the lambda. Today that means cascade,
which means issue 3 (`else if` chaining) is *the* unblocker for
clean tabular ratio functions like `bar_partials`, `prime_ratio`,
arbitrary user tunings.

**Two competing fixes**

- **Promote arrays to first-class values** — `let p = [1.0, 2.0]; p[0]`
  works in any expression context. Larger language change; cleanest
  outcome.
- **Just ship `else if` chaining (issue 3)** — narrower fix, makes
  the cascade form readable.

Probably both, eventually. Issue 3 is smaller and unblocks the same
sound-design work, so it goes first. Arrays-as-values is a deeper
language refactor and can wait for a "language polish" session.

**Documentation bug**

COMPOSING.md currently claims `wave(0, [...])` indexed by `n-1` is
the workaround for tabular ratios. It isn't — `wave` interpolates a
time-varying signal across an array, it isn't an index-into-array
operation, and let-binding the array fails per the symptom above.
Fix the doc when issue 3 lands (cascade becomes readable, or
arrays-as-values lands and the original advice becomes correct).

**Severity**: medium. Same severity bucket as issue 3 — not
blocking, but every additional Region-2 sound experiment hits this.

## 4c. MIDI subscription silently drops on patch reload (sometimes)

**Symptom**

After a sequence of `aither send <patch>` reloads (especially when
several patches are loaded and individually muted/unmuted), the
Minilab keyboard stops triggering anything. `./aither midi list`
shows the port is still visible to ALSA. Reissuing
`./aither midi connect Minilab3:0` returns `OK connected` and
playback resumes immediately.

So it's not the hardware, not ALSA, not the engine crashing — it's
the engine's subscription to the MIDI source being lost without any
log line saying so.

**Why this matters**

In a live-coding session, MIDI dropping silently is a "did the
patch break or did the engine break" diagnostic blind spot. The
user reaches for the keys, hears nothing, and has to guess whether
the patch's gate logic is wrong (fix in code) or the engine forgot
the controller (fix with `midi connect`).

**Fix direction**

- Auto-resubscribe on patch reload if the previous subscription
  was active.
- OR: log a clear line to `/tmp/aither.err` whenever a
  subscription is dropped, so the diagnostic is immediate.
- Plus: include the connected MIDI port in `./aither list` output
  so a quick check can confirm or rule it out.

**Severity**: low-medium. Easy workaround (`midi connect` is one
command), but the silent failure mode wastes time and breaks live
flow.

## 5. Already-shipped language enablers (mention for SPEC completeness)

These landed inside commit 28cbb75 as part of unblocking the stdlib
work, not in their own commit. Worth a SPEC pass to make sure they
read as first-class language features, not stdlib implementation
details:

- **Group-depth tokenization** — newlines inside `(...)` and `[...]`
  are whitespace. (Already documented in SPEC.md and GUIDE.md.)
- **Let-prefixed lambda bodies** — `n => let pf = n*freq; <expr>`
  parses as `nkBlock`. (Documented in SPEC.md "Lambdas" section.)

No bug here, just noting that these are now load-bearing for any
non-trivial `sum(N, fn)` use.

## 6. Forward direction — sound design

(Not a bug — a note for tomorrow's session so it doesn't get lost.)

The cello + bell verification proved that `additive(freq, shape, N)`
and `inharmonic(freq, ratio, amp, N)` aren't just "synth that
replicates acoustic instruments." They're a fully open spectral
designer. Real opportunity:

- **Hybrid spectra** — interpolate a `shape` between two named
  shapes via a knob (the cello_verify knob-blend already does this:
  `bright * (1-blend) + warm * blend`). Same trick for ratio
  functions: `(1-k) * stiff_string(n) + k * phi_partials(n)` is a
  valid ratio fn, and the result is a sound that doesn't exist in
  any acoustic instrument.
- **Time-varying spectra** — replace the constant `shape` with one
  that takes `t` and morphs slowly; or replace the constant `freq`
  with a chord array indexed via `wave`.
- **Stochastic ratios** — use `noise()`-driven jitter on partial
  ratios for swarms / clusters.
- **Aliasing as a knob** — the `if pf >= 24000 then 0` band-limit in
  `additive`/`inharmonic` is conservative. A patch that overrides
  this and lets partials wrap can deliberately produce metallic
  digital character.

The next session's work direction should bias toward one such
experimental patch — something that couldn't exist on a Casio
keyboard or in a hardware synth — rather than perfecting more
acoustic-instrument emulations. The point of additive is freedom,
not realism.

## Order to tackle

1. Solo bug fix (small, unblocks live A/B comparison).
2. `prev` inline-call investigation + regression test (medium,
   unblocks edge-triggered patches like the bell strike).
3. `else if` chaining (small, cosmetic).
4. Dangling-binop continuation (small-medium, quality of life).
5. Stdlib cleanup pass — rewrite `bar_partials` / `plate_partials`
   in the simpler `else if` form once (3) lands.
6. Push everything that's been queued (currently 5 commits ahead of
   origin: 2adb710, a0563ef, 28cbb75, f604143, 5df01c4).
7. Open-ended sound design session — start from a non-acoustic
   target and see how far the additive/inharmonic primitives can
   take it.
