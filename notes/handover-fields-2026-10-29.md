# Handover: fields, space and spatial audio

For a fresh session. Written 2026-10-29 at the end of a long one. Read
this first, then [fields-2026-10-29.md](fields-2026-10-29.md) (the whole
design history, options and measurements) and
[stepping-2026-10-29.md](stepping-2026-10-29.md).

## The aim

Roland's words: "my whole point of aitherlang is to be the best
language to express field physics." It is general purpose, "not just
for me", and must stay "simplex and elegant". Spatial audio, sound as a
field around a listener, is the musical side of the same aim.

## The limitation

aither's model is one sample at one place: a program is what happens
here, now, and `$` reaches one moment back. That is why it is simple and
why waves, modes, shared coupling and polyphase read well. A field is
many places at one moment, and aither has no way to name "all the
places" as one thing. Everything that needs that is hard or missing:
grids (contacts, terrain, FDTD), spectral processing (phase vocoder,
freeze), FDN reverbs (delays through a matrix), and spatial audio
beyond stereo (ambisonics, wave field synthesis, beamforming). Every
value is a pair, so stereo is built in and everything wider is
hand-made.

## What Roland decided (don't relitigate)

- **V (fields as values, `{x, y}` pairs, whole-field arithmetic) is
  out:** "it takes away from the simplicity of the rest of the
  language." Two models of a program in one patch.
- **B (a field declared with its local law) and S (`space(shape)` with
  neighbour reads) were passed on.** Both new constructs.
- **The direction is M and X, side by side; users choose by the
  medium.**
  - **M: a field is its modes.** Each mode a phasor (magnitude and
    spin); the medium is its dispersion ω(k); interactions are phasors
    multiplied under selection rules; space and time enter only where
    you observe (a turn by k·x − ωt). Roland: "field theory doesn't do x
    and y", "mag and spin might do true space even though they don't
    represent it as three d." The note's M section has the syntax
    sketch (`modes`, `spin`, `life`, `energy`, `at`, `kick`).
  - **X: coordinates like `t`** for fields that need places (contacts,
    terrain): `x`, `y` given to the program as `t` is, a declared domain
    in metres, state read at another place, an observation for the
    sample. Not `$x` (coordinates aren't state). Details open.
- **Arrays may change**, "we don't even care about breaking arrays are
  data not a map", but nothing about pairs.
- This agrees with the older [bachPolyphase.md](bachPolyphase.md): a
  field stays dimensionless (magnitude and phase, no coordinates); M
  keeps that and puts space only in the observation.

## What was tried and measured (2026-10-29)

M, as a gong in today's aither, then dropped ("not an amazing gong
sound"). The note's "M tried" section has the numbers. In short: a free
field in modes is exact and cheap; one mode label (a ring) can't cascade,
two labels (a plate, Chladni's f ∝ (m + 2n)², m conserved, Pythagorean
resonances) can; the stepping had to be hand-written or a hard strike ran
away; a one-sample strike sounds like a crash, a mallet's contact is a
low-pass.

## Spatial audio, the open part

- **The hardware is stereo.** `tools/play` opens 2 channels; the
  uConsole's es8388 is a stereo codec, and Roland listens on its speaker
  or headphones. So what reaches an ear here is two channels:
  binaural (headphones) or a stereo decode. More channels need other
  hardware and an engine change (`play` and the voice's output are
  pairs).
- **M covers the representation.** A sound field around a listener is
  its modes: spherical harmonics, ambisonics' channels. Rotating the
  field turns its modes; decoding to a direction is `at`. Sources at
  places project into the modes. This was noted, not worked through.
- **Binaural is the realistic target on this machine:** a field's modes
  decoded to two ears through a head model (HRTF). It needs filters per
  direction, which is the spectral/convolution gap again.
- Earlier ideas: [GAME_AUDIO_ROADMAP.md](GAME_AUDIO_ROADMAP.md) §8 lists
  HRTF, distance, Doppler, occlusion, room responses as primitives on
  top of a listener (its §6, "listener decoupling").

## Constraints (from the repo and Roland)

- **No machine in patches.** No control rate, no `slow()`: every state
  every sample; speed only from what the compiler proves unheard. Judge
  on general shapes (`make bench`). (Memory: aither-no-machine-in-patches.)
- **Design first.** Discuss and write the note, with real patches
  rewritten both ways, before building. Spec every language change in
  SPEC.md in the same commit.
- **The corpus** (`make corpus`) must stay bit-identical unless a sound
  change is intended; `tests/corpus` and `tests/run` link the compiler
  statically, so rebuild them before trusting a run.
- Commit explicit paths; push only when asked.
- **Speed.** Patches compile to C run by TCC, which doesn't optimise.
  On battery `uconsole-keeper` turns the two A72 cores off and caps the
  A53s at 1 GHz: check `/sys/devices/system/cpu/online` before timing,
  and time on battery for anything meant to play live.
- Roland prefers measuring to describing: record the speaker with the
  headset mic when listening is the question.

## Where things stand in the code

- Pair elements in arrays (`array(N, [1, 0])`), `each`, `sum`, state
  and stepped arrays: SPEC "State arrays".
- `array(N)` reads 0 past its ends, local or state; a list cycles.
- Compile-time folding: indexes, `if` conditions, `let`s and zero terms
  known at compile time cost nothing at run time; `spin` is a turner
  alone. A field of sixteen coupled modes fits in real time on mains.
- `^` is the power. `unturn`, `log(x, base)` exist.
- Arrays brief, still open: a def can't return an array; a lambda can't
  take one.

## Where to start

1. Work M through for spatial audio on paper: a source at a direction
   projected into a few spherical-harmonic modes, the field rotated, the
   modes decoded to two ears. Write it in today's aither by hand
   (pair arrays and `each`), measure, and see what reads badly. That is
   the brief for whatever M needs from the language.
2. Decide whether `modes` and friends are stdlib defs (they were, and
   worked: `mode_now`, `mode_step`, `observe`; dropped with the gong) or
   need the arrays brief first (a def returning an array).
3. Keep the stepping question (stepping-2026-10-29.md) in view: any
   coupled field M builds will need it.
