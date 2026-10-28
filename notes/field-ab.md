# A/B: the didge, digital and analogue electronics

2026-10-26. Roland's favourite didge (aitherLang `patches/didge_sticks`,
"the new didge is great") without the sticks.

- **A** (`didge_a.aither`): the patch as it is.
- **B** (`didge_b.aither`): the same text, generated from A, with only
  the electronics swapped for `../analogue.aither`'s circuits: the
  three formant `bpf`s, `drive`, the two `hpf`s and the `lpf`. The
  instrument's own physics (lips, bore, tract, `lp1`, `delay`) and the
  room (`reverb`) are untouched.

What B's circuits add: integrators that saturate toward ±1.5 (two
half-steps a sample), ±1% seeded tolerance on each cutoff, a slow 0.2%
drift, and a drive stage with a little asymmetry (even harmonics).

## Measured (60 s each, 48 kHz, offline, the uConsole)

| | A | B |
|---|---|---|
| CPU for 60 s | 23.6 s (39% of a core) | 42.5 s (71% of a core): **+80%** |
| RMS | 0.0841 (-21.5 dBFS) | 0.0682 (-23.3 dBFS) |
| peak | 0.497 | 0.359 |
| crest factor | 15.4 dB | 14.4 dB: the rails take the peaks |
| spectral centroid (`audit`) | 1042 Hz | 1277 Hz: brighter |
| top peaks | 366, 147, 513 Hz | 366, 147, 513, **220, 293 Hz** |

293 Hz is the 4th harmonic of the drone (73.3 Hz): the drive's even
harmonics, which A's symmetric drive doesn't make. The sample-by-sample
difference is only 7 dB below A, mostly because tolerance moves each
formant a little and so shifts phases; it says the waveforms differ,
not how much it is heard.

## Listening

Loudness-matched (B raised by 1.8 dB, so louder can't pass for better)
and blind: two WAVs, X and Y, in the session's scratchpad, the answer
sealed beside them until Roland has listened.

## Result

Roland listened blind, loudness-matched (2026-10-26): **X preferred,
"more grungy and raw".** Unsealed: X was **A**, the patch as it is.

The analogue electronics softened what makes this didge: the rails and
the integrators' saturation took the peaks (crest 15.4 -> 14.4 dB),
where the lips' slap and the formants' spikes live, and the even
harmonics and tolerance rounded the formants. On this instrument,
"warm" meant less grit, at 80% more CPU. aither's electronics stay as
they are for it.

One listener, one listen, one patch, and B changed several things at
once, so this says "not for this didge", not "analogue modelling is
worse for aither"; the integrators' saturation is the likeliest cause.
