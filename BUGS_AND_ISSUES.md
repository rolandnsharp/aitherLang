# Bugs and issues

**Open as of 2026-10-17:** issue 6 (a sound-design direction, not a
bug). Every other numbered item is resolved; the write-ups stay
below as a record of what went wrong and why.

## Session log: 2026-10-17

| Issue | Status |
|-------|--------|
| the engine's stdin went through stdio, which reads ahead: it answered only every second block, so a program on both ends (`tools/card`) deadlocked | RESOLVED cb0fd08 (read(2)/write(2); lockstep is in SPEC) |
| `echo "bye"` on `kill` wrote text into the float stream on stdout | RESOLVED 3874713 (stderr) |
| an expression can't continue onto the next line (issue 4) | RESOLVED 5bd77e1 |
| blocks were delimited by indentation (issue 7) | RESOLVED (a block ends at its expression; indentation means nothing) |
| an offline render or `audit` with an idle pipe on stdin (ssh, a script) waited forever | RESOLVED 43ec94c (offline reads stdin only for a patch that calls `input()`) |
| after an engine was killed (Ctrl-C), the next one died on "Address already in use": `fileExists` is false for a socket, so the stale one was never removed | RESOLVED (probe it: a live engine answers and the new one says so; a stale socket is removed) |
| a client that hung up before reading its reply (a socket probe, a killed `aither list`) froze the engine's command loop for good: Nim's `send` with SafeDisconn retries a closed peer forever. Sound kept playing, `kill` hung | RESOLVED (send without SafeDisconn, so it raises; no reply to an empty command) |
| `adsr` starts a note only when `gate > 0.5`, but `midi_keyboard`'s gate was the velocity (0..1): a key played softer than half velocity never sounded, and strike detectors (`gate > 0.5 and prev(gate) < 0.5`) missed soft notes | RESOLVED: gates are 1/0 (`midi_gate`, `midi_voice_gate`); velocity is `midi_vel` / `midi_voice_vel` and the voice function's `vel`, kept through the release; `key` is new. `adsr` keeps its 0.5 threshold, which dusk and weather rely on with decaying gates |
| a top-level `def ratio(n, k)` breaks stdlib's `inharmonic`, whose *parameter* `ratio` is called as `ratio(n)`: "ratio: missing 'k' (stdlib:186)", even when `inharmonic` isn't called. A parameter should shadow a global def | RESOLVED (normalizeCalls skips calls to a name a def or lambda parameter shadows; tests/param_shadow.nim) |
| `midi_cc(...)` isn't hoisted: a coefficient that depends only on knobs (spectra's per-partial `exp`s) runs every sample, though the knob can only change between blocks | open, an idea to discuss: treat `midi_cc` as constant within a block |
| the uConsole's es8388 capture reads a flat constant on every input, headset plugged in | open, hardware side; see notes/card-2026-10-16.md |

## Session log: 2026-10-12 parallel session (from notes/handoff-2026-10-11.md)

Fractional `delay`, state arrays + `each`, `import` + `watch`, migration
keyed by owning def, `park`, scope-aware meters, `render`, `status`,
`%`, `1e-3`, stereo `haas`/`reverb`. Rows below marked RESOLVED
2026-10-12; status table in the handoff. Open: lyrics in the language
(by choice), server-side visuals.

## Session log: 2026-10-10 live set (bush doof on native title land)

Two silences, slow iteration and inaudible layers during a streamed set.

| Issue | Status |
|-------|--------|
| a hot-swap that blows up goes silent until resent | RESOLVED (rollback to the previous version within 20 s of a swap; +9.5 dBFS bar) |
| NaN in a `$var` re-NaNs every sample (resetPool skipped vars) | RESOLVED (scrubState; hard reset after 100 ms; NaN vars never migrate) |
| runaway guard mutes until resent | RESOLVED (wipes state, fades back in after 2 s, 3 tries) |
| inserting a stateful call shifts slots in unrelated plays | RESOLVED (regions keyed by statement: `play x/let y`) |
| `send` meter reads -inf (taken before any sound) | RESOLVED (`send` waits 1 s and reports; `check`) |
| no per-part levels; parts isolated by sed | RESOLVED (`parts` meters; part table in `audit`/`try` with <200 Hz share) |
| offline audit replays from t=0, not the live state | RESOLVED (`try`; `audit --from`) |
| let used before its line → TCC "'l_x' undeclared" | RESOLVED |
| `$x = init` can't see lets | RESOLVED (vars init where declared) |
| `max`/`min` take only two args | RESOLVED |
| play-only file compiles to silence | RESOLVED (sums its plays) |
| haas / reverb reject stereo input | RESOLVED 2026-10-12 (haas delays one side of a pair; reverb runs one per side) |
| a def param named like a top-level stereo let (adsr's `s`) was read as that pair | RESOLVED (the nearest scope binding wins) |
| no number exponents (`1e-3`) | RESOLVED 2026-10-12 (`1e-3`, `2.5E6`; `%` is an alias of `mod` too) |
| renamed statement inherits a vanished statement's state when only (type, perTypeIdx, size) match, even from a different def: replacing `stick` ($x,$dx x3) with `bar` ($bv,$bx x3) copied old displacement in, `try` showed +24 dBFS (2026-10-11) | RESOLVED 2026-10-12 (regions are also keyed by the def they sit in and the `$state` name: `bar/$bv`; another def in the same place starts clean) |
| a set that grows to many muted parts renders near real time and the stream underruns (holes of exact zeros = crackle); mute doesn't skip computation | RESOLVED 2026-10-12 (`park <voice> <part>`: fades out, then the part's body is skipped and its state frozen; `unpark`/`unmute` resume. `mute` still computes, so clocks stay in step) |

