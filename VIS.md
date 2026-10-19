# vis: fields of colour (the `vis` branch, an experiment)

An audio patch is `f(t)`, run at every sample. A field patch is
`f(pixel, t)`, run at every pixel of every frame, on the GPU, 60 times a
second at full screen. It's the same language: the same parser, numbers
and pairs (complex numbers), `def`, `def rec`, tables, `if`. Its value at a
pixel is a pair, shown as `tv` shows one: hue from the phase, brightness
from the magnitude (full at 1).

```
./aither music.aither | tools/play hw:rockchipes8388c 20 | tools/vis fields/swirl.aither
tools/vis fields/julia.aither                    # no music
```

`vis` sits in the pipe like `tv`: pairs in, the same pairs out. Saving
the patch reloads it within a frame; an error is printed and the old
field keeps running. The field is computed at half the screen's size
(`--size WxH` to choose) and drawn full screen (`--window` for a
window); `f` toggles, `q` or Escape quits. `--frames FILE` records every
frame as raw RGB at 60 a second, and prints the `ffmpeg` line.

`aither shader field.aither` prints the GLSL that `vis` runs
(`shader.nim`). The audio engine is untouched.

## What a field patch has

| Name | What it is |
|---|---|
| `pixel()` | where this pixel is, as a pair: x across −1..1, y up, square pixels (y spans ±h/w), as `tv`'s `pixel(w, h)` |
| `t`, `dt` | time in seconds (the sample being heard, with music; the clock without), the frame's length |
| `input()` | the music's pair at the sample being heard |
| `level()` | its loudness then: RMS over the last 2048 samples |
| `last(p)` | last frame's value at point `p`, smoothly between pixels: the field's memory, for flow, trails and feedback |
| `phasor(f)` | `fract(f·t)`: the clock, exact, with no state |
| `noise()` | −1..1, fresh at every pixel, every frame, every call |

Maths as in audio: `sin cos tan exp log log2 abs floor ceil sqrt tanh
int fract min max pow clamp`, the shapes `saw tri sqr` (on pairs, each
side), and the pair operations `cmul cdiv cscale rotate conj magnitude
phase`. Pair arithmetic is as in audio: `+ - * /` side by side, a number
broadcast to both.

## What it doesn't have

There is no state: `last()` is the memory. There's no `play` (a field is
one picture) and no imports, lambdas or functions as values yet. Each
is an error that says so.

A `def rec` is a loop, as in audio. A GPU has no clock to watch, so a
recursion stops after 1000 steps and gives 0 (the watchdog's place).

## Fields

| Field | What it shows |
|---|---|
| `waves` | three plane waves of colour crossing, brighter with the music |
| `julia` | z → z² + c at every pixel, `c` walking round the edge of the Mandelbrot set, pushed out by the music's loudness |
| `swirl` | last frame turned and drawn in, so colour flows like water down a drain; a ring of the music's phase is poured in every frame |
