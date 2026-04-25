## Pure spectral feature extraction. No engine, no CLI, no I/O.
##
## Input: a real-valued audio buffer + sample rate.
## Output: a SpectrumSummary (top peaks, centroid, RMS/peak in dB, ZCR,
## fundamental Hz). cli_output.formatSpectrum renders it.
##
## Implementation: Cooley-Tukey radix-2 FFT (no external deps), Hann
## window, biased autocorrelation for fundamental. Designed to be
## copyable as a single file and fed any seq[float64].

import std/[math, algorithm]

type
  SpectralPeak* = object
    freqHz*: float64
    magDb*: float64

  SpectrumSummary* = object
    sr*: float64
    peaks*: seq[SpectralPeak]      # top N, descending magnitude
    centroidHz*: float64
    rmsDb*: float64
    peakDb*: float64
    zeroCrossingRate*: float64     # crossings per second
    fundamentalHz*: float64        # 0.0 if no clear pitch

# ---------------------------------------------------------------- helpers

proc nextPow2(n: int): int =
  result = 1
  while result < n: result *= 2

proc hannWindow*(n: int): seq[float64] =
  result.setLen(n)
  if n <= 1:
    if n == 1: result[0] = 1.0
    return
  let denom = float64(n - 1)
  for i in 0 ..< n:
    result[i] = 0.5 * (1.0 - cos(2.0 * PI * float64(i) / denom))

proc dbOf(linear: float64): float64 {.inline.} =
  if linear < 1e-12: -240.0 else: 20.0 * log10(linear)

# ----------------------------------------------------------------- FFT

# Iterative radix-2 Cooley-Tukey on packed (re, im) parallel arrays.
# Bit-reverse permute, then log2(n) butterfly passes. Both arrays
# rewritten in place. n must be a power of two.
proc fftInPlace(re, im: var seq[float64]) =
  let n = re.len
  doAssert n == im.len
  doAssert (n and (n - 1)) == 0, "fftInPlace requires power-of-two length"
  if n <= 1: return

  # Bit-reverse permutation.
  var j = 0
  for i in 1 ..< n - 1:
    var bit = n shr 1
    while (j and bit) != 0:
      j = j xor bit
      bit = bit shr 1
    j = j or bit
    if i < j:
      swap(re[i], re[j])
      swap(im[i], im[j])

  var size = 2
  while size <= n:
    let half = size shr 1
    let theta = -2.0 * PI / float64(size)
    let wReStep = cos(theta)
    let wImStep = sin(theta)
    var k = 0
    while k < n:
      var wRe = 1.0
      var wIm = 0.0
      for jj in 0 ..< half:
        let ti = k + jj + half
        let bi = k + jj
        let tRe = wRe * re[ti] - wIm * im[ti]
        let tIm = wRe * im[ti] + wIm * re[ti]
        re[ti] = re[bi] - tRe
        im[ti] = im[bi] - tIm
        re[bi] = re[bi] + tRe
        im[bi] = im[bi] + tIm
        let nwRe = wRe * wReStep - wIm * wImStep
        let nwIm = wRe * wImStep + wIm * wReStep
        wRe = nwRe
        wIm = nwIm
      k += size
    size = size shl 1

proc fftMag*(samples: openArray[float64]): seq[float64] =
  ## Hann-windowed magnitude spectrum, length = nextPow2(samples.len)/2 + 1.
  ## Caller does the freq-bin math via `bin * sr / fftLen`.
  let nIn = samples.len
  if nIn == 0: return @[]
  let nFft = nextPow2(nIn)
  let win = hannWindow(nIn)
  var re = newSeq[float64](nFft)
  var im = newSeq[float64](nFft)
  for i in 0 ..< nIn:
    re[i] = samples[i] * win[i]
  fftInPlace(re, im)
  let outLen = nFft div 2 + 1
  result.setLen(outLen)
  for k in 0 ..< outLen:
    result[k] = sqrt(re[k] * re[k] + im[k] * im[k])

# ---------------------------------------------------------------- features

proc centroidHz*(magSpec: openArray[float64]; sr: float64): float64 =
  ## Magnitude-weighted mean frequency. magSpec is length nFft/2+1.
  if magSpec.len < 2: return 0.0
  let nFft = (magSpec.len - 1) * 2
  var num = 0.0
  var den = 0.0
  for k in 0 ..< magSpec.len:
    let freq = float64(k) * sr / float64(nFft)
    num += freq * magSpec[k]
    den += magSpec[k]
  if den < 1e-12: 0.0 else: num / den

