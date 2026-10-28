# Architecture

## Overview

```
aither.c           ~430 lines   the entry point: the engine, a command sent to it, or `audit`
parser.c           ~890 lines   tokenizer + recursive descent → syntax tree
imports.c          ~170 lines   `import`: libraries spliced in, once each
codegen.c         ~2760 lines   tree → C source + per-helper-type state layout
voice.c            ~650 lines   a voice: TCC compile → tick(); hot-reload migration, rollback, the -O2 swap
dsp.c              ~340 lines   native DSP primitives (filters, delay, tape, reverb…)
engine.c          ~1660 lines   the pump, voice table, guards, -O2 builds, rollback, UNIX socket + commands
render.c           ~190 lines   offline render to memory, with each play's level
analysis.c         ~230 lines   FFT + spectral features
cli_output.c       ~320 lines   every text aither prints: list, levels, parts, check, spectrum, audit
midi.c             ~290 lines   MIDI as signals: held notes, the natives, the input thread, resubscribe
alsa_midi.c        ~170 lines   ALSA seq client (small wrapper)
realtime.c          ~85 lines   the mix thread on the audio core, at real-time priority
stdlib.aither      ~460 lines   composition layer (pure aither defs), embedded in the binary
tests/*.c                       the suite (tests/run), the corpus, exact
tools/play.c                    a speaker on the end of the pipe (ALSA, not part of aither)
tools/scope.c, tools/display.h  a vector display on the pipe (SDL3, OpenGL ES 3); display.h is
                                what scope and tv share (stdin, pass-through, the heard-sample
                                clock, GL, the window, terminal and frames destinations)
tools/tv.c                      a raster display on the pipe: w×h samples a frame, domain colouring
tools/pad.c                     the uConsole gamepad as MIDI knobs
```

All of it is C, about 8,200 lines, built by `cc` alone (`make`), and
so are the tests (notes/c-port-2026-10-28.md). One binary. No runtime
dependencies beyond libtcc, ALSA, and the system audio library.

## Layers

```
  user patch (.aither)          source file
        │
        ▼
  parser.c, imports.c           tokens → syntax tree, imports spliced in
        │
        ▼
  codegen.c                     tree → C source string
        │                       + per-helper-type state region map
        ▼
  voice.c                       TCC in-memory compile → tick()
        │                       + state migration (preserves per-region state
        │                         across hot-reloads, skips NaN-poisoned regions)
        ▼
  engine.c (pump)               per-voice tick → per-part gain → master sum
        │                       + per-voice rolling stats / FFT buffer
        ▼
  stdout                        raw float32 pairs; a player / scope / SDR / file reads them
```

The engine accepts CLI commands over a UNIX socket; `aither.c`
is the entry point that either runs the engine (`start`) or
dispatches a command (`send`, `stop`, `list`, `audit`, `spectrum`,
…) to the running engine.

## Compilation pipeline

A patch source is parsed once into a syntax tree, then `codegen.c`
emits a single C source string containing:

- `tick(state, t)` returning a stereo sample
- `init(state)` zeroing the state region
- A per-voice float64 state pool layout (region map: which DSP
  helper owns which slot indices)

The C is handed to TCC (libtcc, called from `voice.c`). TCC compiles
to machine code in memory in a few milliseconds. `voice.c` takes the
`tick` and `tick_block` entry points and holds them, with the state
buffer and its layout, in a `Voice`.

Hot-reload (`aither send <file>` against an existing voice)
compiles the new code off the audio thread, then atomically
swaps the `tick` pointer under a brief mutex. Per-region state
migration runs at swap time:

- For each new region, find the matching old region by
  `(typeName, perTypeIdx, size)`.
- If the old region contains no NaN/Inf, copyMem it across.
- If poisoned, leave the new region zeroed (recovers cleanly).

This is what makes "edit one helper, keep the reverb tail
ringing" work — every per-helper-type state slot is matched by
identity, not by raw byte offset, so insertions don't shift the
storage of everything after them.

## Data model

