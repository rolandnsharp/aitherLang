import sys, re, numpy as np
from scipy.io import wavfile
sr, x = wavfile.read(sys.argv[1]); x = x.astype(float)
src = open(sys.argv[2]).read()
lyr = [int(v) for v in re.search(r'let lyr = \[(.*?)\]', src, re.S).group(1).replace('\n', ' ').split(',')]
IDS = 'sil IY IH EH AE AA AO UH UW AH ER AX M N NG L R W Y HH S SH F TH Z V DH ZH P T K B D G CH JH VB EY OW'.split()
slot = 60 / 108 / 8; res = {}
for k in range(int(len(x) / sr / slot)):
    bar = k // 32; sb = bar % 32; li = sb // 4
    if sb % 4 >= 3: continue
    s = (sb % 4) * 32 + k % 32; ph = IDS[lyr[li * 96 + s]]
    seg = x[int(k * slot * sr):int((k + 1) * slot * sr)]
    res.setdefault(ph, []).append((np.abs(seg).max(), np.sqrt((seg ** 2).mean())))
ref = np.median([r for p in ['AA', 'AE', 'EH', 'AO', 'AH'] for _, r in res.get(p, [])])
print('phoneme: median rms / max peak (dB re vowel rms)')
print(' '.join(f"{p}:{20*np.log10(np.median([r for _, r in v])/ref):+.0f}/{20*np.log10(max(pk for pk, _ in v)/ref):+.0f}" for p, v in sorted(res.items(), key=lambda kv: -max(pk for pk, _ in kv[1]))))
