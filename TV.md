# Drawing with pixels

**Status:** built: `tools/tv`. Its design and what building it taught
are in [notes/displays-2026-10-16.md](notes/displays-2026-10-16.md).

A patch's final expression is a pair. Pipe the engine into `tv` and
the stream fills a picture: w×h samples a frame, in raster order (rows
top to bottom, each left to right), each pair one pixel's complex
value. Its phase is the hue (the positive reals red, then yellow,
green, cyan, blue and magenta going round) and its amplitude the
brightness, black at 0 and full at 1.

```
make tools/tv                                                    # needs sdl3
AITHER_RATE=1152000 ./aither patches/mandelbrot.aither | AITHER_RATE=1152000 tools/tv --size 160x120 > /dev/null
./aither send patches/mandelbrot.aither                          # hot-swaps as always
```

The frame rate is the rate divided by w×h: 160×120 at 1.152 MHz is 60
frames a second. Give the engine and `tv` the same `AITHER_RATE`. `tv`
shows each frame whole at the sample being heard, as `scope` does, and
passes the stream on unchanged. A whole frame is one moment of a
patch's field (one step of an `every = w·h` array), so it is the
default. `--scan` draws rows as they arrive instead, each over the same
row of the frame before, as a CRT draws: the raster's own timing shown
rather than held back, and smooth for a big picture whose frame rate is
low (a new picture wipes down the screen rather than jumping in). A
field that steps once a frame shows a seam where the new step meets the
old. `--frames` records whole frames either way. Pixels are crisp squares, or soft with
`--smooth`. It opens full screen (`--window` for a window); `f`
toggles, and `q` or Escape closes it at once.
`--term` draws in the terminal in half-blocks (over ssh), and
`--frames FILE` writes every frame as raw RGB and prints the `ffmpeg`
line that makes it a video.

## Which pixel this sample is

- `pixel(w, h)`: the centre of the pixel the current sample fills, as
  a pair: x across −1..1, y up, square pixels (y spans ±h/w). A pair
  is a complex number, so it is the point of the plane to compute.
- `pixel_index(w, h)`: its raster index, 0 .. w·h − 1, for state
  arrays.
- `frame_number(w, h)`: the frame it is in, from 0, for what changes
  once a frame.

All three follow the engine clock, so a hot swap lands on the grid
`tv` is counting. The patch and `tv` must agree on w and h; a mismatch
shears the picture.

## What to draw

### A function of the plane

`pixel(64, 48)` on its own is the colour wheel: every hue round the
centre, black at it. Any complex function of the pixel is a picture of
that function: `turn(p, p)` winds the wheel twice, `rotate(p, t)`
turns it, and a polynomial shows each root as a point where every
colour meets. Squeeze the result into the unit circle with
`z / (1 + amplitude(z))`, which keeps the phase and bounds the size
(see Practicalities).

### A system you watch run

A pixel can hold state and run. In `patches/mandelbrot.aither` every
pixel keeps its own z in a state array and takes one step of
z → z² + c a frame, so the set grows in:

```
let k = pixel_index(w, h)
let c = pixel(w, h) * 1.5 - [0.5, 0]
$z = array(2 * w * h)
let z = [$z[2 * k], $z[2 * k + 1]]
let next = if amplitude(z) > 2 then z else turn(z, z) + c
```

Points inside the set settle into cycles, which flicker frame to frame
at the period of their bulb. The patch shows z smoothed over about 20
frames, a second state array, and the picture holds still. Julia sets,
the burning ship and Newton's method are the same patch with one line
changed.

### Escape time, with `def rec`

A view that moves can't keep per-pixel state: z belongs to the point
the pixel showed last frame. So a zoom works out each pixel afresh
every frame, with a recursion that stops as soon as the point leaves
(`patches/seahorse.aither`):

```
def rec escape(z, c, n):
  if n == 0 or z[0] * z[0] + z[1] * z[1] > 4 then [n, z[0] * z[0] + z[1] * z[1]]
  else escape(turn(z, z) + c, c, n - 1)
```

Compare |z|² with 4 rather than `amplitude(z)` with 2: `amplitude`
takes a square root every step.

## Practicalities

- **Size against rate.** Frames a second = rate ÷ (w·h). On the
  uConsole, `patches/mandelbrot.aither` runs at 160×120 and 60 frames
  a second (1.152 MHz), and the 64-step zoom at 128×96 (737 kHz) runs
  at 2.1× real time on mains.
- **Pictures that hear the music** run at the music's rate: a visuals
  engine whose `input()` is 44.1 kHz music makes 44 100 pixels a second,
  48×27 at 34 frames a second. `play` hands the stream on after the
  speaker has it, so one pipe does it: `./aither music.aither | play |
  AITHER_SOCK=/tmp/aither-picture.sock ./aither picture.aither | tv`
  (demos/plate listens to demos/chorale). A larger picture needs the engine to hold each input
  frame for several samples (an open question in the displays note).
- **Stay within ±1.** The engine guards a picture as it guards sound:
  within 20 s of a send, a sample above 3.0 rolls the voice back, and
  above 8.0 mutes it. A NaN resets the pool, which wipes the state
  arrays, so keep iterations bounded (freeze escaped points).
- **Pixels update in scan order, in place.** A pixel that reads its
  neighbours' state sees this frame's values above and to its left
  and last frame's below and to its right. When every pixel must step
  from the same frame (a wave, the Game of Life, a blur, a warp),
  declare the state stepped: `$u = array(w * h, every = w * h)` reads
  last frame's values everywhere and lands its stores at the next
  (SPEC, Stepped state arrays; demos/ridge and demos/phased_array).
- **A moving view recomputes.** State that belongs to the view, like a
  fractal's z, can't follow it (escape time above). State that belongs
  to the world, like a simulation's field, can stay put while the view
  moves over it.
