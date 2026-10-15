# Drawing with the beam

**Status: design, being built.** This describes the scope loop decided
in [notes/pair-output-2026-10-15.md](notes/pair-output-2026-10-15.md).
Until it lands, the live stream draws with `play scope:` parts (branch
`scope`), which this replaces.

A scope program is an ordinary aither program sent to the scope loop:

```
./aither scope trace.aither        # start drawing (hot-swaps like send)
./aither stop trace 1              # fade the beam out
```

Its final expression is a pair, read as **x, y**: a point on the
screen, −1..1 each way, one point per sample. The display joins the
points and fades them, so a slow motion is bright and a fast one is
faint, as on a phosphor tube. A float is read as a real number: the
point `[s, 0]`, on the horizontal axis.

The scope loop runs at the sound rate, in the same tick as the sound
programs, after them. So a trace reads *this sample's* values of
anything the music has a name for:

| name                 | what it is                                    |
|----------------------|-----------------------------------------------|
| `didge`              | a voice's output (a pair; `didge[0]` is left)  |
| `didge.drone`        | one of its `play` parts                        |
| `didge.bar_phase`    | a top-level `let` in it                        |
| `didge.lip_pos`      | a `let` or `$state` inside its model           |
| `didge.sticks.rms`   | the engine's meter for a part                  |
| `master.low`         | band energy of the mix (one filter, engine-side)|

Names are sample-accurate. Nothing is analysed or guessed from the
mix: the trace reads the pitch, the phase, the envelope the sound
actually uses. That is what makes the figures below possible; a
hardware scope, which only ever sees the mix, can draw just the first.

Brightness and blanking are not a third channel. The display reads a
named signal for brightness if you give it one, and a jump to a point
off-screen blanks the beam, as it does on a real scope.

## What to draw

### The audio itself

```
[didge[0], didge[1]]
```

Mono is a 45° line. Stereo width opens it into an ellipse; a phase
difference tilts it; clipping flattens the ends. This is the classic
vector scope, and it is free.

### A waveform that holds still

A scope needs a trigger to freeze a waveform. Here the trigger is a
name: sweep once per period of the note being played.

```
let x = phasor(didge.f0) * 2 - 1
[x, didge.drone]
```

The waveform stands still whatever the pitch does. Sweep on the bar
instead (`phasor(tempo / 4)`) and a bar of rhythm is one screen width,
the sticks as spikes at their beat positions.

### The instrument's own dynamics

A physical model has state: the didge's lip has a position and a
velocity. Plot one against the other and the instrument draws its own
phase portrait, a limit cycle that breathes with the player.

```
[didge.lip_pos * 4, didge.lip_vel * 0.1]
```

For a plucked string, displacement against velocity; for a cybernetic
patch, the state variables that make it cybernetic. Nobody has watched
their instrument do this live.

### The spectrum, as epicycles

An additive voice is a sum of partials. Sum rotating pairs with the
same frequencies and amplitudes and the beam draws the Fourier series
of the sound as a spirograph: change a partial in the sound and a loop
in the picture changes.

```
sum(8, n => rotate([didge.amp(n), 0], TAU * phasor(n * didge.f0)))
```

### Complex dynamics

Each sample is one iteration of a complex map and the beam traces the
orbit. The language's complex operations do the work; the parameter is
the player's breath.

```
$z = [0, 0]
let c = [didge.breath * 0.6 - 0.4, didge.lip_pressure * 0.5]
$z = if magnitude($z) > 2 then [0, 0] else cmul($z, $z) + c
$z
```

Mandelbrot and Julia orbits, Hénon maps, strange attractors: anything
written as "the next point from this one" is three lines.

### Shapes and figures

Parametric curves and tables, traced by a phasor and modulated by the
music.

```
let a = TAU * phasor(60)
let r = 0.4 + 0.3 * didge.sticks               # radius breathes with the hit
rotate([r * cos(3 * a), r * sin(2 * a)], TAU * didge.bar_phase)
```

Polygons from `wave` over a table of angles; letters and logos from a
table of points; a torus knot from a 3D curve projected by hand. Two
figures at once: alternate between them sample by sample and the
phosphor shows both.

## The layer on top

Trails, feedback, the kaleidoscope fold, colour: these are the
display's business, not the language's. The display keeps an effect
chain whose knobs are live and read named signals (`didge.sticks.rms`,
`didge.bar_phase`), with presets in `stream/scenes/`. That is where the
Winamp look lives, and it sits on top of any trace above. The trace
stays a pair; the effects never feed back into the language.

## Practicalities

- 48 000 points a second is 800 per frame at 60 fps. Enough for any
  figure here; a closed curve needs its period to divide into that.
- A beam draws lines, so a jump between two points draws a retrace
  line. Keep curves continuous, or blank the jump off-screen.
- Everything in a trace costs the same CPU as in a sound program and
  runs on the same core. On the uConsole keep traces as lean as
  patches.
- `aither render trace.aither out.wav` renders the beam as a 2-channel
  file: a WAV of x and y, playable through any XY display or an
  oscilloscope's audio input.

## What a beam cannot do

Filled areas, raster fields, textures, much text. Those are the
display's job if they are wanted at all.
