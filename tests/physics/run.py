#!/usr/bin/env python3
"""The physics suite: aither's fields against known answers and against
themselves as the step shrinks (notes/one-system-2026-11-11.md).

    make physics                      every case
    tests/physics/run.py springs wake a case, or any whose name has the word

Each case says what the physics is, what it measures, and the bound it holds
the engine to. A case fails when today's engine misses the physics: that is
the point of it, so this suite is not part of `make test`. Needs numpy."""

import os, subprocess, sys, numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
AI = os.environ.get('AI', os.path.join(HERE, '..', '..', 'aither'))

def render(patch, rate, seconds):
    """Both channels of an offline run, as float64, at `rate`."""
    env = dict(os.environ, AITHER_RATE=str(rate))
    p = subprocess.run([AI, os.path.join(HERE, patch), str(seconds)], stdin=subprocess.DEVNULL,
                       capture_output=True, env=env)
    said = [l for l in p.stderr.decode(errors='replace').split('\n') if 'error' in l or 'exceeded' in l]
    a = np.frombuffer(p.stdout, dtype=np.float32).astype(np.float64)
    return a[0::2], a[1::2], said

def peak(x, rate, near, span=3.0, pad=8):
    """The strongest frequency within `span` Hz of `near`, interpolated, and its level."""
    w = np.hanning(len(x))
    S = np.abs(np.fft.rfft(x * w, len(x) * pad))
    df = rate / (len(x) * pad)
    lo, hi = int((near - span) / df), int((near + span) / df)
    i = lo + int(np.argmax(S[lo:hi]))
    a, b, c = np.log(S[i - 1:i + 2] + 1e-300)
    d = 0.5 * (a - c) / (a - 2 * b + c)
    return (i + d) * df, np.exp(b - 0.25 * (a - c) * d)

def strongest(x, rate, lo=50, hi=3000, pad=8):
    S = np.abs(np.fft.rfft(x * np.hanning(len(x)), len(x) * pad))
    df = rate / (len(x) * pad)
    i = int(np.argmax(S[int(lo / df):int(hi / df)])) + int(lo / df)
    return peak(x, rate, i * df, span=2 * df * pad)[0]

def cents(f, g):
    return 1200 * np.log2(f / g)

# ------------------------------------------------------------------- cases

def mode_drift(L, R, rate, f, which):
    """A coupled pair's mode, (L + R) or (L - R): pitch against f, and level over the run."""
    m = L + R if which == 0 else L - R
    def at(t0, t1):
        i0, i1 = int(t0 * rate), int(t1 * rate)
        t = np.arange(i0, i1) / rate
        s, c = np.sum(m[i0:i1] * np.sin(2 * np.pi * f * t)), np.sum(m[i0:i1] * np.cos(2 * np.pi * f * t))
        return np.hypot(s, c), np.arctan2(c, s)
    (a0, p0), (a1, p1) = at(0.0, 0.1), at(1.9, 2.0)
    dp = (p1 - p0 + np.pi) % (2 * np.pi) - np.pi
    return 1200 * np.log2(1 + dp / (2 * np.pi * f * 1.9)), 20 * np.log10(max(a1, 1e-300) / a0)

def springs(patch):
    """Two springs at 440 Hz, coupled so their modes are 440 and 466.16 Hz,
    lossless, one kicked: each mode within 0.5 cents, and its level held to
    0.1 dB over 2 s, at 48, 96 and 192 kHz."""
    worst_c = worst_db = 0
    for rate in (48000, 96000, 192000):
        L, R, said = render(patch, rate, 2)
        if said or len(L) < rate * 2: return False, '%d Hz: %s' % (rate, said[0] if said else 'cut short')
        for k, f in enumerate((440.0, 466.16)):
            c, db = mode_drift(L, R, rate, f, k)
            worst_c, worst_db = max(worst_c, abs(c)), max(worst_db, abs(db))
    return worst_c < 0.5 and worst_db < 0.1, 'worst %.3f cents, %.4f dB' % (worst_c, worst_db)

def case_springs_as_laws():
    return springs('springs_laws.aither')
case_springs_as_laws.__doc__ = 'As four lines of laws. ' + springs.__doc__

def case_springs_as_dho():
    return springs('springs_dho.aither')
case_springs_as_dho.__doc__ = 'As two dho, coupled through $state. ' + springs.__doc__

