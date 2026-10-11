"""learn_vowels.py bigdata.dat: speaker-neutral vowel targets and their
movement, from Hillenbrand et al. (1995): 139 talkers, 12 vowels, F1-F3
at 0-80% of each vowel. Each talker is scaled to a common vocal tract
(their geometric-mean formant -> the adult-male mean), then averaged."""
import sys, numpy as np, collections
rows = []
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) < 30 or not p[0][0] in 'mwbg' or not p[1].isdigit(): continue
    v = np.array([float(x) for x in p[3:30]]).reshape(9, 3)
    if (v == 0).any(): continue
    rows.append((p[0][0], p[0][:3], p[0][3:5], float(p[1]), v))
talk = collections.defaultdict(list)
for g, t, vw, d, v in rows: talk[t].append(v)
gm = {t: np.exp(np.mean(np.log(np.array(vs)))) for t, vs in talk.items()}
ref = np.mean([gm[t] for t in gm if t[0] == 'm'])
acc = collections.defaultdict(list); dur = collections.defaultdict(list)
for g, t, vw, d, v in rows:
    acc[vw].append(v * ref / gm[t]); dur[vw].append(d)
print(f'{len(rows)} tokens, {len(talk)} talkers, reference scale {ref:.0f} Hz')
names = {'iy': 'IY heed', 'ih': 'IH hid', 'eh': 'EH head', 'ae': 'AE had', 'ah': 'AA hod', 'aw': 'AO hawed',
         'oo': 'UH hood', 'uw': 'UW whod', 'uh': 'AH hud', 'er': 'ER heard', 'ei': 'EY haid', 'oa': 'OW boat'}
for vw in ['iy', 'ih', 'ei', 'eh', 'ae', 'ah', 'aw', 'oa', 'oo', 'uw', 'uh', 'er']:
    m = np.median(np.array(acc[vw]), axis=0)          # 9 x 3
    s, mid, e = m[1], m[5], m[8]                      # 10%, 50%, 80%
    print(f'{names[vw]:10s} dur {np.median(dur[vw]):4.0f}  start {s[0]:4.0f} {s[1]:5.0f} {s[2]:5.0f}  mid {mid[0]:4.0f} {mid[1]:5.0f} {mid[2]:5.0f}  end {e[0]:4.0f} {e[1]:5.0f} {e[2]:5.0f}')
