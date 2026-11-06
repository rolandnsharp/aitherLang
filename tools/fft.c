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
   the last samples out. Exits when the pipe closes. Build: make tools/fft */
#include <string.h>
#include "fft.h"

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) {
    fprintf(stderr, "usage: tools/fft N [left|right]   (N a power of two: float32 pairs in, bins out)\n");
    return 2;
  }
  long n = frame_size("fft", argv[1]), h = n / 2;
  double wl = 0.5, wr = 0.5;
  if (argc == 3) {
    if (!strcmp(argv[2], "left")) wl = 1, wr = 0;
    else if (!strcmp(argv[2], "right")) wl = 0, wr = 1;
    else { fprintf(stderr, "fft: the side is left or right, not %s\n", argv[2]); return 2; }
  }
  double* win = sine_window(n);
  double *cs, *sn;
  fft_turns(n, &cs, &sn);
  double* x = calloc(n, sizeof *x);          /* the last N samples */
  double* re = malloc(n * sizeof *re);
  double* im = malloc(n * sizeof *im);
  float* in = malloc(2 * h * sizeof *in);
  float* out = malloc(2 * h * sizeof *out);
  int ended = 0;
  size_t got;
  do {                                       /* until a frame takes nothing new: the tail is out */
    got = ended ? 0 : fread(in, 2 * sizeof(float), h, stdin);
    if (got < (size_t)h) ended = 1;
    memmove(x, x + h, h * sizeof *x);
    for (long i = 0; i < h; i++)
      x[h + i] = i < (long)got ? wl * in[2 * i] + wr * in[2 * i + 1] : 0.0;
    for (long i = 0; i < n; i++) { re[i] = x[i] * win[i]; im[i] = 0.0; }
    fft(re, im, n, cs, sn, 1);
    double scale = 2.0 / n;
    out[0] = (float)(re[0] * scale);
    out[1] = (float)(re[h] * scale);
    for (long k = 1; k < h; k++) {
      out[2 * k] = (float)(re[k] * scale);
      out[2 * k + 1] = (float)(im[k] * scale);
    }
    if (fwrite(out, 2 * sizeof(float), h, stdout) != (size_t)h) return 0;
    fflush(stdout);
  } while (got > 0);
  return 0;
}
