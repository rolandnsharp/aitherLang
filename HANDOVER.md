# Handover: Building the Aither Interpreter

## What to build

A tree-walking interpreter for a small audio DSP language,
written in Nim. Read SPEC.md first — it's the complete
language definition.

## Architecture

```
parser.nim     tokenizer + recursive descent → AST  (~250 lines)
eval.nim       tree-walking evaluator               (~250 lines)
engine.nim     audio callback + socket CLI           (~200 lines)
stdlib.aither  DSP library written in aither         (~200 lines)
miniaudio.nim  FFI wrapper (copy from ../aitherNimScript/)
miniaudio_wrapper.c  (copy from ../aitherNimScript/)
miniaudio.h    (copy from ../aitherNimScript/)
Makefile
```

Target: one binary, under 1 MB, no dependencies beyond
system audio library.

## Key design decisions (already settled)

These were debated extensively. Don't revisit them.

1. **`osc(shape, freq)` is the oscillator API.** `sin`, `saw`,
   `tri`, `sqr` are pure math/shape functions. `osc` applies
   a shape to a phasor. No shortcuts like `saw(440)`.

2. **`phasor(freq)` is the only stateful oscillator builtin.**
   Everything else is stdlib written in aither. `noise()` is
   the only other stateful builtin (random source).

3. **Three value types:** float, function, array. Function
   values exist for `osc(shape, freq)`. Arrays for polyphony
   (input) and stereo (output).

4. **`var` for persistent state.** Top-level `var` is keyed by
   name. `var` inside `def` is keyed by call-site counter
   (same call order = same state, like claimDsp pattern).

5. **`let` for per-sample bindings.** Computed every sample.

6. **`if/then/else` not `if/else:`.** No significant whitespace.
   Expressions work on single lines.

7. **`|>` pipe operator.** Parse-time rewrite: `a |> f(b)`
   becomes `f(a, b)`. Lowest precedence.

8. **No closures.** Functions are first-class values (for osc)
   but don't capture enclosing scope. Not needed — pipes
   handle composition.

9. **No UFCS.** Only `|>` for chaining. One pipe syntax.

10. **Auto-wrapping.** The engine reads a file, separates `var`
    and `def` lines (module scope) from expression lines
    (tick body). User never writes a proc signature.

11. **Mutable arrays.** `array(n, init)`, `buf[i]`, `buf[i] = x`,
    `len(buf)`. Needed for delay/reverb written in aither.

12. **Math builtins hardcoded in evaluator.** `sin`, `cos`,
    `tan`, `exp`, `log`, `log2`, `pow`, `sqrt`, `abs`,
    `floor`, `ceil`, `min`, `max`, `clamp`, `int`.

13. **Stdlib embedded as const string.** One binary, no files
    to distribute.

14. **Feedback is mutation.** `var fb = 0.0; fb = sin(TAU *
    phasor(440 + fb * 500))`. State variables are the honest
    expression of feedback.

## Value type

```nim
type
  ValueKind = enum vkFloat, vkFunc, vkArray
  Value = object
    case kind: ValueKind
    of vkFloat: f: float64
    of vkFunc: name: string  # function name for lookup
    of vkArray: arr: seq[Value]
```

Arithmetic broadcasts: `float + array` = add to each element.
`array + array` = element-wise. Same as NumPy.

## AST nodes

Minimum node types:

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
Block(statements)      # last expr is the return value
```

## Evaluator state per voice

```nim
type
  Voice = object
    ast: AstNode                      # parsed patch
    vars: Table[string, Value]        # top-level var state
    callSiteState: seq[Value]         # per-call-site state
    callSiteCounter: int              # reset each sample
    funcs: Table[string, FuncDef]     # user-defined functions
    startT: float64                   # time when voice first loaded
    active: bool
    fadeGain, fadeDelta: float64
```

Each sample:
1. Reset `callSiteCounter` to 0
2. Set `t` and `start_t` in vars (`start_t` = voice.startT)
3. Walk AST, return float64
4. Clamp NaN/Inf to 0
5. Apply fade gain
6. Sum all voices, tanh soft clip, write to buffer

## Engine CLI

```
aither start               launch engine
aither send <file>         load patch (instant)
aither send <file> <fade>  load with fade-in
aither stop <name>         stop immediately
aither stop <name> <fade>  fade out
aither mute <name>         silence (state runs)
aither unmute <name>       resume
aither solo <name> <fade>  fade out everything else
aither list                show active voices
aither clear               stop all
aither clear <fade>        fade all out
aither kill                shut down engine
```

## Audio threading

Same pattern as the working NimScript prototype:

- Audio callback on miniaudio's thread
- Main thread handles socket commands
- `tryAcquire(mutex)` in audio callback — if locked
  (script loading), output silence for that buffer
- Lock mutex during patch loading/swapping

## Reference implementations

Two working prototypes exist for reference:

- `../aitherNim/` — compiled .so version (original)
  - `dsp.nim` has all DSP algorithms (port the math)
  - `engine.nim` has the audio/socket architecture

- `../aitherNimScript/` — NimScript VM version
  - `dsp.nim` has DSP functions with global state pattern
  - `engine.nim` has nimscripter integration + auto-wrapping
  - Working examples in `examples/`

The DSP math (SVF filter, Schroeder reverb, etc.) is
identical across all versions. Port the algorithms,
not the glue code.

## Build order

1. Parser — tokenizer + recursive descent for a simple
   expression like `osc(sin, 440) * 0.3`
2. Evaluator — tree walk that can evaluate the parsed AST
3. Wire to audio — miniaudio callback evaluates per sample
4. Stdlib — shapes, osc, filters, effects in aither
5. Socket CLI — send/stop/list/kill
6. Hot-reload — parse new file, preserve var state
7. Fade in/out — fadeGain/fadeDelta per voice
8. Test with all examples from SPEC.md

## What NOT to build yet

- Stereo/polyphony (array expansion) — get mono working first
- Composition (hold) — future
- MIDI — future
- Signal references (conductor) — future
- Oscilloscope — future
- Browser target — future (but same parser compiles to JS
  via nim js)

## Testing

The acid bass example is the acid test:

```
let freq = wave(2, [55, 55, 82, 55, 73, 55, 98, 55])
let env = discharge(impulse(2), 8)
osc(saw, freq) |> lpf(200 + env * 4000, 0.85) |> gain(0.4)
```

If this plays correctly, the parser, evaluator, stdlib,
and engine all work. It exercises: let bindings, function
calls, pipes, array literals, stateful DSP (phasor, filters),
and arithmetic.
