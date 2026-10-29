/* The corpus check: does every patch still sound exactly as it did?
 *
 *   make corpus          compare every tracked .aither file with tests/corpus.golden
 *   make corpus-update   re-record the golden file after an intended change
 *
 * Each file is rendered offline with every MIDI knob at half, in three
 * windows, so keyboard and knob driven patches sound too and later
 * sections are reached:
 *   0-0.5 s    a C-E-G chord held at velocity 100
 *   0.5-1.5 s  the same voice on: the chord released, F-A-C struck at
 *              velocity 50 (note-offs, release tails, new attacks)
 *   150 s      a fresh voice started with its clock at 150 s, the C-E-G
 *              chord held, for 0.5 s: timeline pieces whose sections
 *              fade in on `t` (cathedral's bells, rhythm, lead) are heard
 * and with a voice on input() throughout, so patches that listen are
 * heard too (test_voice below): on the left side only, so a patch that
 * reads the left, the two sides' difference (the uConsole's headset mic)
 * or their mono sum all hear it. Patches that don't call input() never
 * read it.
 * All on TCC (AITHER_OPT=0): no compile wait, and the same bits as the
 * cc -O2 build (tests/exact). Switching heavy patches to cc was
 * tried and was slower: on the A53s a cc compile costs a minute or more
 * with the other workers running. Patches are split across one worker
 * process per core. Its fingerprint is a hash of every sample's
 * exact bits, plus RMS and peak to make a change readable. A change to
 * the language, the stdlib or the engine that alters a patch it wasn't
 * meant to touch fails here; one that was meant to shows up as a diff
 * of tests/corpus.golden in the commit.
 *
 * Patches are rendered as committed at HEAD (extracted to a temporary
 * tree, so imports resolve as usual), so uncommitted edits to a patch
 * neither fail the check nor get recorded by an update. The engine and
 * stdlib are the working tree's: those are what the check is testing.
 * Noise and all other state start fresh in each render, so the result
 * doesn't depend on file order. */

#define _GNU_SOURCE
#include <ftw.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../voice.h"
#include "../render.h"
#include "../midi.h"

extern const char aither_stdlib[];

#define GOLDEN "tests/corpus.golden"
#define LATE 150.0            /* where the third window's fresh voice starts */

/* ---------------------------------------------------------------- strings */

typedef struct { char** v; int n, cap; } List;

static void push(List* l, char* s) {
  if (l->n == l->cap) l->v = realloc(l->v, (l->cap = l->cap ? l->cap * 2 : 64) * sizeof *l->v);
  l->v[l->n++] = s;
}

static int find(const List* l, const char* s) {
  for (int i = 0; i < l->n; i++)
    if (strcmp(l->v[i], s) == 0) return i;
  return -1;
}

/* A command's stdout, one line each (empty lines dropped). */
static List lines_of(const char* cmd) {
  List l = {0};
  FILE* p = popen(cmd, "r");
  if (!p) return l;
  char* line = NULL;
  size_t cap = 0;
  ssize_t k;
  while ((k = getline(&line, &cap, p)) > 0) {
    if (line[k - 1] == '\n') line[--k] = 0;
    if (k > 0) push(&l, strdup(line));
  }
  free(line);
  pclose(p);
  return l;
}

static int by_str(const void* a, const void* b) { return strcmp(*(char* const*)a, *(char* const*)b); }

/* A shell word in single quotes. */
static char* quoted(const char* s) {
  char* o = malloc(strlen(s) * 4 + 3), *q = o;
  *q++ = '\'';
  for (; *s; s++) {
    if (*s == '\'') { memcpy(q, "'\\''", 4); q += 4; }
    else *q++ = *s;
  }
  *q++ = '\'';
  *q = 0;
  return o;
}

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