def case_string_partials():
    """A string, fixed ends, 110 Hz, struck at 0.3 for 0.5 ms, heard at 0.13:
    its first 12 partials at n * 110 Hz within 1 cent, and their levels
    within 0.5 dB of the exact (from the strike's and the pickup's places),
    away from nodes, at 48, 96 and 192 kHz."""
    f0, at, hear, tau = 110.0, 0.3, 0.13, 0.0005
    def exact(n):
        w = 2 * np.pi * f0 * n
        return abs(np.sin(np.pi * n * at) * np.sin(np.pi * n * hear) * np.sinc(w * tau / 2 / np.pi) / w)
    worst_c = worst_db = 0
    for rate in (48000, 96000, 192000):
        L, _, said = render('grid_string.aither', rate, 4)
        if said: return False, said[0]
        x = L[int(0.1 * rate):]
        P = [peak(x, rate, n * f0) for n in range(1, 13)]
        for n, (f, a) in enumerate(P, 1):
            if exact(n) / exact(1) < 0.05: continue
            worst_c = max(worst_c, abs(cents(f, n * f0)))
            worst_db = max(worst_db, abs(20 * np.log10(a / P[0][1]) - 20 * np.log10(exact(n) / exact(1))))
    return worst_c < 1 and worst_db < 0.5, 'worst %.2f cents, %.1f dB' % (worst_c, worst_db)

def partials(patch, f_of, level_of, n_max=12, bound_c=1.0, bound_db=0.5):
    """Partials of a struck line against exact: pitch within bound_c cents,
    level against the first within bound_db dB, away from nodes, at 48, 96
    and 192 kHz."""
    worst_c = worst_db = 0
    for rate in (48000, 96000, 192000):
        L, _, said = render(patch, rate, 4)
        if said: return False, said[0]
        x = L[int(0.1 * rate):]
        P = [peak(x, rate, f_of(n), span=min(3.0, f_of(n) * 0.01)) for n in range(1, n_max + 1)]
        for n, (f, a) in enumerate(P, 1):
            if level_of(n) / level_of(1) < 0.05: continue
            worst_c = max(worst_c, abs(cents(f, f_of(n))))
            worst_db = max(worst_db, abs(20 * np.log10(a / P[0][1]) - 20 * np.log10(level_of(n) / level_of(1))))
    return worst_c < bound_c and worst_db < bound_db, 'worst %.2f cents, %.2f dB' % (worst_c, worst_db)

F0, TAU_STRIKE, HEAR = 110.0, 0.0005, 0.13
def sinc_of(f): return np.sinc(f * TAU_STRIKE)
def share_spread(n, at, w):
    """2 times the integral over the line of a Gaussian (area 1, sd w, at)
    times sin(n pi s): what of it lies past an end is gone."""
    s = np.linspace(max(0, at - 10 * w), min(1, at + 10 * w), 200001)
    g = np.exp(-0.5 * ((s - at) / w) ** 2) / (w * np.sqrt(2 * np.pi))
    return 2 * np.trapezoid(g * np.sin(np.pi * n * s), s)

def case_string_spread():
    """A string struck spread, δ(0.3, 0.02): each partial's level scaled by
    the Gaussian's transform, e^{-(n pi w)^2 / 2}, exactly."""
    lv = lambda n: abs(np.sin(np.pi * n * 0.3) * np.exp(-0.5 * (np.pi * n * 0.02) ** 2) * np.sin(np.pi * n * HEAR) *
                       sinc_of(n * F0) / (n * F0))
    return partials('string_spread.aither', lambda n: n * F0, lv)

def case_string_spread_end():
    """Struck spread near an end, δ(0.03, 0.02): what lies past the end is
    gone, each mode's share the integral over the line."""
    lv = lambda n: abs(share_spread(n, 0.03, 0.02) * np.sin(np.pi * n * HEAR) * sinc_of(n * F0) / (n * F0))
    return partials('string_spread_end.aither', lambda n: n * F0, lv)

def case_string_place():
    """Struck on the place's rate, $x' = $v + force * δ(0.3): a kick of the
    place, so a partial's level doesn't fall as 1 / w."""
    lv = lambda n: abs(np.sin(np.pi * n * 0.3) * np.sin(np.pi * n * HEAR) * sinc_of(n * F0))
    return partials('string_place.aither', lambda n: n * F0, lv)

def case_bar_pinned():
    """A bar, $v' = -ΔΔ$x, pinned at both ends (fixed), period 1 / 110: its
    first mode at 110 Hz and the rest at n^2 times it, the textbook's."""
    lv = lambda n: abs(np.sin(np.pi * n * 0.3) * np.sin(np.pi * n * HEAR) * sinc_of(n * n * F0) / (n * n * F0))
    return partials('bar_pinned.aither', lambda n: n * n * F0, lv, n_max=8)

def case_pair_partials():
    """Heaviside's line, v' = -∇i - v g, i' = -∇v - i g, v fixed, i free:
    partials at n * 110 Hz, struck on v's own rate, so levels as a kick of
    the field: the strike's and the pickup's shapes, no 1 / w."""
    lv = lambda n: abs(np.sin(np.pi * n * 0.3) * np.sin(np.pi * n * HEAR) * sinc_of(n * F0))
    return partials('pair_partials.aither', lambda n: n * F0, lv)

