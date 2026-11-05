/* A block of tasks computes what a sample at a time does, to the bit.
 *
 *   make blocks
 *
 * A voice's code is a set of tasks (codegen, notes/multicore-2026-10-18.md):
 * the engine runs each task over a whole block, one after another, with
 * the values that cross between them in buffers; tick() runs them over a
 * block of one. If a task read something its buffers don't carry, or
 * read it a sample out, the two would part. This renders every tracked
 * patch for a second both ways, with the corpus's MIDI (knobs at half, a
 * C-E-G chord), in blocks of 300 (so the runs of 128 inside a block are
 * crossed too), and compares every sample's bits.
 *
 * With AITHER_THREADS set, the blocks run on the pool's workers (pool.h),
 * tasks on several cores at once, against one core a sample at a time.
 *
 * Patches named on the command line are checked instead of every one. */

#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../voice.h"
#include "../render.h"
#include "../midi.h"
#include "../pool.h"

extern const char aither_stdlib[];

enum { N = 48000, BLOCK = 300 };

static char* slurp(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  size_t cap = 8192, n = 0, k;
  char* s = malloc(cap);
  while ((k = fread(s + n, 1, cap - n - 1, f)) > 0) {
    n += k;
    if (n + 1 >= cap) s = realloc(s, cap *= 2);
  }
  fclose(f);
  s[n] = 0;
  return s;
}

static Voice* load(const char* path, char* err, int errlen) {
  midi_reset_all();
  for (int cc = 0; cc <= 127; cc++) midi_cc(cc, 64);
  static const int ceg[3] = {60, 64, 67};
  for (int i = 0; i < 3; i++) midi_note_on(ceg[i], 100);
  char* src = slurp(path);
  if (!src) {
    snprintf(err, errlen, "cannot open: %s", path);
    return NULL;
  }
  int kind;
  Prepared* p = voice_prepare(aither_stdlib, src, path, path, 48000.0, 0, 1, &kind, err, errlen);
  free(src);
  if (!p) return NULL;
  Voice* v = voice_new();
  voice_commit(v, p);
  VoiceHeader* h = v->cur.state;
  h->fade_gain = 1.0;
  h->fade_delta = 0.0;
  h->step = 0.0;
  return v;
}

/* A sample at a time, sample i at step i, as the engine counts. */
static int single(const char* path, double* out, char* err, int errlen) {
  Voice* v = load(path, err, errlen);
  if (!v) return -1;
  for (int i = 0; i < N; i++) voice_tick(v, (double)i / 48000.0, &out[2 * i], &out[2 * i + 1]);
  voice_dispose(v);
  return 0;
}

static int blocks(const char* path, double* out, char* err, int errlen) {
  Voice* v = load(path, err, errlen);
  if (!v) return -1;
  static double l[BLOCK], r[BLOCK];
  for (int i = 0; i < N;) {
    int n = N - i < BLOCK ? N - i : BLOCK;
    voice_tick_block(v, n, l, r);
    for (int k = 0; k < n; k++) { out[2 * (i + k)] = l[k]; out[2 * (i + k) + 1] = r[k]; }
    i += n;
  }
  voice_dispose(v);
  return 0;
}

static int by_str(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

int main(int argc, char** argv) {
  if (getenv("AITHER_THREADS")) {
    printf("on %d threads\n", pool_start());
    pool_claim();
  }
  char** files = NULL;
  int n = 0, cap = 0;
  if (argc > 1) {
    files = argv + 1;
    n = argc - 1;
  } else {
    FILE* p = popen("git ls-files '*.aither'", "r");
    char* line = NULL;
    size_t lcap = 0;
    ssize_t k;
    while (p && (k = getline(&line, &lcap, p)) > 0) {
      if (line[k - 1] == '\n') line[--k] = 0;
      if (k == 0 || strstr(line, "/lib/") || strncmp(line, "tests/", 6) == 0) continue;
      if (n == cap) files = realloc(files, (cap = cap ? cap * 2 : 256) * sizeof *files);
      files[n++] = strdup(line);
    }
    if (p) pclose(p);
    qsort(files, n, sizeof *files, by_str);
  }
  static double a[2 * N], b[2 * N];
  int differ = 0, failed = 0;
  char err[4096];
  for (int i = 0; i < n; i++) {
    if (single(files[i], a, err, sizeof err) || blocks(files[i], b, err, sizeof err)) {
      failed++;
      printf("ERROR  %s: %.*s\n", files[i], (int)strcspn(err, "\r\n"), err);
      fflush(stdout);
      continue;
    }
    long first = -1;
    double worst = 0;
    for (long s = 0; s < 2 * N; s++) {
      double x = a[s], y = b[s];
      /* the engine's guard silences what isn't a sound: not a difference */
      if (!(x == x) || fabs(x) > 1e6) x = 0.0;
      uint64_t bx, by;
      memcpy(&bx, &x, 8);
      memcpy(&by, &y, 8);
      if (bx != by) {
        if (first < 0) first = s / 2;
        if (worst <= fabs(x - y)) worst = fabs(x - y);
      }
    }
    if (first >= 0) {
      differ++;
      printf("DIFFER %s: from sample %ld, largest gap %.2e\n", files[i], first, worst);
    }
    fflush(stdout);
  }
  printf("%d patches: %d bit-identical, %d differ, %d failed to render\n", n, n - differ - failed, differ,
         failed);
  return differ + failed == 0 ? 0 : 1;
}
