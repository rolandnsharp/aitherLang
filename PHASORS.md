# Phasors: why aither turns pairs

Every value in aither is a pair, and a pair is a point that can turn.
This page is why: the ideas the language is built on, where they come
from, and what we measured when we tried the alternatives. Read it to
understand why aither code is written the way it is: `turn` instead of
multiplying sides, `amplitude` instead of rectifying, `dho` returning a
pair, a `_` being worth a second look.

## 1. A pair is a phasor

Charles Steinmetz showed in 1893 that a steady oscillation is best
handled as a point turning in a plane: its length is how big it is,
its angle where in its cycle it is, and what would be calculus on
sines becomes arithmetic on points [1]. Electrical engineering has
worked that way since, and so does aither. The two sides of a pair are
two real numbers in fixed quadrature; aither does not add a "complex
number" type, it adds the operations that read a pair as one
(PHILOSOPHY.md, "Complex numbers as operations, not types"):

| operation       | what it does to the point                          |
|-----------------|----------------------------------------------------|
| `spin(hz)`      | a unit point turning `hz` times a second           |
| `turn(z, w)`    | `z` turned by `w`'s angle and scaled by its length |
| `conj(z)`       | the same point turning the other way               |
| `amplitude(z)`  | how far it is from the centre                      |
| `phase(z)`      | which way it points                                |

**Which way is positive: anticlockwise.** A positive frequency is a
point turning anticlockwise, `e^(+iωt)`, which is `[cos, sin]`, which
is `spin(hz)`: the convention of mathematics, of electrical engineering
since Steinmetz, and of signal processing, and the one an SDR's IQ
uses (a station above the tuned frequency turns anticlockwise). Some
physics, quantum mechanics and optics, writes waves as `e^(−iωt)`; a
signal language follows the engineers. Everything in aither that
turns, turns this way: `spin`, `analytic`, `rotate`, `dho`, `vdp`; and
`tv` draws y upward, so on screen it is anticlockwise too.

The one rule worth remembering: **turning by a spin adds its speed.**
`turn(spin(220), spin(50))` is `spin(270)`, and `turn(p, p)` doubles
`p`'s speed. A frequency shift, a radio tuned down, single-sideband
mixing and octave doubling are all this rule.

A phasor has an amplitude and a phase, in the engineers' words since
Steinmetz, and those are the names: `amplitude(z)` is its length (what
mathematics calls the modulus, `|z|`), `phase(z)` its angle. Not `abs`:
a function given a pair runs on each side, so `abs` of a pair is two
absolute values, and the length gets its own word.

`+`, `-`, `*` and `/` stay side by side, so stereo arithmetic means
what it looks like and nothing changes meaning because a value
happened to be a pair. Only the named operations above read a pair as
a turning point.

## 2. A signal as spins: `analytic`

A real signal, one number a sample, has no direction to turn in. Dennis
Gabor's analytic signal (1946) gives it one: pair each frequency in the
signal with the same frequency a quarter turn behind, and the signal
becomes a sum of points each turning at its own frequency [2].
`analytic(x)` does this with two cascaded all-pass networks whose
outputs stay 90° apart across the audio band (Olli Niemitalo's
coefficients [3]); it is clean above about 30 Hz.

Its length is the signal's envelope and its turning rate the signal's
frequency, at every instant, which is section 4.

## 3. Moving every frequency: `freq_shift`

`freq_shift(signal, hz)` is `turn(analytic(signal), spin(hz))`: every
spin in the signal turned `hz` faster, so every frequency moves up by
`hz` Hz, with no mirror image below. This is single-sideband
modulation by the phasing method, which Ralph Hartley worked out for
radio in the 1920s [4]. Because Hz are added rather than multiplied,
harmonics stop being harmonic: shifted, a voice turns metallic, a chord
bell-like. It returns the turned pair, in quadrature, and what hears
it decides: speakers play its sides a quarter turn apart, an SDR sends
it as IQ. Summed to mono it doesn't cancel: cos + sin is a sine 3 dB up.

## 4. Read the whole pair

A pair from an oscillator or from `analytic` is a phasor, and most of
what is wanted from it reads both sides at once:

