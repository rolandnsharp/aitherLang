# The output is a pair

Decided 2026-10-15 (Roland, with Claude on the uConsole). How aither
serves sound, the oscilloscope and radio without a second language,
how the engine becomes a Unix filter, and what that means for the
`scope` branch merge.

## The decision

A program is a function of state whose output is its final expression:
a pair. A float at the output means `[s, s]`. That is the whole
interface.

What the two numbers mean is decided outside the language, by
whatever program reads the engine's output:

| reader      | reads the pair as                          | `[s, s]` is          |
|-------------|--------------------------------------------|----------------------|
| a player    | left, right                                | mono                 |
| a scope     | x, y: a point in the plane                 | the 45° line         |
| an SDR      | I, Q: complex baseband                     | a real signal, rotated 45°, which no receiver can tell from `[s, 0]` |
| a file      | two channels                               |                      |

This is PHILOSOPHY.md's "two cells become a complex number when an
operation reads them as one" taken literally. The player reads the
pair as two channels, the scope as the Argand diagram (visualising a
complex signal *is* drawing it), the radio as baseband, where `rotate`
is a frequency shift, `magnitude` the AM envelope and `phase` the FM.
Same two numbers, same operations, three meanings, and the language
names none of them.

The float shorthand is a *definition*, not a reader's interpretation:
there is one rule, `[s, s]`, so nothing depends on who is reading.
It keeps `sin(TAU * phasor(220)) * 0.2` a complete program. Inside a
program floats stay floats (oscillators, envelopes, filters, physics);
`play` parts and `let`s may be either; `mono(s)` remains, an operation
that reads a pair as a real.

## The engine is a filter

aither does one thing: it runs the voices it has been sent, at one
rate, and writes the mix to standard output as raw interleaved
float32 pairs, no header. Everything that touches hardware or a file
format is another program on the end of a pipe.

```
aither | aplay -f FLOAT_LE -c 2 -r 48000       # the engine, listening; a player on the end
aither set.aither | aplay …                    # same, with a first voice loaded
aither send set.aither 2                       # hot-swap with a 2 s fade, as today
aither mute didge drone 2                      # the gestures, as today
aither solo / park / stop / retrigger / parts / check / spectrum …
aither set.aither 30 > take.raw                # offline: 30 s from t=0, then exit
aither audit set.aither                        # the instrument panel, offline
arecord -f FLOAT_LE -c 2 -r 48000 | aither fx.aither | aplay …    # input() from the pipe
AITHER_RATE=192000 aither fm.aither | sdr-tx   # radio
aither set.aither 30 | sox -t raw -r 48000 -e float -b 32 -c 2 - take.wav
```

- The bare command is the engine. `start` goes.
- `send` stays and is the way a change arrives. Send-on-save is the
  editor's job (a `:w` hook, a browser editor), not the engine's;
  `watch` goes.
- The control socket stays: a performance gesture is about *when* it
  happens, which is what a command is for and a code edit is not. All
  the voice/part verbs keep working unchanged.
- The rate is `AITHER_RATE`. Programs are written in seconds and
  hertz, never samples, so the same text runs at 48 kHz for sound,
  192 kHz for radio, 160 kHz for the bat lure.
- `input()` reads standard input when it is a pipe; silence when it
  is a terminal. aither is then an effect between two programs.
- If standard output is a terminal the engine refuses with a one-line
  hint rather than dump floats into the shell. That is the only place
  it looks at what it is connected to.
- The offline renderer stays as the internal API that `audit`, `try`,
  `check`, the lesson checker and the tests use. The `render` verb,
  the WAV writer and the WAV reader go: a WAV is the pair stream with
  a 44-byte header, and sox and ffmpeg exist. (Neither is on the
  uConsole yet: `alsa-utils` for `aplay`/`arecord`, `sox` for files.)
- miniaudio leaves the engine entirely: the 4 MB header, the wrapper,
  device init, duplex, the 5-channel pinning. The engine builds
  without ALSA and runs where there is no sound card. For the
  uConsole a sixty-line `tools/play.c` in the style of `chime.c` is
  an option if `aplay`'s buffering is not good enough for live use;
  a tool beside aither, not a verb of it.

Sound, picture and radio are not simultaneous. A session is heard, or
seen, or transmitted, by whatever is on the end of the pipe.

## Visuals

The beam is the visual ([SCOPE.md](../SCOPE.md) is the guide). A
trace is the final expression of the patch it draws:

```
play drone: ...                # the didge model
...
[lip_pos * 4, lip_vel * 0.1]   # the last line: the phase portrait comes out
```

The trace lives in the same file as the music, so every name is in
scope: the pitch, the bar phase, the state of the physical model.
Change the last line and the session shows the instrument's dynamics
instead of playing it. The plain vector scope of the music is the
ordinary patch piped into a scope program.

The raster look (trails, feedback, kaleidoscope, palette: the Winamp
layer) is whatever scope program is on the pipe, with the `.paint`
scenes from the stream as that program's presets. It is not aither's
concern and the language never needs a colour or a third channel;
brightness is dwell, blanking is a jump off-screen, as on a real
scope. We are not building that program as part of the language.

