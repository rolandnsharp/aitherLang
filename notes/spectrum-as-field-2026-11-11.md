# The spectrum as a field (2026-11-11)

**Status: a design, for discussion. Nothing built.** Roland: "are you saying
fft can be modeled as a field and doesnt need to be seperate. we seperated
it before because we thought it was outside aitherlang scope but now with
fields perhaps we have a perspective change". And, on where it goes: no bash
in the end, aither piping everything itself, live reload keeping phase and
state, perhaps an aitherOS.

## Why it was outside (2026-11-06)

notes/spectrum-2026-11-06.md put the FFT in a pipe: `tools/fft`, a patch
reading bins with `input()`, `tools/ifft`. The reasons were sound at the
time:

- An FFT is a whole-array operation at a step: all N samples in, N bins
  out, at once. aither then had nothing of that shape (graph-2026-11-02.md,
  "the spectral gap", option B held in reserve).
- It has a latency of its own, N / 2 samples, and in a loop that is heard.
  Kept in the pipe, the latency was a number on the command line, not a
  machine inside a patch.
- It needed nothing added to the language: a bin is a pair, a spectrum a
  stream of pairs, a frame a row of `pixel_index`.

It worked. Four patches stand on it (freeze, smear, cross, medium), and
SPECTRAL.md says how to use them. What it costs is in the second half of
this note.

## What changed: fields

Since then aither has fields, `field(period, ends)`, and a core that holds
a linear field exactly as its modes (medium/, one-system-2026-11-11.md). That
changes the question. The FFT is not something outside fields. It is one.

**A frame is a ring field.** The last N samples of a sound, laid round a
ring of N places, one place a sample, written as the sound goes by: a tape
loop. Its ends are `round`, because the DFT treats a frame as one turn of
something periodic.

**A spectrum is that ring read by its modes.** The modes of a ring of N
places are the N sinusoids that go round it a whole number of times,
e^(2πi k n / N). How much of each mode the ring holds is the k-th bin.
That is the definition of the DFT, nothing more. The field core already
holds round fields as their modes, for strings and rings. A spectrum is
the same move, applied to a ring the sound is written into.

**The bins are a field too, over frequency.** A partial lies across a
few neighbouring bins, and every spectral patch so far reads neighbours:
the freeze and smear turn a harmonic's bins together, the cross smooths
over a band, the medium is a string with a law between neighbours
(Δ along pitch). A spectrum is a place-indexed thing with neighbours and
a law: a field.

**The transform has a physical form: a bank of resonators.** One
resonator per bin, each driven by the sound:

    z_k ← e^(iω_k) · z_k + x(t) − x(t − N)      (the sliding DFT: exact bins every sample)
    z_k ← r · e^(iω_k) · z_k + x(t)             (damped: a cochlea, one bin a hair cell)

Each place's law reads only the sound, no neighbours, and it is writable
in aither today: `$z = array(N, (0, 0))` and `each`. So conceptually
nothing about a spectrum is outside the language. The sound drives a
medium of oscillators, and the spectrum is that medium's state.

**The FFT is the fast way to read that field at a step.** The bank costs
N complex products a sample; the FFT, at a hop of N / 2, costs about
5 N log₂ N / (N / 2) operations a sample. At N = 4096 that is about 120 a
sample against about 12,000: a hundred times less. The bank sampled at
the hops, through the window, is what the FFT gives. It is the same
relation as `every = M` already has: the law is the medium's, and the
engine chooses how often it is evaluated.

**And back:** the inverse is the modes turned back into places, each bin
an oscillator, summed: additive synthesis. Overlap-add is its fast form.
The freeze already treats each bin as an oscillator.

So: the semantics is a field (a ring holding the sound, read by its
modes, or a bank of resonators, which is the same thing); the evaluation
is an FFT every hop. The pipe was the evaluation without the semantics.

## What it could look like

Only a sketch; the spelling is the field core's to settle.

```
play didge: ...                       # any sound

let s = spectrum(didge, 4096)         # its modes: 2048 bins, a field over frequency, stepped a hop
$u' = ...                             # a law on the bins, as on any field (spectrum_medium's string)
sound(s * gain)                       # back to a signal, overlap-added
```