| you want                      | read the pair with                              |
|-------------------------------|-------------------------------------------------|
| how loud                      | `amplitude(p)`                                  |
| how fast it turns: frequency  | `phase(turn(p, conj(prev(p)))) / (TAU * dt)`    |
| move it, shift it, mix it     | `turn(p, w)`                                    |
| hear it                       | the pair itself                                 |

Length as instantaneous amplitude and turning rate as instantaneous
frequency are Gabor's, and Boualem Boashash's review is the standard
account of what instantaneous frequency means and how far it can be
trusted [5]. The frequency formula turns this sample's point back by
last sample's, so what is left is the angle turned in one sample, with
no wrapping to undo.

**The crystal radio shows why.** A 1906 crystal set [6] has an
antenna, a tuned circuit (a coil and a capacitor, ringing at the
station's frequency), a crystal diode and an earphone. It has one side
of the signal, the voltage across the coil, so to hear the station's
loudness it must keep the upper half of each wave (the diode) and
average it (the earphone). A `dho` is the tuned circuit (a coil and a
capacitor is a damped harmonic oscillator) and gives both sides, so its
length is the station's sound at every instant:

```
amplitude(dho(antenna * 2 * damp * w * w, dial, damp))
```

That line is `patches/crystal.aither`, with `patches/station.aither`
broadcasting two AM stations down the pipe:
`aither patches/station.aither | aither patches/crystal.aither | tools/play`.
Tuned to 6 kHz it gives the melody station about 30 times louder than
the other; tuned to 9 kHz, the chord station about 280 times (with the
turning `dho` of section 5; the stepped one managed 22 and 100).

**So a `_` is a sign.** `let [x, _] = p` throws half the information
away. When you write one, look for the operation that reads the whole
pair; `_` is right only where one number is all there is room for, a
sound into a mono mix or a filter that takes one.

What we measured, on a synthesised singer gliding 150 to 300 Hz
(2026-10-28):

| pitch from                                      | mean error | worst |
|-------------------------------------------------|-----------:|------:|
| zero crossings of the low-passed mic            | 0.69%      | 1.0%  |
| how fast `analytic` of it turns, every sample   | 0.93%      | 3.3%  |

Close, and the phasor needs no state machine and gives a pitch every
sample (`patches/follow.aither`). With a loud kick in the same mic the
zero crossings held up better (3% against 8%): a phasor follower wants
one instrument.

## 5. The oscillator: `dho` turns

`dho(force, freq, damp)` is the damped harmonic oscillator,
`x'' = force − 2·damp·w·x' − w²·x`, the spring every resonance is. Its
free motion is a point turning at `wd = w·sqrt(1 − damp²)` while
shrinking by `exp(−damp·w)` a second. So aither samples it exactly:
each sample the phasor turns by `wd·dt`, shrinks, and the force pushes
it:

```
$z = turn($z, exp(-damp * w * dt) * [cos(wd * dt), sin(wd * dt)]) + [0, -force * dt / wd]
```

This is the "phasor filter" Max Mathews and Julius Smith described for
very high-Q resonators that stay well behaved however they are tuned
[7]: one complex multiply a sample, with the real part the resonator's
output.

**Why not step the equation?** Until 2026-10-28 `dho` stepped velocity
then position (semi-implicit, or symplectic, Euler). That method keeps
an oscillator's energy bounded, which is why it was chosen, but what it
keeps is a slightly different energy: its orbit is an ellipse that
leans more as the frequency rises, and it runs away once `w·dt`
passes 2, about 14 kHz at 44.1 kHz [8]. We measured it:

| `dho` driven at its own frequency | stepped (old)   | turned (now) |
|-----------------------------------|-----------------|--------------|
| phasor length at 220 Hz           | 0.496 – 0.504   | 0.488 – 0.513 |
| at 6 kHz                          | 0.28 – 0.44     | 0.50 – 0.53  |
| at 18 kHz                         | runs away       | stable       |

(The small ripple left is the real drive's own mirror-image half, at
twice the frequency, which a real signal always has.)

**What the pair means.** `dho` returns `[x, q]`: `x` the position, the
sound; `q` the position a quarter turn behind. It turns the way `spin`
does, so `phase(dho(...))` increases with time and turning it by a spin
adds speeds, as everywhere else.

**The second side isn't velocity.** Undamped, `q` is exactly `−v / w`.
Damped, the point also shrinks as it turns, so the velocity takes both
sides. With `z = x + i·q = A·e^((−damp·w + i·wd)·t)`, differentiating
the real part gives

```
v = -(wd * q + damp * w * x)
```

The didge patches use this for their bore modes' damping force, with a
comment saying so. Taking `−w·q` alone would be off by about the
damping, 5–9% for those modes, which is the `_` rule earning its keep:
the right answer needed both sides.

**The push.** The force enters as `force·dt / wd`, which is a strike's
velocity kick in the phasor's units, so a strike of a given force swings
as far as it always did, and a constant force stretches the spring to
exactly `force / w²`, as the equation says, at any damping below 1.

**An oscillator turns.** A spring damped past critical (`damp ≥ 1`)
doesn't oscillate, it creeps back, and a turning point can't represent
it. `dho` keeps `damp` just under 1; an envelope that glides is
`swell` or `slew`'s job.

## 6. Steady sines turn

The same picture makes plain sines cheaper. `sin(TAU * phasor(f))` with
`f` steady is a point turning by `TAU·f/sr` each sample, so instead of
a sine a sample the compiler keeps the point and turns it: four
multiplies by a step computed once a block, then one Newton step,
`z *= 1.5 − 0.5·|z|²`, to put its length back to 1, so rounding never
lets it grow or shrink over hours. This is the recursive (rotation)
sine oscillator of the DSP literature [10]; aither applies it where it
is safe, when the frequency can't change within a block, and keeps the
phasor and the sine wherever it can (vibrato, FM). The compiler
chooses; the patch reads the same.

Measured on the uConsole's CPU, live (`tick_block`, the `cc -O2` build
that plays; 2026-10-28):

