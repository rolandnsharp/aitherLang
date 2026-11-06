/* ifft: a stream of spectrum back into a signal.
     aither song.aither | tools/fft 1024 | aither spectral.aither | tools/ifft 1024 | tools/play
   Reads frames of bins as tools/fft writes them (tools/fft.h), N / 2
   pairs a frame, and writes the signal to both sides as float32 pairs:
   each frame turned back, windowed and added to the half before it.
   With the bins untouched it is tools/fft's input N / 2 samples later.
   Frames start at the stream's first pair, so a patch between the two
   must start with the stream (given on the command line, not sent to a
   running engine). Exits when the pipe closes. Build: make tools/ifft */
#include <string.h>
#include "fft.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: tools/ifft N   (N as tools/fft's: bins in, float32 pairs out)\n");
    return 2;
  }
  long n = frame_size("ifft", argv[1]), h = n / 2;
  double* win = sine_window(n);
  double *cs, *sn;
  fft_turns(n, &cs, &sn);
  double* re = malloc(n * sizeof *re);
  double* im = malloc(n * sizeof *im);
  double* tail = calloc(h, sizeof *tail);    /* the second half of the frame before */
  float* in = malloc(2 * h * sizeof *in);
  float* out = malloc(2 * h * sizeof *out);
  while (fread(in, 2 * sizeof(float), h, stdin) == (size_t)h) {
    re[0] = in[0]; im[0] = 0.0;
    re[h] = in[1]; im[h] = 0.0;
    for (long k = 1; k < h; k++) {
      re[k] = in[2 * k]; im[k] = in[2 * k + 1];
      re[n - k] = in[2 * k]; im[n - k] = -in[2 * k + 1];
    }
    fft(re, im, n, cs, sn, -1);
    /* bins came scaled by 2 / N, and going back wants 1 / N: a half */
    for (long i = 0; i < h; i++) {
      double y = tail[i] + 0.5 * re[i] * win[i];
      out[2 * i] = out[2 * i + 1] = (float)y;
      tail[i] = 0.5 * re[h + i] * win[h + i];
    }
    if (fwrite(out, 2 * sizeof(float), h, stdout) != (size_t)h) return 0;
    fflush(stdout);
  }
  return 0;
}
