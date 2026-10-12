"""tube_model.py: the exact frequency response of the aitherLang tube24
(state-space: 24 forward + 24 backward waves + the lip low-pass), so a fit
optimises what the patch actually plays. Mirrors gen_tube24.py:
 f0' = L(src + kG b0);  f[j+1]' = L((1+k) f[j] - k b[j+1]);  b[j]' = L(k f[j] + (1-k) b[j+1])
 lipR = (1-a) l + a f23;  b23' = L kL lipR;  out = f23 + kL lipR;  l' = lipR;  y = out - out_prev"""
import numpy as np
N, K, SR, LOSS = 24, 8, 48000.0, 0.996
F = np.linspace(60, 4500, 900)
z = np.exp(1j * 2 * np.pi * F / SR)
A_LP = 19580 / SR                      # aither lp1(x, 19580): a = cutoff / sr
def sections(cp):
    xs = np.arange(N) * (K - 1) / (N - 1)
    return np.maximum(0.02, np.interp(xs, np.arange(K), cp))
def response(cp):
    s = sections(cp); k = (s[:-1] - s[1:]) / (s[:-1] + s[1:])
    kG = (s[0] - 0.15) / (s[0] + 0.15); kL = (s[-1] - 25) / (s[-1] + 25)
    M = 2 * N + 1; A = np.zeros((M, M)); B = np.zeros(M); C = np.zeros(M)
    fi = lambda j: j; bi = lambda j: N + j; li = 2 * N
    A[fi(0), bi(0)] = LOSS * kG; B[fi(0)] = LOSS
    for j in range(N - 1):
        A[fi(j + 1), fi(j)] = LOSS * (1 + k[j]); A[fi(j + 1), bi(j + 1)] = -LOSS * k[j]
        A[bi(j), fi(j)] = LOSS * k[j]; A[bi(j), bi(j + 1)] = LOSS * (1 - k[j])
    A[bi(N - 1), li] = LOSS * kL * (1 - A_LP); A[bi(N - 1), fi(N - 1)] = LOSS * kL * A_LP
    A[li, li] = 1 - A_LP; A[li, fi(N - 1)] = A_LP
    C[fi(N - 1)] = 1 + kL * A_LP; C[li] = kL * (1 - A_LP)
    I = np.eye(M)
    H = np.array([C @ np.linalg.solve(zz * I - A, B) for zz in z])
    return np.abs(H * (1 - 1 / z))
def formants(cp, n=3):
    H = 20 * np.log10(response(cp) + 1e-12)
    i = np.nonzero((H[1:-1] > H[:-2]) & (H[1:-1] >= H[2:]) & (F[1:-1] > 150))[0][:n] + 1
    a, b, c = H[i - 1], H[i], H[i + 1]
    off = 0.5 * (a - c) / (a - 2 * b + c - 1e-12)
    out = F[i] + off * (F[1] - F[0])
    return np.concatenate([out, np.full(n - len(out), 5000.0)])
if __name__ == '__main__':
    import sys
    for line in open(sys.argv[1]):
        p = line.split(); cp = np.array([float(x) for x in p[1:]])
        fm = formants(cp)
        print(f'{p[0]:4s} model {fm[0]:5.0f} {fm[1]:5.0f} {fm[2]:5.0f}')
