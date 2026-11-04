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

**A play's statements are units too** (done in the report): a play's lets
are names shared within the play, its last statement writes the play, and
each statement is timed on its own. `tests/tasks.c` holds the rules on
general shapes rather than on any one set: a band of plays (apart), lets
in one play (apart, the output waits for both), a conductor (readers
apart, each waits), a read before the writer (a need, not a merge), two
parts that need each other (one task), two writers of a name (one task).

## Step 2, done, and what it costs on one core (2026-11-04)

Committed b719f14; rows indexed through pointers made once a task after.
Seconds for 10 s of sound on the uConsole, before step 2 → after, on
general shapes (notes/multicore/shapes, timed by notes/multicore/tm.c;
"live" is tick_block, "offline" is tick):

| shape | TCC live | TCC offline | -O2 live | -O2 offline |
|---|---|---|---|---|
| band (8 plays) | 3.37 → 4.11 | 3.84 → 4.80 | 1.78 → 1.78 | 2.48 → 2.49 |
| bank (64 partials, one line) | 3.89 → 3.97 | 12.1 → 12.2 | 1.61 → 1.60 | 7.55 → 7.56 |
| field (16 coupled) | 1.53 → 1.76 | 1.42 → 1.88 | 0.43 → 0.43 | 0.43 → 0.50 |
| chain (into a reverb) | 0.61 → 0.73 | 1.12 → 1.31 | 0.43 → 0.43 | 1.03 → 1.07 |
| lets (120 one-line lets) | 7.09 → 9.54 | 21.8 → 29.8 | 2.88 → 3.60 | 13.8 → 15.7 |

- **-O2 live, what a voice plays once its build lands: unchanged**, but
  for tiny units (120 lets, +25%): each pays a loop and a buffer store
  as dear as its own work. The grain problem (notes/graph-2026-11-02.md).
- **TCC: 15–35% slower,** its unoptimised code paying each task's
  overhead. It matters on the uConsole, where a voice runs on TCC until
  its -O2 build lands (5 s to two minutes).
- **The fix for both is clustering:** about as many tasks as cores,
  balanced by cost. One core: one task, the code before step 2 exactly.
- **Offline is 1.4–4.7× slower than live on the same -O2 code**, before
  step 2 as after: tick() runs hoist_init every sample. Renders, audit,
  try, the corpus and the tests would gain from rendering through
  tick_block.

**Offline renders through tick_block (2026-11-04).** render_voice runs a
block at a time, with its own clock (t0 + i / sr), each sample before
the guard and the fade, and each part's sound per sample, through three
header pointers the engine leaves NULL (t_buf, raw_out, part_buf). Corpus
unchanged, make exact 180/180, make test 67/67. The heaviest renders
gain most (bush_doof_e 19 s → 9 s, didge_scope_vP_trombone_singer 18 s →
11 s); the corpus as a whole 92 s → 86 s, most of its time compiling.

## Clustering (2026-11-04)

The finest split stays what `aither profile` predicts from. The voice
runs merged tasks: each unit costs its tree (a node 1, a transcendental
4, a def call its body, `sum`/`each` N times their body), and the
cheapest task merges into a partner it feeds or is fed by, if no path
runs between them through a third (or the merge would need itself).
Merging keeps the file's order inside a task, so any clustering gives
the same bits: `make blocks` at AITHER_TASKS unset, 0 (finest), 2 and 3,
every patch bit-identical.

Merging stops at AITHER_TASKS tasks (default 1, until there are
workers) or where a merge would pass a cost of 2000. The cap came from
measuring: TCC is slow on one huge function (bush_doof_e, blocks per
10 s of sound: 28 s as one task, 18–20 s in tasks of 1000–3000), and
-O2 doesn't care. A patch under the cap is one task.

Seconds per 10 s of sound, uConsole, block | per sample:

| shape | before step 2 | finest | clustered |
|---|---|---|---|
| band, TCC | 0.85 \| 0.93 | 0.97 \| 1.21 | 0.98 \| 1.14 |
| band, -O2 | 0.53 \| 0.60 | 0.52 \| 0.65 | 0.52 \| 0.64 |
| bank, TCC | 0.86 \| 2.80 | 0.90 \| 2.78 | 0.88 \| 2.76 |
| field, TCC | 0.35 \| 0.31 | 0.41 \| 0.45 | 0.34 \| 0.33 |
| lets, TCC | 1.72 \| 5.04 | 2.11 \| 9.08 | 1.54 \| 5.09 |
| lets, -O2 | 0.68 \| 2.88 | 1.34 \| 3.49 | 0.63 \| 2.78 |
| bush_doof_e, TCC | 26.3 \| 31.6 | 18.3 \| 45.5 | 18.9 \| 34.9 |
| bush_doof_e, -O2 | 9.5 \| 12.6 | 9.0 \| 20.6 | 8.8 \| 13.2 |
| tesla_jam, TCC | 9.1 \| 15.6 | 7.1 \| 19.9 | 7.2 \| 16.3 |
| tesla_jam, -O2 | 4.3 \| 6.6 | 4.1 \| 7.7 | 4.2 \| 6.7 |

