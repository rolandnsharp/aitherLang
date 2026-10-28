## Spectral features, which are C (analysis.c), as the Nim tests see
## them until they are C too (notes/c-port-2026-10-28.md).

import std/os

{.compile: "analysis.c".}

const AnalysisH = currentSourcePath().parentDir / "analysis.h"

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

  CPeak* {.importc: "SpectralPeak", header: AnalysisH, bycopy.} = object
    freq_hz*, mag_db*: float64
  CSummary* {.importc: "SpectrumSummary", header: AnalysisH, bycopy.} = object
    sr*: float64
    peaks*: array[32, CPeak]
    npeaks*: cint
    centroid_hz*, rms_db*, peak_db*, zero_crossing_rate*, fundamental_hz*: float64

proc c_analyze(x: ptr float64; n: clong; sr: cdouble; npeaks: cint; o: ptr CSummary)
  {.importc: "analyze", header: AnalysisH.}
proc analysis_rms_db(x: ptr float64; n: clong): cdouble {.importc, header: AnalysisH.}
proc analysis_peak_db(x: ptr float64; n: clong): cdouble {.importc, header: AnalysisH.}
proc analysis_zero_crossing_rate(x: ptr float64; n: clong; sr: cdouble): cdouble
  {.importc, header: AnalysisH.}

template first(x: openArray[float64]): ptr float64 =
  (if x.len > 0: unsafeAddr x[0] else: nil)

proc analyze*(samples: openArray[float64]; sr: float64;
              nPeaks: int = 8): SpectrumSummary =
  var c: CSummary
  c_analyze(first(samples), clong(samples.len), sr, cint(nPeaks), addr c)
  result = SpectrumSummary(sr: c.sr, centroidHz: c.centroid_hz, rmsDb: c.rms_db,
                           peakDb: c.peak_db, zeroCrossingRate: c.zero_crossing_rate,
                           fundamentalHz: c.fundamental_hz)
  for i in 0 ..< int(c.npeaks):
    result.peaks.add SpectralPeak(freqHz: c.peaks[i].freq_hz, magDb: c.peaks[i].mag_db)

proc rmsDb*(samples: openArray[float64]): float64 =
  analysis_rms_db(first(samples), clong(samples.len))

proc peakDb*(samples: openArray[float64]): float64 =
  analysis_peak_db(first(samples), clong(samples.len))

proc zeroCrossingRate*(samples: openArray[float64]; sr: float64): float64 =
  analysis_zero_crossing_rate(first(samples), clong(samples.len), sr)
