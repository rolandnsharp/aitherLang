"""gen_trombone.py: writes the aitherLang def `trombone`, a translation of
the tract in Pink Trombone 1.1 (c) 2017 Neil Thapen, MIT licence (see
notes/voice-eval/third_party/). 44 sections run twice per audio sample, a
28-section nose joins at section 17 through a 3-way junction, turbulence
enters where the tract is narrow but open, and a closure's release
injects a decaying transient. Our part: the shape comes from 8 control
points (glottis -> lips, cm^2) and two gestures, as in tube24."""
import sys
LIGHT = '--light' in sys.argv
# full: 48 sections run twice a sample (0.365 cm each; Pink Trombone: 44, a 16 cm tract)
# light: 24 sections once a sample (0.73 cm each), a quarter of the work, for live use
N, NL, K = (24, 16, 8) if LIGHT else (48, 31, 8)
STEPS = 1 if LIGHT else 2
NS = N - NL + 1                                    # nose start (17)
P = []
args = ', '.join(f'p{i}' for i in range(1, K + 1))
P.append('# Translated from Pink Trombone 1.1 by Neil Thapen (MIT licence):')
P.append('#   Copyright 2017 Neil Thapen. Permission is hereby granted, free of charge, to any person')
P.append('#   obtaining a copy of this software ... (full notice: notes/voice-eval/third_party/pinktrombone-1.1.html)')
P.append(f'def trombone(glot, turb, relT, place, deg, place2, deg2, velum, {args}):')
P.append('  # the shape: control points -> 44 diameters (Pink Trombone units, ~0.75 sqrt(cm^2))')
for j in range(N):
    x = j * (K - 1) / (N - 1) + 1
    lo = int(x); hi = min(K, lo + 1); fr = x - lo
    base = f'p{lo}' if fr < 1e-9 else f'(p{lo} * {1 - fr:.4f} + p{hi} * {fr:.4f})'
    P.append(f'  let a{j} = max(0.0004, {base} * (1 - deg * exp(-({x:.4f} - place) * ({x:.4f} - place) * 3)) * (1 - deg2 * exp(-({x:.4f} - place2) * ({x:.4f} - place2) * 3)) * 0.5625)')
for j in range(1, N):
    P.append(f'  let r{j} = (a{j-1} - a{j}) / (a{j-1} + a{j})')
# nose: fixed diameters, velum opening at its first section
nd = []
for i in range(NL):
    d = 2 * (i / NL); dia = 0.4 + 1.6 * d if d < 1 else 0.5 + 1.5 * (2 - d); nd.append(min(dia, 1.9))
P.append('  let nA0 = velum * velum')
for i in range(1, NL):
    a0 = 'nA0' if i == 1 else f'{nd[i-1] ** 2:.4f}'
    P.append(f'  let nr{i} = ({a0} - {nd[i] ** 2:.4f}) / ({a0} + {nd[i] ** 2:.4f})')
P.append(f'  let jsum = a{NS} + a{NS+1} + nA0')
P.append(f'  let rjL = (2 * a{NS} - jsum) / jsum')
P.append(f'  let rjR = (2 * a{NS+1} - jsum) / jsum')
P.append('  let rjN = (2 * nA0 - jsum) / jsum')
P.append('  # turbulence where the tract is narrow but open (thinness x openness), at the constriction')
P.append(f'  let cidx = min({N - 1}, (place - 1) / {K - 1} * {N - 1})          # section index of the gesture')
for j in range(N):
    P.append(f'  let h{j} = max(0, 1 - abs({j} - cidx))')