### Patch state region map

```c
typedef struct {            /* codegen.h */
  const char* type_name;    /* "phasor", "lpf", "discharge", … */
  int per_type_idx;         /* nth phasor / nth lpf / … in source order */
  const char* scope;        /* the statement it sits in: `let bass`, `play drums/out` */
  int scope_idx;            /* its index among same-type calls there */
  const char* owner;        /* the defs it is inlined through, and `/$name` for state */
  long offset, size;        /* in slots of the pool */
} Region;
```

Codegen counts every stateful helper call site and records its
region. Both old and new compiles produce a region list; migration
matches them by `(scope, type, owner, scope_idx)`, then by
`(type, owner, per_type_idx)`.

### Voice

`Voice` (voice.h) holds the current `Version` (its TCC state, `tick`
and `tick_block`, its state buffer: the generated struct, then its
pool, and its layout: var names, part names, regions), the version
before the last commit for rollback, the code of old versions the
audio thread may still be running, the voice's sweep origin, its
generation (so a stale -O2 build is refused), and its parts' gains,
fades, meters and park flags.

### Slot (engine-side per-voice state)

```c
typedef struct {
  char*      name;
  Voice*     voice;          /* voice.h */
  int        active;         /* true while audible */
  int        muted;
  double     fade_gain, fade_delta;
  Stats      stats;          /* rolling RMS / peak / clips / envelope */
  RecentRing recent;         /* 0.5 s float buffer for `spectrum` */
  int        held, park_at_zero, parked;   /* mute and park */
  /* the guards' bookkeeping, reset by a send: swap_at, want_rollback,
     stuck, rolled_back, bad_run, nan_events, runaway_events, retry_at */
} Slot;
```

Master bus has its own `Stats` and `RecentRing`. `MAX_VOICES = 16`.

## Per-sample execution

`tick(state, t)` is the entire hot path. Codegen emits straight-line
C with no allocations and no conditionals beyond the user's own
`if/then/else`. The C code reads from / writes to its slot range
in `pool[]` directly via offset constants baked at compile time.

Values are float64 throughout. Stereo is two adjacent floats
written to output pointers. There is no Value box, no stack VM,
no instruction dispatch — the patch IS the compiled function.

## Engine

### Audio callback

```
for i in 0 ..< frames:
  advance t
  lMix = rMix = 0
  for v in 0 ..< slotCount:
    (l, r) = slots[v].voice.tick(t)
    if NaN: zero pool, log once, contribute 0
    else:
      gl = l * fadeGain
      gr = r * fadeGain
      update stats
      write to recent ring
      lMix += gl ; rMix += gr
  update master stats + master recent ring
  output = (lMix, rMix)          # no limiter: the reader may be a scope or a radio
```

Tanh is the master limiter. Per-voice peak/RMS, clip count, and
the rolling 0.5 s float32 buffer for `spectrum` are all tracked
lock-free (audio thread writes; socket commands snapshot under a
brief mutex).

### Threading

- The pump (run every voice for a block via its compiled `tick_block`,
  mix, write the period to stdout) on its own thread; a blocking write
  is how a real-time reader paces the engine. The per-sample work
  (clock, input, parts, fade and gain ramps, part meters) is generated
  C; the engine does guards, stats, ring and mix per block.
- Hoisting (codegen `isInvariant` / `hoist`): a native whose
  coefficient arguments are compile-time invariant is emitted as its
  `_c` step with the coefficient in a static computed by `hoist_init`,
  called once per block (and per call in the one-sample `tick`). The
  natives in `dsp.c` are split into `coef` and `_c` halves, with the
  plain native being one calling the other, so both paths are the
  same bits. `AITHER_HOIST=0` disables it.
- ALSA MIDI input on its own thread (auto-resubscribes if the
  port is dropped — see `midi.c`).
- Main thread accepts socket commands.
- `mtx` locks around patch load, voice add/remove, part-gain
  adjustment, and stats snapshots. The mix waits for it rather than
  skip a block (the stream is gapless), and a command waiting for it
  goes first.