proc topPeaks*(magSpec: openArray[float64]; sr: float64;
               n: int): seq[SpectralPeak] =
  ## Top-N local maxima above a -60 dB floor. Suppresses sub-peaks
  ## within ±3 bins of an already-found peak.
  if magSpec.len < 3 or n <= 0: return @[]
  let nFft = (magSpec.len - 1) * 2
  var maxMag = 0.0
  for v in magSpec:
    if v > maxMag: maxMag = v
  if maxMag < 1e-9: return @[]
  let floorMag = maxMag * pow(10.0, -60.0 / 20.0)

  type Cand = tuple[bin: int; mag: float64]
  var cands: seq[Cand]
  for k in 1 ..< magSpec.len - 1:
    if magSpec[k] >= magSpec[k - 1] and magSpec[k] >= magSpec[k + 1] and
       magSpec[k] >= floorMag:
      cands.add (k, magSpec[k])
  cands.sort(proc(a, b: Cand): int =
    if a.mag > b.mag: -1 elif a.mag < b.mag: 1 else: 0)

  for c in cands:
    if result.len >= n: break
    var clash = false
    for p in result:
      let pBin = int(p.freqHz * float64(nFft) / sr + 0.5)
      if abs(c.bin - pBin) < 3:
        clash = true; break
    if clash: continue
    # Parabolic interpolation for sub-bin freq accuracy.
    let y0 = magSpec[c.bin - 1]
    let y1 = magSpec[c.bin]
    let y2 = magSpec[c.bin + 1]
    let denom = y0 - 2.0 * y1 + y2
    let offset =
      if abs(denom) < 1e-12: 0.0
      else: 0.5 * (y0 - y2) / denom
    let interpBin = float64(c.bin) + offset
    let freq = interpBin * sr / float64(nFft)
    result.add SpectralPeak(freqHz: freq, magDb: dbOf(y1 / maxMag))

proc rmsDb*(samples: openArray[float64]): float64 =
  if samples.len == 0: return -240.0
  var sumSq = 0.0
  for v in samples: sumSq += v * v
  dbOf(sqrt(sumSq / float64(samples.len)))

proc peakDb*(samples: openArray[float64]): float64 =
  if samples.len == 0: return -240.0
  var p = 0.0
  for v in samples:
    let a = abs(v)
    if a > p: p = a
  dbOf(p)

proc zeroCrossingRate*(samples: openArray[float64]; sr: float64): float64 =
  if samples.len < 2: return 0.0
  var prev = samples[0]
  var crossings = 0
  for i in 1 ..< samples.len:
    let cur = samples[i]
    if (prev <= 0.0 and cur > 0.0) or (prev >= 0.0 and cur < 0.0):
      inc crossings
    prev = cur
  float64(crossings) * sr / float64(samples.len)

proc estimateFundamental*(samples: openArray[float64];
                          sr: float64): float64 =
  ## Normalised autocorrelation over lag range 40 Hz – 4 kHz. Returns
  ## 0.0 when no lag yields a clear peak — drum/noise mixes have only
  ## smoothly-decaying autocorr and shouldn't masquerade as pitched.
  ##
  ## Algorithm: compute r[lag] / (n - lag) (unbiased), pick the lag
  ## whose value is a local maximum AND ≥ 0.4 of r[0]/(n) (the
  ## zero-lag normalised energy). Without the local-max requirement,
  ## broadband content trivially wins at the smallest lag because the
  ## signal is highly correlated with a 1-sample shift of itself.
  if samples.len < 64: return 0.0
  let minLag = max(2, int(sr / 4000.0))
  let maxLag = min(samples.len div 2, int(sr / 40.0))
  if minLag >= maxLag - 1: return 0.0

  template autocorr(lag: int): float64 =
    block:
      let nLag = samples.len - lag
      if nLag <= 0: 0.0
      else:
        var s = 0.0
        for i in 0 ..< nLag: s += samples[i] * samples[i + lag]
        s / float64(nLag)

  let r0 = autocorr(0)
  if r0 < 1e-12: return 0.0
  let threshold = 0.4 * r0

  # Walk lags; return the FIRST local maximum that crosses the
  # threshold. The first peak corresponds to the period itself
  # (subsequent peaks are integer multiples). Without the local-max
  # requirement, broadband content trivially wins at the smallest
  # lag because the signal correlates with a 1-sample shift of itself.
  var prev = autocorr(minLag)
  var cur = autocorr(minLag + 1)
  var lag = minLag + 1
  while lag < maxLag:
    let nxt = autocorr(lag + 1)
    if cur > prev and cur > nxt and cur >= threshold:
      let denom = prev - 2.0 * cur + nxt
      let offset =
        if abs(denom) < 1e-12: 0.0
        else: 0.5 * (prev - nxt) / denom
      return sr / (float64(lag) + offset)
    prev = cur
    cur = nxt
    lag += 1
  0.0

proc analyze*(samples: openArray[float64]; sr: float64;
              nPeaks: int = 8): SpectrumSummary =
  let mag = fftMag(samples)
  result.sr = sr
  result.peaks = topPeaks(mag, sr, nPeaks)
  result.centroidHz = centroidHz(mag, sr)
  result.rmsDb = rmsDb(samples)
  result.peakDb = peakDb(samples)
  result.zeroCrossingRate = zeroCrossingRate(samples, sr)
  result.fundamentalHz = estimateFundamental(samples, sr)