| patch         | before | turning |
|---------------|-------:|--------:|
| `acid_lead`   | 64% of a core | 43% |
| `complex_pad` | 16%    | 6%      |

and it matches the sine it replaces to within 1e-9.

## 7. Listening

Patches that start from `input()` are plain aither on these ideas, not
features (notes/listening-2026-10-28.md):

- `patches/listen.aither`, a vocoder: Homer Dudley's 1939 idea [9],
  each band's loudness the length of a `dho` the mic drives, opening
  the same band of a saw chord.
- `patches/sympathetic.aither`: resonators tuned to a scale that the
  room sets ringing, a sitar's sympathetic strings; a crystal set
  without the crystal.
- `patches/follow.aither`: pitch as turning rate, section 4.
- `patches/duck.aither`: the room's loudness ducking a pad.

## References

1. C. P. Steinmetz, "Complex quantities and their use in electrical
   engineering", Proceedings of the International Electrical Congress,
   Chicago, 1893.
2. D. Gabor, "Theory of communication. Part 1: The analysis of
   information", Journal of the IEE, Part III, 93 (26), 1946.
   https://doi.org/10.1049/ji-3-2.1946.0074
3. O. Niemitalo, "Hilbert transform", yehar.com (polyphase IIR
   Hilbert transformer coefficients, as used in `dsp.c`).
4. R. V. L. Hartley, "Relations of carrier and side-bands in radio
   transmission", Proceedings of the IRE, 1923.
   https://doi.org/10.1109/jrproc.1923.219862
5. B. Boashash, "Estimating and interpreting the instantaneous
   frequency of a signal. I. Fundamentals", Proceedings of the IEEE,
   80 (4), 1992. https://doi.org/10.1109/5.135376
6. G. W. Pickard, "Means for receiving intelligence communicated by
   electric waves", US Patent 836,531, 1906 (the silicon crystal
   detector).
7. M. Mathews and J. O. Smith, "Methods for synthesizing very high Q
   parametrically well behaved two pole filters", Proceedings of the
   Stockholm Music Acoustics Conference (SMAC), 2003. (Not in the
   index we checked these against.)
8. E. Hairer, C. Lubich and G. Wanner, *Geometric Numerical
   Integration: Structure-Preserving Algorithms for Ordinary
   Differential Equations*, 2nd ed., Springer, 2006 (symplectic Euler
   and the modified energy it conserves).
9. H. Dudley, "Remaking speech", Journal of the Acoustical Society of
   America, 11, 1939. https://doi.org/10.1121/1.1916020
10. P. Symons, "Recursive sine wave oscillators", book chapter,
    Cambridge University Press, 2013.
    https://doi.org/10.1017/cbo9781139108072.004
