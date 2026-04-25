## Pure spectral feature tests for analysis.nim. Inputs are synthetic
## buffers (sine, two-tone, sum-of-sines saw, deterministic noise) so
## the targets are math-exact and survive future FFT tweaks. No engine,
## no patches, no I/O.

import std/[math]
import ../analysis

const SR = 48000.0

proc sineBuf(freq: float64; n: int; sr: float64; amp = 1.0): seq[float64] =
  result.setLen(n)
  for i in 0 ..< n:
    result[i] = amp * sin(2.0 * PI * freq * float64(i) / sr)

proc lcgNoise(n: int; seed: uint64 = 0x1234567ABCDEF'u64): seq[float64] =
  ## Deterministic [-1, 1] white-noise via xorshift64*. Same seed each
  ## run, so the test is reproducible.
  result.setLen(n)
  var s = seed
  for i in 0 ..< n:
    s = s xor (s shl 13)
    s = s xor (s shr 7)
    s = s xor (s shl 17)
    let x = float64(s and 0xFFFFFFFF'u64) / 4294967295.0
    result[i] = x * 2.0 - 1.0

# --- 1. Pure 440 Hz sine: top peak, centroid, fundamental all match.
block pureSine:
  let buf = sineBuf(440.0, 48000, SR)
  let s = analyze(buf, SR, nPeaks = 3)
  doAssert s.peaks.len > 0, "should detect at least one peak"
  doAssert abs(s.peaks[0].freqHz - 440.0) < 1.0,
    "top peak should be ~440 Hz, got " & $s.peaks[0].freqHz
  doAssert abs(s.centroidHz - 440.0) < 5.0,
    "centroid should be ~440 Hz, got " & $s.centroidHz
  doAssert abs(s.fundamentalHz - 440.0) < 1.0,
    "fundamental should be ~440 Hz, got " & $s.fundamentalHz
  # Sine RMS = amp/sqrt(2) = 0.707 → about -3 dB.
  doAssert abs(s.rmsDb - (-3.0)) < 1.0,
    "RMS dB ≈ -3 for unit sine, got " & $s.rmsDb

# --- 2. White noise: no clear pitch, centroid roughly sr/4.
block noise:
  let buf = lcgNoise(48000)
  let s = analyze(buf, SR)
  doAssert s.fundamentalHz == 0.0,
    "noise has no fundamental, got " & $s.fundamentalHz
  doAssert abs(s.centroidHz - SR / 4.0) < SR / 8.0,
    "noise centroid should be roughly sr/4, got " & $s.centroidHz

# --- 3. Two-tone (440 + 880, equal amp): top two peaks at those freqs,
# second peak within ~1 dB of the first.
block twoTone:
  let n = 48000
  var buf = sineBuf(440.0, n, SR, amp = 0.5)
  let buf2 = sineBuf(880.0, n, SR, amp = 0.5)
  for i in 0 ..< n: buf[i] += buf2[i]
  let s = analyze(buf, SR, nPeaks = 4)
  doAssert s.peaks.len >= 2, "should detect both tones"
  let f1 = s.peaks[0].freqHz
  let f2 = s.peaks[1].freqHz
  let lo = min(f1, f2); let hi = max(f1, f2)
  doAssert abs(lo - 440.0) < 2.0,
    "low peak should be ~440, got " & $lo
  doAssert abs(hi - 880.0) < 2.0,
    "high peak should be ~880, got " & $hi
  doAssert abs(s.peaks[0].magDb - s.peaks[1].magDb) < 1.0,
    "equal-amp tones should have equal-mag peaks: " &
    $s.peaks[0].magDb & " vs " & $s.peaks[1].magDb

# --- 4. Hand-built 1/n saw at 110 Hz: 3 strongest harmonics at
# 110/220/330 with magnitude falloff matching 1/n (within 2 dB).
block sawHarmonics:
  let n = 48000
  var buf = newSeq[float64](n)
  for h in 1 .. 8:
    let amp = 1.0 / float64(h) * 0.5      # leave headroom
    let f = 110.0 * float64(h)
    for i in 0 ..< n:
      buf[i] += amp * sin(2.0 * PI * f * float64(i) / SR)
  let s = analyze(buf, SR, nPeaks = 3)
  doAssert s.peaks.len >= 3, "should detect at least 3 harmonics"
  let f1 = s.peaks[0].freqHz
  doAssert abs(f1 - 110.0) < 2.0, "fundamental peak ~110, got " & $f1
  # Peak 2 should be near 220 (some adjacency, but the fundamental
  # dominates), peak 3 near 330.
  var saw220 = false
  var saw330 = false
  for p in s.peaks:
    if abs(p.freqHz - 220.0) < 3.0: saw220 = true
    if abs(p.freqHz - 330.0) < 3.0: saw330 = true
  doAssert saw220, "should find a peak near 220 Hz"
  doAssert saw330, "should find a peak near 330 Hz"
  # Magnitude falloff: 220 should be ~6 dB below 110 (1/2 = -6 dB).
  # Allow ±3 dB slack for FFT bin spread.
  for p in s.peaks:
    if abs(p.freqHz - 220.0) < 3.0:
      doAssert abs(p.magDb - (-6.0)) < 3.0,
        "220 Hz peak should be ~-6 dB rel to fundamental, got " & $p.magDb

# --- 5. ZCR sanity: 440 Hz sine → ~880 zero crossings/sec.
block zcrSanity:
  let buf = sineBuf(440.0, 48000, SR)
  let z = zeroCrossingRate(buf, SR)
  doAssert abs(z - 880.0) < 5.0,
    "440 Hz sine ZCR ≈ 880/sec, got " & $z

# --- 6. peakDb / rmsDb for a known scaled sine.
block dbCheck:
  let buf = sineBuf(440.0, 48000, SR, amp = 0.1)
  doAssert abs(peakDb(buf) - (-20.0)) < 0.5,
    "0.1-amp sine peak should be ~-20 dB, got " & $peakDb(buf)
  doAssert abs(rmsDb(buf) - (-23.0)) < 0.5,
    "0.1-amp sine RMS should be ~-23 dB, got " & $rmsDb(buf)

echo "analysis ok"
