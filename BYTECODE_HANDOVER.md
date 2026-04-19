# Bytecode VM — Performance Fix

## Problem

The tree-walking evaluator in eval.nim is too slow for
real-time audio. With 4 voices at 48kHz, the engine falls
behind real-time — the beat slows down progressively.
The melody's delay buffer makes it worse (array indexing
in interpreted code is expensive).

This defeats the purpose of writing the interpreter in
Nim. The whole point was native performance.

## Solution

Replace the tree-walking evaluator with a bytecode
compiler + stack VM. Same parser, same engine, same
language. Only eval.nim changes.

### Compile step (once per patch load)

Walk the AST, emit bytecode instructions into a flat
array. This happens once when `aither send` is called.

### Execute step (48,000 times per second)

The VM executes bytecode in a tight loop. No AST
allocation, no recursive tree walking, no Value variant
dispatch. Just a flat instruction array, a float64 stack,
and a program counter.

## Bytecode instructions

```
OP_CONST      index      # push constants[index]
OP_LOAD_VAR   index      # push vars[index]
OP_STORE_VAR  index      # pop → vars[index]
OP_LOAD_T                # push current time
OP_LOAD_SR               # push sample rate
OP_LOAD_DT               # push dt
OP_LOAD_START_T           # push start_t
OP_ADD                    # pop b, pop a, push a+b
OP_SUB                    # pop b, pop a, push a-b
OP_MUL                    # pop b, pop a, push a*b
OP_DIV                    # pop b, pop a, push a/b
OP_MOD                    # pop b, pop a, push a mod b
OP_NEG                    # pop a, push -a
OP_EQ, OP_NEQ, OP_LT, OP_GT, OP_LTE, OP_GTE
OP_AND, OP_OR, OP_NOT
OP_JMP        offset     # unconditional jump
OP_JMP_FALSE  offset     # pop, jump if zero (for if/then/else)
OP_CALL       func_id  n # call builtin func_id with n args
OP_CALL_USER  func_id  n # call user-defined function
OP_CALL_FUNC_ARG func_id n  # call where first arg is a function value
OP_LOAD_FUNC  func_id    # push function value (for osc(sin, 440))
OP_ARRAY_LIT  n          # pop n values, push array
OP_ARRAY_GET             # pop index, pop array, push element
OP_ARRAY_SET             # pop value, pop index, pop array
OP_ARRAY_LEN             # pop array, push length
OP_RETURN                # end of function body
```

## Data structures

```nim
type
  OpCode = enum
    opConst, opLoadVar, opStoreVar,
    opLoadT, opLoadSr, opLoadDt, opLoadStartT,
    opAdd, opSub, opMul, opDiv, opMod, opNeg,
    opEq, opNeq, opLt, opGt, opLte, opGte,
    opAnd, opOr, opNot,
    opJmp, opJmpFalse,
    opCall, opCallUser, opCallFuncArg, opLoadFunc,
    opArrayLit, opArrayGet, opArraySet, opArrayLen,
    opReturn

  Instruction = object
    op: OpCode
    arg: int32          # index, offset, or arity

  Chunk = object
    code: seq[Instruction]
    constants: seq[float64]
    varNames: seq[string]  # for hot-reload mapping

  VM = object
    stack: array[256, float64]   # fixed-size, no allocation
    sp: int                       # stack pointer
    pc: int                       # program counter
```

The stack is a fixed-size array — NO allocation during
execution. Constants are stored once at compile time.
Variable values are in the voice's var table (persists
across hot-reloads).

## Performance estimate

Each instruction is: read opcode (1 branch), execute
(1-3 ops), advance pc. At ~5ns per instruction, a
20-instruction patch takes ~100ns per sample. Budget
is 20,800ns (48kHz). That's 0.5% CPU per voice vs
~25% for tree walking.

Target: 16+ voices simultaneously with headroom.

## What to keep

- parser.nim — unchanged
- engine.nim — change eval call from tree-walk to VM
- stdlib.aither — unchanged
- All .aither patches — unchanged

## What to replace

- eval.nim — delete tree walker, write:
  1. Compiler: AST → Chunk (bytecode)
  2. VM: execute Chunk per sample
  
## Call-site state (claimDsp equivalent)

Each OP_CALL to a stateful function gets state keyed
by the instruction's position in the bytecode. The
compiler assigns each stateful call a unique slot index.
The VM uses this index to access the voice's DSP pool.

On hot-reload: if the bytecode has the same stateful
calls in the same order, state maps correctly (same as
claimDsp counter pattern).

## Function values (for osc(sin, 440))

OP_LOAD_FUNC pushes a function ID onto the stack.
OP_CALL_FUNC_ARG pops the function ID and calls it.
The function table maps IDs to both builtins (sin, cos)
and user-defined functions. Simple integer dispatch.

## User-defined functions (def)

User functions are compiled to separate chunks. 
OP_CALL_USER switches to that chunk, executes, returns
to the calling chunk. Arguments are pushed onto the
stack before the call. The called function pops them.

## Build order

1. Define OpCode enum and Instruction type
2. Write the compiler (AST → seq[Instruction])
3. Write the VM loop (execute instructions)
4. Wire into engine (replace evalNode call with vm.run)
5. Test with acid bass example
6. Test with 4+ simultaneous voices
7. Benchmark: should be 10-50x faster than tree walking

## Acceptance test

Load kick, hat, snare, and melody simultaneously.
All four must play at correct tempo without slowing.
The melody's delay buffer must work. This is what
broke under the tree walker.
