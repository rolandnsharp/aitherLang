/* `import "lib/x.aither"` splices a library's defs and constant tables
 * into the patch: paths relative to the importing file, nested imports,
 * a file imported twice included once, cycles ended, the patch's own
 * def winning over the library's, and clear errors for a missing file
 * or a library that holds more than defs and tables. */

#define _GNU_SOURCE
#include <sys/stat.h>
#include <unistd.h>
#include "check.h"

static char dir[512];

/* The fixtures, relative to `dir`, so they can be removed at the end. */
static const char* written[16];
static int nwritten;

static void write_file(const char* rel, const char* text) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, rel);
  FILE* f = fopen(path, "w");
  CHECK(f, "can't write %s", path);
  if (!f) return;
  fputs(text, f);
  fclose(f);
  written[nwritten++] = rel;
}

/* The patch at dir/rel rendered for a millisecond, as `aither render`
 * does: "" and its first left sample in *first, or the error (static). */
static const char* err_of(const char* rel, double* first) {
  static char err[4096];
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", dir, rel);
  char* src = slurp(path);
  if (!src) {
    snprintf(err, sizeof err, "patch not found: %s", path);
    return err;
  }
  Rendered r;
  int rc = render_source(aither_stdlib, src, path, 0.001, 48000, NULL, NULL, 0, 0.0, &r, err,
                         sizeof err);
  free(src);
  if (rc != 0) return err;
  if (first) *first = r.n > 0 ? r.left[0] : NAN;
  rendered_free(&r);
  return "";
}

TEST(imports) {
  const char* tmp = getenv("TMPDIR");
  snprintf(dir, sizeof dir, "%s/aither_imports_%d", tmp && *tmp ? tmp : "/tmp", (int)getpid());
  char lib[600];
  snprintf(lib, sizeof lib, "%s/lib", dir);
  mkdir(dir, 0755);
  mkdir(lib, 0755);

  write_file("lib/tables.aither",
             "let ratios = [1.0, 2.0, 3.0]\n"
             "let gainK = 0.5\n");
  write_file("lib/voice.aither",
             "import \"tables.aither\"\n"
             "import \"tables.aither\"\n"
             "def third(x): x * ratios[2]\n"
             "def half(x): x * gainK\n");
  write_file("lib/cycle_a.aither", "import \"cycle_b.aither\"\ndef ca(x): x + 1\n");
  write_file("lib/cycle_b.aither", "import \"cycle_a.aither\"\ndef cb(x): x + 2\n");
  write_file("patch.aither",
             "import \"lib/voice.aither\"\n"
             "import \"lib/cycle_a.aither\"\n"
             "def half(x): x * 0.25\n"
             "play a:\n"
             "  third(1) + half(1) + ca(0) + cb(0)\n"
             "a\n");

  /* third(1)=3, the patch's half wins (0.25), ca(0)=1, cb(0)=2. */
  double first = NAN;
  const char* e = err_of("patch.aither", &first);
  CHECK(!*e, "patch doesn't render: %s", e);
  CHECK(first == 3.0 + 0.25 + 1.0 + 2.0, "patch gives %g", first);

  write_file("missing.aither", "import \"lib/nope.aither\"\n0\n");
  e = err_of("missing.aither", NULL);
  CHECK(strstr(e, "not found") && strstr(e, "nope.aither") && strstr(e, "missing.aither:1"), "%s",
        e);

  write_file("lib/noisy.aither", "def ok(x): x\nplay p:\n  1\n");
  write_file("badlib.aither", "import \"lib/noisy.aither\"\n0\n");
  e = err_of("badlib.aither", NULL);
  CHECK(strstr(e, "defs and constant tables only") && strstr(e, "play") &&
        strstr(e, "noisy.aither:2"), "%s", e);

  write_file("inner.aither", "def f(x):\n  import \"lib/tables.aither\"\n  x\nf(1)\n");
  e = err_of("inner.aither", NULL);
  CHECK(strstr(e, "top of the file"), "%s", e);

  /* A compile error inside a library points at the library file. */
  write_file("lib/broken.aither", "def bad(x): x + nothing\n");
  write_file("usesbroken.aither", "import \"lib/broken.aither\"\nbad(1)\n");
  e = err_of("usesbroken.aither", NULL);
  CHECK(strstr(e, "unknown identifier: nothing") && strstr(e, "broken.aither:1"), "%s", e);

  /* Inline source resolves imports against the current directory. Go
   * back afterwards, so nothing after runs from a directory that no
   * longer exists. */
  char back[1024];
  if (getcwd(back, sizeof back) && chdir(dir) == 0) {
    Rendered r;
    char err[4096];
    int rc = render_src("import \"lib/voice.aither\"\nthird(2)", 0.001, 48000, &r, err, sizeof err);
    CHECK(rc == 0, "inline import doesn't render: %s", err);
    if (rc == 0) {
      CHECK(r.left[0] == 6.0, "third(2) gives %g", r.left[0]);
      rendered_free(&r);
    }
    CHECK(chdir(back) == 0, "can't go back to %s", back);
  } else CHECK(0, "can't enter %s", dir);

  for (int i = 0; i < nwritten; i++) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, written[i]);
    unlink(path);
  }
  rmdir(lib);
  rmdir(dir);
}