### Hot reload

`send <file>` either creates a new voice or in-place recompiles:

- New voice: parse, codegen, TCC compile, dlopen, register.
- Hot-swap: snapshot old part gains by name, parse + compile new,
  migrate state by region identity, swap function pointers, restore
  matching part gains. New parts default to gain 1.
- `sweep <voice> [seconds]`: read or set the voice's sweep (its origin on the engine clock).

### Voice slot lifecycle

When a voice's fade-out completes (in the audio callback), its
slot is marked inactive but stays in the table. The next `send`
sweeps inactive slots out before checking the `MaxVoices` limit.
This keeps `aither stop X; aither send Y` working indefinitely
without manual cleanup.

### Stats

Per-voice + master both use the same `Stats` struct:

- peak (exponential decay, ~300 ms)
- RMS (exponential smoothing, ~200 ms)
- clips (counter; clears on read)
- envelope ring buffer (20 bins × 50 ms = 1-second sparkline)
- recent samples ring (24000 frames = 0.5 s, float32, mono mix —
  read by `spectrum` for FFT analysis)

## CLI architecture

`aither.c` is the entry point (`make` builds it). It dispatches:

- no verb (or patch files and a duration) — call `engine_run`
  in-process: the engine itself.
- `audit <patch> [seconds]` — call `render_source` then
  `analyze` then `cli_audit` and print.
  Pure offline, no engine connection.
- All other commands — open the UNIX socket, send the command
  string, print the response. Engine handles the dispatch
  server-side and returns formatted text.

The engine's command handler returns formatted text directly
because the response is plain stdout. Internally, engine.c takes
snapshots under the lock (`VoiceSnap`, `StatsSnap`, `CheckSnap`, in
engine.h) and lets go of it; the `cli_output.c` formatters turn them
into text. The split lets engine state be tested without parsing
strings.

## Parser

Indentation-sensitive tokenizer with group-depth handling
(newlines inside `(...)` and `[...]` are whitespace). Recursive
descent with precedence climbing. AST kinds:

```
nkNum, nkIdent, nkBinOp, nkUnary, nkCall, nkIf,
nkVar, nkLet, nkDef, nkPlay, nkAssign, nkLambda,
nkArr, nkIdx, nkIdxAssign, nkBlock
```

Precedence (low → high):

```
parsePipe   →  |>
parseOr     →  or
parseAnd    →  and
parseCmp    →  == < > <= >= !=
parseAdd    →  + -
parseMul    →  * / mod
parseUnary  →  -x  not x
parsePostfix → primary | primary[idx]
parsePrimary → literal | ident | (expr) | call | array | if | lambda
```

`else if` chains parse cleanly because `parsePrimary` recurses
into `if` after `else`. Single-arg lambdas (`n => expr`) are
recognised in builtin-arg position only; the body may be a
let-prefixed block.

