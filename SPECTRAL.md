# Spectral patches: a sound's spectrum through an aither patch

A spectral patch hears a sound as its spectrum and changes it there. It
can freeze the sound, leave its colour ringing, give it another sound's
colour, or let its spectrum ripple like a string. The spectrum comes
down a pipe, one bin a sample, and the patch is an ordinary aither patch
that reads it with `input()`.

```
source patch ──► tools/fft 4096 ──► spectral patch ──► tools/ifft 4096 ──► tools/play
  a signal        its spectrum:      a bin in,           back to a
                  2048 bins a frame  a bin out           signal
```

This is how spectra work for now. The plan is for the spectrum to become
a field inside aither, with no pipe (notes/spectrum-as-field-2026-11-11.md).
The patches below would mostly keep their bodies when that happens.

## Run one

Build the tools once:

```
cd ~/aitherLang && make tools/fft tools/ifft tools/play
```

Then run a source through a spectral patch:

```
./aither patches/field_didge.aither | tools/fft 4096 \
  | AITHER_SOCK=/tmp/medium.sock ./aither patches/spectrum_medium.aither \
  | tools/ifft 4096 | tools/play
```

Three things must be right, or the sound comes out wrong with no error:

1. **The frame size is in three places.** `tools/fft N`, `tools/ifft N`
   and the patch's `let frame = N / 2` must agree. All the patches here
   use 4096, so 2048 bins.
2. **Every live engine needs its own socket.** The second engine on a
   machine needs its own `AITHER_SOCK=/tmp/<name>.sock`, or it fights the
   first for the default one. A third needs a third.
3. **The spectral patch starts with the stream,** given on the command
   line. One sent to a running engine would start mid-frame. Live
   changes to a patch already in the chain (its socket, `aither send`)
   are fine: frames stay in step.

On the uConsole, `tools/play plughw:rockchipes8388c` is the speaker.

### Two sources

`tools/fft` given files hears several sounds at once. Each one's lowest
bins sit side by side in one frame: with two, the first sound fills bins
0 to 1023 and the second 1024 to 2047. Each sound's band then stops at
11 kHz (a quarter of the rate), because two share the pipe. In bash,
`<(…)` makes a patch's sound a file:

```
tools/fft 4096 <(./aither patches/field_didge.aither) \
               <(AITHER_SOCK=/tmp/ring.sock ./aither patches/scanned_ring.aither) \
  | AITHER_SOCK=/tmp/cross.sock ./aither patches/spectrum_cross.aither \
  | tools/ifft 4096 | tools/play
```

### Offline, to check before listening

An engine given seconds renders that much and exits. Give every engine
in the chain a length:

```
AITHER_RT=0 ./aither patches/field_didge.aither 10 > /tmp/didge.raw
tools/fft 4096 < /tmp/didge.raw \
  | AITHER_RT=0 ./aither patches/spectrum_medium.aither 10 | tools/ifft 4096 > /tmp/out.raw
```

Never leave an engine in a chain without a length offline. An engine
whose input never arrives plays silence forever, and silence at 44.1 kHz
fills /tmp, a RAM disk, at 350 kB a second.

### Beside the rest of a song

`--solo` sends one play down the chain, and `tools/add` puts it back
with the rest, `--delay N/2` lining the two up:

```
tools/add --delay 2048 <(./aither song.aither --mute horn) \
  <(./aither song.aither --solo horn | tools/fft 4096 \
    | AITHER_SOCK=/tmp/s.sock ./aither patches/spectrum_freeze.aither | tools/ifft 4096) | tools/play
```

## The patches

| patch | what it does | controls (tools/pad) |
|---|---|---|
| `spectrum_freeze` | holds the sound as it is, its partials ringing on as oscillators | X up freezes, X down lets go |
| `spectrum_smear` | leaves the sound's colour ringing after it, and can make it climb in pitch | Y up and down: how long; Y left and right: how fast it climbs; X: freeze |
| `spectrum_cross` | the second sound in the first sound's colour (two sources) | none: which two sounds go in |
| `spectrum_medium` | the spectrum as a string along pitch: changes in the sound ripple up and down it | none: four numbers in the patch, `shake` first |

The pad's controls reach every engine in a chain, so the source patch
and the spectral patch both hear them. The spectral patches use Y and
X, which leaves A, B and the arrows to play the source.

## Write one

The stream is complex numbers, one a sample, the frame's bins in order:

- `input()` gives `[re, im]`, the bin's complex value.
- `pixel_index(frame, 1)` is which bin it is, 0 to `frame - 1`. Bin k
  is k × rate / 4096 Hz: about 11 Hz apart at 44.1 kHz.
- What the patch gives back is the bin `tools/ifft` turns into sound.
  A bin left as it came is the sound unchanged, `N / 2` samples late.
- A state array stepped `every = frame` steps once a frame. Reads see
  the last frame, so a bin's neighbours on both sides are at hand, as
  they were a frame ago. Only the bin coming in is this frame's.
- Bin 0's pair is two real numbers, the frame's lowest and highest
  bins: leave it alone unless you mean it. With two sources, each
  stream's first bin is `[its lowest, 0]`.

The smallest one keeps only what's under 1 kHz:

```
let frame = 2048                    # bins a frame: tools/fft 4096 gives 4096 / 2
let bin = pixel_index(frame, 1)     # which bin this sample holds, 0 to 2047
let [re, im] = input()              # the bin, as a complex number
let here = (re, im)
if bin * sr / 4096 < 1000 then here else (0, 0)    # only what's under 1 kHz
```

The didge through it keeps its sound under 900 Hz as it was, and above
1.5 kHz drops a further 86 dB or more.

**Making a bin ring on.** A bin can't just be held: from one frame to
the next, a partial in bin k turns half a turn per bin, plus how far its
frequency lies from the bin's. Hold the number still and it buzzes at
the frame rate. `spectrum_freeze` is the pattern to copy. It measures
each bin's turn from the frame before, keeps it turning, and turns a
harmonic's three or so bins together, at the speed of the loudest.

**Not popping.** A frame is 46 ms. Anything that jumps a bin's level
from one frame to the next is heard as a flutter at about 20 Hz. Let a
level swell over a few frames (`spectrum_medium`'s `hold`), and where a
bin has sound in it, add to it in its own phase, not beside it at another
phase, which beats.

## What it doesn't do

- **Latency.** The pipe is N / 2 samples late (46 ms at 4096 and 44.1
  kHz), and a patch that needs the whole frame (`spectrum_cross`) adds
  a frame more.
- **Mono.** One side, or the sides' mean, goes through. Stereo is two
  chains (`tools/fft N left`, `right`).
- **The window.** The frame's window is a sine both ways. Its spill
  from loud low partials is much of what a dark sound has up high, so
  effects that rescale the highs (cross, medium) hear less there than
  there is.
- **Feedback** through the chain needs a fifo, and the loop then hears
  the pipes' and the engine's delays. That's one reason for folding the
  spectrum into the language.
- **Long convolution** (a room's echo) doesn't fit a stream of bins.

The tools are `tools/fft.c`, `tools/ifft.c` and `tools/add.c`, with what
they share in `tools/fft.h`. Their tests are in `tests/pipe_tools.c`.
How the pipe was decided: notes/spectrum-2026-11-06.md.
