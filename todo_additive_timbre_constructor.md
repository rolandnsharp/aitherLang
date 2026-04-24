# TODO — Additive timbre constructor

A design + implementation plan for replacing aither's "saw / square as
oscillators" model with **additive synthesis from sines as the only true
oscillator primitive**. This both eliminates the aliasing problem and
makes timbre design a first-class data structure.

## Motivation

### Why this isn't really about aliasing

Aliasing in `osc(saw, freq)` and `osc(sqr, freq)` is the surface symptom.
The deeper issue: **saw and square are not natural mathematical primitives
for sound.** They're analog-circuit shortcuts to "rich harmonic content,"
copied into digital synths because that's what the 1980s expected.

Sine is uniquely natural — every physical resonant system (string, drum,
vocal cord, tube, bell, plate) vibrates as a *sum of sines* (Fourier
theorem). Nothing in nature produces a perfect saw. Saw is a mathematical
fiction with `1/n`-weighted infinite harmonics; PolyBLEP is engineering
to mask the fiction's failure mode in digital systems.

The aither move: **treat sine as the only oscillator, treat every other
"waveform" as a recipe of sines.** Aliasing disappears not because we
fixed it but because we removed the cause.

### Aither philosophy alignment

- **Smaller language.** One oscillator (`sin`), not four.
- **Composable.** Timbres become arrays of harmonic amplitudes —
  data, not code. Sound design becomes spectrum design.
- **Educational.** Reading the stdlib teaches the user that a saw is
  `[1, 1/2, 1/3, 1/4, ...]`. A bell is inharmonic phi ratios. The
  math behind classic waveforms becomes visible.
- **Inharmonic sounds first-class.** Bells, glass, gongs, plate partials
  — all just different recipes, no special primitives.
- **CPU cost visible per voice.** User picks how many harmonics per
  voice. The trade-off is in the patch, not hidden in the engine.

### What this is NOT

- Not a deprecation of `saw(p)`, `sqr(p)`, `tri(p)` as **shape functions**.
  They remain useful as transfer curves for waveshaping (`x |> saw` style).
  What's deprecated is using them as oscillators (`osc(saw, freq)`).
- Not a breaking change. Existing patches keep working — `osc(saw, freq)`
  remains as a naive/aliased convenience, useful for chiptune/lofi sounds
  where the digital character IS the desired sound.

## Design

### Surface area: one new primitive

```
additive(freq, recipe)
```

Where `recipe` is a compile-time-constant array of harmonic amplitudes:
- `recipe[0]` = amplitude of fundamental (1× freq)
- `recipe[1]` = amplitude of 2nd harmonic (2× freq)
- `recipe[2]` = amplitude of 3rd harmonic (3× freq)
- ... etc

Returns the sum: `Σ recipe[n-1] * sin(2π · n · freq · t)` for n=1..length.

Auto-band-limited: harmonics where `n * freq >= sample_rate / 2` are
skipped (both for correctness and to save CPU).

### Stdlib library of recipes

```aither
# Classic synth waveforms — sums of sines, mathematically pure
let saw_h    = [1.0, 0.5, 0.333, 0.25, 0.2, 0.167, 0.143, 0.125,
                0.111, 0.1, 0.091, 0.083, 0.077, 0.071, 0.067, 0.063]
let sqr_h    = [1.0, 0.0, 0.333, 0.0, 0.2, 0.0, 0.143, 0.0,
                0.111, 0.0, 0.091, 0.0, 0.077, 0.0, 0.067, 0.0]
let tri_h    = [1.0, 0.0, -0.111, 0.0, 0.04, 0.0, -0.020, 0.0,
                0.012, 0.0, -0.008, 0.0, 0.006, 0.0, -0.004, 0.0]

# Physical / expressive recipes
let bowed_h    = [1.0, 0.6, 0.4, 0.3, 0.25, 0.2, 0.15, 0.1]
let bell_h     = [1.0, 2.756, 5.404, 8.933, 13.345]      # bar partials (inharmonic)
let glass_h    = [1.0, 2.295, 3.873, 5.612]              # plate partials
let voice_ah_h = [1.0, 0.7, 0.5, 0.6, 0.3, 0.4, 0.2]     # vowel "ah" formant approximation
let phi_h      = [1.0, 1.618, 2.618, 4.236, 6.854]       # golden inharmonic stack
let warm_h     = [1.0, 0.5, 0.25, 0.125, 0.0625]         # rolled-off (acoustic-ish)
```

`additive(freq, recipe)` is the user-facing constructor as well as the native
primitive — no extra wrapper needed. One name, one concept.

Note: for `bell_h`, `glass_h`, `phi_h` the values aren't traditional
"harmonics" (integer multiples) — they're INHARMONIC partial ratios.
These need special handling in `additive` (treat the recipe as a list
of frequency multipliers, not amplitudes at integer harmonics).