- A picture costs CPU like sound, on the same core: a frame is w·h
  samples of the patch.

## Readouts: any patch, read

The raster above is a picture a patch draws for tv. The readouts are
the other way round: tv reads any stream, a patch made for nothing but
sound, and lays it out so something about it can be seen that the
scope can't show. The scope shows the signal against itself (x against
y); the readouts show it against time, folded, against frequency, and
against itself at other times. Each keeps the stream's own timing:
they scroll smoothly at whatever rate the rows come, and never wait for
a frame of w·h samples. One at a time; `--size` is then columns × rows
(256×192, or 256×256 for `--recur`), and `--gain` brightens a quiet
stream.

```
./aither patches/duffing.aither | tools/tv --fold 220 --gain 2 | tools/play hw:rockchipes8388c 20
./aither patches/aeolian.aither | tools/tv --fold auto --gain 3 | tools/play hw:rockchipes8388c 20
```

- **`--fold F`**: each row is one period of F Hz (or of a beat:
  `--fold 108bpm`), the newest at the bottom, scrolling up. A note in
  tune with F stands still in columns; one a little off leans, and how
  far it leans is how far off it is; beating drifts down the rows; a
  beat's hits line up. The period is kept fractional, so a note doesn't
  drift from rounding. This is a strobe tuner's picture, and the
  phase-against-time plot astronomers fold a pulsar's signal into.
- **`--fold auto`** finds the pitch and holds the fold on it. Ten times
  a second it finds the period by the YIN method (de Cheveigné and
  Kawahara, JASA 2002), and while the sound still repeats at the fold,
  as a harmonic of it alone does, the fold holds; when it doesn't, tv
  folds at the new period once it has heard it twice, and says so on
  stderr. Between, each row is compared with the one before: if the
  pattern slid sideways, the period is nudged until it stops, a
  phase-locked loop on the picture itself, keeping within 3% of the
  pitch found. A pitch alone is too coarse to hold columns still for a
  screenful of rows (one part in fifty thousand); the loop isn't.
- **`--average`**, with `--fold`: the bottom third is the mean of the
  last second's periods, drawn as a curve (each column filled up to its
  magnitude, in its phase's hue). What repeats at the fold builds up and
  what doesn't cancels: a waveform out of noise, as epoch folding finds
  a pulse, or averaging an EEG finds an evoked response.
- **`--spectrum F`**: each row is a spectrum, −F to +F Hz, 100 rows a
  second (a 4096-point FFT, Hann-windowed). A pair has two sides: a
  spin turning forwards is on the right, one turning backwards on the
  left, so `conj` is a mirror and a real signal is symmetric. Brightness
  is level, 70 dB of it, full at a spin of amplitude 1. Hue is the
  pitch class of the frequency each bin truly holds, measured from how
  far its phase turned since the row before (the phase vocoder's
  instantaneous frequency): C red, round the wheel a semitone at a
  time, every octave the same colour, and a tone's neighbouring bins
  the same colour as it. Its phase, as hue, would turn at the bin's
  distance from the tone, and a steady tone would come out a rainbow.
- **`--recur S`**: a recurrence plot (Eckmann, Kamphorst and Ruelle,
  Europhysics Letters 1987) of the last S seconds, redrawn every
  picture. Pixel (i, j) is bright when the signal at moment i (across)
  is near where it was at moment j (up); a moment is its pair and the
  pair just before, so passing a value going the other way isn't
  coming back. Near is against the window's own spread. The diagonal is
  each moment with itself. A period draws lines parallel to it, a
  period apart; quasi-periodic motion breaks them into dashes; chaos
  into short scattered strokes; noise into dots; and a change of
  behaviour shows as a block. Hue is the pair's phase there: where
  round its cycle the signal came back. S should hold a few periods of
  what's to be seen: 0.05 for a note at 110 Hz.

`patches/duffing.aither` is made to be read by them: a driven
double-well oscillator on the road to chaos, folded at its drive. Each
period doubling shows as rows that repeat every two, then four; chaos
as rows all different; its periodic windows as patterns five and three
rows deep.

## The pond

`--pond` is a picture the stream makes without a patch drawing it: a
pond the sound stirs, at full screen and 60 steps a second.

```
./aither patches/aeolian.aither | tools/tv --pond | tools/play hw:rockchipes8388c 20
./aither patches/knot.aither | tools/tv --pond | tools/play hw:rockchipes8388c 20
```

The water is stepped as the `vis` branch stepped `rain`'s, on the GPU at
half the screen's size and drawn full screen: a pair a pixel, the height
now and a frame ago; each pixel pulled toward its eight neighbours'
mean, read 2.5 pixels apart so the ripples come out round, keeping 99.3%
of its speed, with a gentle spring back to level. Its hue is the phase
of the swing and its brightness how far it swings, as rain's.

What stirs it is the beam: every 60th of a second the stream's pairs
since the last are drawn into the water as the scope would draw them
(square, in the middle), and the push is how that drawing changed from
the last one, softened over a few pixels. So a shape that moves, a knot
turning, a cloud of voices churning, leaves wakes and rings that spread,
cross and reflect off the edges; a shape that stands still stops
pushing, and the water around it calms. The beam itself shows faintly on
top. `--size` sets the water's size (half the screen unless given; 640×360
in `--frames`, which records 30 of the 60 steps a second), `--gain` its
brightness.

## What tv cannot do

Thin lines and curves: a raster shows a curve only where its pixels
sample it, and at these sizes that is coarse. Draw those with `scope`
([SCOPE.md](SCOPE.md)). In the raster `tv` has no effects or presets
and no memory of its own: trails, feedback and persistence are state
arrays in the patch. The readouts keep only what they show.
