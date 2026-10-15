"""fit_exact.py: vowel shapes for the aitherLang tube24, fitted on the
tube's exact state matrix (tube_model.py, which matches the render sample
for sample). Formants = the system's poles (eigenvalues): angle ->
frequency, radius -> bandwidth. No calibration needed."""
import numpy as np, sys
sys.path.insert(0, '/home/roland/voice-learn')
import tube_model as tm
from scipy.optimize import minimize
N, K, SR, LOSS = tm.N, tm.K, tm.SR, tm.LOSS
def statematrix(cp):
    s = tm.sections(cp); k = (s[:-1] - s[1:]) / (s[:-1] + s[1:])
    kG = (s[0] - 0.15) / (s[0] + 0.15); kL = (s[-1] - 25) / (s[-1] + 25)
    M = 2 * N + 1; A = np.zeros((M, M)); a = tm.A_LP
    A[0, N] = LOSS * kG
    for j in range(N - 1):
        A[j + 1, j] = LOSS * (1 + k[j]); A[j + 1, N + j + 1] = -LOSS * k[j]
        A[N + j, j] = LOSS * k[j]; A[N + j, N + j + 1] = LOSS * (1 - k[j])
    A[2 * N - 1, 2 * N] = LOSS * kL * (1 - a); A[2 * N - 1, N - 1] = LOSS * kL * a
    A[2 * N, 2 * N] = 1 - a; A[2 * N, N - 1] = a
    return A
def formants(cp, n=3):
    p = np.linalg.eigvals(statematrix(cp))
    p = p[np.imag(p) > 0]
    f = np.angle(p) * SR / (2 * np.pi); bw = -np.log(np.abs(p) + 1e-12) * SR / np.pi
    keep = (f > 150) & (f < 4800) & (bw < 600)
    f = np.sort(f[keep])
    return np.concatenate([f[:n], np.full(max(0, n - len(f[:n])), 5000.0)])
TARGET = {'IY': (359, 2381, 2929), 'IH': (448, 1953, 2641), 'EH': (603, 1774, 2586), 'AE': (634, 1854, 2530),
 'AA': (774, 1350, 2434), 'AO': (679, 1061, 2436), 'UH': (480, 1227, 2432), 'UW': (383, 976, 2373),
 'AH': (618, 1274, 2520), 'ER': (457, 1383, 1705), 'AX': (500, 1500, 2500),
 'EYs': (481, 2083, 2647), 'EYe': (391, 2249, 2677), 'OWs': (526, 964, 2427), 'OWe': (425, 875, 2435)}
LO = np.log(np.array([0.3] + [0.15] * (K - 1))); HI = np.log(np.array([2.5] + [9.0] * (K - 1)))
def fit(tgt):
    tgt = np.array(tgt, float)
    def cost(la):
        A = np.exp(np.clip(la, LO, HI)); F = formants(A)
        return np.sum(np.log(F / tgt) ** 2) + np.sum(np.diff(la) ** 2) * 0.004 + np.sum((la - np.log(2.5)) ** 2) * 0.0008
    best = None
    for seed in range(6):
        x0 = np.log(2.5) + np.random.default_rng(seed).normal(0, 0.6, K)
        r = minimize(cost, x0, method='Nelder-Mead', options={'maxiter': 3000, 'xatol': 1e-4, 'fatol': 1e-9})
        if best is None or r.fun < best.fun: best = r
    A = np.exp(np.clip(best.x, LO, HI)); return A, formants(A)
def job(item):
    v, t = item; A, F = fit(t); return v, t, A, F
if __name__ == '__main__':
    from multiprocessing import Pool
    with Pool(7) as pool, open('/tmp/va24x.txt', 'w') as out:
        for v, t, A, F in pool.imap(job, TARGET.items()):
            out.write(v + ' ' + ' '.join('%.2f' % a for a in A) + '\n')
            print(f'{v:4s} F {F[0]:4.0f} {F[1]:5.0f} {F[2]:5.0f}  target {t[0]} {t[1]} {t[2]}', flush=True)
