# Architecture

## Overview

```
parser.nim      ~465 lines   tokenizer + recursive descent → AST
eval.nim       ~1150 lines   bytecode compiler + stack VM
dsp.nim         ~200 lines   native DSP primitives
engine.nim      ~500 lines   audio callback, socket CLI, voice management
stdlib.aither   ~100 lines   composition layer (pure aither)
miniaudio.h/c               audio backend (upstream, unchanged)
```

One binary, ~500 KB. No external dependencies beyond the
system audio library.

## Layers

```
  user patch (.aither)          source file
        │
        ▼
  parser.nim                    tokens → AST (ref objects)
        │
        ▼
  eval.nim (compile)            AST → bytecode chunks
        │
        ▼
  eval.nim (VM)                 bytecode → sample (× 48k/sec)
        │
        ├──► dsp.nim             native fast-path for filters/delay/etc.
        │
        ▼
  engine.nim (callback)         per-voice tick → per-part gain → master sum → tanh
        │
        ▼
  miniaudio                     OS audio
```

## Data model

### Value

```nim
type
  Value = object
    case kind: ValueKind
    of vkFloat: f: float64
    of vkArr:   buf: Buffer        # ref to growable float64 seq
    of vkFunc:  fid: int           # function id (builtin or user)
```

Three types: floats (the default), arrays (for buffers,
stereo pairs, wavetables, polyphony), function values
(passed as arguments, e.g. `osc(sin, 440)`).

### Chunk

```nim
type
  Chunk = ref object
    code:         seq[Instruction]
    constants:    seq[float64]
    constArrays:  seq[Buffer]
    callSites:    seq[CallSite]    # for user-def call-site state
    numLocals:    int              # stack-frame size
    numStateSlots: int
```

Each user `def` compiles to its own Chunk. Play-block
bodies compile inline into the main chunk.

### Voice

```nim
type
  Voice = ref object
    sr, t, startT:  float64
    mainChunk:      Chunk
    funcChunks:     seq[Chunk]              # user defs, indexed by fid
    vars:           seq[Value]              # top-level var state
    varSlots:       Table[string, int]      # name → var slot
    callSiteState:  seq[Value]              # call-site state for defs
    dspState:       DspState                # native DSP state pool (4 MB)

    partNames:       seq[string]             # per-play name
    partGains:       seq[float64]            # engine-controlled
    partFadeDeltas:  seq[float64]
    partFadeTargets: seq[float64]
```

## The VM

Stack-based, float-first. One `Value` stack shared across
frames. A frame stack holds user-def call/return.

Hot paths (binary arithmetic, variable loads, native calls)
use float-specific helpers (`popF`, `pushF`, `setTopF`) that
skip Value unboxing. Binary arith is polymorphic *only*
when operands are arrays — the float/float path is a tight
inline fast path.

### Per-sample execution

1. Reset `dspState.idx = 0` (native functions claim slots
   from index 0 each tick).
2. Set `t` global; `start_t` is per-voice.
3. `enterFrame(mainChunk, 0)` and `run()`.
4. Return value is `float` (mono) or `vkArr` of length 2
   (stereo).
5. Engine mirrors mono to both channels.

### Play-block compilation

The main chunk's top-level processing:

```
for each top-level stmt:
  if nkPlay:
    allocate play-local scope
    compile body
    emit opPartGain(idx)
    allocLocal(play_name)
    emit opStoreLocal(slot)
  elif nkLet/nkVar/nkDef:
    compile as stmt (no stack effect)
  elif (final) expression:
    compile with wantValue=true
  else:
    error
```

Play names become regular locals in the main chunk, making
forward cross-play references automatic (a later play that
reads `kick` just gets `opLoadLocal(kickSlot)`).

### State semantics

- **Top-level `var`**: keyed by name (`varSlots` table).
  Shared across all plays and defs. Safe to reorder.
- **`var` inside `def`**: keyed by call-site counter. Each
  call location gets an independent slot via the
  `callSiteState` array.
- **`let` inside `play`**: lexically scoped to that play
  (compiler pushes a fresh `Scope`, pops after body).

### Native DSP pool

`dspState.pool` is a fixed `array[524288, float64]` (4 MB).
Native DSP functions (lpf, delay, reverb, resonator, etc.)
each call `claim(n)` to take `n` slots. The compiler
pre-computes total slot need per chunk via a fixed-point
iteration over `countStateSlots`.

Pool claim is bounds-checked: if `idx + n` would overflow,
`claim` returns a shared "overflow slot" near the pool end
and sets a flag. Audio degrades (state collisions) but the
engine doesn't segfault.

## Engine

### Voice table