static double epoch(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------ fingerprint */

static double db(double x) { return 20 * log10(x < 1e-12 ? 1e-12 : x); }

static void knobs_half(void) {
  midi_reset_all();
  for (int cc = 0; cc <= 127; cc++) midi_cc(cc, 64);
}

static void chord(const int* keys, int vel) {
  for (int i = 0; i < 3; i++) midi_note_on(keys[i], vel);
}

/* A voice with the patch, or 0 with the message in err. */
static Voice* loaded(const char* src, const char* path, char* err, int errlen) {
  const char* h = getenv("AITHER_HOIST");
  int kind;
  Prepared* p = voice_prepare(aither_stdlib, src, path, "", 48000.0, 0, !(h && strcmp(h, "0") == 0),
                              &kind, err, errlen);
  if (!p) return NULL;
  Voice* v = voice_new();
  voice_commit(v, p);
  return v;
}

/* "ERROR  " and the message's first line, double spaces made single
 * (one pass, left to right, as Nim's replace). */
static char* error_line(const char* msg) {
  size_t n = strcspn(msg, "\r\n");
  char* o = malloc(n + 8);
  strcpy(o, "ERROR  ");
  char* q = o + 7;
  for (size_t i = 0; i < n;) {
    if (i + 1 < n && msg[i] == ' ' && msg[i + 1] == ' ') { *q++ = ' '; i += 2; }
    else *q++ = msg[i++];
  }
  *q = 0;
  return o;
}

/* The voice every render hears on input(), from its window's own clock: a
 * 110 Hz voice of 30 harmonics singing "wow", its two formants gliding
 * from oo (300, 870 Hz) to ah (750, 1200 Hz) and back once a second, with
 * a pop (a 3 ms burst three times as loud) every half second. Peak about
 * 0.2. Deterministic: the same samples on every machine. */
static void test_voice(double t0, double seconds, int sr, double** l, double** r, long* n) {
  *n = (long)(seconds * sr + 0.5);
  *l = malloc(sizeof(double) * *n);
  *r = malloc(sizeof(double) * *n);
  for (long i = 0; i < *n; i++) {
    double t = t0 + (double)i / sr;
    double m = (1 - cos(2 * M_PI * t)) * 0.5;
    double f1 = 300 + 450 * m, f2 = 870 + 330 * m, x = 0;
    for (int k = 1; k <= 30; k++) {
      double f = 110.0 * k;
      double g = exp(-pow((f - f1) / 120, 2)) + 0.5 * exp(-pow((f - f2) / 160, 2)) + 0.05;
      x += g / k * sin(2 * M_PI * f * t);
    }
    double since = fmod(t, 0.5);
    x *= 0.08 * (1 + 2 * exp(-since / 0.003));
    (*l)[i] = x;
    (*r)[i] = 0;
  }
}

static char* fingerprint(const char* path) {
  static const int ceg[3] = {60, 64, 67}, fac[3] = {65, 69, 72};
  char err[4096];
  Rendered w[3] = {0};
  int nw = 0;
  Voice *v = NULL, *late = NULL;
  char* out = NULL;
  double* in[3][2];
  long nin[3];
  test_voice(0.0, 0.5, 48000, &in[0][0], &in[0][1], &nin[0]);
  test_voice(0.5, 1.0, 48000, &in[1][0], &in[1][1], &nin[1]);
  test_voice(LATE, 0.5, 48000, &in[2][0], &in[2][1], &nin[2]);
  knobs_half();
  char* src = slurp(path);
  if (!src) {
    snprintf(err, sizeof err, "cannot open: %s", path);
    goto fail;
  }
  chord(ceg, 100);
  if (!(v = loaded(src, path, err, sizeof err))) goto fail;
  if (render_voice(v, 0.5, 48000, in[0][0], in[0][1], nin[0], 0, 0.0, 1, &w[nw], err, sizeof err)) goto fail;
  nw++;
  for (int i = 0; i < 3; i++) midi_note_off(ceg[i]);
  chord(fac, 50);
  if (render_voice(v, 1.0, 48000, in[1][0], in[1][1], nin[1], 0, 0.5, 1, &w[nw], err, sizeof err)) goto fail;
  nw++;
  knobs_half();
  chord(ceg, 100);
  if (!(late = loaded(src, path, err, sizeof err))) goto fail;
  if (render_voice(late, 0.5, 48000, in[2][0], in[2][1], nin[2], 0, LATE, 1, &w[nw], err, sizeof err)) goto fail;
  nw++;
  uint64_t h = 0xcbf29ce484222325ull;          /* FNV-1a over the samples' bits */
  double sum = 0, peak = 0;
  long count = 0;
  for (int k = 0; k < nw; k++)
    for (long i = 0; i < w[k].n; i++)
      for (int side = 0; side < 2; side++) {
        double x = side ? w[k].right[i] : w[k].left[i];
        uint64_t bits;
        memcpy(&bits, &x, 8);
        for (int b = 0; b < 8; b++) {
          h = (h ^ (bits & 0xff)) * 0x100000001b3ull;
          bits >>= 8;
        }
        sum += x * x;
        if (peak <= fabs(x)) peak = fabs(x);
        count++;
      }
  double rms = sqrt(sum / (double)(count > 1 ? count : 1));
  if (asprintf(&out, "%016llX  %.1f  %.1f", (unsigned long long)h, db(rms), db(peak)) < 0) out = NULL;
  goto done;
fail:
  out = error_line(err);
done:
  for (int k = 0; k < nw; k++) rendered_free(&w[k]);
  for (int k = 0; k < 3; k++) free(in[k][0]), free(in[k][1]);
  if (v) voice_dispose(v);
  if (late) voice_dispose(late);
  free(src);
  return out;
}

/* ------------------------------------------------------------------- main */

static char head[4096];

static int rm_one(const char* p, const struct stat* sb, int flag, struct FTW* ftw) {
  (void)sb; (void)flag; (void)ftw;
  return remove(p);
}

static void remove_head(void) {
  if (head[0]) nftw(head, rm_one, 16, FTW_DEPTH | FTW_PHYS);
}

static void die(const char* msg) {
  fprintf(stderr, "%s\n", msg);
  exit(1);
}

/* Up to the first double space: the path, then the print. */
static void parse(const char* line, char** path, char** rest) {
  const char* i = strstr(line, "  ");
  if (!i) i = line + strlen(line);
  *path = strndup(line, i - line);
  *rest = strdup(*i ? i + 2 : i);
}

/* Whitespace-separated words of s (at most 8). */
static int words(const char* s, char w[8][256]) {
  int n = 0;
  while (*s) {
    while (*s && strchr(" \t\r\n\v\f", *s)) s++;
    if (!*s) break;
    size_t k = strcspn(s, " \t\r\n\v\f");
    if (n < 8) snprintf(w[n], 256, "%.*s", (int)k, s);
    n++;
    s += k;
  }
  return n;
}

typedef struct { double secs; char* path; } Took;

int main(int argc, char** argv) {
  setenv("AITHER_OPT", "0", 1);    /* TCC throughout: same bits as cc, no compile wait */

  List tracked = lines_of("git ls-files '*.aither'");
  List files = {0};
  for (int i = 0; i < tracked.n; i++)   /* tests/lang: cases, not patches */
    if (!strstr(tracked.v[i], "/lib/") && strncmp(tracked.v[i], "tests/", 6) != 0) push(&files, tracked.v[i]);
  qsort(files.v, files.n, sizeof *files.v, by_str);

  /* Every tracked patch, libraries included, as committed at HEAD. */
  const char* tmp = getenv("TMPDIR");
  snprintf(head, sizeof head, "%s/aither-corpus-%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
  char* q = quoted(head);
  char* cmd;
  if (asprintf(&cmd, "mkdir -p %s", q) < 0 || system(cmd) != 0) die("corpus: couldn't make a temporary tree");
  atexit(remove_head);
  size_t len = 64 + strlen(q);
  for (int i = 0; i < tracked.n; i++) len += strlen(tracked.v[i]) * 4 + 3;
  cmd = malloc(len);
  strcpy(cmd, "git archive HEAD --");
  for (int i = 0; i < tracked.n; i++) {
    char* w = quoted(tracked.v[i]);
    strcat(strcat(cmd, " "), w);
    free(w);
  }
  strcat(strcat(cmd, " | tar -x -C "), q);
  if (system(cmd) != 0) die("corpus: couldn't extract the committed patches");

  /* stdlib.aither is compiled into the engine from the working tree, as
   * the check intends, so its edits are tested, not ignored. */
  List dirty0 = lines_of("git diff --name-only HEAD -- '*.aither'"), dirty = {0};
  for (int i = 0; i < dirty0.n; i++)
    if (strcmp(dirty0.v[i], "stdlib.aither") != 0) push(&dirty, dirty0.v[i]);
  if (dirty.n > 0) {
    printf("corpus: rendering the committed version of %d patch(es) with uncommitted edits: ", dirty.n);
    for (int i = 0; i < dirty.n; i++) printf("%s%s", i ? ", " : "", dirty.v[i]);
    printf("\n");
  }
  fflush(stdout);

  /* One worker per core, each taking every Nth patch; then in path order. */
  long nproc = sysconf(_SC_NPROCESSORS_ONLN);
  int workers = nproc > 1 ? (int)nproc : 1;
  int* fds = calloc(workers, sizeof *fds);
  pid_t* pids = calloc(workers, sizeof *pids);
  for (int k = 0; k < workers; k++) {
    int fd[2];
    if (pipe(fd) != 0) die("corpus: no pipe");
    pids[k] = fork();
    if (pids[k] < 0) die("corpus: couldn't fork a worker");
    if (pids[k] == 0) {
      close(fd[0]);
      FILE* o = fdopen(fd[1], "w");
      for (int i = k; i < files.n; i += workers) {
        char* path;
        if (asprintf(&path, "%s/%s", head, files.v[i]) < 0) _exit(1);
        double t0 = epoch();
        char* print = fingerprint(path);
        fprintf(o, "%s\t%s\t%.1f\n", files.v[i], print ? print : "ERROR  out of memory", epoch() - t0);
        fflush(o);
        free(print);
        free(path);
      }
      fclose(o);
      _exit(0);                    /* the parent removes the tree */
    }
    close(fd[1]);
    fds[k] = fd[0];
  }
  char** got = calloc(files.n, sizeof *got);
  Took* took = calloc(files.n + 1, sizeof *took);
  int ntook = 0;
  for (int k = 0; k < workers; k++) {
    FILE* in = fdopen(fds[k], "r");
    char* line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, in)) > 0) {
      if (line[n - 1] == '\n') line[--n] = 0;
      char* t1 = strchr(line, '\t');
      char* t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
      if (!t2 || strchr(t2 + 1, '\t')) continue;
      *t1 = *t2 = 0;
      int i = find(&files, line);
      if (i < 0) continue;
      free(got[i]);
      got[i] = strdup(t1 + 1);
      if (ntook <= files.n) took[ntook++] = (Took){atof(t2 + 1), files.v[i]};
    }
    free(line);
    fclose(in);
    waitpid(pids[k], NULL, 0);
  }
  /* Slowest first, ties in the order they came (a stable insertion sort). */
  for (int i = 1; i < ntook; i++) {
    Took x = took[i];
    int j = i;
    for (; j > 0 && took[j - 1].secs < x.secs; j--) took[j] = took[j - 1];
    took[j] = x;
  }
  fprintf(stderr, "corpus: slowest: ");
  for (int i = 0; i < ntook && i < 5; i++)
    fprintf(stderr, "%s%s %d s", i ? ", " : "", took[i].path, (int)took[i].secs);
  fprintf(stderr, "\n");
  for (int i = 0; i < files.n; i++)
    if (!got[i]) got[i] = "ERROR  worker produced no fingerprint";

  if (argc >= 2 && strcmp(argv[1], "update") == 0) {
    FILE* o = fopen(GOLDEN, "w");
    if (!o) die("corpus: couldn't write " GOLDEN);
    for (int i = 0; i < files.n; i++) fprintf(o, "%s  %s\n", files.v[i], got[i]);
    fclose(o);
    printf("corpus: recorded %d files in %s\n", files.n, GOLDEN);
    return 0;
  }

  List old_path = {0}, old_print = {0};
  char* text = slurp(GOLDEN);
  for (char* p = text; p && *p;) {
    char* e = strchr(p, '\n');
    if (e) *e = 0;
    if (*p) {
      char *path, *rest;
      parse(p, &path, &rest);
      int i = find(&old_path, path);
      if (i >= 0) old_print.v[i] = rest;          /* a repeat: the last wins */
      else { push(&old_path, path); push(&old_print, rest); }
    }
    p = e ? e + 1 : p + strlen(p);
  }
  int bad = 0;
  for (int i = 0; i < files.n; i++) {
    int j = find(&old_path, files.v[i]);
    if (j < 0) {
      printf("  new      %s  %s\n", files.v[i], got[i]);
      bad++;
    } else if (strcmp(old_print.v[j], got[i]) != 0) {
      char o[8][256], n[8][256];
      int no = words(old_print.v[j], o), nn = words(got[i], n);
      if (no == 3 && nn == 3) printf("  changed  %s  rms %s -> %s dB\n", files.v[i], o[1], n[1]);
      else printf("  changed  %s  %s  ->  %s\n", files.v[i], old_print.v[j], got[i]);
      bad++;
    }
  }
  for (int j = 0; j < old_path.n; j++)
    if (find(&files, old_path.v[j]) < 0) {
      printf("  gone     %s\n", old_path.v[j]);
      bad++;
    }
  if (bad > 0) {
    printf("corpus: %d of %d files differ from %s. If intended: make corpus-update, and commit the golden file.\n",
           bad, files.n, GOLDEN);
    return 1;
  }
  printf("corpus ok (%d files)\n", files.n);
  return 0;          /* through atexit, which removes the extracted tree */
}
