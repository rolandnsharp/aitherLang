# Multicore: a voice's parts on several cores

**Status (2026-10-30):** step 1 built (04f52cc); step 2 refreshed for the C compiler below ("Step 2 in today's C"), not built.

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

## Done, and step 2 in today's C (2026-10-30)

**Why now.** Measured on battery (four A53s at 1.0 GHz), with `-O2` and
a 20 s gap between renders: `chimera` needs 3.29 cores, `bush_doof_e`
2.94, `tesla_jam` 2.66, `acid_complex` 1.92, `lattice` 1.48, `qigong_kit`
1.12. `aither profile` splits them well: `bush_doof_e` over about 260
units, the largest (the final line) 8.8% of the voice; `tesla_jam`'s
largest part `pad`, 25%. (The profile's total, "72.8% of one core", is a
share of a free-running engine's time, not a cost a second of sound.)

**Step 1, done.** Natives take their region's offset as an argument
(`site`); `s->idx` is no longer shared scratch. Corpus and `make exact`
bit-identical.

**Step 2 against codegen.c as it is now.** `tick1` is the whole program
for one sample: top-level `let`s are C locals (`l_x`, `l_x_l/_r`), a
play writes `p_x_l/_r`, file-level `$state` is `s->v_x` in the pool,
and `tick_block` calls `tick1` a sample at a time, then meters each part.
The design holds; four things are decided by the C:

1. **`t`, `sweep` and the input as buffers.** Generated code reads
   `s->t`; tasks running on other cores can't share that field. The
   block's clock is computed once, as now (the same additions, so the
   same bits), into a buffer each task reads by sample.
2. **File-level `$state` read across units, through a buffer.**
   `bush_doof_e`'s conductor (`$bpos` and the scenes) is written by one
   unit and read by many later parts. Merging every unit that touches it
   would leave one task. Instead the writer stores the value as it
   stands after its unit, a sample at a time, and later readers read
   that: what they read today, in source order. A unit that reads it
   before the writer (earlier in the file) reads the writer's value a
   sample late, as today: still only a need, the writer runs its block
   first. Units merge only when they need each other (a cycle), or both
   write one name.
3. **Def bodies count.** A def may read and write file `$state`, so a
   unit's reads and writes include those of every def it calls, through
   any depth.
4. **Meters and guards after the tasks.** Each part's output goes to a
   block buffer; the per-sample fades, meters and NaN guard run over the
   buffers once the tasks are done, in the order they run now.

Profiling (`aither profile`) times statements with shared counters; it
runs the tasks one after another, so it stays exact.

The check, as before: corpus and `make exact` bit-identical with the
tasks run on one core in order.

**`chimera`**, one play, gains nothing from units. Its parallelism is
its voices: with `poly`'s loop (notes/loops-2026-10-30.md) the voices are
already one body over a slot index, so splitting the loop's range across
cores is the voice-level version of the same idea, after step 3.

## The split, measured (2026-10-30)

`aither profile` now reports how a voice would split: codegen works out
each unit's reads and writes (defs through any depth), merges cycles and
shared writers, and emits `task_of` / `task_needs`; the profile weights
the tasks and schedules them a block at a time (handing-over cost not
in). The uConsole, free-running engine, `-O2` swapped in:

| patch         | tasks / units | largest              | chain | 2 cores | 4 cores | 8 cores |
|---------------|---------------|----------------------|-------|---------|---------|---------|
| bush_doof_e   | 250 / 261     | play organ 8.7%      | 19.7% | 1.84x   | 3.12x   | 4.80x   |
| tesla_jam     | 43 / 43       | play pad 22.1%       | 26.2% | 1.92x   | 3.54x   | 3.82x   |
| qigong_kit    | 23 / 23       | play harp 34.0%      | 37.3% | 1.74x   | 2.68x   | 2.68x   |
| cathedral_set | 26 / 26       | play choir 27.8%     | 37.3% | 1.82x   | 2.68x   | 2.68x   |
| acid_complex  | 35 / 35       | play pads 57.8%      | 61.4% | 1.63x   | 1.63x   | 1.63x   |
| lattice       | 15 / 15       | play lattice 31.5%   | 75.7% | 1.19x   | 1.32x   | 1.32x   |
| chimera       | 13 / 13       | let wet 36.2%        | 82.8% | 1.14x   | 1.21x   | 1.21x   |

Merging costs almost nothing: the conductor's state reads are needs, not
cycles. What caps the rest is one heavy part, of two kinds:

- **A play holding several instruments.** `acid_complex`'s `pads` is six
  handpans as six lets; `cathedral_set`'s `choir` two additive voices;
  `qigong_kit`'s `harp` its chord notes. A play's own statements as
  units (the same rules, one level in) splits them.
- **A chain: instrument, then the reverb over its mix** (`lattice`,
  `chimera`'s `wet`). Pipelining (the reverb on the block before) splits
  it at a block's latency, the sound unchanged but late; and `chimera`'s
  voices split by the poly loop's range.
