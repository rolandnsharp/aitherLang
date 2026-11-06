/* `import "lib/voice.aither"`: a library's defs and constant tables
 * spliced into the program. Paths are relative to the
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

/* Whether `name` is file-level state in n: a `$name` in a play is the
 * file's, by name. */
static int file_state(const Node* n, const char* name) {
  if (!n || n->kind == nkDef || n->kind == nkLambda) return 0;   /* a def's own state is its own */
  if ((n->kind == nkVar || n->kind == nkAssign) && !strcmp(n->str, name)) return 1;
  for (int i = 0; i < n->nkids; i++) if (file_state(n->kids[i], name)) return 1;
  return 0;
}

/* What the patch names comes first: a stdlib constant (Hamilton's i, j,
 * k) is dropped when the patch or what it imports names it at the file's
 * level (a let, a def, state, in a play too), and a stdlib def when they
 * define one of the name, so neither collides. A parameter, a lambda's
 * `k =>` or a play's own let shadows it as any name is shadowed. */
static int shadowed(const Node* s, Kids* kids, int from) {
  if (s->kind != nkLet && s->kind != nkDef) return 0;
  for (int i = from; i < kids->n; i++) {
    const Node* u = kids->v[i];
    if (s->kind == nkDef) { if (u->kind == nkDef && !strcmp(u->str, s->str)) return 1; continue; }
    if ((u->kind == nkLet || u->kind == nkDef) && !strcmp(u->str, s->str)) return 1;
    if (file_state(u, s->str)) return 1;
  }
  return 0;
}

/* ---- fields (notes/nabla-and-medium-2026-11-06.md) ------------------------
 *
 * An array assigned bare (`$x = ...`) is a field: the stereo rule at N
 * places, arithmetic at each place. It is written here as the lines a patch
 * would write by hand: the field steps one place a sample (every = N, the
 * place from pixel_index), a bare `$x` reads this sample's place, `$x = e`
 * stores there, and later lines at that place see e. A patch never holds an
 * index or a neighbour: the words below are the only reach to one. From
 * outside, `$x[p]` reads at a point, p 0..1 round, and `$v[p] = push` adds
 * a push there, which lands as the field reaches that place. */

typedef struct {
  const char* name;
  Node* size;
  Node* init;
  const char* place;                     /* the hidden let: this sample's place */
  const char* in;                        /* the hidden array of pushes, or NULL */
  const char* now;                       /* the hidden let holding what was just stored */
  Node* period;                          /* ring(period = T): the engine owns its places, else NULL */
  const char* rate;                      /* the hidden let: its speed squared, places a second */
  const char* step;                      /* the hidden let: how long its step is */
} Field;

typedef struct { Arena* a; Field* f; int n; int made; const char* step; } Fields;

