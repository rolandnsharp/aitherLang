"""fit_tract.py: 8-region vocal tract areas for each vowel, fitted to the
learned formants (Hillenbrand 1995, tract-normalised). Model: a chain of
8 lossy cylinders (17 cm), closed-ish at the glottis, radiating at the
lips; formants = peaks of the volume-velocity transfer function."""
import numpy as np
from scipy.optimize import minimize
C, RHO, LEN, N = 35000.0, 0.00114, 17.0, 8      # cm/s, g/cm^3, cm, regions
f = np.linspace(80, 4200, 700)
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
    i = np.nonzero((H[1:-1] > H[:-2]) & (H[1:-1] >= H[2:]) & (f[1:-1] > 150))[0][:n] + 1
    a, b, c = H[i - 1], H[i], H[i + 1]                      # parabolic refinement between grid points
    off = 0.5 * (a - c) / (a - 2 * b + c - 1e-12)
    F = f[i] + off * (f[1] - f[0])
    return np.concatenate([F, np.full(n - len(F), 5000.0)])
TARGET = {   # mid-vowel F1 F2 F3 (learned)
 'IY': (359, 2381, 2929), 'IH': (448, 1953, 2641), 'EH': (603, 1774, 2586), 'AE': (634, 1854, 2530),
 'AA': (774, 1350, 2434), 'AO': (679, 1061, 2436), 'UH': (480, 1227, 2432), 'UW': (383, 976, 2373),
 'AH': (618, 1274, 2520), 'ER': (457, 1383, 1705), 'AX': (500, 1500, 2500),
 'EYs': (481, 2083, 2647), 'EYe': (391, 2249, 2677), 'OWs': (526, 964, 2427), 'OWe': (425, 875, 2435)}
neutral = np.full(N, 3.0)
# Calibrated against the aitherLang tube: its F1 lands ~10% above this model's,
# so aim F1 that much lower.
F1_CAL = 0.9
def fit(tgt):
    tgt = np.array(tgt, float) * np.array([F1_CAL, 1, 1])
    def cost(la):
        A = np.exp(np.clip(la, np.log(0.15), np.log(9.0))); F = formants(A)
        e = np.sum(np.log(F / tgt) ** 2)
        smooth = np.sum(np.diff(la) ** 2) * 0.004
        return e + smooth + np.sum((la - np.log(3.0)) ** 2) * 0.0008
    best = None
    for seed in range(5):
        rng = np.random.default_rng(seed)
        x0 = np.log(3.0) + rng.normal(0, 0.7, N)
        r = minimize(cost, x0, method='Nelder-Mead', options={'maxiter': 2500, 'xatol': 1e-4, 'fatol': 1e-8})
        if best is None or r.fun < best.fun: best = r
    A = np.exp(best.x); return A, formants(A)
def job(item):
    v, tgt = item; A, F = fit(tgt); return v, tgt, A, F
if __name__ == '__main__':
    from multiprocessing import Pool
    out = {}
    with Pool(7) as pool:
        for v, tgt, A, F in pool.imap(job, TARGET.items()):
            out[v] = np.clip(A, 0.15, 9.0)
            print(f'{v:4s} areas ' + ' '.join(f'{a:4.2f}' for a in out[v]) + f'   F {F[0]:4.0f} {F[1]:5.0f} {F[2]:5.0f}  target {tgt[0]} {tgt[1]} {tgt[2]}', flush=True)
    np.save('/tmp/vowel_areas.npy', out)
