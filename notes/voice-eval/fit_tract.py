"""fit_tract.py: 8-region vocal tract areas for each vowel, fitted to the
learned formants (Hillenbrand 1995, tract-normalised). Model: a chain of
8 lossy cylinders (17 cm), closed-ish at the glottis, radiating at the
lips; formants = peaks of the volume-velocity transfer function."""
import numpy as np
from scipy.optimize import minimize
C, RHO, LEN, N = 35000.0, 0.00114, 17.0, 8      # cm/s, g/cm^3, cm, regions
f = np.linspace(80, 4200, 1400)
def transfer(areas, length=LEN):
    w = 2 * np.pi * f; k = w / C * (1 - 0.006j)   # small wall/viscous loss
    l = length / N
    a, b, c, d = np.ones_like(f, complex), np.zeros_like(f, complex), np.zeros_like(f, complex), np.ones_like(f, complex)
    for A in areas:                                # glottis -> lips
        Z = RHO * C / A
        m = (np.cos(k * l), 1j * Z * np.sin(k * l), 1j * np.sin(k * l) / Z, np.cos(k * l))
        a, b, c, d = a * m[0] + b * m[2], a * m[1] + b * m[3], c * m[0] + d * m[2], c * m[1] + d * m[3]
    Al = areas[-1]; r = np.sqrt(Al / np.pi)
    Zr = RHO * C / Al * (0.25 * (k.real * r) ** 2 + 1j * 0.61 * k.real * r)   # piston radiation load
    return np.abs(1 / (c * Zr + d))                # U_lips / U_glottis
def formants(areas, n=3):
    H = 20 * np.log10(transfer(areas) + 1e-12)
    pk = [i for i in range(1, len(f) - 1) if H[i] > H[i - 1] and H[i] >= H[i + 1] and f[i] > 150]
    return np.array([f[i] for i in pk[:n]] + [5000] * max(0, n - len(pk[:n])))
TARGET = {   # mid-vowel F1 F2 F3 (learned)
 'IY': (359, 2381, 2929), 'IH': (448, 1953, 2641), 'EH': (603, 1774, 2586), 'AE': (634, 1854, 2530),
 'AA': (774, 1350, 2434), 'AO': (679, 1061, 2436), 'UH': (480, 1227, 2432), 'UW': (383, 976, 2373),
 'AH': (618, 1274, 2520), 'ER': (457, 1383, 1705), 'AX': (500, 1500, 2500),
 'EYs': (481, 2083, 2647), 'EYe': (391, 2249, 2677), 'OWs': (526, 964, 2427), 'OWe': (425, 875, 2435)}
neutral = np.full(N, 3.0)
def fit(tgt):
    tgt = np.array(tgt, float)
    def cost(la):
        A = np.exp(la); F = formants(A)
        e = np.sum(np.log(F / tgt) ** 2)
        smooth = np.sum(np.diff(la) ** 2) * 0.004
        return e + smooth + np.sum((la - np.log(3.0)) ** 2) * 0.0008
    best = None
    for seed in range(14):
        rng = np.random.default_rng(seed)
        x0 = np.log(3.0) + rng.normal(0, 0.7, N)
        r = minimize(cost, x0, method='Nelder-Mead', options={'maxiter': 5000, 'xatol': 1e-4, 'fatol': 1e-7})
        r = minimize(cost, r.x, method='Powell', bounds=[(np.log(0.15), np.log(9))] * N)
        if best is None or r.fun < best.fun: best = r
    A = np.exp(best.x); return A, formants(A)
out = {}
for v, tgt in TARGET.items():
    A, F = fit(tgt); out[v] = A
    print(f'{v:4s} areas ' + ' '.join(f'{a:4.2f}' for a in A) + f'   F {F[0]:4.0f} {F[1]:5.0f} {F[2]:5.0f}  target {tgt[0]} {tgt[1]} {tgt[2]}')
np.save('/tmp/vowel_areas.npy', out)
