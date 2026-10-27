# aither

A live-coded language for audio. The contract is `f(state) → sample`.

The engine calls your function 48,000 times per second. You return a
stereo sample. The engine sends it to the speakers. That's the whole
contract — the engine has no opinion about whether you're doing
additive synthesis, physical modeling, FM, granular, sample playback,
or something nobody has named yet. Your patch IS the synthesis method.

```
play piano:
  midi_keyboard((freq, gate) =>
    additive(freq, warm_shape, 16) * adsr(gate, 0.01, 0.2, 0.7, 0.4))

piano |> reverb(2.5, 0.2)
```

Polyphonic warm pad — `midi_keyboard` provides 8 voices of polyphony,
`additive` builds the spectrum from 16 sine waves, `adsr` shapes each
key's amplitude. Edit the shape function and resend; the chord ringing
under your fingers re-tunes mid-note.

Compiled to native code in milliseconds. Hot-reloaded without
dropping state. State persists across edits — every `$` slot, every
filter, every delay tail.

## What aither is

A function-of-state language for sound. The state is yours: every
`$state` you declare, every filter's internal memory, every phasor's
phase, every reverb's tail buffer. Aither preserves it across
hot-reloads by per-helper-type identity, so editing one helper
doesn't shift the storage of everything after it. The function is
yours: you write what synthesis you want by writing the math.

The stdlib ships starter kits for the common cases — `additive`,
`inharmonic`, the shape/ratio library, envelopes, filters, reverb,
stereo helpers, polyphony — but none of them are the LANGUAGE.
They're convenience defs. The language is the contract:
`f(state) → sample`, evaluated at audio rate, hot-reloadable,
composable.

## Five paradigms, one interface

Different musical ideas want different math. Aither's interface
supports all of them through the same `f(state) → sample` contract:

**Stateless function of time** — pure math, no memory:
```
sin(TAU * 440 * t) * 0.3
```

**Phase accumulation** — explicit state, hot-swappable. The `$`
sigil marks state at every use site:
```
$phase = 0.0
$phase = ($phase + 440 / sr) mod 1.0
sin(TAU * $phase) * 0.3
```

**Additive synthesis** — sums of sines via the `sum` fold:
```
sum(16, n => sin(TAU * phasor(n * 440)) / n) * 0.3
```

**Physical modeling** — damped harmonic oscillator, the universal
physical primitive `ẍ + 2γẋ + ω²x = F(t)`:
```
$x = 0.0
$dx = 0.0
let w2 = (TAU * 440) * (TAU * 440)
$dx = $dx + (-2 * $dx - w2 * $x + impulse(2) * w2) * dt
$x = $x + $dx * dt
$x * 0.3
```

**Spatial / field-based** — multi-dimensional state for spatial
processes:
```
$x_pos = 0.0; $y_pos = 0.0
# ...wave equation across ($x_pos, $y_pos)...
```

The engine doesn't know which paradigm you picked. It calls your
function and plays the result. Use any. Mix any. Invent new ones.

## Build

```
make
```

One binary, ~800 KB, and it never touches a sound card: the engine
writes raw float pairs to standard output and whatever is on the end
of the pipe (a player, a scope, an SDR, a file) gives them their
meaning. Linux-only for now (MIDI via ALSA seq).

Needs Nim, a C compiler, libtcc and the ALSA headers. On
Alpine/postmarketOS:

```
apk add nim build-base tcc-dev tcc-libs-static alsa-lib-dev
```

`tcc-libs-static` provides TCC's runtime library (`libtcc1.a`), which
TCC links into every patch it compiles.

```
make test            the test suite
make corpus          does every patch still sound exactly as it did?
make corpus-update   re-record that, after a change meant to alter some
```