num = ' + '.join(f'a{j} * h{j}' for j in range(N)); den = ' + '.join(f'h{j}' for j in range(N))
P.append(f'  let dcon = sqrt(({num}) / max(0.001, {den}))       # the diameter at the gesture')
P.append('  let thin = clamp(8 * (0.7 - dcon), 0, 1)')
P.append('  let opn  = clamp(30 * (dcon - 0.3), 0, 1)')
P.append('  let fz   = turb * thin * opn * 0.66 * clamp((deg - 0.5) * 4, 0, 1)')
P.append('  let tr   = discharge(relT, 138.6) * 0.3                 # a release: 0.3 * 2^(-200 t)')
# state
for j in range(N): P.append(f'  $R{j} = 0.0'); P.append(f'  $L{j} = 0.0')
for i in range(NL): P.append(f'  $nR{i} = 0.0'); P.append(f'  $nL{i} = 0.0')
def step(src, R, L, nR, nL, suf):
    """one tract step: wave names in (R, L, nR, nL) -> new names with suffix"""
    Q = []
    # transients and turbulence are added into this step's waves first
    for j in range(N):
        Q.append(f'  let iR{j}{suf} = {R(j)} + (fz + tr) * h{j} * 0.5')
        Q.append(f'  let iL{j}{suf} = {L(j)} + (fz + tr) * h{j} * 0.5')
    Q.append(f'  let jR0{suf} = iL0{suf} * 0.75 + {src}')
    Q.append(f'  let jL{N}{suf} = iR{N-1}{suf} * -0.85')
    for j in range(1, N):
        if j == NS: continue
        Q.append(f'  let w{j}{suf} = r{j} * (iR{j-1}{suf} + iL{j}{suf})')
        Q.append(f'  let jR{j}{suf} = iR{j-1}{suf} - w{j}{suf}')
        Q.append(f'  let jL{j}{suf} = iL{j}{suf} + w{j}{suf}')
    j = NS
    Q.append(f'  let jL{j}{suf} = rjL * iR{j-1}{suf} + (1 + rjL) * ({nL(0)} + iL{j}{suf})')
    Q.append(f'  let jR{j}{suf} = rjR * iL{j}{suf} + (1 + rjR) * (iR{j-1}{suf} + {nL(0)})')
    Q.append(f'  let njR0{suf} = rjN * {nL(0)} + (1 + rjN) * (iL{j}{suf} + iR{j-1}{suf})')
    for j in range(N):
        Q.append(f'  let oR{j}{suf} = clamp(jR{j}{suf} * 0.999, -20, 20)')
        Q.append(f'  let oL{j}{suf} = clamp(jL{j+1}{suf} * 0.999, -20, 20)')
    Q.append(f'  let njL{NL}{suf} = {nR(NL-1)} * -0.85')
    for i in range(1, NL):
        Q.append(f'  let nw{i}{suf} = nr{i} * ({nR(i-1)} + {nL(i)})')
        Q.append(f'  let njR{i}{suf} = {nR(i-1)} - nw{i}{suf}')
        Q.append(f'  let njL{i}{suf} = {nL(i)} + nw{i}{suf}')
    for i in range(NL):
        Q.append(f'  let onR{i}{suf} = clamp(njR{i}{suf}, -20, 20)')
        Q.append(f'  let onL{i}{suf} = clamp(njL{i+1}{suf}, -20, 20)')
    return Q
P += step('glot', lambda j: f'$R{j}', lambda j: f'$L{j}', lambda i: f'$nR{i}', lambda i: f'$nL{i}', 'a')
last = 'a'
if STEPS == 2:
    P += step('glot', lambda j: f'oR{j}a', lambda j: f'oL{j}a', lambda i: f'onR{i}a', lambda i: f'onL{i}a', 'b'); last = 'b'
for j in range(N): P.append(f'  $R{j} = oR{j}{last}'); P.append(f'  $L{j} = oL{j}{last}')
for i in range(NL): P.append(f'  $nR{i} = onR{i}{last}'); P.append(f'  $nL{i} = onL{i}{last}')
if STEPS == 2:
    P.append(f'  (oR{N-1}a + onR{NL-1}a + oR{N-1}b + onR{NL-1}b) * 0.125')
else:
    P.append(f'  (oR{N-1}a + onR{NL-1}a) * 0.25')
print('\n'.join(P))