```nim
type
  Slot = object
    name:       string
    voice:      Voice
    active:     bool
    muted:      bool
    fadeGain:   float64
    fadeDelta:  float64
    stats:      Stats          # per-voice rolling RMS / peak / clips / envelope
```

`MaxVoices = 16`. Master-bus `Stats` tracks the same
fields for the pre-tanh mix.

### Audio callback

```nim
for i in 0 ..< frames:
  advance t
  lMix = rMix = 0
  for v in 0 ..< slotCount:
    (l, r) = voice.tick(t)
    advance slot.fadeGain per sample
    advance per-part gains toward their targets
    update slot.stats
    lMix += l * slot.fadeGain
    rMix += r * slot.fadeGain
  update master stats on (lMix, rMix)
  output = (tanh(lMix), tanh(rMix))
```

Tanh is the master limiter. Per-voice peak/RMS and
master peak/RMS are tracked lock-free (audio thread writes,
socket command reads eventually-consistently).

### Threading

- Audio callback on miniaudio's thread.
- Main thread accepts socket commands.
- `mtx` locks around patch load, voice add/remove, and
  part-gain adjustment. Callback uses `tryAcquire`; if it
  fails, outputs silence for that buffer (rare).

### Hot reload

`send <file>` either creates a new voice or in-place
recompiles an existing one:

- New voice: allocate, parse, compile, reset state.
- Hot-swap: snapshot old `(partName → gain, fadeDelta)`,
  recompile, restore gains by matching names. Parts that
  disappear simply stop; new parts default to gain 1.
- Retrigger (stop-then-send or `retrigger` command): reset
  `startT` so the composition clock restarts from zero.

### Stats

Per-voice + master both use the same `Stats` struct:
- peak (exponential decay, ~300 ms)
- RMS (exponential smoothing, ~200 ms)
- clips (counter; clears on read)
- envelope ring buffer (20 bins × 50 ms = 1-second sparkline)

## Parser

Indentation-sensitive tokenizer, recursive-descent parser
with precedence climbing. AST nodes:

```
nkNum, nkIdent, nkBinOp, nkUnary, nkCall, nkIf,
nkVar, nkLet, nkDef, nkPlay, nkAssign,
nkArr, nkIdx, nkIdxAssign, nkBlock
```

Precedence (low to high):
```
parsePipe  →  |>
parseOr    →  or
parseAnd   →  and
parseCmp   →  == < > <= >= !=
parseAdd   →  + -
parseMul   →  * / mod
parseUnary →  -x not x
parsePostfix → primary | primary[idx]
parsePrimary → literal | ident | (expr) | call | array | if
```

Pipe is the lowest-precedence operator — matching
OCaml/Elixir/F#. The parser emits a dedicated error for
pipe-result-used-in-arithmetic (`x |> f() * y`), suggesting
parens or a `let` binding.

## Stdlib

`stdlib.aither` is `staticRead` into the binary and
prepended to every user patch at load time (both are
parsed separately and their AST kids merged — error line
numbers refer to the user file, not the combined source).

The aither stdlib is small (~100 lines) and contains only
things expressible as defs: oscillator wrappers (`osc`,
`pulse`), character effects (`drive`, `wrap`, `bitcrush`),
envelopes (`pluck`, `swell`, `adsr`), stereo helpers
(`pan`, `haas`, `width`, `mono`), one-sample memory
(`prev`), and a few misc helpers (`gain`, `fold`).

Heavier DSP (SVF filters, Schroeder reverb, ring-buffer
delay) lives in `dsp.nim` as compiled Nim, accessed from
bytecode via `opCallNative`. This is a performance
optimisation, not a design distinction — from the user's
perspective they're the same kind of primitive.

## Design invariants

Established through the project's evolution; not to be
re-debated without a concrete musical need:

1. **`osc(shape, freq)`** — shape and clock composed. No
   shortcuts like `saw(440)`.
2. **Two stateful builtin primitives: `phasor`, `noise`.**
   Everything stateful in user-visible stdlib is built
   from these.
3. **Three value types:** float, function, array.
4. **`var` / `let` scope rules** — see SPEC.md's Scope
   section. Top-level `var` shared by name; `def` var is
   per-call-site; `play` body `let` is lexically block-scoped.
5. **`play` blocks compile inline** into the main chunk
   (so they see file-level lets). `def` bodies compile as
   separate chunks.
6. **Final expression is the voice output.** No implicit
   sum. The file ends with the mix.
7. **Stdlib embedded** as a const string. One binary.
8. **Feedback is mutation.** Any `var` reading itself is
   the entire state model for recurrence.

## Not built

- MIDI input
- Spectral / FFT primitives
- Multichannel output beyond stereo
- Sample file I/O
- Networked multi-engine

See SPEC.md and COMPOSING.md for current capabilities.