`make corpus` renders every tracked `.aither` file as committed (so
uncommitted edits to a patch don't count), with the working tree's
engine and stdlib and every knob at half, in three short windows: a
chord held, a chord change (release tails, new attacks), and a fresh
start at t = 150 s, so sections that fade in later are heard. It
compares each one's exact samples with
`tests/corpus.golden`, naming every patch whose sound changed and by
how much. A change to the language or the stdlib that touches a patch
it wasn't meant to shows up there; one that was meant to shows up as
the golden file's diff in its commit.

## Use

```
aither | aplay -f FLOAT_LE -c 2 -r 48000      the engine: pairs on stdout, a player on the end
aither | tools/play hw:rockchipes8388c 20     (the uConsole, with AITHER_RATE=44100 in .bashrc:
                                              its codec only does 44.1 kHz; make tools/play builds the player)
aither set.aither | tools/scope | tools/play hw:rockchipes8388c 20
                                              see it as well: a vector scope on the GPU (make tools/scope, needs sdl3)
AITHER_RATE=1152000 aither patches/mandelbrot.aither | AITHER_RATE=1152000 tools/tv --size 160x120 > /dev/null
                                              a picture: each pair a pixel's complex value, as colour (make tools/tv)
aither music.aither | tools/play hw:rockchipes8388c 20 | AITHER_SOCK=/tmp/b.sock aither picture.aither | tools/tv --size 48x27
                                              a picture that listens: play hands the music on after the speaker has it,
                                              and the second engine's input() hears it (its own socket; demos/plate)
aither set.aither 30 > take.raw               offline: 30 s from t=0, then exit (sox makes the WAV)
AITHER_RATE=192000 aither fm.aither | sdr-tx  the rate is yours; the reader agrees
arecord -f FLOAT_LE -c 2 -r 48000 | aither fx.aither | tools/play    input() reads stdin
tools/card -- aither fx.aither                the card on both ends: input() hears it, the output plays,
                                              at a fixed loop delay; sets the rate itself (make tools/card)

aither send piano.aither              load & play
aither send piano.aither 2            ...with 2-second fade in
aither stop piano 1                   fade out over 1 second
aither mute piano                     silence whole voice (state keeps running)
aither solo piano                     fade other voices to muted
aither park piano.drone 2             fade one play out, then stop computing it
aither list                           show active voices + per-play gains
aither parts piano                    focused per-play view for one voice
aither levels master                  master-bus RMS / peak / clips / envelope
aither spectrum [voice]               FFT of voice's recent buffer (or master)
aither audit piano.aither 2           offline render + spectral analysis
aither sweep piano 0                  the piece plays from the top (aither sweep piano: where it is)
aither midi list                      ALSA seq input ports
aither midi connect 28:0              subscribe to a specific port
aither clear 2                        fade everything out
aither kill                           shut down
```

## MIDI input

When the engine starts it auto-connects to the first ALSA seq
input port it finds. The polyphonic front door is `midi_keyboard`:

```
play piano:
  midi_keyboard((freq, gate) =>
    additive(freq, vowel_ah, 12) * adsr(gate, 0.01, 0.3, 0.7, 0.5))
```

Press multiple keys; hear multiple voices. 8-voice polyphony, oldest
note evicted at the 9th held key. The lambda runs once per held key
with that key's frequency and gate (1 while held); name `vel` and `key`
too, `(freq, gate, vel, key) =>`, for its velocity and MIDI key number.

For controllers (knobs, sliders, pads) and monophonic synths:

```
midi_cc(n)        # knob/slider n — 0..1
midi_freq()       # Hz of most recent note (mono)
midi_gate()       # 1 while the most recent note is held (mono)
midi_vel()        # its velocity 0..1, kept after release
midi_trig(n)      # 1.0 for one sample on each note-on for note n
midi_note(n)      # velocity while note n is held; 0 else
```

If no device is connected all return 0 — the patch still runs.
Hot-reload preserves held notes and knob positions.

## The model

A file is a voice. Three kinds of declaration, then a final
expression that's the voice's stereo output.

```
def warm_pad(f, g):                  # helper — your choice of synthesis
  additive(f, warm_shape, 12) * swell(g, 0.4, 1.2) * 0.15

let pos = sweep                      # this piece's own time; visible to all parts
let breathe = (sin(TAU * pos / 30) + 1) * 0.5

play pad:
  midi_keyboard((f, g) => warm_pad(f, g))

play drone:
  additive(110, warm_shape, 8) * 0.05 * breathe

(pad + drone) |> reverb(3.0, 0.3)    # final expression = voice output
```

Parts return float (mono) or `[L, R]` (stereo). Arithmetic is
polymorphic: `pad + drone` works whether either is mono or stereo.
The final expression IS the mix; subgrouping, sidechain, mastering
are just expressions.

## Signal primitives

**Hardcoded math:** `sin cos tan exp log pow sqrt abs floor clamp
int min max mod`. No surprises.

**Stateful sources:** `phasor(freq)` for ramp 0→1 at `freq` Hz;
`noise()` for white noise. Plus the `$` sigil for any state you
declare yourself — that's the entire memory model. A filter is
`$s1; $s2; ...`. A delay is `$buf[N]; $idx; ...`. A damped HO is
`$x; $dx; ...`. The language doesn't distinguish "primitive
state" from "user state." Every reference to memory wears the
`$` so the reader can see which values carry state and which
are recomputed each sample.

**Compile-time fold:** `sum(N, n => expr)` unrolls into N parallel
expressions at codegen time. The standard composition primitive for
anything you want N copies of (additive partials, modal banks,
voice pools).

**Native DSP** in `dsp.c`, callable from any patch: `lpf hpf bpf
notch lp1 hp1 delay fbdelay tape reverb impulse discharge wave`: only
what needs a buffer or a C loop. The oscillators (`spin`, `dho`, `vdp`)
and the helpers over them are aither, in the stdlib.

**Stdlib starter kits** (pure aither, in `stdlib.aither`):

- Spectral synthesis: `additive(freq, shape, max_n)`, `inharmonic`.
  Most patches stop here.
- Shape functions: `saw_shape`, `warm_shape`, `bright_shape`,
  `vowel_ah`, `vowel_ee`, `cello_shape`, etc.
- Ratio functions for inharmonic: `stiff_string`, `bar_partials`,
  `phi_partials`, etc.
- Polyphony: `midi_keyboard(voice_fn)`, `poly(N, voice_fn)`.
- Envelopes: `pluck`, `swell`, `adsr`.
- Stereo: `pan`, `haas`, `width`, `mono`.
- Effects: `drive`, `wrap`, `bitcrush`, `downsample`, `dropout`,
  `fold`, `gain`.
- Misc: `prev`, `ease`.
- Oscillator wrappers: `osc(saw, f)`, `osc(sqr, f)`, `osc(sin, f)` —
  for chiptune / lo-fi character or convenience. Aliases above ~1 kHz.

These are recipes, not the language. Read them, copy them, ignore
them, replace them. The language is the contract above; everything
else is your code.

## Examples across paradigms

### Polyphonic vowel pad (additive)

```
play choir:
  midi_keyboard((f, g) =>
    additive(f, vowel_ah, 16) * swell(g, 0.5, 1.5) * 0.12)

choir |> reverb(3.5, 0.3)
```

### Tuning fork (physical model — damped harmonic oscillator)

```
play tuning_fork:
  $x = 0.0
  $dx = 0.0
  let w2 = (TAU * 440) * (TAU * 440)
  $dx = $dx + (-2 * $dx - w2 * $x + impulse(2) * w2) * dt
  $x = $x + $dx * dt
  $x * 0.3
```

Same equation as a real tuning fork. Strike (impulse), ring at
440 Hz, decay naturally. No envelope needed — the physics has decay
baked in.

### FM feedback (one sample of memory)

```
play fm:
  $fb = 0.0
  $fb = sin(TAU * phasor(440 + $fb * 500))
  $fb * 0.3
```

### Acid bass (subtractive — saw through filter)

```
let notes = wave(2, [55, 55, 82, 55, 73, 55, 98, 55])
let env   = discharge(impulse(2), 8)

play acid:
  osc(saw, notes) |> lpf(200 + env * 4000, 0.85) |> gain(0.4)
```

### Chaos (coupled physics — Lorenz attractor)

```
play chaos:
  $x = 0.1; $y = 0.0; $z = 0.0
  let ddt = dt * 50
  let dx = 10 * ($y - $x); let dy = $x * (28 - $z) - $y; let dz = $x * $y - 2.67 * $z
  $x = $x + dx * ddt; $y = $y + dy * ddt; $z = $z + dz * ddt
  [$x / 22, ($z - 25) / 18] * 0.1
```

All five examples are `f(state) → sample`. The engine doesn't know
which is which.

## Verifying a sound

`./aither audit patch.aither 2` renders 2 seconds offline and prints
a spectral summary — top peaks, fundamental, centroid, RMS. Lets you
verify the spectrum you wrote IS the spectrum you hear before you
bother playing it.

```
$ ./aither audit examples/cello.aither 2
audit: examples/cello.aither (2.0s @ 48000 Hz)
  RMS:        -18.4 dB    Peak: -3.2 dB
  Fundamental: 110.0 Hz
  Centroid:   1842 Hz
  Top peaks:
    1.   110.0 Hz   0.0 dB
    2.   220.3 Hz  -6.1 dB
    3.   330.7 Hz  -8.2 dB
    ...
```

`./aither spectrum [voice]` does the same against the engine's
recent ~0.5 s of audio for a live voice — verify a played sound on
the fly.

## Compared to

**SuperCollider** — graph of precompiled UGens sequenced from a
Smalltalk dialect. The DSP itself lives in C++ you don't see and
can't easily change. Aither's stdlib is written in aither; you can
read the cello, edit it, hear the result on the next reload.

**FAUST** — block-diagram functional DSL that compiles ahead of
time to fast C++. Excellent for plugins; the compile-then-run model
means you can't sweep a knob mid-render. Aither also compiles —
each patch is transpiled to C and compiled in-process by TCC on
load — so edits are running native code within milliseconds.

**Sonic Pi / Tidal / Strudel** — friendly live-coding pattern
languages on top of synths and samples. You sequence pre-built
instruments. Aither is a level lower: you write the oscillator, the
spectrum, the envelope. Sequencing is `wave(beat_rate, [...])`,
not a separate concept.

The trade-off is honest: aither is slower per sample than a
compiled FAUST patch or a SuperCollider UGen written in C++. For
the kind of pieces aither targets — live-coded, evolving,
keyboard-played — that trade-off is worth it. For mixing 50
high-quality reverb tails simultaneously, use a DAW.

## Read more

- [PHILOSOPHY.md](PHILOSOPHY.md) — the contract, the five paradigms,
  why composition over graphs
- [SPEC.md](SPEC.md) — complete language reference
- [COMPOSING.md](COMPOSING.md) — composition idioms, common patterns,
  what to reach for and when
- [GUIDE.md](GUIDE.md) — hands-on how-to for writing and performing
- [SCOPE.md](SCOPE.md) — drawing with the beam: the output as x, y and
  what a trace can show
- [TV.md](TV.md) — drawing with pixels: the output as a raster of
  complex values, and what a picture can show
- [ARCHITECTURE.md](ARCHITECTURE.md) — implementation overview
- [BUGS_AND_ISSUES.md](BUGS_AND_ISSUES.md) — known issues + session logs
- [stdlib.aither](stdlib.aither) — the starter kits, in aither
- [notes/](notes/) — research notebooks, speculation and parked plans
  (sound frontiers, cybernetics, polyphase, game audio). Not part of
  the language; some record ideas that were tried and dropped.

## Architecture

```
parser.c           ~840 lines   tokenizer + recursive descent → syntax tree
imports.c          ~170 lines   `import`: libraries spliced in, once each
codegen.c         ~2680 lines   tree → C source + per-helper-type state layout
ccompile.nim       ~130 lines   the compiler as Nim sees it: source in, C and layout out
voice.nim          ~650 lines   TCC compile → tick(); hot-reload migration
tcc.nim             ~40 lines   minimal TCC FFI
dsp.c              ~360 lines   native DSP primitives (filters, delay, tape, reverb…)
dsp.nim             ~90 lines   dsp.c's functions for Nim, and the table of natives
midi.nim           ~360 lines   ALSA seq input thread + auto-resubscribe + held notes
engine.nim        ~1310 lines   audio callback + UNIX socket server + per-voice stats
engine_types.nim   ~100 lines   data structs (VoiceInfo, StatsSnapshot, MidiStatus…)
cli_output.nim     ~250 lines   text formatters for list/scope/parts/spectrum/audit
analysis.nim       ~250 lines   pure FFT + spectral feature extraction
render.nim         ~160 lines   offline patch render to in-memory buffer
aither.nim         ~210 lines   CLI dispatch (entry point — `nim c aither.nim`)
stdlib.aither      ~430 lines   composition layer (pure aither defs)
```

About 4800 lines Nim total. A patch is parsed to an AST, transpiled
to C, handed to TCC which compiles it to machine code in memory, and
the resulting `tick(state, t)` function pointer is called once per
sample from the audio callback. Hot reload compiles the new code off
the audio thread, then swaps pointers under a brief mutex while
migrating state regions by `(typeName, perTypeIdx, size)` identity.

TCC compiles fast but its code is slow, so each load also hands the
same C to a background worker that builds it with `cc -O2` (0.5–2 s)
and swaps the voice's `tick()` for the fast one: same state, so no
glitch. Heavy additive patches run up to ~5x cheaper once it lands
(the engine logs `optimized (cc -O2)`). A reload while it builds
just drops the stale result. Needs a C compiler on the PATH; set
`AITHER_OPT=0` to turn it off.
Dependencies: Nim's stdlib, libtcc, ALSA, and the system audio
library.
