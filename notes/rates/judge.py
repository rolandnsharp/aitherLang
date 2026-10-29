# For each patch: differences between renders at 44.1/88.2/176.4 kHz.
# pitch: cents of the strongest peak; shape: rms dB difference of level-normalised third-octave bands
# (bands within 40 dB of the loudest); level: overall dB.
# conv = d(88,176) / d(44,88): about 0.25 for second-order step error, 0.5 first order, ~1 for a law written per sample.
import sys, math, collections
F = collections.defaultdict(dict)
for l in open(sys.argv[1]):
    w = l.split()
    if len(w) < 5 or w[2] == 'FAIL': continue
    F[w[0]][int(w[1])] = (float(w[2]), float(w[3]), [float(x) for x in w[4:]])
def shape(a, b):
    top = max(max(a), max(b)); idx = [i for i in range(25) if max(a[i], b[i]) > top - 40]
    ma = sum(a[i] for i in idx) / len(idx); mb = sum(b[i] for i in idx) / len(idx)   # level against the bands with sound only
    return (sum((a[i] - ma - b[i] + mb) ** 2 for i in idx) / len(idx)) ** 0.5
rows = []
for p, d in F.items():
    if len(d) < 3: continue
    A, B, C = d[44100], d[88200], d[176400]
    if min(A[0], B[0], C[0]) < -70 or min(A[1], B[1], C[1]) <= 0: continue   # silent without MIDI
    c1 = 1200 * math.log2(A[1] / C[1]); c2 = 1200 * math.log2(B[1] / C[1])
    s1, s2 = shape(A[2], B[2]), shape(B[2], C[2])
    l1, l2 = A[0] - B[0], B[0] - C[0]
    rows.append((p, c1, c2, s1, s2, l1, l2))
rows.sort(key=lambda r: -max(abs(r[1]) if abs(r[1]) < 100 else 0, r[3] * 5))
print(f"{'patch':42s} {'pitch c 44/88 vs 176':>22s} {'shape dB 44-88, 88-176':>24s} {'level dB 44-88, 88-176':>24s}")
for p, c1, c2, s1, s2, l1, l2 in rows:
    pc = "peak moved" if abs(c1) > 100 or abs(c2) > 100 else f"{c1:+7.1f} {c2:+7.1f}"
    print(f"{p:42s} {pc:>22s} {s1:11.2f} {s2:11.2f} {l1:+11.2f} {l2:+11.2f}")
print(f"\n{len(rows)} patches compared")
