# Listening patches

**2026-10-28, Roland, with Claude.** First called "aither as an ear"; it
is not a feature or a component. `input()` already brings sound in, and
the rest is plain aither: a patch that starts from what it hears.
Nothing gets a name in the stdlib until it has repeated across real
patches (`lp1(abs(x), 30)`, the detector, may earn `follow` one day).

The core is the crystal set: a tuned circuit (`bpf` or `resonator`,
which is a `dho`, an LC tank) and a detector (`lp1(abs(...))`). A bank
of them is a cochlea, a vocoder's analysis half, and drawn on `tv`, a
waterfall.

## Written

- `patches/listen.aither`: a vocoder, 16 bands of the mic opening the
  same bands of a saw chord.
- `patches/sympathetic.aither`: resonators tuned to D minor that the
  room sets ringing, a sitar's sympathetic strings.
- `patches/follow.aither`: pitch from zero crossings of the low-passed
  mic; a voice a fifth above and a sub below follows what you play.
  Tracks a sung glide to within 1%.
- `patches/duck.aither`: the mic's loudness, fast up and slow down,
  ducks a pad: a sidechain from the room.

Run on the uConsole with `tools/card -- aither patches/listen.aither`
(the card on both ends), or `arecord ... | aither ... | tools/play`.

## Worth writing next

- A waterfall: a bank of 64 bands written into a state array, drawn by
  `tv`.
- The crystal set at radio rates: an SDR into `input()`, a resonator
  on a station, a detector. Turning a station down to 0 is
  `turn(iq, conj(spin(f)))`.
- Measuring on `tools/card` (built 2026-10-17,
  [card-2026-10-16.md](card-2026-10-16.md)): a lock-in amplifier (send a
  tone, turn what comes back by its conjugate spin, low-pass), a swept
  sine, the metal detector and cable tracer of
  [sensing-2026-10-16.md](sensing-2026-10-16.md), VLF through the
  card's input.

## The one real gap

Rates. An SDR gives MHz, a patch runs at one rate, and decimation has
no primitive yet (stepped arrays' `every` hints at its shape). Leave it
until a radio is plugged in.

## Zara

A cochlea of oscillators is the front end of Zara's oscillator-first
recogniser. As an instrument it is a patch; once it feeds a recogniser
that is trained or evaluated, it is model work, to be registered on
experiments.blah.dev before the first run (~/.claude/CLAUDE.md).
