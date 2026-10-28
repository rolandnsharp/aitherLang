/* tests/run: every TEST in the C files under tests/, or those named on
 * the command line, each in a child process. Prints `name ok` or `name
 * FAILED`, and exits 1 if any failed. */

#define _GNU_SOURCE
#include <sys/wait.h>
#include <unistd.h>
#include "check.h"

int check_failures;

typedef struct { const char* name; void (*fn)(void); } Test;
static Test tests[256];
static int ntests;

void test_register(const char* name, void (*fn)(void)) {
  if (ntests < 256) tests[ntests++] = (Test){name, fn};
}

static int by_name(const void* a, const void* b) {
  return strcmp(((const Test*)a)->name, ((const Test*)b)->name);
}

/* ------------------------------------------------------------ the helpers */

static int hoist_on(void) {
  const char* h = getenv("AITHER_HOIST");
  return !(h && strcmp(h, "0") == 0);
}

Voice* load_voice(const char* src, const char* tag, double sr, char* err, int errlen) {
  int kind;
  Prepared* p = voice_prepare(aither_stdlib, src, tag, strcmp(tag, "<inline>") ? tag : "", sr, 0,
                              hoist_on(), &kind, err, errlen);
  if (!p) return NULL;
  Voice* v = voice_new();
  voice_commit(v, p);
  return v;
}

Voice* must_load(const char* src, double sr) {
  char err[4096];
  Voice* v = load_voice(src, "<inline>", sr, err, sizeof err);
  CHECK(v, "doesn't compile: %s", err);
  return v;
}

int reload(Voice* v, const char* src, double sr) {
  char err[4096];
  int kind;
  Prepared* p = voice_prepare(aither_stdlib, src, "<inline>", "", sr, 0, hoist_on(), &kind, err, sizeof err);
  if (!p) {
    CHECK(0, "reload doesn't compile: %s", err);
    return -1;
  }
  voice_commit(v, p);
  return 0;
}

void tick_at(Voice* v, double t, double* l, double* r) {
  *l = *r = 0.0;
  voice_tick(v, t, l, r);
}

int compile_full(const char* src, Generated* g, char* err, int errlen) {
  Arena* a = arena_new();
  int kind;
  Node* prog = build_program(a, aither_stdlib, src, "<inline>", 0, &kind, err, errlen);
  if (!prog) return -1;
  return generate_full(a, prog, "", 48000.0, hoist_on(), g, err, errlen) ? -2 : 0;
}

char* compile_csrc(const char* src, const char* tag, char* err, int errlen) {
  Arena* a = arena_new();
  int kind;
  Node* prog = build_program(a, aither_stdlib, src, tag, 0, &kind, err, errlen);
  Generated g;
  char* r = NULL;
  if (prog && generate_full(a, prog, strcmp(tag, "<inline>") ? tag : "", 48000.0, hoist_on(), &g,
                            err, errlen) == 0)
    r = strdup(g.csrc);
  arena_free(a);
  return r;
}

int render_src(const char* src, double seconds, int sr, Rendered* out, char* err, int errlen) {
  return render_source(aither_stdlib, src, "<inline>", seconds, sr, NULL, NULL, 0, 0.0, out, err, errlen);
}

char* slurp(const char* path) {
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

/* ------------------------------------------------------------------- main */

int main(int argc, char** argv) {
  engine_init(aither_stdlib);
  setenv("AITHER_RT", "0", 1);          /* a test's engine never takes the audio core */
  qsort(tests, ntests, sizeof *tests, by_name);
  int failed = 0, ran = 0;
  for (int i = 0; i < ntests; i++) {
    int wanted = argc < 2;
    for (int k = 1; k < argc; k++) wanted |= strcmp(argv[k], tests[i].name) == 0;
    if (!wanted) continue;
    ran++;
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
      check_failures = 0;
      tests[i].fn();
      fflush(stdout);
      _exit(check_failures ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok && WIFSIGNALED(status)) fprintf(stderr, "  killed by signal %d\n", WTERMSIG(status));
    printf("%s %s\n", tests[i].name, ok ? "ok" : "FAILED");
    failed += !ok;
  }
  if (ran == 0) {
    fprintf(stderr, "run: no test named that\n");
    return 1;
  }
  printf(failed ? "%d of %d FAILED\n" : "all %d ok\n", failed ? failed : ran, ran);
  return failed ? 1 : 0;
}
