/* fft: a signal into a stream of its spectrum.
     aither song.aither | tools/fft 1024 | aither spectral.aither | tools/ifft 1024 | tools/play
   Reads raw float32 pairs from stdin and writes each frame's bins to
   stdout as float32 pairs, one a sample, so the spectrum is a stream at
   the signal's rate and a patch reads it through input(). A frame is a
   row and a bin a pixel: with H = N / 2, pixel_index(H, 1) is the bin
   input() holds, 0 to H - 1, and an array stepped `every = H` steps once
   a frame. tools/fft.h has the frame, the window and the scale.

   The signal is one side: the sides' mean, or `left` or `right` after N.
   Stereo is two chains. The size is the patch's to choose and the only
   delay there is: tools/ifft gives the signal back N / 2 samples later,
   whatever the engine's block. When stdin ends, one more frame carries
   the last samples out. Exits when the pipe closes.

   Several streams, for a patch that hears one sound through another:
     tools/fft 4096 <(aither a.aither) <(aither b.aither) | aither cross.aither | ...
   With K files (`-` is stdin), each one's sides' mean, a frame holds K
   spectra one after another, H / K pixels each: pixel s * H / K + k is
   stream s's bin k, as [re, im], bin 0's im being 0. The pipe has room
   for one spectrum a frame, so K share it: each keeps its lowest H / K
   bins, up to a K-th of the half rate (11 kHz for two at 44.1 kHz).
   Exits when every stream has ended. Build: make tools/fft */
#include <string.h>
#include "fft.h"

enum { MAX_STREAMS = 8 };

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: tools/fft N [left|right]   (N a power of two: float32 pairs in, bins out)\n"
                    "       tools/fft N FILE FILE ...   (several streams' spectra, side by side in a frame)\n");
    return 2;
  }
  long n = frame_size("fft", argv[1]), h = n / 2;
  double wl = 0.5, wr = 0.5;
  FILE* in_f[MAX_STREAMS];
  int streams = 1;
  in_f[0] = stdin;
  if (argc == 3 && !strcmp(argv[2], "left")) wl = 1, wr = 0;
  else if (argc == 3 && !strcmp(argv[2], "right")) wl = 0, wr = 1;
  else if (argc >= 3) {
    streams = argc - 2;
    if (streams > MAX_STREAMS) { fprintf(stderr, "fft: %d streams at most\n", MAX_STREAMS); return 2; }
    if (h % streams || h / streams < 2) {
      fprintf(stderr, "fft: %ld bins a frame don't share out among %d streams: a power of two, and a bigger N\n", h, streams);
      return 2;
    }
    for (int s = 0; s < streams; s++) {
      in_f[s] = strcmp(argv[2 + s], "-") ? fopen(argv[2 + s], "rb") : stdin;
      if (!in_f[s]) { fprintf(stderr, "fft: can't open %s\n", argv[2 + s]); return 2; }
    }
  }
  long share = h / streams;                  /* each stream's bins a frame */
  double* win = sine_window(n);
  double *cs, *sn;
  fft_turns(n, &cs, &sn);
  double* x = calloc((size_t)streams * n, sizeof *x);   /* each stream's last N samples */
  double* re = malloc(n * sizeof *re);
  double* im = malloc(n * sizeof *im);
  float* in = malloc(2 * h * sizeof *in);
  float* out = malloc(2 * h * sizeof *out);
  int ended[MAX_STREAMS] = {0};
  size_t any;
  do {                                       /* until a frame takes nothing new: the tail is out */
    any = 0;
    for (int s = 0; s < streams; s++) {
      size_t got = ended[s] ? 0 : fread(in, 2 * sizeof(float), h, in_f[s]);
      if (got < (size_t)h) ended[s] = 1;
      if (got > any) any = got;
      double* xs = x + (size_t)s * n;
      memmove(xs, xs + h, h * sizeof *xs);
      for (long i = 0; i < h; i++)
        xs[h + i] = i < (long)got ? wl * in[2 * i] + wr * in[2 * i + 1] : 0.0;
      for (long i = 0; i < n; i++) { re[i] = xs[i] * win[i]; im[i] = 0.0; }
      fft(re, im, n, cs, sn, 1);
      double scale = 2.0 / n;
      float* o = out + 2 * s * share;
      o[0] = (float)(re[0] * scale);
      o[1] = streams == 1 ? (float)(re[h] * scale) : 0.0f;
      for (long k = 1; k < share; k++) {
        o[2 * k] = (float)(re[k] * scale);
        o[2 * k + 1] = (float)(im[k] * scale);
      }
    }
    if (fwrite(out, 2 * sizeof(float), h, stdout) != (size_t)h) return 0;
    fflush(stdout);
  } while (any > 0);
  return 0;
}
