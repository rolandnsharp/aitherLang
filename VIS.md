# vis: a patch's picture (the `vis` branch, an experiment)

A patch's sound is `f(t)`, run at every sample. Its picture is the sum
of its `draw`s, `f(pixel, t)`, run at every pixel of every frame, on the
GPU, 60 times a second at full screen. One file holds both. A `draw` in
a play sees that play's lets, so each part draws itself from its own
envelope, note and beat, on the same sample it sounds:

```
play kick:
  let env = exp(-phasor(2) * 14)
  draw petals(pixel(), 0.1 + env * 0.1) * env      # the hit, seen
  sin(TAU * phasor(50 + 180 * env)) * env           # the hit, heard
```

```
./aither patches/pulse.aither | tools/play hw:rockchipes8388c 20 | tools/vis patches/pulse.aither
tools/vis fields/julia.aither                    # a picture alone
```

The sound never sees a draw: the parser drops them unless it is
compiling the picture, so a patch sounds the same, sample for sample,
with or without them. `aither shader patch.aither` compiles the picture
(`shader.nim`): each draw, and only the lets and defs it uses. A def is
built only where it's called, so picture helpers that use `pixel()`
live in defs. If a draw needs something with memory (`lpf`, `$state`),
that's an error naming it: only the sound runs those.

The draws add, as plays mix: each pixel's pairs are summed, so they
interfere. Two draws in step brighten; in opposite phase they cancel
to black.

## Memory: `$state`, a pixel at a time

A picture remembers as sound does, with `$state`. In a def a draw
calls, `$x` is a field: one value a pixel (a number or a pair), kept
from frame to frame, the call site's own (call the def twice, two
fields). As in sound, the first `$x = …` declares it and says what it
starts from (a number or a pair of numbers); later ones set it.

- `$x` is this pixel's value: last frame's, until it is set this frame.
- `$x[p]`, with a point `p` (a pair), is last frame's value at `p`,
  smoothly between pixels: what flow, trails, warps and feedback read.
- `$x[0]`, `$x[1]`, with a number, are the two sides of this pixel's
  pair.

Every pixel reads last frame and writes this one, as a stepped state
array does in sound (`every = w·h`), so the order pixels run in never
matters. A trail:

```
def trail(p, new):
  $g = [0, 0]
  $g = $g[rotate(p * 0.97, 0.05)] * 0.95 + new     # last frame, drawn in and turned, fading
  $g
```

and a wave equation keeps the water's height now and a frame ago as
one pair (`patches/rain`). Each field is a texture on the GPU (32-bit
floats where it can, else 16); the picture and up to 3 fields are
written in one pass. A reload that keeps the number of fields keeps
their values; one that changes it starts them afresh.

`vis` sits in the pipe like `tv`: pairs in, the same pairs out. Saving
the patch reloads the picture within a frame (and `aither send` the
sound, as ever); an error is printed and the old picture keeps
running. It is computed at half the screen's size (`--size WxH` to
choose) and drawn full screen (`--window` for a window); `f` toggles,
`q` or Escape quits. `--frames FILE` records every frame as raw RGB at
60 a second, each at its sixtieth of a second of the music, and prints
the `ffmpeg` line.

## What a picture has

| Name | What it is |
|---|---|
| `pixel()` | where this pixel is, as a pair: x across −1..1, y up, square pixels (y spans ±h/w), as `tv`'s `pixel(w, h)` |
| `t` | time in seconds: the sample being heard (with music; the clock without) |
| `sr`, `dt`, `start_t` | as in sound; `start_t` is 0, the picture counting from where the stream began |
| `input()` | the music's pair at the sample being heard |
| `level()` | its loudness then: RMS over the last 2048 samples |
| `low()` | its loudness below about 150 Hz (the kick and the bass): RMS of the last 1024 samples through two 150 Hz low-passes |
| `pixel_size()` | one pixel's width, in `pixel()`'s units: for reading a field's neighbours |
| `phasor(f)` | `fract(f·t)`: the clock, exact, with no state |
| `noise()` | −1..1, fresh at every pixel, every frame, every call |

Maths as in audio: `sin cos tan exp log log2 abs floor ceil sqrt tanh
int fract min max pow clamp`, the shapes `saw tri sqr` (on pairs, each
side), and the pair operations `cmul cdiv cscale rotate conj magnitude
phase`. Pair arithmetic is as in audio: `+ - * /` side by side, a number
broadcast to both.

## What it doesn't have

A picture can't read the sound's `$state` (only the sound runs it), nor
filters or anything else with memory. There are no imports or lambdas
yet. A function or a table passes by name (`osc(saw, f)`), as in sound.

A `def rec` is a loop, as in audio. A GPU has no clock to watch, so a
recursion stops after 1000 steps and gives 0 (the watchdog's place).

## Pictures

| Patch | What it shows |
|---|---|
| `waves` | three plane waves of colour crossing, brighter with the music |
| `julia` | z → z² + c at every pixel, `c` walking round the edge of the Mandelbrot set, pushed out by the music's loudness |
| `patches/pulse` | four on the floor, each part drawing itself: the kick a ring of petals fired from the middle, the bass rings as fine as its note is high, the hats sparks, the chord a rose in its own colour; all flying outward through a six-way kaleidoscope, a mandala a beat |
| `patches/rain` | a pond you can hear: water obeying the wave equation at every pixel, sixty steps a second. Bells fall as raindrops, each landing where its note sits on the pitch circle, and their ripples interfere; the bass is two sources whose fringes close in as it rises; in the storm the kick drops a stone a beat; each splash leaves ink of its note's colour, carried down the water's slope. Drizzle, shower, storm, clearing |
| `patches/unfold` | two minutes in four sections, sound and picture changing together: dawn (a pad opening, bells, a Julia set breathing as its c walks a quarter turn a chord), rise (the beat coming in, the flow turning into a kaleidoscope tunnel, a riser spinning it), bloom (the full beat, a ring of petals a kick, each kick its own colour), drift (bells lighting where their notes sit on the pitch circle, the picture swirling down a drain) |
| `bloom` (with music) | every kick fires a ring of petals from the middle, and the frames before fly outward through a six-way kaleidoscope: a mandala a beat, sailing off the screen. Driven by `low()` |
| `swirl` | last frame turned and drawn in, so colour flows like water down a drain; a ring of the music's phase is poured in every frame |