Pipe is the lowest-precedence operator (OCaml/Elixir/F# style).
The parser emits a dedicated error for `x |> f() * y` suggesting
parens or a `let` binding.

## Codegen

Single-pass emission from AST to C source:

- Numeric literal propagation through `let`, `def` parameters,
  and `sum`'s loop bound. Lets `additive(f, shape, 16)` resolve
  `max_n = 16` at compile time so `sum(max_n, ...)` unrolls.
- `sum(N, lambda)` is a special form: walks the lambda body N
  times with `n` substituted, emitting N parallel C expressions.
  Each iteration's stateful primitives claim their own region
  in the state pool — that's why `sum(16, n => phasor(n*f))`
  produces 16 independent phasor states.
- A `def rec` is the one def that isn't inlined: each call site
  becomes a C `for (;;)` loop, a repeat in the def's body a
  `continue` with the parameters reassigned, any other result a
  `break`. One copy of the body runs again and again, so its state
  regions are shared by every step. The loop checks the block's
  deadline every 65 536 steps (`n_overrun`); past it, the voice is
  flagged `stuck` and the engine rolls it back or mutes it
  (notes/tail-recursion-2026-10-16.md).
- Per-helper-type state region tracking. Each call to a stateful
  helper (`phasor`, `lpf`, `discharge`, …) is assigned a unique
  `perTypeIdx` so insertions/deletions migrate cleanly.
- Stereo-aware emission. `refsStereo` walks expressions to detect
  whether an operand is `[L, R]`; codegen emits separate `outL`
  and `outR` paths only when needed.

## Stdlib

`stdlib.aither` is `staticRead` into the binary and prepended to
every user patch at load time. Source line numbers in errors
refer to the user file (the combined source's offset is tracked
internally).

The aither stdlib contains things expressible as pure aither defs:

- Oscillators (`osc`, `pulse`, `spin`, `dho`, `resonator`, `vdp`)
- Spectral synthesis (`additive`, `inharmonic`, plus the shape
  / ratio / amp library: `saw_shape`, `cello_shape`, `vowel_ah`,
  `stiff_string`, `bar_partials`, `phi_partials`, `bell_decay`,
  …)
- Character effects (`drive`, `wrap`, `bitcrush`, `downsample`,
  `dropout`, `fold`)
- Envelopes (`pluck`, `swell`, `adsr`)
- Stereo helpers (`pan`, `haas`, `width`, `mono`)
- Misc (`gain`, `prev`, `slew`, `tremolo`, `ease`)

Heavier DSP (SVF filters, Schroeder reverb, ring-buffer delay and
tape, impulse, discharge, `wave`) lives in `dsp.c`, plain C compiled into
aither with no fused multiply-adds, called from generated C.

## Analysis CLI

`analysis.c` is pure (no engine or stdlib dependencies). Takes
a buffer of doubles + sample rate, fills a `SpectrumSummary`:
top-N peaks, spectral centroid, RMS, peak dB, zero-crossing rate,
estimated fundamental.

`render.c` runs a patch offline (parse → codegen → TCC compile
→ tick N samples → return buffer). Doesn't touch the engine.

`./aither audit <patch> [seconds]` composes them: render →
analyze → format → print. ~100 ms turnaround for a 2-second
render. Useful for verifying a patch produces what you intended
(correct fundamental, harmonic series falloff, no aliased peaks)
without bothering anyone with playback.

`./aither spectrum [voice]` does the same for a LIVE voice's
recent buffer (the engine maintains a 0.5 s rolling float32
ring per voice; `spectrum` in engine.c copies it out under the lock
and the analysis runs against that).

## Design invariants

Established through the project's evolution; not to be re-debated
without a concrete musical need:

1. **`osc(shape, freq)`** — shape and clock composed. No
   shortcuts like `saw(440)`.
2. **Two stateful builtin primitives: `phasor`, `noise`.**
   Everything stateful in user-visible stdlib is built from
   these (plus the native DSP helpers in `dsp.c`).
3. **`$state` / `let` scope rules** — see SPEC.md Scope section.
   Top-level `$state` shared by name; `def` `$state` per call-
   site; lambda-body `$state` per-iteration; `play`/`def` body
   `let` lexically block-scoped.
4. **`play` blocks compile inline** into the main chunk so they
   see file-level lets. `def` bodies compile as separate
   functions.
5. **Final expression is the voice output.** No implicit sum.
6. **Stdlib embedded** as a const string. One binary.
7. **Feedback is mutation.** Any `$state` reading itself is the
   entire state model for recurrence.
8. **Sine is the only true oscillator primitive.** Saw/square/
   triangle and every named instrument are sums of sines via
   `additive` / `inharmonic` built on `sum(N, lambda)`.

## Not built

- Multichannel output beyond stereo.
- Sample file I/O (deliberate — math, not samples).
- Networked multi-engine.
- Spatial audio beyond pan / Haas (no HRTF, no occlusion).
- Polyphony beyond per-voice MIDI mono (voice-allocated chord
  playback).

See SPEC.md and COMPOSING.md for current capabilities.
