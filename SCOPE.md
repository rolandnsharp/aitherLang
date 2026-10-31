# Drawing with the beam

**Status:** the engine writes one pair to stdout
([notes/pair-output-2026-10-15.md](notes/pair-output-2026-10-15.md)),
and `tools/scope` draws it ([notes/displays-2026-10-16.md](notes/displays-2026-10-16.md)).
The live stream still draws with `play scope:` parts on the `scope`
branch's engine, which this replaces.

A patch's final expression is a pair. Pipe the engine into `scope`
and the pair is read as **x, y**: a point on the screen, −1..1 each
way, one point per sample.

```
make tools/scope                                        # needs sdl3
./aither trace.aither | tools/scope                     # see it
./aither set.aither | tools/scope | tools/play hw:rockchipes8388c 20   # see it and hear it
./aither send trace.aither                              # hot-swaps as always
```

`scope` passes the stream on unchanged, so it can sit anywhere in a
chain, and it draws the sample being heard: with `play` after it, the
picture keeps time with the sound. `--palette green|amber|cyan|blue|
white|red` picks the phosphor, `--decay 0.85` how much of a trace is
left after 1/60 s. It opens full screen (`--window` for a window);
`f` toggles, and `q` or Escape closes it at once. `--term` draws in
the terminal (over ssh), and `--frames FILE --size 720` writes raw RGB
frames for `ffmpeg`.

The display joins the points and fades them, so a slow motion is
bright and a fast one faint, as on a phosphor tube. Brightness is
dwell; a jump to a point off-screen blanks the beam. A float is
`[s, s]`: the 45° line.

A trace is just the last line of the patch, so it can use every name
the music has: the pitch, the bar phase, an envelope, the state inside
a physical model. Nothing is analysed or guessed from the mix. That is
what makes the figures below possible; a hardware scope, which only
ever sees the mix, can draw just the first.

## What to draw

### The audio itself

```
drone + sticks
```

The patch as it is. Mono is a 45° line. Stereo width opens it into an
ellipse; a phase difference tilts it; clipping flattens the ends. This
is the classic vector scope, and it is free.

### A waveform that holds still

A scope needs a trigger to freeze a waveform. Here the trigger is a
name: sweep once per period of the note being played.

```
let x = phasor(f0) * 2 - 1
[x, drone]
```

The waveform stands still whatever the pitch does. Sweep on the bar
instead (`phasor(tempo / 4)`) and a bar of rhythm is one screen width,
the sticks as spikes at their beat positions.

### The instrument's own dynamics

A physical model has state: the didge's lip has a position and a
velocity. Plot one against the other and the instrument draws its own
phase portrait, a limit cycle that breathes with the player.

```
[lip_pos * 4, lip_vel * 0.1]
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
sum(8, n => rotate([amp(n), 0], TAU * phasor(n * f0)))
```

### Complex dynamics

Each sample is one iteration of a complex map and the beam traces the
orbit. The language's complex operations do the work; the parameter is
the player's breath.

```
$z = [0, 0]
let c = [breath * 0.6 - 0.4, lip_pressure * 0.5]
$z = if amplitude($z) > 2 then [0, 0] else turn($z, $z) + c
$z
```

Mandelbrot and Julia orbits, Hénon maps, strange attractors: anything
written as "the next point from this one" is three lines.

### Shapes and figures

Parametric curves and tables, traced by a phasor and modulated by the
music.

```
let a = TAU * phasor(60)
let r = 0.4 + 0.3 * sticks               # radius breathes with the hit
rotate([r * cos(3 * a), r * sin(2 * a)], TAU * bar_phase)
```

Polygons from `wave` over a table of angles; letters and logos from a
table of points; a torus knot from a 3D curve projected by hand. Two
figures at once: alternate between them sample by sample and the
phosphor shows both.

## Beyond the beam

`scope` has no effects, modes or presets: the phosphor, the palette
and the decay are all it decides. Everything else that moves is aither
code. Trails, feedback, the kaleidoscope fold and filled pictures
(fractals that grow one step a frame) belong to `tv`, the raster
display, driven by a second engine whose `input()` is the music:

