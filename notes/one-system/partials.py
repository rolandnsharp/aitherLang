# Partials of a struck string against the exact answer: frequency error in
# cents and level against the first partial, at three rates, and run time.
import sys, os, subprocess, time, numpy as np
AI = os.environ.get('AI', '/home/roland/aitherLang/aither')
f0, at, hear, tau = 110.0, 0.3, 0.13, 0.0005
def render(patch, rate, sec):
    env = dict(os.environ, AITHER_RATE=str(rate))
    t = time.time()
    p = subprocess.run([AI, patch, str(sec)], stdin=subprocess.DEVNULL, capture_output=True, env=env)
    a = np.frombuffer(p.stdout, dtype=np.float32)
    return a[0::2].astype(np.float64), time.time() - t
def peaks(x, rate, n_max):
    seg = x[int(0.1 * rate):]
    w = np.hanning(len(seg)); pad = 4
    S = np.abs(np.fft.rfft(seg * w, len(seg) * pad)); df = rate / (len(seg) * pad)
    out = []
    for n in range(1, n_max + 1):
        k = int(round(n * f0 / df)); lo, hi = k - int(3 / df), k + int(3 / df)
        i = lo + int(np.argmax(S[lo:hi]))
        a, b, c = np.log(S[i - 1:i + 2] + 1e-30); d = 0.5 * (a - c) / (a - 2 * b + c)
        out.append(((i + d) * df, np.exp(b - 0.25 * (a - c) * d)))
    return out
def exact(n):        # struck for tau at `at`, heard at `hear`: x ~ shapes * sinc / w
    w = 2 * np.pi * f0 * n
    return abs(np.sin(np.pi * n * at) * np.sin(np.pi * n * hear) * np.sinc(w * tau / 2 / np.pi) / w)
N = 12
for patch in sys.argv[1:]:
    for rate in (48000, 96000, 192000):
        x, took = render(patch, rate, 4)
        _, took2 = render(patch, rate, 12)
        P = peaks(x, rate, N)
        cents = [1200 * np.log2(f / (n * f0)) for n, (f, _) in zip(range(1, N + 1), P)]
        lv = [20 * np.log10(a / P[0][1]) - 20 * np.log10(exact(n) / exact(1)) for n, (_, a) in zip(range(1, N + 1), P)]
        keep = [i for i in range(N) if exact(i + 1) / exact(1) > 0.05]     # partials not at a node
        print('%-20s %6d Hz  cents off, partials 1-%d: %s' % (os.path.basename(patch), rate, N, ' '.join('%+.2f' % c for c in cents)))
        print('%-20s %6s     level off (dB), away from nodes: %s   worst %.2f dB;  %.2f s of cpu a second' %
              ('', '', ' '.join('%+.1f' % lv[i] for i in keep), max(abs(lv[i]) for i in keep), (took2 - took) / 8))