**Open question:** does `additive` interpret the recipe as
"amplitude at harmonic n" (where n is the position+1) OR as a list of
`(ratio, amplitude)` pairs? Cleaner alternative API:

```
additive(freq, [ratio_amp_pair, ratio_amp_pair, ...])
e.g. additive(220, [(1, 1.0), (2, 0.5), (3, 0.333)])
```

But aither doesn't have tuple syntax. Practical resolution: separate
two flavors of stdlib recipes:
- **Harmonic recipes** (amplitudes at integer harmonics): saw_h, sqr_h,
  tri_h, bowed_h, voice_ah_h, warm_h
- **Inharmonic recipes** (literal frequency multipliers): bell_h, glass_h,
  phi_h, accessed via `additive_partials(freq, ratios, amps)`

Or: keep `additive(freq, [amplitudes])` for harmonic, add a separate
`partials(freq, ratios, amps)` for inharmonic. Two primitives, both small.

## Implementation

### dsp.nim — new native primitive

```nim
proc nAdditive*(s: var DspState; freq: float64;
                recipe: ptr float64; length: int): float64
    {.cdecl, exportc: "n_additive".} =
  ## State layout: `length` phase slots starting at s.idx, one per harmonic.
  let baseIdx = s.idx
  s.idx += length
  let nyquist = 24000.0          # half of 48 kHz sample rate
  var sum = 0.0
  for n in 1..length:
    let amp = recipe[n - 1]
    if amp == 0.0: continue                    # zero amplitude → skip
    let f_n = freq * float64(n)
    if f_n >= nyquist: continue                # above Nyquist → skip (auto band-limit)
    let i = baseIdx + n - 1
    s.pool[i] += f_n / 48000.0
    if s.pool[i] >= 1.0: s.pool[i] -= 1.0
    sum += amp * sin(TAU * s.pool[i])
  sum
```

Note: no normalization. The user is responsible for picking recipe
amplitudes that sum to a reasonable level. For canonical recipes,
`saw_h * 0.64` and `sqr_h * 0.85` are the standard approximations.

### codegen.nim — recognize `additive` builtin

Add to `NativeArities`:
```nim
"additive": -1     # variadic — array literal or top-level array let
```

Add emission case in `emitExpr`:
```nim
if name == "additive":
  if n.kids.len != 2:
    raise newException(ValueError, "additive takes 2 args " & errLoc(n))
  let freq = c.emitExpr(sc, n.kids[0])
  let arr = n.kids[1]
  var sym = ""
  var length = 0
  if arr.kind == nkIdent:
    let found = c.lookupArray(sc, arr.str)
    if not found.ok:
      raise newException(ValueError,
        "additive: " & arr.str & " is not a numeric-literal array " & errLoc(arr))
    (sym, length) = (found.sym, found.length)
  elif arr.kind == nkArr:
    sym = c.fresh("arr")
    length = arr.kids.len
    var items: seq[string] = @[]
    for k in arr.kids:
      if k.kind != nkNum:
        raise newException(ValueError,
          "additive recipe must be numeric literals only " & errLoc(k))
      items.add numLit(k.num)
    c.arrayDecls.add &"static const double {sym}[{length}] = {{" &
      items.join(", ") & "};\n"
  else:
    raise newException(ValueError,
      "additive: second arg must be an array literal or let-bound array " & errLoc(arr))
  let off = c.registerRegion("additive", length)
  return &"(s->idx = {off}, n_additive((DspState*)s, {freq}, (double*){sym}, {length}))"
```

Add extern declaration in prelude:
```
extern double n_additive(DspState*,double,double*,int);
```

### voice.nim — register symbol

```nim
discard s.addSymbol("n_additive", cast[pointer](nAdditive))
```

### stdlib.aither — recipes + constructor

Add the recipe arrays as documented above. No wrapper proc needed —
`additive(f, recipe)` is the user-facing API directly.

### Tests

`tests/additive.nim`:
```nim
## Verify additive synthesis: a recipe of [1.0] should match a pure sine.
## A multi-harmonic recipe should produce a richer spectrum.

import std/math
import ../parser, ../voice

const SinePatch = """
additive(440, [1.0])
"""
const SawPatch = """
let saw_h = [1.0, 0.5, 0.333, 0.25]
additive(440, saw_h)
"""

# Tick both, compare
let v1 = newVoice(48000.0)
v1.load(parseProgram(SinePatch), 48000.0)
let s1 = v1.tick(0.0)
doAssert abs(s1.l - 0.0) < 0.001, "sine at t=0 should be 0"

let s2 = v1.tick(1.0 / (4.0 * 440.0))   # quarter cycle
doAssert abs(s2.l - 1.0) < 0.001, "sine at quarter cycle should be 1.0"

let v2 = newVoice(48000.0)
v2.load(parseProgram(SawPatch), 48000.0)
# Just verify it produces a non-zero output without NaN
var anyNonzero = false
for i in 0..1000:
  let s = v2.tick(float64(i) / 48000.0)
  doAssert s.l == s.l, "saw should not produce NaN"
  if abs(s.l) > 0.01: anyNonzero = true
doAssert anyNonzero, "saw recipe should produce audible output"

echo "additive ok"
```