static Node* fnode(Fields* fs, NodeKind k, const char* str, const Node* at) {
  Node* n = arena_alloc(fs->a, sizeof(Node));
  n->kind = k; n->str = str ? str : ""; n->line = at->line; n->source = at->source;
  return n;
}
static void fkids(Fields* fs, Node* n, int count, Node** kids) {
  n->kids = arena_alloc(fs->a, (count ? count : 1) * sizeof(Node*));
  for (int i = 0; i < count; i++) n->kids[i] = kids[i];
  n->nkids = count;
}
static Node* fident(Fields* fs, const char* name, const Node* at) { return fnode(fs, nkIdent, name, at); }
static Node* fnum(Fields* fs, double v, const Node* at) { Node* n = fnode(fs, nkNum, "", at); n->num = v; return n; }
static Node* fcall(Fields* fs, const char* name, const Node* at, int count, Node** kids) {
  Node* n = fnode(fs, nkCall, name, at);
  fkids(fs, n, count, kids);
  return n;
}
static Node* fidx(Fields* fs, const char* array, Node* index, const Node* at) {
  Node* n = fnode(fs, nkIdx, "", at);
  fkids(fs, n, 2, (Node*[]){fident(fs, array, at), index});
  return n;
}
static Node* fbin(Fields* fs, const char* op, Node* l, Node* r, const Node* at) {
  Node* n = fnode(fs, nkBinOp, op, at);
  fkids(fs, n, 2, (Node*[]){l, r});
  return n;
}
static const char* fname(Fields* fs, const char* a, const char* b, int k) {
  char buf[256];
  snprintf(buf, sizeof buf, k >= 0 ? "__%s_%s_%d" : "__%s_%s", a, b, k);
  char* r = arena_alloc(fs->a, strlen(buf) + 1);
  strcpy(r, buf);
  return r;
}
static Field* field_named(Fields* fs, const char* name) {
  for (int i = 0; i < fs->n; i++) if (!strcmp(fs->f[i].name, name)) return &fs->f[i];
  return NULL;
}
static int make_ring(const Node* n) { return n && n->kind == nkCall && !strcmp(n->str, "ring"); }
static int make_array(const Node* n) { return n && n->kind == nkCall && (!strcmp(n->str, "array") || make_ring(n)); }
/* A ring's named argument, or its positional one. */
static Node* ring_arg(const Node* c, const char* name, int at) {
  for (int k = 0; k < c->nkids; k++)
    if (c->kids[k]->kind == nkNamedArg && !strcmp(c->kids[k]->str, name)) return c->kids[k]->kids[0];
  return at < c->nkids && c->kids[at]->kind != nkNamedArg ? c->kids[at] : NULL;
}

/* A bare field as a word's argument, and the word it needs. */
static Field* bare_field(Fields* fs, const Node* n) { return n && n->kind == nkIdent ? field_named(fs, n->str) : NULL; }

/* A neighbour word on a ring carries the ring's speed. */
static Node* at_speed(Fields* fs, Field* f, Node* word, const Node* at) {
  return f->period ? fbin(fs, "*", word, fident(fs, f->rate, at), at) : word;
}

static Node* field_expr(Fields* fs, Node* n) {
  if (!n) return n;
  if (n->kind == nkIdent) {
    if (fs->step && !strcmp(n->str, "dt")) return fident(fs, fs->step, n);   /* in a ring's line, its step */
    Field* f = field_named(fs, n->str);
    if (!f) return n;
    return f->now ? fident(fs, f->now, n) : fidx(fs, f->name, fident(fs, f->place, n), n);
  }
  if (n->kind == nkIdx && bare_field(fs, n->kids[0])) {       /* read at a point */
    return fcall(fs, "field_at", n, 2, (Node*[]){n->kids[0], field_expr(fs, n->kids[1])});
  }
  if (n->kind == nkIdxAssign && bare_field(fs, n->kids[0])) { /* a push at a point */
    Field* f = bare_field(fs, n->kids[0]);
    Node* at = fcall(fs, "field_place", n, 2, (Node*[]){field_expr(fs, n->kids[1]), f->size});
    Node* push = field_expr(fs, n->kids[2]);
    if (f->period)                                            /* a rate per length of ring: the grain falls out */
      push = fbin(fs, "*", push, fbin(fs, "*", f->size, fident(fs, "dt", n), n), n);
    Node* sum = fbin(fs, "+", fidx(fs, f->in, at, n), push, n);
    Node* r = fnode(fs, nkIdxAssign, "", n);
    fkids(fs, r, 3, (Node*[]){fident(fs, f->in, n), at, sum});
    return r;
  }
  if (n->kind == nkCall) {
    if (!strcmp(n->str, "len")) return n;
    Field* f = n->nkids == 1 ? bare_field(fs, n->kids[0]) : NULL;
    if (f && !strcmp(n->str, "concentration"))
      return at_speed(fs, f, fcall(fs, "concentration", n, 2, (Node*[]){n->kids[0], fident(fs, f->place, n)}), n);
    Node* in = n->nkids == 1 ? n->kids[0] : NULL;
    f = in && in->kind == nkCall && in->nkids == 1 ? bare_field(fs, in->kids[0]) : NULL;
    if (f && !strcmp(n->str, "slope") && !strcmp(in->str, "convergence"))
      return at_speed(fs, f, fcall(fs, "slope_of_convergence", n, 2, (Node*[]){in->kids[0], fident(fs, f->place, n)}), n);
    if (f && !strcmp(n->str, "curl") && !strcmp(in->str, "curl"))
      return at_speed(fs, f, fcall(fs, "curl_of_curl", n, 2, (Node*[]){in->kids[0], fident(fs, f->place, n)}), n);
  }
  for (int i = 0; i < n->nkids; i++) n->kids[i] = field_expr(fs, n->kids[i]);
  return n;
}

