"""gen_tube24.py: the aitherLang def `tube24`, a 24-section Kelly-Lochbaum
vocal tract, each section exactly one sample (0.73 cm at 48 kHz: 17.5 cm
in all), its waves held in $vars. Areas come in as 8 control points
(glottis -> lips) and are interpolated smoothly to the 24 sections, so the
vowel tables and gestures stay small; the gesture adds its own narrowing
per section."""
N, K = 24, 8
L = []
args = ', '.join(f'p{i}' for i in range(1, K + 1))
L.append(f'def tube24(src, noiz, place, deg, {args}):')
L.append('  # place: where the constriction is (1 glottis .. 8 lips, in control-point units); deg: 0 open .. 1 closed')
for j in range(N):
    x = j * (K - 1) / (N - 1) + 1                    # control-point coordinate of section j
    lo = int(x); hi = min(K, lo + 1); fr = x - lo
    base = f'p{lo}' if fr < 1e-9 else f'(p{lo} * {1 - fr:.4f} + p{hi} * {fr:.4f})'
    L.append(f'  let s{j} = max(0.02, {base} * (1 - deg * exp(-({x:.4f} - place) * ({x:.4f} - place) * 3)))')
for j in range(N):
    L.append(f'  $f{j} = 0.0'); L.append(f'  $b{j} = 0.0')
for j in range(N - 1):
    L.append(f'  let k{j} = (s{j} - s{j+1}) / (s{j} + s{j+1})')
for j in range(N - 1):
    x = j * (K - 1) / (N - 1) + 1
    L.append(f'  let n{j} = noiz * exp(-({x:.4f} - place) * ({x:.4f} - place) * 6)')
# all new waves computed from last sample's waves (one sample per section)
L.append('  # the ends are area junctions too: a narrow glottal slit, and the open air past the lips')
L.append('  let kG = (s0 - 0.15) / (s0 + 0.15)')
L.append('  let g0 = src + kG * $b0')
for j in range(N - 1):
    L.append(f'  let fw{j+1} = (1 + k{j}) * $f{j} - k{j} * $b{j+1} + n{j}')
    L.append(f'  let bw{j} = k{j} * $f{j} + (1 - k{j}) * $b{j+1}')
L.append(f'  let kL = (s{N-1} - 25) / (s{N-1} + 25)')
L.append(f'  let lipR = lp1($f{N-1}, 4000)')
L.append(f'  let bwL = kL * lipR')
L.append(f'  let out = $f{N-1} + bwL')
L.append('  $f0 = clamp(g0 * 0.996, -50, 50)')
for j in range(1, N):
    L.append(f'  $f{j} = clamp(fw{j} * 0.996, -50, 50)')
for j in range(N - 1):
    L.append(f'  $b{j} = clamp(bw{j} * 0.996, -50, 50)')
L.append(f'  $b{N-1} = clamp(bwL * 0.996, -50, 50)')
L.append('  out - prev(out)')
print('\n'.join(L))