```
aither sound.aither | scope | play | aither visuals.aither | tv
```

See [TV.md](TV.md) and
[notes/displays-2026-10-16.md](notes/displays-2026-10-16.md). `play`
passing its input on isn't built yet.

## Ridges

`--ridge` draws the stream another way: folded at a period, each period
a ridge, stacked up the screen with the newest in front and the ones
behind hidden wherever a nearer one stands in front of them. It is
Harold Craft's 1970 plot of successive pulses of the first pulsar, CP
1919 (the one on the cover of *Unknown Pleasures*), drawn with the
beam.

```
./aither patches/aeolian.aither | tools/scope --ridge auto --gain 4 | tools/play hw:rockchipes8388c 20
./aither patches/sync.aither | tools/scope --ridge auto --gain 2 | tools/play hw:rockchipes8388c 20
```

- **`--ridge F`** folds at F Hz, or a beat (`108bpm`), or `auto`: the
  pitch, found and held as `tv --fold auto` holds it (TV.md, Readouts;
  the two share the folding). A ridge is one period, 192 points across,
  its height x; one is kept every 24th of a second, 48 of them, so the
  screen is two seconds deep and slides back smoothly.
- **What it shows:** whatever repeats stands as a range of hills that
  holds its shape; a change of harmonic is a new range rising in front;
  a crowd out of step averages to flat ground, and falling into step
  raises one repeating hill; a period doubling, ridges alternating in
  height.
- **`--gain G`** makes the ridges G times taller (x of ±1 is 0.3 of the
  screen at 1). Taller ridges hide more of the ones behind.
- The glow is short (`--decay 0.2` unless given), since the ridges move:
  a long one smears each into the next.
- Ridges fill the screen, wide, where the beam's picture is square; in
  `--frames` the height is 9/16 of `--size`.

## The helix

`--helix` draws the pair as what it is, a complex number, in three
dimensions: its real part toward you, its imaginary part up, and time
along an axis across the screen, seen from in front, a little to the
right and above. The last 8 periods are drawn, ending on a period
(found and held as for `--ridge`, which `--helix` takes the same F as),
so a steady sound stands still and one a little off turns slowly.

```
./aither patches/hopf.aither | tools/scope --helix auto | tools/play hw:rockchipes8388c 20
./aither patches/sync.aither | tools/scope --helix auto --gain 3 | tools/play hw:rockchipes8388c 20
```

- A spin is a corkscrew; `conj` turns the other way, the mirror
  corkscrew; two spins beating, a rope whose radius swells and shrinks;
  a harmonic, small curls riding on the big one.
- The shadows: the real part's on the floor and the imaginary part's on
  the back wall, the cosine and the sine of the textbook picture of a
  phasor, fainter than the helix, and the axis fainter still.
- `--gain G` makes it G times wider (a pair of 1 is 0.55 of the axis's
  length). Like ridges, it fills the screen wide, with a short glow.

## Practicalities

- A session is heard and seen at once: `scope` passes the stream on to
  `play`. A trace that isn't the music (a phase portrait, a figure)
  is its own patch, so it is seen, not heard, or run beside the music
  in a second engine.
- How bright a figure is depends on how long its path is, as on a real
  tube: a short figure that repeats is bright, music that never
  repeats is fainter, and a beam that stands still burns white.
- 48 000 points a second is 800 per frame at 60 fps. Enough for any
  figure here; a closed curve needs its period to divide into that.
- A beam draws lines, so a jump between two points draws a retrace
  line. Keep curves continuous, or blank the jump off-screen.
- A trace costs the same CPU as sound and runs on the same core. On
  the uConsole keep traces as lean as patches.
- `aither trace.aither 30 > trace.raw` gives 30 s of x and y as raw
  float pairs; `sox` turns that into a WAV playable through any XY
  display or an oscilloscope's audio input. `scope --frames` turns it
  into video.

## What a beam cannot do

Filled areas, raster fields, textures, much text. Those are `tv`'s
job ([TV.md](TV.md)).
