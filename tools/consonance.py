#!/usr/bin/env python3
"""consonance: a scale that suits a spectrum, from its dissonance curve.

    python3 tools/consonance.py            the tables tuned_keys.aither uses

Sethares (Tuning, Timbre, Spectrum, Scale): two tones are rough where
their partials sit about a quarter of a critical band apart, so which
intervals sound consonant depends on the partials, not on the numbers
2, 3/2, 5/4. Move one tone against another by a ratio r, add up the
roughness of every pair of partials, and the dips in that curve are the
intervals this spectrum makes consonant. A harmonic spectrum's dips are
the just intervals (6/5, 5/4, 4/3, 3/2, 5/3 ... 2). A metal bar's are
somewhere else, and it has no dip at 2: its octave is its own 2nd
partial, 2.778.

For each spectrum this prints a period (where the scale repeats: a dip
the spectrum itself defines) and the STEPS - 1 deepest dips inside it,
ranked by prominence (how far the curve rises on both sides before
falling lower), so a 7-step scale per period whatever the spectrum. A
spectrum with too few dips (phi) gets its widest gaps split evenly in
pitch, marked * in the output.
Same-length tables line up degree by degree, so a patch can morph the
tuning between spectra the way it morphs the partials.
"""
import math

N = 8          # partials per tone, amplitude 1/n (as tuned_keys plays them)
STEPS = 7      # scale degrees per period, the unison included
F0 = 261.63    # C4: the roughness model depends on absolute frequency

SPECTRA = {    # partial ratios, and the period each scale repeats at
    "string": ([n for n in range(1, N + 1)], 2.0),
    "bar":    ([((2 * n + 1) / 3) ** 2 for n in range(1, N + 1)], ((2 * 2 + 1) / 3) ** 2),
    "phi":    ([1.618 ** (n - 1) for n in range(1, N + 1)], 1.618 ** 2),
}

def roughness(f1, a1, f2, a2):
    """Sethares' fit to Plomp and Levelt: two sines, frequencies and amplitudes."""
    lo, hi = min(f1, f2), max(f1, f2)
    s = 0.24 / (0.0207 * lo + 18.96)
    return min(a1, a2) * (math.exp(-3.51 * s * (hi - lo)) - math.exp(-5.75 * s * (hi - lo)))

def dissonance(ratios, amps, r):
    return sum(roughness(F0 * p, a, F0 * r * q, b)
               for p, a in zip(ratios, amps) for q, b in zip(ratios, amps))

def scale(ratios, period):
    amps = [1 / n for n in range(1, N + 1)]
    step = 0.0005
    rs = [1 + i * step for i in range(int((period - 1) / step) + 1)]
    d = [dissonance(ratios, amps, r) for r in rs]
    dips = []
    for i in range(1, len(d) - 1):
        if d[i] < d[i - 1] and d[i] <= d[i + 1]:
            # prominence: the lower of the highest points on each side
            # before the curve drops below this dip again
            left = right = d[i]
            j = i - 1
            while j >= 0 and d[j] >= d[i]:
                left = max(left, d[j]); j -= 1
            j = i + 1
            while j < len(d) and d[j] >= d[i]:
                right = max(right, d[j]); j += 1
            dips.append((min(left, right) - d[i], rs[i]))
    s = [1.0] + sorted(r for _, r in sorted(dips, reverse=True)[:STEPS - 1])
    filled = []
    while len(s) < STEPS:
        # Too few dips (phi has three): split the widest gap, the period
        # included, at its geometric middle, so every table has STEPS
        # degrees. These are marked: they are even steps, not consonances.
        edges = s + [period]
        k = max(range(len(s)), key=lambda i: edges[i + 1] / edges[i])
        mid = math.sqrt(edges[k] * edges[k + 1])
        s.insert(k + 1, mid)
        filled.append(mid)
    return s, filled

if __name__ == "__main__":
    for name, (ratios, period) in SPECTRA.items():
        s, filled = scale(ratios, period)
        cents = " ".join(f"{1200 * math.log2(r):.0f}" + ("*" if r in filled else "") for r in s)
        print(f"# {name}: period {period:.4f}; cents {cents}" +
              ("  (* an even step, not a dip)" if filled else ""))
        print(f"let {name}_scale = [{', '.join(f'{r:.4f}' for r in s)}]")