`tests/additive_band_limit.nim`:
```nim
## A 4kHz fundamental with 16 harmonics should auto-skip harmonics above Nyquist.
## Specifically: harmonics 7-16 are above 24kHz at f=4000, must contribute nothing.

import ../parser, ../voice

const Patch = """
let h16 = [1.0, 0.5, 0.333, 0.25, 0.2, 0.167, 0.143, 0.125,
           0.111, 0.1, 0.091, 0.083, 0.077, 0.071, 0.067, 0.063]
additive(4000, h16)
"""

let v = newVoice(48000.0)
v.load(parseProgram(Patch), 48000.0)

# Compare with a recipe truncated to only the harmonics that should be active (1-6)
const PatchTrunc = """
let h6 = [1.0, 0.5, 0.333, 0.25, 0.2, 0.167]
additive(4000, h6)
"""
let v2 = newVoice(48000.0)
v2.load(parseProgram(PatchTrunc), 48000.0)

# Sample-by-sample comparison should be near-identical (the auto-skipped harmonics
# in the 16-recipe should add zero, matching the 6-recipe output).
var diff = 0.0
for i in 0..1000:
  let t = float64(i) / 48000.0
  let s16 = v.tick(t)
  let s6 = v2.tick(t)
  diff += abs(s16.l - s6.l)
doAssert diff / 1000.0 < 0.001, "auto-band-limit should match truncated recipe"

echo "additive_band_limit ok"
```

### Documentation

- **SPEC.md**: add `additive(freq, recipe)` to the primitives section.
  Document that recipe is a compile-time-constant array of harmonic
  amplitudes. Document auto-band-limiting behavior.
- **stdlib.aither comments**: explain each recipe's character
  (saw_h is the classic synth saw, bell_h is metallic-inharmonic, etc).
- **GUIDE.md**: add a "Designing timbres" section showing how to
  create custom recipes and morph between them.
- **README.md**: update the primitives list to mention `additive`.
- **PHILOSOPHY.md**: add a paragraph about sine being the only true
  oscillator primitive, all other waveforms being recipes.

## Implementation order

1. Native `additive` in dsp.nim + codegen registration + voice.nim symbol.
2. Test `tests/additive.nim` — basic behavior.
3. Test `tests/additive_band_limit.nim` — auto-skip above Nyquist.
4. stdlib.aither — add recipes (no constructor wrapper; `additive` is the API).
5. Update existing example patches in patches/ to use `additive(f, saw_h)`
   instead of `osc(saw, f)` — verify they still sound right.
6. Documentation updates.

Single commit, ~150 lines net change. Estimate: 1.5-2 hours.

## Out of scope

- **Inharmonic partial recipes** (`bell_h`, `glass_h`, `phi_h`) need a
  different primitive that takes `(ratios, amplitudes)` pairs instead of
  fixed integer harmonics. Defer to follow-up if integer-harmonic-only
  isn't enough.
- **Time-varying recipes** (recipe values that change per-sample) would
  enable spectral morphing in real time. Cool but adds complexity.
  Defer.
- **Removing `osc(saw, freq)` and `osc(sqr, freq)`** — keep them as
  naive/aliased convenience for chiptune/lofi/character work. Only the
  documentation changes to recommend `additive(f, saw_h)` for clean audio.

## Decision log

- **Why not PolyBLEP?** PolyBLEP is engineering to mask the fact that
  sawtooth-as-oscillator is mathematically wrong for digital audio.
  Additive synthesis is mathematically correct by construction. The
  CPU cost difference (additive 16-harmonic vs PolyBLEP) is ~3-4x per
  voice, but typical aither patches use few enough voices that this is
  a small fraction of overall CPU budget. Reverb dominates. The
  philosophical clarity wins.
- **Why keep `saw(p)`, `sqr(p)`, `tri(p)` shape functions?** They have
  a separate use as TRANSFER CURVES — `x |> saw` applies the saw
  transfer to an existing signal, which is a different (and useful)
  operation than generating a periodic saw. Don't conflate the two
  uses by removing the shape functions.
- **Why a single `additive` primitive instead of `bl_saw_4`,
  `bl_saw_8`, ...?** The recipe IS the harmonic count. User picks the
  recipe length per voice. One primitive, infinite timbres.
- **Why not implement `additive` purely in stdlib (no native primitive)?**
  Could be done — `additive(f, recipe) = sum of conditional sines`. But
  every harmonic gets computed even when above Nyquist, wasting CPU.
  Native primitive can short-circuit, halving compute for high-pitched
  notes.
