/* tcc and cc -O2 compute the same bits.
 *
 *   make exact
 *
 * Every voice starts on TCC's code and is swapped for a cc -O2 build of
 * the same C when that's ready. The swap keeps the state, so it's only
 * seamless if both builds do the same arithmetic: otherwise every voice
 * jumps to a slightly different trajectory the moment the fast code
 * lands, and two voices meant to stay locked drift apart. This renders
 * every tracked patch for a second both ways, from fresh state and with
 * the corpus's MIDI (knobs at half, a C-E-G chord), and compares every
 * sample's bits.
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

extern const char aither_stdlib[];

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

/* A second of the patch, on TCC or its -O2 build; 0, or -1 with err. */
static int render_with(const char* path, int optimized, Rendered* out, char* err, int errlen) {
  midi_reset_all();
  for (int cc = 0; cc <= 127; cc++) midi_cc(cc, 64);
  static const int ceg[3] = {60, 64, 67};
  for (int i = 0; i < 3; i++) midi_note_on(ceg[i], 100);
  char* src = slurp(path);
  if (!src) {
    snprintf(err, errlen, "cannot open: %s", path);
    return -1;
  }
  const char* h = getenv("AITHER_HOIST");
  int kind, r = -1;
  Prepared* p = voice_prepare(aither_stdlib, src, path, path, 48000.0, 0, !(h && strcmp(h, "0") == 0),
                              &kind, err, errlen);
  free(src);
  if (!p) return -1;
  Voice* v = voice_new();
  voice_commit(v, p);
  if (optimized) {
    render_optimize(v);
    if (!v->cur.optimized) {
      snprintf(err, errlen, "%s: the cc -O2 build didn't land", path);
      goto done;
    }
  }
  r = render_voice(v, 1.0, 48000, NULL, NULL, 0, 0, 0.0, 1, out, err, errlen);
done:
  voice_dispose(v);
  return r;
}

static int by_str(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

int main(int argc, char** argv) {
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
      /* tests/lang: cases, not patches */
      if (k == 0 || strstr(line, "/lib/") || strncmp(line, "tests/", 6) == 0) continue;
      if (n == cap) files = realloc(files, (cap = cap ? cap * 2 : 256) * sizeof *files);
      files[n++] = strdup(line);
    }
    if (p) pclose(p);
    qsort(files, n, sizeof *files, by_str);
  }
  int differ = 0, failed = 0;
  char err[4096];
  for (int i = 0; i < n; i++) {
    Rendered a = {0}, b = {0};
    if (render_with(files[i], 0, &a, err, sizeof err) || render_with(files[i], 1, &b, err, sizeof err)) {
      failed++;
      printf("ERROR  %s: %.*s\n", files[i], (int)strcspn(err, "\r\n"), err);
      fflush(stdout);
      rendered_free(&a);
      rendered_free(&b);
      continue;
    }
    long first = -1;
    double worst = 0;
    for (long s = 0; s < a.n; s++)
      for (int side = 0; side < 2; side++) {
        double x = side ? a.right[s] : a.left[s], y = side ? b.right[s] : b.left[s];
        uint64_t bx, by;
        memcpy(&bx, &x, 8);
        memcpy(&by, &y, 8);
        if (bx != by) {
          if (first < 0) first = s;
          if (worst <= fabs(x - y)) worst = fabs(x - y);
        }
      }
    if (first >= 0) {
      differ++;
      printf("DIFFER %s: from sample %ld, largest gap %.2e\n", files[i], first, worst);
    }
    fflush(stdout);
    rendered_free(&a);
    rendered_free(&b);
  }
  printf("%d patches: %d bit-identical, %d differ, %d failed to render\n", n, n - differ - failed, differ,
         failed);
  return differ + failed == 0 ? 0 : 1;
}
