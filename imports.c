/* `import "lib/voice.aither"`: a library's defs and constant tables
 * spliced into the program (imports.nim, in C). Paths are relative to the
 * importing file; a file imported twice, or in a cycle, is included once.
 * A library holds defs, constant lets and further imports: plays, state
 * and sound belong in the patch. */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "imports.h"

static char* read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char* s = malloc(n + 1);
  if (fread(s, 1, n, f) != (size_t)n) n = 0;
  s[n] = 0;
  fclose(f);
  return s;
}

static const char* adup(Arena* a, const char* s) {
  size_t n = strlen(s);
  char* d = arena_alloc(a, n + 1);
  memcpy(d, s, n);
  return d;
}

/* `a/./b/../c` to `a/c`, as Nim's normalizedPath. */
static const char* normalized(Arena* a, const char* path) {
  const char* parts[512];
  int n = 0;
  int abs = path[0] == '/';
  char* buf = strdup(path);
  for (char* tok = strtok(buf, "/"); tok; tok = strtok(NULL, "/")) {
    if (!strcmp(tok, ".")) continue;
    if (!strcmp(tok, "..") && n > 0 && strcmp(parts[n - 1], "..")) { n--; continue; }
    if (!strcmp(tok, "..") && abs) continue;
    if (n < 512) parts[n++] = tok;
  }
  char out[PATH_MAX * 2];
  size_t k = 0;
  if (abs) out[k++] = '/';
  for (int i = 0; i < n; i++) {
    size_t l = strlen(parts[i]);
    if (k + l + 2 >= sizeof out) break;
    if (i) out[k++] = '/';
    memcpy(out + k, parts[i], l);
    k += l;
  }
  if (k == 0) out[k++] = '.';
  out[k] = 0;
  free(buf);
  return adup(a, out);
}

static const char* parent_dir(Arena* a, const char* path) {
  const char* slash = strrchr(path, '/');
  if (!slash) return ".";
  if (slash == path) return "/";
  char* d = arena_alloc(a, slash - path + 1);
  memcpy(d, path, slash - path);
  return d;
}

static int is_const_let(const Node* n) {
  if (n->kind != nkLet) return 0;
  const Node* v = n->kids[0];
  if (v->kind == nkNum) return 1;
  if (v->kind != nkArr || v->nkids == 0) return 0;
  for (int i = 0; i < v->nkids; i++) if (v->kids[i]->kind != nkNum) return 0;
  return 1;
}

typedef struct { const char** v; int n, cap; } Seen;

static int seen_has(Seen* s, const char* k) {
  for (int i = 0; i < s->n; i++) if (!strcmp(s->v[i], k)) return 1;
  return 0;
}
static void seen_add(Seen* s, const char* k) {
  if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->v = realloc(s->v, s->cap * sizeof(char*)); }
  s->v[s->n++] = k;
}

typedef struct { Node** v; int n, cap; } Kids;
static void kid(Kids* k, Node* n) {
  if (k->n == k->cap) { k->cap = k->cap ? k->cap * 2 : 64; k->v = realloc(k->v, k->cap * sizeof(Node*)); }
  k->v[k->n++] = n;
}

static const char* key_of(Arena* a, const char* path) {
  char real[PATH_MAX];
  return realpath(path, real) ? adup(a, real) : path;
}

static int expand(Arena* a, Node* ast, const char* from, Seen* seen, int as_library,
                  Kids* out, char* err, int errlen) {
  char cwd[PATH_MAX];
  const char* base = from[0] ? parent_dir(a, from) : (getcwd(cwd, sizeof cwd) ? adup(a, cwd) : ".");
  for (int i = 0; i < ast->nkids; i++) {
    Node* s = ast->kids[i];
    if (s->kind == nkImport) {
      char joined[PATH_MAX * 2];
      if (s->str[0] == '/') snprintf(joined, sizeof joined, "%s", s->str);
      else snprintf(joined, sizeof joined, "%s/%s", base, s->str);
      const char* path = normalized(a, joined);
      const char* where = from[0] ? from : "<inline>";
      if (access(path, R_OK) != 0) {
        snprintf(err, errlen, "import: %s not found (looked at %s) (%s:%d)", s->str, path, where, s->line);
        return -1;
      }
      const char* key = key_of(a, path);
      if (seen_has(seen, key)) continue;
      seen_add(seen, key);
      char* src = read_file(path);
      char perr[1024];
      Node* lib = src ? parse_program(a, src, perr, sizeof perr) : NULL;
      free(src);
      if (!lib) { snprintf(err, errlen, "in %s: %s", path, perr); return -1; }
      set_source(a, lib, path);
      if (expand(a, lib, path, seen, 1, out, err, errlen)) return -1;
    } else {
      if (as_library && !(s->kind == nkDef || is_const_let(s))) {
        const char* what =
          s->kind == nkPlay ? "play" :
          (s->kind == nkVar || s->kind == nkAssign || s->kind == nkIdxAssign) ? "state" :
          s->kind == nkLet ? "let (its value isn't a constant)" : "expression";
        snprintf(err, errlen, "a library holds defs and constant tables only; this %s belongs in "
                 "the patch that imports it (%s:%d)", what, from, s->line);
        return -1;
      }
      kid(out, s);
    }
  }
  return 0;
}

Node* build_program(Arena* a, const char* stdlib_src, const char* src, const char* src_tag,
                    int seed_self, int* kind, char* err, int errlen) {
  char perr[1024];
  *kind = BUILD_PARSE;
  Node* std = parse_program(a, stdlib_src, perr, sizeof perr);
  if (!std) { snprintf(err, errlen, "stdlib: %s", perr); return NULL; }
  set_source(a, std, "stdlib");
  Node* user = parse_program(a, src, err, errlen);
  if (!user) return NULL;
  set_source(a, user, src_tag);
  const char* from = strcmp(src_tag, "<inline>") ? src_tag : "";
  Seen seen = {0};
  Kids kids = {0};
  if (seed_self && from[0]) seen_add(&seen, key_of(a, from));
  for (int i = 0; i < std->nkids; i++) kid(&kids, std->kids[i]);
  *kind = BUILD_IMPORT;
  int r = expand(a, user, from, &seen, 0, &kids, err, errlen);
  free(seen.v);
  if (r) { free(kids.v); return NULL; }
  *kind = BUILD_OK;
  Node* prog = arena_alloc(a, sizeof(Node));
  prog->kind = nkBlock; prog->line = 1; prog->str = ""; prog->source = "";
  prog->nkids = kids.n;
  prog->kids = arena_alloc(a, (kids.n ? kids.n : 1) * sizeof(Node*));
  if (kids.n) memcpy(prog->kids, kids.v, kids.n * sizeof(Node*));
  free(kids.v);
  return prog;
}
