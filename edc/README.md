# Learn aither

Fourteen playable lessons, from a single sine wave to an
instrument you play from a keyboard. Each lesson is an
`.aither` file: read the comments, send it, listen, then
change things and send it again. Most lessons end with a
puzzle that checks itself.

## Start

From `~/aitherLang`:

```
./aither | tools/play hw:rockchipes8388c 20    # once per session, in its own terminal
edc/learn                 # the lessons, and which puzzles you've solved
edc/learn play 1          # hear lesson 1
edc/learn check 1         # check your answer to puzzle 1
edc/learn puzzle 1        # hear your puzzle attempt
edc/learn stop            # fade everything out
```

The engine writes sound to its standard output; `tools/play` (built
with `make tools/play`) is the uConsole's speaker on the end of the
pipe. The uConsole runs at 44.1 kHz (`AITHER_RATE=44100` in
`.bashrc`), the only rate its codec does. Elsewhere, `./aither |
aplay -f FLOAT_LE -c 2 -r 48000`.
Headphones help: the built-in speaker can't play much below 200 Hz.

## The lessons

| #  | Lesson                                  | Puzzle                           |
|----|-----------------------------------------|----------------------------------|
| 01 | The clock and the sine                  | Up an octave, down in volume     |
| 02 | TAU: the circle inside every sound      | Three dots, no sound             |
| 03 | Waveforms and harmonics                 | The odd one out                  |
| 04 | Mixing, chords and beating              | A minor chord                    |
| 05 | Additive synthesis                      | Make the third harmonic sing     |
| 06 | Envelopes and rhythm                    | Teach the robot to swing (♪)     |
| 07 | Filters (subtractive synthesis)         | Under a blanket                  |
| 08 | Modulation: making sounds alive         | Ghost notes from a wobble        |
| 09 | FM synthesis                            | Find the modulator               |
| 10 | Drums, noise and groove                 | Make it sizzle                   |
| 11 | Physical modelling                      | Tune the spring                  |
| 12 | Bells, metal and spectral tricks        | Slide the spectrum               |
| 13 | Composing a piece                       | Write a breakdown (♪)            |
| 14 | Your instrument: playing from a keyboard | Design a knob (♪)               |

♪ puzzles are judged by ear: the checker only makes sure
they compile and make a sound.

## How puzzles check themselves

Each puzzle lists its goals in `#@` lines, like
`#@ top 600 5` ("the loudest peak is within 5 Hz of
600"). `edc/learn check` renders the patch offline with
`aither audit` (no speaker needed) and compares the
spectrum with the goals. The goal types are documented at
the top of `edc/learn`.

Answers are in `edc/solutions/`. Try first: the hints at
the bottom of each puzzle come in two steps.

For maintainers: `edc/learn selftest` checks that every
starter fails and every solution passes.
