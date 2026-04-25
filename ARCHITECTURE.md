# Architecture

## Overview

```
parser.nim         ~560 lines   tokenizer + recursive descent → AST
codegen.nim       ~1350 lines   AST → C source + per-helper-type state layout
voice.nim          ~260 lines   TCC compile → dlopen'd tick(); hot-reload migration
tcc.nim             ~40 lines   minimal TCC FFI
dsp.nim            ~205 lines   native DSP primitives (filters, delay, reverb…)
midi.nim           ~235 lines   ALSA seq input thread + auto-resubscribe
engine.nim         ~735 lines   audio callback + UNIX socket server + per-voice stats
engine_types.nim    ~50 lines   data structs (VoiceInfo, StatsSnapshot, MidiStatus…)
cli_output.nim     ~155 lines   text formatters for list/scope/parts/spectrum/audit
analysis.nim       ~250 lines   pure FFT + spectral feature extraction
render.nim          ~65 lines   offline patch render to in-memory buffer
aither.nim         ~105 lines   CLI dispatch (entry point — `nim c aither.nim`)
stdlib.aither      ~225 lines   composition layer (pure aither defs)
miniaudio.h/c                   audio backend (upstream, unchanged)
alsa_midi.c                     ALSA seq client (small wrapper)
```

About 4250 lines Nim total. One binary, ~970 KB. No runtime
dependencies beyond libtcc, ALSA, and the system audio library.

## Layers

```
  user patch (.aither)          source file
        │
        ▼
  parser.nim                    tokens → AST (ref objects)
        │
        ▼
  codegen.nim                   AST → C source string
        │                       + per-helper-type state region map
        ▼
  voice.nim                     TCC in-memory compile → dlopen'd tick()
        │                       + state migration (preserves per-region state
        │                         across hot-reloads, skips NaN-poisoned regions)
        ▼
  engine.nim (callback)         per-voice tick → per-part gain → master sum → tanh
        │                       + per-voice rolling stats / FFT buffer
        ▼
  miniaudio                     OS audio
```

The engine accepts CLI commands over a UNIX socket; `aither.nim`
is the entry point that either runs the engine (`start`) or
dispatches a command (`send`, `stop`, `list`, `audit`, `spectrum`,
…) to the running engine.

## Compilation pipeline

A patch source is parsed once into an AST, then `codegen.nim`
emits a single C source string containing:

- `tick(state, t)` returning a stereo sample
- `init(state)` zeroing the state region
- A per-voice float64 state pool layout (region map: which DSP
  helper owns which slot indices)

The C is handed to TCC (`tcc.nim` is a thin FFI). TCC compiles
to machine code in memory in a few milliseconds. `voice.nim`
extracts the `tick` and `init` function pointers via dlopen and
holds them in a `Voice` object.

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

```nim
type
  Region = object
    typeName:    string   # "phasor", "lpf", "discharge", …
    perTypeIdx:  int      # nth phasor / nth lpf / … in source order
    offset:      int      # byte offset into the float64 pool
    size:        int      # slots claimed
```

Codegen counts every stateful helper call site and records its
region. Both old and new compiles produce a region list; migration
matches them by `(typeName, perTypeIdx, size)`.

### Voice

```nim
type
  Voice = ref object
    sr:           float64
    startT:       float64                # composition clock zero
    pool:         seq[float64]           # state pool (sized by codegen)
    regions:      seq[Region]            # for migration
    tickFn:       TickFn                 # dlopen'd from TCC
    libHandle:    pointer                # for dlclose on swap
```

### Slot (engine-side per-voice state)

```nim
type
  Slot = object
    name:       string
    voice:      Voice
    active:     bool                     # true while audible
    muted:      bool
    fadeGain:   float64
    fadeDelta:  float64
    stats:      Stats                    # rolling RMS / peak / clips / envelope
    recent:     RecentRing               # 0.5s float32 buffer for `spectrum`
    nanLogged:  bool                     # one-shot NaN warning per session
```

Master bus has its own `Stats` and `RecentRing`. `MaxVoices = 16`.

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
  output = (tanh(lMix), tanh(rMix))
