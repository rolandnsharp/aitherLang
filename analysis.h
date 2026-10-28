/* Spectral features of a buffer: top peaks, centroid, RMS and peak in
 * dB, zero crossings, fundamental. No engine, no I/O. */

#ifndef AITHER_ANALYSIS_H
#define AITHER_ANALYSIS_H

#define ANALYSIS_MAX_PEAKS 32

typedef struct {
  double freq_hz;
  double mag_db;
} SpectralPeak;

typedef struct {
  double       sr;
  SpectralPeak peaks[ANALYSIS_MAX_PEAKS];   /* descending magnitude */
  int          npeaks;
  double       centroid_hz;
  double       rms_db;
  double       peak_db;
  double       zero_crossing_rate;          /* crossings per second */
  double       fundamental_hz;              /* 0: no clear pitch */
} SpectrumSummary;

/* The summary of `n` samples at `sr`, with up to `npeaks` peaks (8 is usual). */
void analyze(const double* samples, long n, double sr, int npeaks, SpectrumSummary* out);

/* Three of its features on their own. */
double analysis_rms_db(const double* x, long n);
double analysis_peak_db(const double* x, long n);
double analysis_zero_crossing_rate(const double* x, long n, double sr);

/* (L + R) / 2, in fresh storage. */
double* mono_mix(const double* l, const double* r, long n);

#endif
