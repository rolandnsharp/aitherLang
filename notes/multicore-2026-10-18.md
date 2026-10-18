# Multicore: a voice's parts on several cores

**Status:** design, for review. Nothing built yet.

## Why

Big sets are one file with many `play` parts, and one file is one voice:
all of it runs in one C function, one sample at a time, on one core.
Measured 2026-10-18 on the uConsole, `patches/bush_doof_e.aither` (25
parts) needs 87% of an A72 or 235% of an A53 just for the audio, plus the
TCC phase after a send. With the new batteries all six cores run (2 A72,
4 A53, roughly 3.5 A72s of capacity), but one voice can use one.

Splitting a set into separately sent files by hand doesn't work: the
parts share a conductor (`$bpos`, the jam scenes, `$m9start = t`
recorded at load), ducking (`kDuck`, `dDuck`) and a final line that mixes
and reverbs combinations of parts. The compiler sees all of that, so the
compiler does the split.

## The rule that has to hold

**Bit-identical to one core.** Every part computes exactly what it
computes today, sample for sample; only which core runs it, and when
within the block, changes. Checked by the corpus fingerprints and
`make exact`, plus a run with one thread against a run with many.

Nothing in the language changes. Patches don't change.

## How a voice runs today (from codegen.nim)

- `tick1` emits the program's statements in **source order**: top-level
  `let`s (C locals), `$state` (`s->v_name`), each `play` (its body in a
  block, writing locals `p_name_l/_r`), top-level code after plays (which
  can read `p_name`), then the final expression. `tick_block` loops it
  over the block's samples, then does fades and meters per part.
- A play may read earlier plays, file-level lets and `$state`; a later
  play only through `prev(name)` (last sample's value). Play-body `$x` is
  file-level by name: two plays writing `$count` share it.
- Every stateful call site has its own pool region, so parts never
  share a filter, oscillator or delay. Shared memory between parts is
  only: file-level `$state` and state arrays, values one unit writes and
  another reads, and two pieces of runtime scratch: `s->idx` (the cursor
  every native call sets) and `s->overflow`.

## The design

### 1. Units and their dependencies

A **unit** is one top-level statement or one `play`. For each unit,
codegen records what it reads and writes: lets, plays, file-level
`$state` and state arrays, `t`/input (read by everyone, written by none).
That gives a graph:

- **Same-sample edges.** B reads something A wrote earlier in the sample
  (a let, a play's output, a `$state` A updated). B must run after A.
  Source order guarantees these point forward.
- **Next-sample edges.** A reads something B writes later in the sample:
  `prev(B)`, or `$state` B updates that A read first. That's feedback
  through time.

Units joined by a loop through next-sample edges can't be separated:
A's sample 2 needs B's sample 1, which needs A's sample 1. Each such loop
(a strongly connected component) is **merged into one task**, which runs
sample by sample as today. Everything else is a task of its own.

### 2. Tasks run a whole block

Each task becomes its own C function that loops over the block:
`task_k(s, n)`. A task writes the values other tasks need into per-block
buffers (n doubles each, two for stereo: a play's output, a let read by
a later task, `$state` read after) and reads its inputs from them.
`t` and the input are per-block buffers filled up front.

Because the tasks form a graph without loops, running them one after
another in graph order computes the same numbers as today's
sample-by-sample order: each task sees the same inputs at every sample.
That's what makes the bit-exact promise possible, and it's step 2 below:
tasks, still on one core, must match today exactly.

### 3. The engine runs tasks on a worker pool

- A pool of `AITHER_THREADS` workers (default: every core online; set 1
  or 2 on battery). The thread writing the pipe works too.
- Each block, every voice's task graph is queued; a task becomes ready
  when the tasks it reads from are done. Workers take ready tasks, so the
  A72s simply finish more of them. Separately sent voices are just more
  graphs in the same queue: that covers several voices for free.
- When all are done, the pipe thread mixes the voices **in slot order,
  as today**, and does fades, meters and the guards.

### 4. What changes, file by file

- `dsp.nim` and the native calls: the native gets its pool offset as an
  argument instead of through the shared `s->idx` cursor (`claim()`
  advancing a local instead). Removes shared scratch; arguably simpler.
  `s->overflow` the same way.
- `codegen.nim`: read/write sets per unit, the merge rule, one C function
  per task, block buffers; `tick_block` becomes "run the tasks in order".
  The per-sample `tick` stays for `prev`-style single steps if anything
  still needs it, or goes if nothing does.
- `engine.nim`: the worker pool and the ready queue around `mixBlock`.
- `voice.nim`: task count, task entry points, buffer layout.
- SPEC: the runtime section (tasks, `AITHER_THREADS`); `check` reports
  how a voice was split.

### 5. Seeing the split

`aither check <voice>` lists the tasks and why parts were merged
(`didge + didge2: didge reads prev(didge2)`), with each task's share of
the voice's CPU. A set that merged into one big task runs as today; the
report says which line did it, so a part can be restructured if wanted.

## Limits

- **The serial part.** The conductor and whatever in the final line
  combines parts run on one core. bush_doof_e's final line has many
  `reverb(mono(part))` terms; each depends on one part, so each is its
  own small task and runs in parallel. A reverb over a sum of parts
  (`doofM`) has to wait for those parts.
- **Merging.** Feedback between parts (`prev`, shared `$state` written
  late and read early) glues them together. Correct, but less parallel.
- **Block buffers.** A voice keeps n × (values crossing tasks) doubles;
  at 512 frames and a few dozen values, a few hundred KB.
- **Power.** All cores busy draws more current; `AITHER_THREADS` is the
  knob for battery.

## Steps, each checked before the next

0. **Measure.** A per-part CPU meter (useful on its own): where
   bush_doof_e's time goes, and what share is conductor and final line.
1. **Natives without `s->idx`.** Corpus and `make exact` bit-identical.
2. **Tasks on one core.** The split, the buffers, run in graph order.
   Corpus, `make exact`, block_tick: bit-identical. The hardest step.
3. **The worker pool.** `AITHER_THREADS=1` against all cores: identical
   output; engine_live's gapless check; bush_doof_e's real-time load.
4. **`check` reports the split.** SPEC updated.

## Decided (2026-10-18)

- **Merging is silent but visible.** No warnings and nothing rejected:
  merging never changes the sound, only how parallel it runs. `send`'s
  report gets one line ("runs as 9 tasks; largest: didge + didge2, 31%")
  and `check` says why.
- **`AITHER_THREADS` defaults to every core online.** The pipe thread
  works too, and "online" follows the hardware: 4 on battery with the
  A72s off, 6 with them on.
- **Block-based only.** Step 2 replaces the per-sample `tick` path too
  (the offline renderer moves to blocks), so a voice runs one way
  everywhere and block_tick's equivalence check retires with it.
- **Step 0 is opt-in profiling,** `aither profile <voice> [seconds]`:
  timing every unit every sample costs a few percent, so it's on only
  while asked for.
