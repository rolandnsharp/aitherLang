# Drawing with pixels

**Status:** built: `tools/tv`. Its design and what building it taught
are in [notes/displays-2026-10-16.md](notes/displays-2026-10-16.md).

A patch's final expression is a pair. Pipe the engine into `tv` and
the stream fills a picture: w×h samples a frame, in raster order (rows
top to bottom, each left to right), each pair one pixel's complex
value. Its phase is the hue (the positive reals red, then yellow,
green, cyan, blue and magenta going round) and its magnitude the
brightness, black at 0 and full at 1.

```
make tools/tv                                                    # needs sdl3
AITHER_RATE=1152000 ./aither patches/mandelbrot.aither | AITHER_RATE=1152000 tools/tv --size 160x120 > /dev/null
./aither send patches/mandelbrot.aither                          # hot-swaps as always
```

The frame rate is the rate divided by w×h: 160×120 at 1.152 MHz is 60
frames a second. Give the engine and `tv` the same `AITHER_RATE`. `tv`
shows each frame whole at the sample being heard, as `scope` does, and
passes the stream on unchanged. Pixels are crisp squares, or soft with
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
that function: `cmul(p, p)` winds the wheel twice, `rotate(p, t)`
turns it, and a polynomial shows each root as a point where every
colour meets. Squeeze the result into the unit circle with
`z / (1 + magnitude(z))`, which keeps the phase and bounds the size
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
let next = if magnitude(z) > 2 then z else cmul(z, z) + c
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
  else escape(cmul(z, z) + c, c, n - 1)
```

Compare |z|² with 4 rather than `magnitude(z)` with 2: `magnitude`
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

## What tv cannot do

Thin lines and curves: a raster shows a curve only where its pixels
sample it, and at these sizes that is coarse. Draw those with `scope`
([SCOPE.md](SCOPE.md)). `tv` has no effects, modes or presets and no
memory of its own: trails, feedback and persistence are state arrays
in the patch.