## Arrays (settled 2026-10-14/15, restated)

Arrays are data: tables, `wave` lists, buffers, scratch and state
arrays, and the returned pair. Nothing maps a function over an array;
write the repetition out with `sum` / `each` and a table. A function
that wants a table reads a table whatever its length
(`wave(1, [0.25, 0.75])` works, commit 8407443); arithmetic is
element-wise on pairs and broadcasts a float; any other array in
arithmetic is a compile error that says to index it.

## What this means for the `scope` merge

Take the engine and language work on `scope` (state arrays, `each`,
`len`, `import`, fractional `delay`, independent `noise()`, `park`,
migration keyed by def, meters and `status`, `%`, negative literals,
exponents, lp1/hp1 in hertz, haas and reverb taking pairs, the tests).
Then, in one sweep:

- the bare command runs the engine with stdout as the output; `start`,
  `watch`, `render`, `AITHER_SCOPE` and `--scope` go;
- miniaudio and the 5-channel layout go; `input()` reads stdin;
- the magic `scope` / `beam` part names and the third channel go;
- rename the meters verb: `aither scope [voice]` is today the RMS /
  peak / envelope command and the word now means the beam (`levels`?);
- `patches/didge_scope.aither`: its scope and beam parts become the
  last line of a trace file, or the stream's scope program reads the
  mix; the stream's `low` / `mid` / `hit` become that program's own
  analysis;
- keep `stream/` as the web client and its presets; drop the
  server-side C paint renderer from the roadmap;
- convert the one `lp1` call in `edc/11_physics.aither` to hertz
  (done in the merge working tree; it was in a comment);
- resolve the SPEC.md conflict by keeping main's arrays and oscillator
  text and adding scope's state-array / `each` sections (done).

### Retirement inventory (merged tree, 2026-10-15)

Everything that knows about `scope` / `beam` parts, the five channels
or the sound card:

| where | what |
|-------|------|
| `engine_types.nim` | `isScopePart` (the magic names) |
| `engine.nim` | `AITHER_SCOPE`, `ScopeOut`, the per-voice `scopeIdx` / `beamIdx` lookup, the 5-channel write; the miniaudio callback becomes the stdout loop |
| `miniaudio.h`, `miniaudio.nim`, `miniaudio_wrapper.c` | removed |
| `codegen.nim` | `heard` excludes the two names from the final mix |
| `render.nim` | `beam` x/y/z in the render result; `writeWav` / `readWav` |
| `aither.nim` | `start`, `watch`, `render`, `--scope`, usage text |
| `cli_output.nim` | meters skip scope parts ("drawn, not heard") |
| `SPEC.md`, `GUIDE.md`, `README.md` | the `scope`/`beam` paragraph, `render`, `start`, `AITHER_BUFFER` advice |
| `tests/scope_meters.nim`, `tests/render_wav.nim` | delete / rewrite against the raw stream |
| `edc/` | every lesson's "how to play" header says `./aither start` |

## Rejected on the way

- **N-channel outputs** (colour as `[r, g, b]`): 378 of codegen's 2070
  lines are the pair machinery, every site would become a loop over
  channels, and only colour would use it.
- **Loops with their own `send`** (`aither scope`, `aither radio`), a
  rate per loop, and names read across programs with streams and
  samplers at the boundary: all of it exists only to hear and draw at
  once. Not needed.
- **An `out` keyword** naming destinations in the file.
- **`--out` / `--in` / `--rate` flags**: the output is always pairs,
  so there is nothing to choose; stdout is the output and the rate is
  an environment variable like the others.
- **Auto-detecting a terminal to pick the speaker**: `./aither start >
  engine.log` would have filled the log with floats.
- **A `play` / `mic` verb inside aither**: `aplay` and `arecord` read
  and write raw float pairs already; keeping hardware out of the
  binary is the gain.
- **The engine watching files**: an editor's job.
- **Magic part names** (`play scope:`): meaning by name is what the
  philosophy says not to do.
- **A `stereo(l, r)` constructor**: the pair as an array is the point.
- **Mandatory pair at the output**: `[s, s]` as a definition removes
  the ambiguity that argued for it.

## Still open

- **Written fades (TODO, Roland wants something more elegant).** The
  socket verbs (`mute`, `stop`, `park` with a fade) are gestures: they
  happen *when* you type them. Sometimes a fade belongs in the piece
  instead. The obvious route is a `sent_t` global, the time this file
  was last hot-swapped, so `drone * (1 - ease((t - sent_t) / 2))`
  fades from the moment of the send. It works but it is a mechanism,
  not a phrase: it needs a second send to remove the faded part, and
  it says nothing about what the part *is doing*. Wanted: a spelling
  where a part's entrance and exit are part of its definition, so the
  gesture and the score are one thing, and the engine's gain ramp and
  the written fade are the same mechanism seen from two sides. Not
  decided; don't add `sent_t` until it is.
- Pipe latency: one buffer per hop (10 ms at 512 frames). Nothing for
  live coding; worth measuring for the mic-through-effects case.
- The radio sink (`~/aitherRadio` is empty). The shape and operations
  need nothing new.