/* A list of statements, as the lines they stand for. */
static void field_stmts(Fields* fs, Node** in, int n, Kids* out, int top) {
  for (int i = 0; i < n; i++) {
    Node* s = in[i];
    Field* f = (s->kind == nkVar || s->kind == nkAssign) ? field_named(fs, s->str) : NULL;
    if (f && make_array(s->kids[0])) {                       /* its declaration */
      Node* c = s->kids[0];
      int named = 0;
      for (int k = 0; k < c->nkids; k++) if (c->kids[k]->kind == nkNamedArg) named = 1;
      if (f->period) {                                       /* the engine's places, stepped once through */
        c->str = "array";
        fkids(fs, c, 3, (Node*[]){f->size, f->init ? f->init : fnum(fs, 0, c), f->size});
      } else if (!named && c->nkids < 3) {
        Node* init = c->nkids == 2 ? c->kids[1] : fnum(fs, 0, c);
        fkids(fs, c, 3, (Node*[]){c->kids[0], init, c->kids[0]});
      }
      kid(out, s);
      Node* place = fnode(fs, nkLet, f->place, s);
      fkids(fs, place, 1, (Node*[]){fcall(fs, "pixel_index", s, 2, (Node*[]){f->size, fnum(fs, 1, s)})});
      kid(out, place);
      if (f->period) {
        Node* rate = fnode(fs, nkLet, f->rate, s);           /* (N / T)^2: a wave goes round in T */
        Node* speed = fbin(fs, "/", f->size, f->period, s);
        fkids(fs, rate, 1, (Node*[]){fbin(fs, "*", speed, speed, s)});
        kid(out, rate);
        Node* step = fnode(fs, nkLet, f->step, s);
        fkids(fs, step, 1, (Node*[]){fbin(fs, "*", f->size, fident(fs, "dt", s), s)});
        kid(out, step);
      }
      if (f->in) {
        Node* zero = f->init ? fbin(fs, "*", f->init, fnum(fs, 0, s), s) : fnum(fs, 0, s);
        Node* pushes = fnode(fs, nkVar, f->in, s);
        fkids(fs, pushes, 1, (Node*[]){fcall(fs, "array", s, 2, (Node*[]){f->size, zero})});
        kid(out, pushes);
      }
      continue;
    }
    if (f) {                                                 /* `$x = e`, at this place */
      fs->step = f->period ? f->step : NULL;
      Node* value = field_expr(fs, s->kids[0]);
      fs->step = NULL;
      Node* here = fident(fs, f->place, s);
      if (f->in) value = fbin(fs, "+", value, fidx(fs, f->in, here, s), s);
      const char* now = fname(fs, "now", f->name, fs->made++);
      Node* let = fnode(fs, nkLet, now, s);
      fkids(fs, let, 1, (Node*[]){value});
      kid(out, let);
      Node* store = fnode(fs, nkIdxAssign, "", s);
      fkids(fs, store, 3, (Node*[]){fident(fs, f->name, s), here, fident(fs, now, s)});
      kid(out, store);
      if (f->in) {
        Node* clear = fnode(fs, nkIdxAssign, "", s);
        fkids(fs, clear, 3, (Node*[]){fident(fs, f->in, s), here, fnum(fs, 0, s)});
        kid(out, clear);
      }
      f->now = now;
      continue;
    }
    if (s->kind == nkPlay && s->kids[0] && s->kids[0]->kind == nkBlock) {
      Node* body = s->kids[0];
      Kids inner = {0};
      field_stmts(fs, body->kids, body->nkids, &inner, 0);
      fkids(fs, body, inner.n, inner.v);
      free(inner.v);
      for (int k = 0; k < fs->n; k++) fs->f[k].now = NULL;    /* what a play stored is its own */
      kid(out, s);
      continue;
    }
    if (s->kind == nkDef) { kid(out, s); continue; }
    kid(out, field_expr(fs, s));
  }
  (void)top;
}