## Session log: 2026-10-03 pipe chains + runaway guard

Found while writing patches on a Pinebook Pro.

| Issue                                          | Status   |
|------------------------------------------------|----------|
| `x \|> bpf(..) \|> hpf(..)` blows up to ~+200 dB | RESOLVED |
| `f(..) \|> lpf(..)` garbage when `f` has a filter | RESOLVED |
| unstable patch drives the speakers at full scale | RESOLVED (runaway guard) |
| `list` shows `fading-in` forever               | RESOLVED |
| `reverb` won't take stereo                     | RESOLVED 2026-10-12 |

The two pipe bugs were one bug. Each native call compiled to
`(s->idx = off, n_lpf(s, sig, ...))`: s->idx was set *before* the
args ran, so a native inside `sig` moved it and the outer filter ran on
another call site's slots. Codegen now evaluates the args into locals
first and sets s->idx last (`nativeAt`). `patches/flute.aither` was
broken by this and works again. Pinned by `tests/pipe_chain.nim`.

The runaway guard mutes any voice whose output passes ±8 (+18 dBFS)
until it's resent or unmuted, instead of letting the output `tanh`
turn it into a full-scale square wave. Pinned by
`tests/runaway_guard.nim`.

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
| 4 · dangling-binop continuation      | RESOLVED 2026-10-17 | 5bd77e1 |
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

**Fixed 2026-10-17.** A newline ends a statement unless the statement
can't end there (trailing operator or `=`) or the next line can't
begin one (leading infix operator, `then`, `else`). Token-based, so
indentation means nothing; `- not ( [` still start a new line. It works
in both directions, unlike the trailing-only fix proposed below. SPEC
"Group-depth tokenization"; tests/continuation.nim. The original
write-up follows.

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

## 7. Language: blocks without meaningful whitespace

**Resolved 2026-10-17: a block ends at its expression.** Roland's
idea, that a new `let` or `def` could signal the end of a block,
refined: a body is its bindings (`let`, `$state`, element stores,
`each`, the array-handle updates), ended by the expression that is its
value, as lambda bodies already were. No braces, no `end`, and
indentation is skipped, never measured. All 514 bodies in the corpus
already had that shape; the 163 files parse to identical trees. The
top level keeps its one expression, which catches a body that ended
early. SPEC "Blocks end at their expression"; tests/blocks.nim. The
original write-up follows.

Raised 2026-10-17 while settling issue 4. Roland wants aither to move away from meaningful
whitespace (and from extra brackets), toward something more like JS.
Line continuation now looks only at tokens, so indentation means
nothing there. Blocks still do: `play x:` and `def f(x):` bodies are
delimited by indentation (tkIndent/tkDedent).

Options to discuss, none chosen:

- **Braces**: `play x { ... }`, `def f(x) { ... }`. The JS look; every
  block-using patch changes (514 `play`/`def` blocks across
  the corpus), and `{` would be new syntax.
- **An `end` keyword** (Lua, Ruby, Julia): `play x: ... end`. No
  new bracket type; same corpus churn.
- **Keep indentation for blocks only.** The rest of the language is
  whitespace-free now; a block's body is the one place indentation
  still counts.

Whatever is chosen, the corpus check from issue 4 applies: parse all
`.aither` files before and after and diff the trees. A migration that
rewrites patches mechanically would be part of the change. Also
check how the tail-recursion `def rec` and lambda bodies (`n => let
...` lines inside parens) interact with the chosen block form.

## Order to tackle

1. Issue 6 is open-ended: a sound-design session whenever it calls.