The four patches would keep their bodies: what is now "this sample's bin,
via `pixel_index`" becomes "at each place of `s`", the way a field law is
written now.

## For

- **One process, one state.** A live reload keeps every stage's phase
  and state, the source's and the spectral patch's, together. The pipe
  keeps them per process, and a restarted stage starts from nothing.
- **The frame size in one place.** Today it is in three (`tools/fft N`,
  the patch's `let frame`, `tools/ifft N`) and a mismatch garbles with no
  error.
- **No sockets, no `<(…)`, no fifos.** And the aitherOS direction needs
  this anyway: a system with no bash has no pipe.
- **Two sources at full band.** The pipe has room for one spectrum a
  frame, so two share it and each stops at 11 kHz. Inside, a patch can
  have as many spectra as it can afford, each whole. Stereo the same.
- **Feedback with a known delay.** A loop through a spectrum hears it a
  hop late, and that hop is the patch's own number, as `every` is. In the
  pipe, a loop needs a fifo and hears the pipes and the engine's block,
  the one machine graph-2026-11-02.md wanted out.
- **The window becomes the patch's to choose.** The sine window's spill
  is why the cross and medium hear too little up high (SPECTRAL.md). A
  window is a choice a field language can name: a window with lower
  sidelobes, or the damped bank's own response.
- **It is the field view.** spectrum_medium is a string along pitch. As a
  field law on a field, it would be written as every other string is, and
  checked by the same physics tests (tests/physics).

## Against

- **A whole-array step inside the engine.** The FFT happens all at once
  at a hop, so one sample in N / 2 carries the whole cost: a burst in a
  block, where real-time cares about the worst block, not the average.
  It can be spread over the hop (a split-radix FFT done a stage at a
  time), at the cost of code.
- **A new kind of thing in codegen.** A stepped array with a whole-array
  function at its step is option B of graph-2026-11-02.md, never built.
  It touches codegen.c and the field core, where the field-core work is
  going on.
- **A field's period means time, here.** In `field(period = T, ends)`, T
  is how long a push takes to go round, and the engine picks the places
  from it. A spectrum's places are fixed by N and its axis is frequency,
  stepped a hop at a time. Either a spectrum is a different kind of field,
  or `period` grows a second meaning. That has to be argued out, not
  slipped in.
- **The pipe is Unix, and that has uses.** `tools/fft` reads any float
  stream: a radio's, a sound card's (`tools/card`), a recording. Inside
  the language, a spectrum is only of what aither makes or hears through
  `input()`. Keeping the tools as well costs little.
- **The patches change their framing.** One bin a sample (`pixel_index`)
  becomes at each place. The bodies mostly stay, but every spectral patch
  is rewritten once and compared against the pipe's version.

## Options

- **A. Keep the pipe.** It works, and it is documented now (SPECTRAL.md).
  It costs what "For" lists.
- **B. A pair of natives**, `spectrum(x, N)` giving a stepped pair array
  and `sound(a, N)` giving a signal. The smallest step: stepped arrays
  exist, so this is option B of the graph note, a whole-array function
  at a step. It can be checked bit-close against the pipe (the same FFT,
  the same window).
- **C. A spectrum as a kind of field** in the field core: a ring read by
  its modes, with laws on the bins written as field laws (`Δ` along
  pitch, `δ` at a frequency). The most elegant, and the most to design,
  with the field core's owner.

**Recommendation: B, named and shaped as C will want it.** Build the two
natives now, with the field's name and meaning, so that moving into the
field core later changes the implementation and not the patches. Port the
four spectral patches, compare each to its pipe version (they should agree
to float32), and measure on the uConsole, the worst block as well as the
average. Keep the tools for sounds from outside.

## Questions for Roland

1. Is a hop's latency inside a patch acceptable, given it is the patch's
   own number (as `every` is), not the engine's block?
2. Should a spectrum be a field from the start (C), or a stepped array
   first (B) and a field once the field core can hold it?
3. Is a spectrum's axis frequency, a new kind of field, or should
   `period` mean "round in N places" as well as "round in T seconds"?
4. The window: the sine as now (and the pipe's results), or a choice in
   the patch?