```

Tanh is the master limiter. Per-voice peak/RMS, clip count, and
the rolling 0.5 s float32 buffer for `spectrum` are all tracked
lock-free (audio thread writes; socket commands snapshot under a
brief mutex).

### Threading

- Audio callback on miniaudio's thread.
- ALSA MIDI input on its own thread (auto-resubscribes if the
  port is dropped — see `midi.nim`).
- Main thread accepts socket commands.
- `mtx` locks around patch load, voice add/remove, part-gain
  adjustment, and stats snapshots. Audio callback uses
  `tryAcquire`; if it fails, outputs silence for that buffer.

### Hot reload

`send <file>` either creates a new voice or in-place recompiles:

- New voice: parse, codegen, TCC compile, dlopen, register.
- Hot-swap: snapshot old part gains by name, parse + compile new,
  migrate state by region identity, swap function pointers, restore
  matching part gains. New parts default to gain 1.
- `retrigger`: reset `startT` so the composition clock restarts.

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

`aither.nim` is the entry point (`nim c aither.nim`). It dispatches:

- `start` — call `engine.startEngine` in-process.
- `audit <patch> [seconds]` — call `render.renderPatch` then
  `analysis.analyze` then `cli_output.formatAudit` and print.
  Pure offline, no engine connection.
- All other commands — open the UNIX socket, send the command
  string, print the response. Engine handles the dispatch
  server-side and returns formatted text.

The engine's command handler returns formatted text directly
because the response is plain stdout. Internally, engine procs
return data structures (`VoiceInfo`, `StatsSnapshot`, `MidiStatus`,
`SpectrumSummary`); `cli_output.nim` formatters turn those into
text. The split lets engine state be tested without parsing
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

- Oscillator wrappers (`osc`, `pulse`)
- Spectral synthesis (`additive`, `inharmonic`, plus the shape
  / ratio / amp library: `saw_shape`, `cello_shape`, `vowel_ah`,
  `stiff_string`, `bar_partials`, `phi_partials`, `bell_decay`,
  …)
- Character effects (`drive`, `wrap`, `bitcrush`, `downsample`,
  `dropout`, `fold`)
- Envelopes (`pluck`, `swell`, `adsr`)
- Stereo helpers (`pan`, `haas`, `width`, `mono`)
- Misc (`gain`, `prev`, `ease`)

Heavier DSP (SVF filters, Schroeder reverb, ring-buffer delay,
resonator, discharge, `wave`) lives in `dsp.nim` as compiled Nim,
called from generated C via small wrapper functions.

## Analysis CLI

`analysis.nim` is pure (no engine or stdlib dependencies). Takes
a `seq[float64]` + sample rate, returns a `SpectrumSummary`:
top-N peaks, spectral centroid, RMS, peak dB, zero-crossing rate,
estimated fundamental.

`render.nim` runs a patch offline (parse → codegen → TCC compile
→ tick N samples → return buffer). Doesn't touch engine.nim.

`./aither audit <patch> [seconds]` composes them: render →
analyze → format → print. ~100 ms turnaround for a 2-second
render. Useful for verifying a patch produces what you intended
(correct fundamental, harmonic series falloff, no aliased peaks)
without bothering anyone with playback.

`./aither spectrum [voice]` does the same for a LIVE voice's
recent buffer (the engine maintains a 0.5 s rolling float32
ring per voice; `voiceBufferSnapshot` copies it out and the
analysis runs against that).

## Design invariants

Established through the project's evolution; not to be re-debated
without a concrete musical need:

1. **`osc(shape, freq)`** — shape and clock composed. No
   shortcuts like `saw(440)`.
2. **Two stateful builtin primitives: `phasor`, `noise`.**
   Everything stateful in user-visible stdlib is built from
   these (plus the native DSP helpers wrapped from `dsp.nim`).
3. **`var` / `let` scope rules** — see SPEC.md Scope section.
   Top-level `var` shared by name; `def` var per call-site;
   `play` body `let` lexically block-scoped.
4. **`play` blocks compile inline** into the main chunk so they
   see file-level lets. `def` bodies compile as separate
   functions.
5. **Final expression is the voice output.** No implicit sum.
6. **Stdlib embedded** as a const string. One binary.
7. **Feedback is mutation.** Any `var` reading itself is the
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
