"""gen_tube.py: writes the aitherLang def `tube8`, an 8-region
Kelly-Lochbaum vocal tract. Each region is a pair of delay lines (forward
and backward pressure waves); at each boundary the waves scatter with
k = (A_i - A_i+1) / (A_i + A_i+1). The wave state is kept in $vars, so each
region's delay is a sample short of its length (the $ adds the sample)."""
N = 8
args = ', '.join([f'a{i}' for i in range(1, N + 1)])
L = []
L.append(f'def tube8(src, noiz, place, {args}, tlen):')
L.append('  # tlen: the tract scale (1 = 17 cm). One region is 1/8 of it.')
L.append('  # aither\'s delay() truncates to whole samples, so each region reads two')
L.append('  # neighbouring whole-sample taps and crossfades (linear interpolation).')
L.append('  let rs = max(0, 2.125 / 35000 * tlen * sr - 1)')
L.append('  let rn = floor(rs)')
L.append('  let rf = rs - rn')
L.append('  let rt0 = (rn + 0.5) / sr')
L.append('  let rt1 = (rn + 1.5) / sr')
for i in range(1, N + 1):
    L.append(f'  $f{i} = 0.0')
    L.append(f'  $b{i} = 0.0')
for i in range(1, N + 1):
    L.append(f'  let fo{i} = (delay($f{i}, rt0, 0.001) * (1 - rf) + delay($f{i}, rt1, 0.001) * rf) * 0.9985')
    L.append(f'  let bo{i} = (delay($b{i}, rt0, 0.001) * (1 - rf) + delay($b{i}, rt1, 0.001) * rf) * 0.9985')
for i in range(1, N):
    L.append(f'  let k{i} = (a{i} - a{i+1}) / (a{i} + a{i+1})')
L.append('  # frication enters at the narrowest place (1..8), shaped by the tube in front of it')
for i in range(1, N):
    L.append(f'  let n{i} = noiz * exp(-(place - {i}.5) * (place - {i}.5) * 2)')
L.append('  $f1 = src + 0.97 * bo1')
for i in range(1, N):
    L.append(f'  $f{i+1} = (1 + k{i}) * fo{i} - k{i} * bo{i+1} + n{i}')
    L.append(f'  $b{i} = k{i} * fo{i} + (1 - k{i}) * bo{i+1}')
L.append('  let lipR = lp1(fo8, 5000)')
L.append('  $b8 = -0.85 * lipR')
L.append('  let out = fo8 - 0.85 * lipR')
L.append('  out - prev(out)')
print('\n'.join(L))