/* Every array assigned bare, in the top level and the plays. */
static void find_fields(Fields* fs, Node** in, int n) {
  for (int i = 0; i < n; i++) {
    Node* s = in[i];
    if (s->kind == nkPlay && s->kids[0] && s->kids[0]->kind == nkBlock) find_fields(fs, s->kids[0]->kids, s->kids[0]->nkids);
    if ((s->kind == nkVar || s->kind == nkAssign) && s->nkids && !make_array(s->kids[0])) {
      Field* f = field_named(fs, s->str);
      if (f) f->place = fname(fs, "place", f->name, -1);
    }
  }
}
/* Whether a store by index reaches a field from outside. */
static void find_pushes(Fields* fs, const Node* n) {
  if (!n) return;
  if (n->kind == nkDef) return;
  if (n->kind == nkIdxAssign && bare_field(fs, n->kids[0])) {
    Field* f = bare_field(fs, n->kids[0]);
    f->in = fname(fs, "pushes", f->name, -1);
  }
  for (int i = 0; i < n->nkids; i++) find_pushes(fs, n->kids[i]);
}

static void fields_pass(Arena* a, Kids* kids, int from) {
  Fields fs = {a, NULL, 0, 0};
  fs.f = arena_alloc(a, (kids->n + 1) * sizeof(Field));
  for (int i = from; i < kids->n; i++) {                     /* arrays at the file's level */
    Node* s = kids->v[i];
    if (s->kind == nkVar && make_array(s->kids[0]) && s->kids[0]->nkids >= 1 && !field_named(&fs, s->str)) {
      Field* f = &fs.f[fs.n++];
      memset(f, 0, sizeof *f);
      f->name = s->str; f->size = s->kids[0]->kids[0];
      f->init = s->kids[0]->nkids >= 2 ? s->kids[0]->kids[1] : NULL;
      if (make_ring(s->kids[0])) {
        f->period = ring_arg(s->kids[0], "period", 0);
        f->init = ring_arg(s->kids[0], "start", 1);
        f->size = fcall(&fs, "field_places", s, 1, (Node*[]){f->period});
        f->place = fname(&fs, "place", f->name, -1);
        f->rate = fname(&fs, "rate", f->name, -1);
        f->step = fname(&fs, "step", f->name, -1);
      }
    }
  }
  find_fields(&fs, kids->v + from, kids->n - from);
  int kept = 0;                                              /* only what is assigned bare is a field */
  for (int i = 0; i < fs.n; i++) if (fs.f[i].place) fs.f[kept++] = fs.f[i];
  fs.n = kept;
  if (!fs.n) return;
  for (int i = from; i < kids->n; i++) find_pushes(&fs, kids->v[i]);
  Kids out = {0};
  field_stmts(&fs, kids->v + from, kids->n - from, &out, 1);
  kids->n = from;
  for (int i = 0; i < out.n; i++) kid(kids, out.v[i]);
  free(out.v);
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
  int kept = 0;
  for (int i = 0; i < std->nkids; i++)
    if (!shadowed(kids.v[i], &kids, std->nkids)) kids.v[kept++] = kids.v[i];
  memmove(kids.v + kept, kids.v + std->nkids, (kids.n - std->nkids) * sizeof(Node*));
  kids.n -= std->nkids - kept;
  fields_pass(a, &kids, kept);
  *kind = BUILD_OK;
  Node* prog = arena_alloc(a, sizeof(Node));
  prog->kind = nkBlock; prog->line = 1; prog->str = ""; prog->source = "";
  prog->nkids = kids.n;
  prog->kids = arena_alloc(a, (kids.n ? kids.n : 1) * sizeof(Node*));
  if (kids.n) memcpy(prog->kids, kids.v, kids.n * sizeof(Node*));
  free(kids.v);
  return prog;
}