def case_pair_unequal():
    """The same with the losses apart (G 0.02, R 0.002 a round): each mode's
    frequency the exact one of its 2x2 law, sqrt(w^2 - ((G - R) / 2)^2)."""
    def f_of(n):
        w = 2 * np.pi * n * F0; d = 0.5 * (0.02 - 0.002) * F0
        return np.sqrt(w * w - d * d) / (2 * np.pi)
    lv = lambda n: abs(np.sin(np.pi * n * 0.3) * np.sin(np.pi * n * HEAR) * sinc_of(n * F0))
    return partials('pair_unequal.aither', f_of, lv, bound_db=1.0)

def case_sheet_modes():
    """A square sheet, fixed all round, each axis's period T (there and back
    along it, as a line's): modes at sqrt(m^2 + n^2) / T, here (1, 1) at 100
    Hz, struck at (0.31, 0.42), heard at (0.27, 0.61). Each frequency within 1
    cent, its level (the sum over the modes that share it) within 0.5 dB, at
    48, 96 and 192 kHz."""
    T = 1.4142135623730951 / 100
    sp, sq, hp, hq = 0.31, 0.42, 0.27, 0.61
    groups = {}
    for m_ in range(1, 6):
        for n_ in range(1, 6):
            f = np.sqrt(m_ * m_ + n_ * n_) / T
            amp = (np.sin(np.pi * m_ * sp) * np.sin(np.pi * n_ * sq) * np.sin(np.pi * m_ * hp) * np.sin(np.pi * n_ * hq) *
                   sinc_of(f) / f)
            key = m_ * m_ + n_ * n_
            groups[key] = (f, groups.get(key, (f, 0.0))[1] + amp)
    keys = sorted(groups)[:8]
    worst_c = worst_db = 0
    for rate in (48000, 96000, 192000):
        L, _, said = render('sheet_modes.aither', rate, 4)
        if said: return False, said[0]
        x = L[int(0.1 * rate):]
        ref = None
        for key in keys:
            f, amp = groups[key]
            if abs(amp) < 1e-9 * abs(groups[keys[0]][1]): continue
            g, a = peak(x, rate, f, span=2.0)
            if ref is None: ref = (a, abs(amp))
            if abs(amp) / ref[1] < 0.05: continue
            worst_c = max(worst_c, abs(cents(g, f)))
            worst_db = max(worst_db, abs(20 * np.log10(a / ref[0]) - 20 * np.log10(abs(amp) / ref[1])))
    return worst_c < 1 and worst_db < 0.5, 'worst %.2f cents, %.2f dB' % (worst_c, worst_db)

def case_didge_converges():
    """patches/field_didge.aither, its note at 48, 96 and 192 kHz: the same
    within 1 cent, as a model has one answer and the rate is how finely it
    is computed."""
    fs = []
    for rate in (48000, 96000, 192000):
        _, R, said = render(os.path.join('..', '..', 'patches', 'field_didge.aither'), rate, 2.5)
        if said: return False, said[0]
        fs.append(strongest(R[int(1.0 * rate):int(2.0 * rate)], rate, 40, 200))
    spread = cents(max(fs), min(fs))
    return spread < 1, ' '.join('%.3f' % f for f in fs) + ' Hz: %.1f cents apart' % spread

def wake(patch):
    """A van der Pol wake near the 3rd harmonic, coupled at 0.37 to a 110 Hz
    string: what it sings at 48, 96 and 192 kHz each within 1 cent of what it
    sings at 384 kHz, as the answer has to settle as the step shrinks (both
    ends agreeing isn't enough: a wake can miss the lock at both)."""
    rates = (48000, 96000, 192000, 384000)
    fs = []
    for rate in rates:
        _, q, said = render(patch, rate, 4)
        if said: return False, said[0]
        fs.append(strongest(q[int(2 * rate):], rate))
    off = max(abs(cents(f, fs[-1])) for f in fs[:-1])
    return off < 1, ' '.join('%.2f' % f for f in fs) + ' Hz at 48, 96, 192, 384 kHz: %.1f cents from the finest' % off

def case_wake_on_grid():
    return wake('grid_wake.aither')
case_wake_on_grid.__doc__ = 'On a grid string. ' + wake.__doc__

def case_wake_on_modes():
    return wake('modes_wake.aither')
case_wake_on_modes.__doc__ = 'On a string of 60 dho modes, through $state. ' + wake.__doc__

# -------------------------------------------------------------------- run

if __name__ == '__main__':
    cases = [(n[5:], f) for n, f in globals().items() if n.startswith('case_')]
    want = sys.argv[1:]
    if want: cases = [(n, f) for n, f in cases if any(w in n for w in want)]
    failed = 0
    for name, f in cases:
        ok, said = f()
        failed += not ok
        print('%s %-16s %s' % ('pass' if ok else 'FAIL', name, said), flush=True)
    print('%d of %d pass' % (len(cases) - failed, len(cases)))
    sys.exit(1 if failed else 0)
