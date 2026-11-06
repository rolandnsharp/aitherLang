/* add: streams added together, sample for sample.
     tools/add --delay 512 <(aither song.aither --mute trumpet) \
               <(aither song.aither --solo trumpet | tools/fft 1024 | ... | tools/ifft 1024) | tools/play
   Reads raw float32 pairs from each file (a pipe, a fifo, a recording) a
   block at a time, so the streams stay together however fast each comes,
   and writes their sum. `--delay M` before a file starts it M samples
   late: the way to line a stream up with one that has been through
   tools/fft and tools/ifft, N / 2 samples behind. A stream that ends
   adds silence; add exits when all have. Nothing is limited: a sum past
   +-1 goes on as it is. Build: make tools/add */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { BLOCK = 512, MAX_STREAMS = 32 };

int main(int argc, char** argv) {
  FILE* in[MAX_STREAMS];
  long late[MAX_STREAMS];
  int n = 0;
  long delay = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--delay") && i + 1 < argc) {
      char* end;
      delay = strtol(argv[++i], &end, 10);
      if (*end || delay < 0) { fprintf(stderr, "add: --delay is whole samples, not %s\n", argv[i]); return 2; }
      continue;
    }
    if (n == MAX_STREAMS) { fprintf(stderr, "add: %d streams at most\n", MAX_STREAMS); return 2; }
    in[n] = fopen(argv[i], "rb");
    if (!in[n]) { perror(argv[i]); return 1; }
    late[n++] = delay;
    delay = 0;
  }
  if (n == 0) {
    fprintf(stderr, "usage: tools/add [--delay M] FILE [[--delay M] FILE ...]   (float32 pairs, summed to stdout)\n");
    return 2;
  }
  static float buf[2 * BLOCK], sum[2 * BLOCK];
  for (;;) {
    int open = 0;
    memset(sum, 0, sizeof sum);
    long most = 0;
    for (int s = 0; s < n; s++) {
      long skip = late[s] < BLOCK ? late[s] : BLOCK;   /* this block's samples still to wait */
      late[s] -= skip;
      if (skip) open = 1;
      if (skip > most) most = skip;
      if (!in[s]) continue;
      open = 1;
      size_t got = fread(buf, 2 * sizeof(float), BLOCK - skip, in[s]);
      if (got < (size_t)(BLOCK - skip)) { fclose(in[s]); in[s] = NULL; }
      for (size_t i = 0; i < 2 * got; i++) sum[2 * skip + i] += buf[i];
      if (skip + (long)got > most) most = skip + (long)got;
    }
    if (!open) return 0;
    if (most && fwrite(sum, 2 * sizeof(float), most, stdout) != (size_t)most) return 0;
    fflush(stdout);
  }
}
