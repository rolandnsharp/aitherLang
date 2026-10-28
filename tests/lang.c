/* The language's behaviour, written in aither: every .aither file in
 * tests/lang holds cases, each a small patch and what it must give.
 *
 *   # --- what the case shows          a case starts here, named
 *   # expect: 7                        sample 0 is 7 on both sides
 *   # expect: 0.5, -0.5                left 0.5, right -0.5
 *   # expect at 99: 1                  sample 99 (from 0) is 1
 *   # expect until 4800: 0             every sample before 4800 is 0
 *   # error: unknown identifier        it doesn't compile, saying this
 *
 * A case may have several expects; one with none need only compile and
 * run. Numbers match to 1e-9 of their size. Two forms that must agree
 * are one case, `A - B`, expected 0 until some sample. Text before the
 * first case describes the file. */

#define _GNU_SOURCE
#include <glob.h>
#include "check.h"

typedef struct { long at; int until; double l, r; } Expect;

typedef struct {
  char*  name;
  char*  src;
  char*  error;
  int    line;
  Expect expects[32];
  int    nexpects;
} Case;

static char* trimmed(const char* s, size_t n) {
  while (n > 0 && strchr(" \t\r\v\f", *s)) { s++; n--; }
  while (n > 0 && strchr(" \t\r\v\f", s[n - 1])) n--;
  return strndup(s, n);
}

static int starts(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

static int parse_cases(const char* path, Case** out) {
  char* text = slurp(path);
  int n = 0, cap = 16, in_case = 0;
  Case* cases = calloc(cap, sizeof *cases);
  Case cur = {0};
  size_t src_len = 0;
  int lineno = 0;
  for (char* p = text; p && *p;) {
    char* e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    lineno++;
    char* s = trimmed(p, len);
    if (starts(s, "# --- ")) {
      if (in_case) {
        if (n == cap) cases = realloc(cases, (cap *= 2) * sizeof *cases);
        cases[n++] = cur;
      }
      cur = (Case){0};
      cur.name = strdup(s + 6);
      cur.line = lineno;
      cur.src = calloc(1, 1);
      src_len = 0;
      in_case = 1;
    } else if (!in_case) {
      /* the file's description */
    } else if (starts(s, "# expect")) {
      const char* rest = s + strlen("# expect");
      const char* colon = strchr(rest, ':');
      Expect x = {0};
      char* where = trimmed(rest, colon - rest);
      if (starts(where, "at ")) x.at = strtol(where + 3, NULL, 10);
      else if (starts(where, "until ")) {
        x.at = strtol(where + 6, NULL, 10) - 1;
        x.until = 1;
      }
      free(where);
      const char* comma = strchr(colon + 1, ',');
      x.l = strtod(colon + 1, NULL);
      x.r = comma ? strtod(comma + 1, NULL) : x.l;
      if (cur.nexpects < 32) cur.expects[cur.nexpects++] = x;
    } else if (starts(s, "# error:")) {
      cur.error = trimmed(s + 8, strlen(s + 8));
    } else {
      cur.src = realloc(cur.src, src_len + len + 2);
      memcpy(cur.src + src_len, p, len);
      src_len += len;
      cur.src[src_len++] = '\n';
      cur.src[src_len] = 0;
    }
    free(s);
    p = e ? e + 1 : NULL;
  }
  if (in_case) {
    if (n == cap) cases = realloc(cases, (cap + 1) * sizeof *cases);
    cases[n++] = cur;
  }
  free(text);
  *out = cases;
  return n;
}

static int near(double a, double b) { return fabs(a - b) <= 1e-9 * fmax(1.0, fabs(b)); }

TEST(lang) {
  glob_t g;
  if (glob("tests/lang/*.aither", 0, NULL, &g) != 0) {
    CHECK(0, "no tests/lang/*.aither (run from the repository's root)");
    return;
  }
  int ran = 0, failed = 0;
  for (size_t f = 0; f < g.gl_pathc; f++) {
    Case* cases;
    int n = parse_cases(g.gl_pathv[f], &cases);
    const char* file = strrchr(g.gl_pathv[f], '/') + 1;
    for (int i = 0; i < n; i++) {
      Case* c = &cases[i];
      ran++;
      long len = 1;
      for (int k = 0; k < c->nexpects; k++)
        if (c->expects[k].at + 1 > len) len = c->expects[k].at + 1;
      Rendered r;
      char err[4096] = "";
      int rc = render_src(c->src, (double)(len + 1) / 48000.0, 48000, &r, err, sizeof err);
      if (rc != 0) {
        if (!c->error || !strstr(err, c->error)) {
          printf("FAIL %s:%d (%s): %s\n", file, c->line, c->name, err);
          failed++;
        }
        continue;
      }
      if (c->error) {
        printf("FAIL %s:%d (%s): compiled, expected an error with '%s'\n", file, c->line, c->name,
               c->error);
        failed++;
        rendered_free(&r);
        continue;
      }
      for (int k = 0; k < c->nexpects; k++) {
        Expect* x = &c->expects[k];
        for (long s = x->until ? 0 : x->at; s <= x->at; s++) {
          if (s >= r.n || !near(r.left[s], x->l) || !near(r.right[s], x->r)) {
            printf("FAIL %s:%d (%s): sample %ld is %.17g, %.17g; expected %.17g, %.17g\n", file,
                   c->line, c->name, s, s < r.n ? r.left[s] : NAN, s < r.n ? r.right[s] : NAN,
                   x->l, x->r);
            failed++;
            break;
          }
        }
      }
      rendered_free(&r);
    }
  }
  CHECK(failed == 0, "%d of %d language cases failed", failed, ran);
  printf("  %d cases in %zu files\n", ran, g.gl_pathc);
  globfree(&g);
}
