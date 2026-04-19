# Architecture

## Overview

```
parser.nim     tokenizer + recursive descent → AST
eval.nim       bytecode compiler + stack VM
engine.nim     audio callback, socket CLI, voice management
stdlib.aither  DSP library written in aither
miniaudio      audio backend (C, unchanged)
```

One binary. No external dependencies beyond system audio.

## Design decisions

These were debated extensively. Don't revisit them.

1. **`osc(shape, freq)`** — shape + phasor composed. `sin`,
   `saw`, `tri`, `sqr` are pure math/shape functions.
   No shortcuts like `saw(440)`.

2. **`phasor(freq)` and `noise()`** — the ONLY stateful
   builtins. Everything else is stdlib written in aither.

3. **Three value types:** float, function, array. Function
   values exist for `osc(shape, freq)`. Arrays for
   polyphony (input) and stereo (output).

4. **`var` for persistent state.** Top-level: keyed by name.
   Inside `def`: keyed by call-site counter (same call
   order = same state, like claimDsp pattern).

5. **`let` for per-sample bindings.** Computed every sample.

6. **`if/then/else`** not `if/else:`. No significant
   whitespace. Expressions work on single lines.

7. **`|>` pipe operator.** Parse-time rewrite: `a |> f(b)`
   becomes `f(a, b)`. Lowest precedence.

8. **No closures.** Functions are first-class values (for
   osc) but don't capture enclosing scope.

9. **No UFCS.** Only `|>` for chaining.

10. **Auto-wrapping.** Engine separates `var` and `def`
    lines (module scope) from expression lines (tick body).

11. **Mutable arrays.** `array(n, init)`, `buf[i]`,
    `buf[i] = x`, `len(buf)`. For delay/reverb in aither.

12. **Math builtins in evaluator.** `sin`, `cos`, `tan`,
    `exp`, `log`, `log2`, `pow`, `sqrt`, `abs`, `floor`,
    `ceil`, `min`, `max`, `clamp`, `int`.

13. **Stdlib embedded as const string.** One binary.

14. **Feedback is mutation.** `var fb = 0.0; fb = sin(TAU *
    phasor(440 + fb * 500))`.

## AST nodes

```
Number(val)
Ident(name)
BinOp(op, left, right)
UnaryOp(op, operand)
Call(name, args)
Pipe(left, right)      # rewrite to Call at parse time
IfExpr(cond, then, else)
VarDecl(name, init)
LetDecl(name, init)
FuncDef(name, params, body)
Assign(name, value)
ArrayLit(elements)
ArrayIndex(array, index)
ArrayAssign(array, index, value)
Block(statements)      # last expr is return value
```

## Voice state

```nim
type
  Voice = object
    chunk: Chunk                      # compiled bytecode
    vars: Table[string, Value]        # top-level var state
    callSiteState: seq[Value]         # per-call-site state
    callSiteCounter: int              # reset each sample
    funcs: Table[string, FuncDef]     # user-defined functions
    startT: float64                   # time when first loaded
    active: bool
    fadeGain, fadeDelta: float64
```

## Per-sample execution

1. Reset `callSiteCounter` to 0
2. Set `t` and `start_t` globals
3. Execute bytecode, return float64
4. Clamp NaN/Inf to 0
5. Apply fade gain
6. Sum all voices, tanh soft clip, write to buffer

## Audio threading

- Audio callback on miniaudio's thread
- Main thread handles socket commands
- `tryAcquire(mutex)` in audio callback — if locked
  (patch loading), output silence for that buffer
- Lock mutex during patch loading/swapping

## Engine CLI

```
aither start               launch engine
aither send <file>         load patch (instant)
aither send <file> <fade>  fade in
aither stop <name>         stop immediately
aither stop <name> <fade>  fade out
aither mute <name>         silence (state runs)
aither unmute <name>       resume
aither solo <name> <fade>  fade everything else
aither list                show active voices
aither clear               stop all
aither clear <fade>        fade all out
aither kill                shut down
```

## Globals

| Name      | Description                           |
|-----------|---------------------------------------|
| `t`       | time in seconds                       |
| `sr`      | sample rate (48000)                   |
| `dt`      | 1 / sample rate                       |
| `start_t` | time when this voice was first loaded |
| `PI`      | 3.14159...                            |
| `TAU`     | 6.28318...                            |

## Reference implementations

Previous prototypes for reference (don't modify):

- `../aitherNim/` — compiled .so version
- `../aitherNimScript/` — NimScript VM version

## Not yet built

- Stereo / polyphony (array expansion)
- Composition (hold)
- MIDI input
- Signal references (conductor)
- Oscilloscope
- Browser target