Blocks, what the engine runs, are as fast as before step 2 or faster
everywhere but band on TCC (+15%, the same as one task: not the split;
its cost is somewhere in the block's frame, to find). Big patches on TCC
gain a third, which is the uConsole while a -O2 build compiles.

## Step 3: the worker pool (2026-11-04)

`pool.c`. The mix thread and AITHER_THREADS - 1 workers, each pinned to a
core of its own at the mix thread's priority. Each run of a block (128
samples) is a job: the generated tick_block hands the pool its tasks
(`par` in the header), each task with how many it waits for and which
it feeds. A task joins a ready queue when its last need ends; whoever
reaches it first takes it, the mix thread too. Between jobs a worker
spins 200 µs, then sleeps on a futex. Only a thread that claimed the
pool (the engine's mix thread, a test) uses it; any other runs its
tasks in order, as before.

**Clustering changed for it.** Merging by cost alone (the commit
before) glued early and late units together until the tasks were nearly
a chain: bush_doof_e on 4 threads was slower than on 1. Now a merge is
made only if it doesn't lengthen how long the tasks would take on P
cores, at least: the longest path, or the work shared out evenly
(with 20 units of overhead a task), whichever is more. Chains merge;
small tasks side by side merge while the work outweighs the longest
path; big ones stay apart. P is four times the threads, for slack (on
two: planned for 2, 6.4 s; for 8, 5.1 s; for 16, 5.0 s). One thread
plans for one: everything merges, up to the cap, as before.

**Only the fast cores, by default.** A task on a slow core holds up
everything waiting for it. On the uConsole on mains (two A72s, four
A53s, `cpu_capacity` 1024 and 381), bush_doof_e's blocks ran 1.73x as
fast on the two A72s and 1.35x with all six. So the pool takes the
cores within 3/4 of the best: the two A72s on mains, all four A53s on
battery (alike). AITHER_THREADS asks for more.

Seconds per 10 s of sound, blocks, uConsole on mains (2 threads):

| shape | TCC: 1 thread → pool | -O2: 1 thread → pool |
|---|---|---|
| bush_doof_e | 19.6 → 10.3 | 8.9 → 5.0 |
| tesla_jam | 7.3 → 4.4 | 4.2 → 2.7 |
| band | 0.98 → 0.74 | 0.58 → 0.33 |
| lets | 1.55 → 1.27 | 0.61 → 0.51 |
| bank, chain, field | unchanged | unchanged |

Bit-identical: `AITHER_THREADS= make blocks` (2 threads), and 6 threads
on the finest split (A53s too, the most tasks at once), 181/181 each;
make test 67/67 (engine_live runs the pool); make exact 181/181.

**Live** (notes/multicore/pacer.c: reads the pipe at 48 kHz in periods
of 512 like a card, 50 ms latency, restarts after a late period, counts
them; silent). bush_doof_e for 90 s from a cold start:

- one thread: 160 late in the first 10 s (TCC), -O2 landed at about
  12 s, then 2 in 80 s.
- the pool: 4–6 late every 5 s on TCC, -O2 landed at about 37 s, then
  none.

The -O2 build took three times as long beside the pool: cc gets the
A53s while a worker holds the second A72 (the C compiles in the same
16 s for 11 tasks or 50, alone). Fewer late blocks in all (38 against
162), but a longer wait for -O2. Not measured: a laptop's equal cores,
where the gain should go past two.

## Step 4: `check` shows the split (2026-11-04)

`check` (and so `send`) ends with how the voice runs and, for each task
of parts kept together that holds a tenth of its estimated cost or
more, why. From the compiler: `run_of`, `unit_cost`, `task_glue` beside
the profile's tables. For example:

    sync:    runs as 7 tasks on 2 cores; largest 97.5% by estimate (set z (line 58) and others)
             kept in one task:
               7 lines from 58 to 73 (98%): reads the array $z
    field:   kept in one task:
               64 lines from 1 to 64 (94%): both write $x0, $v0, $x1, $v1 and 28 more

Silent when nothing heavy is glued (bush_doof_e: 82 tasks, the largest
8%). A loop is named by its reads from below their writer, the reads
that close it.

## The -O2 build in parallel (2026-11-04)

The -O2 build was one cc over the voice's whole C file: 17.8 s for
bush_doof_e on the uConsole, and beside the pool (a worker holding the
second A72) -O2 landed 37 s after a send. Now codegen marks the file
(the prelude; `/*@@cut@@*/` before hoist_init; `/*@@task@@*/` before each
task; `/*@@tail@@*/` before run_tasks) and voice.c compiles it as several
files at once, one cc each: the tasks shared out by size into one file
per core but one, and the tail (hoist_init, run_tasks, the tables, tick,
tick_block). The hoisted values are `AITHER_SHARED` (static in one file,
defined in the tail and extern in the rest when split), the tasks
`AITHER_TASKFN`. TCC's one file is unchanged. AITHER_CC_JOBS sets how
many files (1: one, as before).

| | one file | parallel |
|---|---|---|
| bush_doof_e, build alone | 17.8 s | 6.9 s |
| tesla_jam, build alone | 10.1 s | 4.0 s |
| bush_doof_e live, pool: -O2 lands after | 37 s | 17 s |
| bush_doof_e live, one thread: -O2 lands after | 12 s | 10.5 s |

Live, bush_doof_e's first minute: 16 late periods on the pool (38
before), all before -O2 landed. make exact 181/181, and 181/181 on the
finest split (the most files).
