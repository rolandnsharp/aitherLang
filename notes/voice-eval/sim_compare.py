import sys, numpy as np
from scipy.io import wavfile
sys.path.insert(0, '/home/roland/voice-learn')
import tube_model as tm
sr, x = wavfile.read(sys.argv[1]); x = x.astype(float) / 32768 / float(sys.argv[3])
areas = {l.split()[0]: np.array([float(v) for v in l.split()[1:]]) for l in open(sys.argv[2])}
n = 'AX'; i = 10
st = int((i + 0.5) * sr)
st = st - 10 + int(np.argmax(np.abs(x[st - 10:st + 40]) > 1e-9))   # first non-zero sample
meas = x[st:st + 400]
s = tm.sections(areas[n]); N = tm.N; k = (s[:-1] - s[1:]) / (s[:-1] + s[1:])
kG = (s[0] - 0.15) / (s[0] + 0.15); kL = (s[-1] - 25) / (s[-1] + 25); a = tm.A_LP; L = tm.LOSS
f = np.zeros(N); b = np.zeros(N); l = 0.0; prev_out = 0.0; sim = []
for t in range(460):
    src = 1.0 if t == 0 else 0.0
    lipR = l + a * (f[-1] - l)
    out = f[-1] + kL * lipR
    nf = np.zeros(N); nb = np.zeros(N)
    nf[0] = L * (src + kG * b[0])
    nf[1:] = L * ((1 + k) * f[:-1] - k * b[1:])
    nb[:-1] = L * (k * f[:-1] + (1 - k) * b[1:])
    nb[-1] = L * kL * lipR
    sim.append(out - prev_out); prev_out = out; f, b, l = nf, nb, lipR
sim = np.array(sim)
# align: find the lag that best matches (aither may be 1-2 samples apart)
first = int(np.argmax(np.abs(sim) > 1e-12)); print('simulated wave reaches the lips after', first, 'samples')
s1 = sim[first:first + 380]; m1 = meas[:380]; best = first
print('lag', best, 'gain ratio', np.dot(m1, s1) / np.dot(s1, s1))
print('first samples meas:', ' '.join('%+.3f' % v for v in m1[:14] / np.abs(m1).max()))
print('first samples sim :', ' '.join('%+.3f' % v for v in s1[:14] / np.abs(s1).max()))
c = np.corrcoef(m1, s1)[0, 1]; print('correlation over 380 samples: %.3f' % c)
