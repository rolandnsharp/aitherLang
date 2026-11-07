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
 * outside, `$x[p]` reads at a point, p 0..1 round, and `$v'[p] += push`
 * adds to a ring's rate there (`$v[p] += push`, an array's value), landing
 * as the field reaches that place. */

typedef struct {
  const char* name;
  Node* size;
  Node* init;
  const char* place;                     /* the hidden let: this sample's place */
  const char* in;                        /* the hidden array of pushes, or NULL */
  const char* now;                       /* the hidden let holding what was just stored */
  Node* period;                          /* ring(period = T): the engine owns its places, else NULL */
  const char* rate;                      /* the hidden let: its speed squared, places a round */
  const char* step;                      /* the hidden let: a step, in rounds */
  int law;                               /* it has a law */
  int stored;                            /* it has a line `$x = e` */
  const Node* pushed;                    /* where it was first pushed, or NULL */
  void* reads;                           /* the field its law reads through `slope`, or NULL */
  int trails;                            /* of a pair: it steps a place behind the other */
  int ends[2];                           /* a line's: 1 fixed, 2 free; a ring's are 0 */
  double extra;                          /* a line: how much longer than its places */
  double off;                            /* ... and how far along its first place stands */
  const char* speed;                     /* the hidden let: places a round */
  const char* length;                    /* the hidden let, a line's: its length in places */
  const char* grain;                     /* the hidden let: a push's step */
} Field;

typedef struct { Arena* a; Field* f; int n; int made; int in_ring; char* err; int errlen; int failed; int in_def; int own_gauss; Field* law; const Node* pair_at; } Fields;

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
static int make_line(const Node* n) { return n && n->kind == nkCall && !strcmp(n->str, "line"); }
static int make_ring(const Node* n) { return n && n->kind == nkCall && (!strcmp(n->str, "ring") || make_line(n)); }
static int make_array(const Node* n) { return n && n->kind == nkCall && (!strcmp(n->str, "array") || make_ring(n)); }
/* A ring's named argument, or its positional one. */
static Node* ring_arg(const Node* c, const char* name, int at) {
  for (int k = 0; k < c->nkids; k++)
    if (c->kids[k]->kind == nkNamedArg && !strcmp(c->kids[k]->str, name)) return c->kids[k]->kids[0];
  return at < c->nkids && c->kids[at]->kind != nkNamedArg ? c->kids[at] : NULL;
}

/* A bare field as a word's argument, and the word it needs. */
static Field* bare_field(Fields* fs, const Node* n) { return n && n->kind == nkIdent ? field_named(fs, n->str) : NULL; }

/* `$x'`, a rate: whether a name is one, and its field, which must be a ring's. */
static int primed(const char* name) {
  size_t n = strlen(name);
  return n && name[n - 1] == '\'';
}
static Field* rate_of(Fields* fs, const char* name) {
  char plain[256];
  snprintf(plain, sizeof plain, "%.*s", (int)strlen(name) - 1, name);
  Field* f = field_named(fs, plain);
  return f && f->period ? f : NULL;
}
static Field* pushed_field(Fields* fs, const Node* target) {
  if (target->kind != nkIdent) return NULL;
  return primed(target->str) ? rate_of(fs, target->str) : field_named(fs, target->str);
}
static void field_fail(Fields* fs, const Node* n, const char* said) {
  if (fs->failed) return;
  fs->failed = 1;
  snprintf(fs->err, fs->errlen, "%s (%s:%d)", said, n->source, n->line);
}

/* A neighbour word on a ring carries the ring's speed. */
static Node* at_speed(Fields* fs, Field* f, Node* word, const Node* at) {
  return f->period ? fbin(fs, "*", word, fident(fs, f->rate, at), at) : word;
}

static Node* field_expr(Fields* fs, Node* n) {
  if (!n) return n;
  if (n->kind == nkIdent) {
    if (fs->in_ring && (!strcmp(n->str, "dt") || !strcmp(n->str, "t")) && !fs->failed) {
      fs->failed = 1;
      snprintf(fs->err, fs->errlen, "`%s` in a ring's line (%s:%d): a field has no time, only what changes. Say "
               "what changes, `$x' = $v`, and its rate in rounds; a second belongs at the edge, in `period`",
               n->str, n->source, n->line);
    }
    Field* f = field_named(fs, n->str);
    if (!f) return n;
    return f->now ? fident(fs, f->now, n) : fidx(fs, f->name, fident(fs, f->place, n), n);
  }
  if (n->kind == nkIdx && bare_field(fs, n->kids[0])) {       /* read at a point */
    Field* f = bare_field(fs, n->kids[0]);
    if (f->length)
      return fcall(fs, "line_at", n, 6, (Node*[]){n->kids[0], field_expr(fs, n->kids[1]), fident(fs, f->length, n),
                                                 fnum(fs, f->off, n), fnum(fs, f->ends[0], n), fnum(fs, f->ends[1], n)});
    return fcall(fs, "field_at", n, 2, (Node*[]){n->kids[0], field_expr(fs, n->kids[1])});
  }
  if (n->kind == nkIdxAssign && pushed_field(fs, n->kids[0])) { /* a push at a point */
    Field* f = pushed_field(fs, n->kids[0]);
    Node* said = n->kids[2];
    int adds = said->kind == nkBinOp && said->num == 1.0 && (!strcmp(said->str, "+") || !strcmp(said->str, "-"));
    int rate = primed(n->kids[0]->str);
    char why[200];
    if (f->period && !rate) {
      snprintf(why, sizeof why, "a push on a ring is a change to its rate: `$%s'[p] += ...`", f->name);
      field_fail(fs, n, why);
    } else if (!adds) {
      snprintf(why, sizeof why, "a push adds: `$%s%s[p] += ...`", f->name, rate ? "'" : "");
      field_fail(fs, n, why);
    }
    if (fs->failed) return n;
    Node* at = f->length
      ? fcall(fs, "line_place", n, 4, (Node*[]){field_expr(fs, n->kids[1]), fident(fs, f->length, n), fnum(fs, f->off, n), f->size})
      : fcall(fs, "field_place", n, 2, (Node*[]){field_expr(fs, n->kids[1]), f->size});
    Node* push = field_expr(fs, said->kids[1]);
    if (f->period)                                            /* a change a round, per length of ring: the grain falls out */
      push = fbin(fs, "*", push, fident(fs, f->grain, n), n);
    Node* sum = fbin(fs, said->str, fidx(fs, f->in, at, n), push, n);
    Node* r = fnode(fs, nkIdxAssign, "", n);
    fkids(fs, r, 3, (Node*[]){fident(fs, f->in, n), at, sum});
    return r;
  }
  if (n->kind == nkCall) {
    if (!strcmp(n->str, "len")) return n;
    Field* f = n->nkids == 1 ? bare_field(fs, n->kids[0]) : NULL;
    if (f && !strcmp(n->str, "slope")) {                      /* of a pair: where they meet, a half place between */
      if (!fs->law) {
        field_fail(fs, n, "`slope($x)` is a law's word: it says how one of a pair changes the other");
        return n;
      }
      Node* word = f->trails                                  /* the leader reads behind it, the trailer the fresh place ahead */
        ? fcall(fs, "slope_behind", n, 3, (Node*[]){n->kids[0], fident(fs, fs->law->place, n), fnum(fs, f->ends[0], n)})
        : fcall(fs, "slope_ahead", n, 4, (Node*[]){n->kids[0], fident(fs, f->now, n), fident(fs, fs->law->place, n), fnum(fs, f->ends[1], n)});
      return fbin(fs, "*", word, fident(fs, f->speed, n), n);
    }
    if (f && !strcmp(n->str, "concentration"))
      return at_speed(fs, f, fcall(fs, "concentration", n, 4, (Node*[]){n->kids[0], fident(fs, f->place, n), fnum(fs, f->ends[0], n), fnum(fs, f->ends[1], n)}), n);
    Node* in = n->nkids == 1 ? n->kids[0] : NULL;
    f = in && in->kind == nkCall && in->nkids == 1 ? bare_field(fs, in->kids[0]) : NULL;
    if (f && !strcmp(n->str, "slope") && !strcmp(in->str, "convergence"))
      return at_speed(fs, f, fcall(fs, "slope_of_convergence", n, 4, (Node*[]){in->kids[0], fident(fs, f->place, n), fnum(fs, f->ends[0], n), fnum(fs, f->ends[1], n)}), n);
    if (f && !strcmp(n->str, "curl") && !strcmp(in->str, "curl"))
      return at_speed(fs, f, fcall(fs, "curl_of_curl", n, 4, (Node*[]){in->kids[0], fident(fs, f->place, n), fnum(fs, f->ends[0], n), fnum(fs, f->ends[1], n)}), n);
  }
  for (int i = 0; i < n->nkids; i++) n->kids[i] = field_expr(fs, n->kids[i]);
  return n;
}

/* The line that stores `value` at this place, with whatever pushes landed. */
static void field_store(Fields* fs, Field* f, const Node* s, Node* value, Kids* out) {
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
}

/* `$x' = e`: a law. Its name less the prime is its field. */
static int is_law(const Node* s) { return (s->kind == nkVar || s->kind == nkAssign) && primed(s->str); }
static Field* law_field(Fields* fs, const Node* s) { return rate_of(fs, s->str); }
/* Fields whose neighbours a law reads: `reached[k]` for the k-th. */
static void find_reached(Fields* fs, const Node* n, char* reached) {
  if (!n) return;
  if (n->kind == nkCall && n->nkids == 1 && bare_field(fs, n->kids[0]) &&
      (!strcmp(n->str, "concentration") || !strcmp(n->str, "convergence") || !strcmp(n->str, "curl") ||
       !strcmp(n->str, "slope")))
    reached[bare_field(fs, n->kids[0]) - fs->f] = 1;
  for (int i = 0; i < n->nkids; i++) find_reached(fs, n->kids[i], reached);
}
/* A block's laws are one system, so the order they're written in means
 * nothing: they are stepped together, where the last of them stands. What
 * moves on what its neighbours do is stepped last, on the rates just found,
 * as force comes before motion. A pair read through `slope` staggers: the
 * one declared first steps at this place, the other a place behind it, so
 * each sees the other fresh on both sides. */
static void step_laws(Fields* fs, Node** in, int n, Kids* out) {
  char* reached = arena_alloc(fs->a, fs->n + 1);
  memset(reached, 0, fs->n + 1);
  for (int i = 0; i < n; i++) if (is_law(in[i])) find_reached(fs, in[i]->kids[0], reached);
  for (int last = 0; last < 2; last++)
    for (int k = 0; k < fs->n; k++) {
      if (reached[k] != last) continue;
      Field* f = &fs->f[k];
      for (int i = 0; i < n; i++) {
        if (!is_law(in[i]) || law_field(fs, in[i]) != f) continue;
        Node* s = in[i];
        fs->in_ring = 1;
        fs->law = f;
        Node* change = fbin(fs, "*", field_expr(fs, s->kids[0]), fident(fs, f->step, s), s);
        fs->in_ring = 0;
        fs->law = NULL;
        field_store(fs, f, s, fbin(fs, "+", field_expr(fs, fident(fs, f->name, s)), change, s), out);
      }
    }
}

/* `$v'[gauss(p, w)] += force`: a source with a shape. */
static int is_gauss(Fields* fs, const Node* n) {
  return !fs->own_gauss && n->kind == nkCall && !strcmp(n->str, "gauss") && n->nkids == 2;
}
static int gauss_source(Fields* fs, const Node* s) {
  if (s->kind != nkIdxAssign || s->kids[0]->kind != nkIdent || !primed(s->kids[0]->str)) return 0;
  const Node* said = s->kids[2];
  return rate_of(fs, s->kids[0]->str) && is_gauss(fs, s->kids[1]) && said->kind == nkBinOp && said->num == 1.0 &&
         (!strcmp(said->str, "+") || !strcmp(said->str, "-"));
}
static Node* fstore(Fields* fs, const char* array, Node* at, Node* value, const Node* s) {
  Node* r = fnode(fs, nkIdxAssign, "", s);
  fkids(fs, r, 3, (Node*[]){fident(fs, array, s), at, value});
  return r;
}
static const char* flet(Fields* fs, const char* what, const char* name, int k, Node* value, const Node* s, Kids* out) {
  const char* let = fname(fs, what, name, k);
  Node* n = fnode(fs, nkLet, let, s);
  fkids(fs, n, 1, (Node*[]){value});
  kid(out, n);
  return let;
}
/* The ring reaches each place once a sweep, so a shape can only be laid
 * down as it does, and a blow shorter than a sweep would miss most of it.
 * So the source keeps a running total of the force, and each place gets all
 * that came since the ring last passed, in the shape. It goes on the place
 * coming next, the same whichever side of the law the line stands. The
 * shape is held as the force stops: what's still to land was struck there,
 * wherever the centre has gone since. */
static void shaped_source(Fields* fs, const Node* s, Kids* out) {
  Field* f = rate_of(fs, s->kids[0]->str);
  Node* shape = s->kids[1];
  Node* said = s->kids[2];
  int k = fs->made++;
  Node* force = field_expr(fs, said->kids[1]);
  if (!strcmp(said->str, "-")) force = fbin(fs, "*", force, fnum(fs, -1, s), s);
  const char* blow = flet(fs, "blow", f->name, k, force, s, out);
  const char* striking = flet(fs, "striking", f->name, k,
    fcall(fs, "field_striking", s, 1, (Node*[]){fident(fs, blow, s)}), s, out);
  const char* held[2];
  for (int i = 0; i < 2; i++) {
    held[i] = fname(fs, i ? "held_width" : "held_centre", f->name, k);
    Node* v = fnode(fs, nkVar, held[i], s);
    fkids(fs, v, 1, (Node*[]){fnum(fs, 0, s)});
    kid(out, v);
    Node* pick = fnode(fs, nkIf, "", s);
    fkids(fs, pick, 3, (Node*[]){fident(fs, striking, s), field_expr(fs, shape->kids[i]), fident(fs, held[i], s)});
    v = fnode(fs, nkAssign, held[i], s);
    fkids(fs, v, 1, (Node*[]){pick});
    kid(out, v);
  }
  const char* total = fname(fs, "total", f->name, k);
  const char* then = fname(fs, "then", f->name, k);
  Node* zero = f->init ? fbin(fs, "*", f->init, fnum(fs, 0, s), s) : fnum(fs, 0, s);
  Node* n = fnode(fs, nkVar, total, s);
  fkids(fs, n, 1, (Node*[]){zero});
  kid(out, n);
  n = fnode(fs, nkVar, then, s);
  fkids(fs, n, 1, (Node*[]){fcall(fs, "array", s, 2, (Node*[]){f->size, zero})});
  kid(out, n);
  n = fnode(fs, nkAssign, total, s);
  fkids(fs, n, 1, (Node*[]){fbin(fs, "+", fident(fs, total, s), fbin(fs, "*", fident(fs, blow, s), fident(fs, "dt", s), s), s)});
  kid(out, n);
  const char* caught = flet(fs, "caught", f->name, k,
    fbin(fs, "/", fbin(fs, "-", fident(fs, total, s), fidx(fs, then, fident(fs, f->place, s), s), s), f->period, s), s, out);
  kid(out, fstore(fs, then, fident(fs, f->place, s), fident(fs, total, s), s));
  const char* next = flet(fs, "next", f->name, k,
    fbin(fs, "mod", fbin(fs, "+", fident(fs, f->place, s), fnum(fs, 1, s), s), f->size, s), s, out);
  Node* along = f->length ? fbin(fs, "/", fbin(fs, "+", fident(fs, next, s), fnum(fs, f->off, s), s), fident(fs, f->length, s), s)
                          : fbin(fs, "/", fident(fs, next, s), f->size, s);
  Node* from = fbin(fs, "-", along, fident(fs, held[0], s), s);
  Node* laid = fbin(fs, "*", fident(fs, caught, s), f->length
    ? fcall(fs, "line_gauss", s, 3, (Node*[]){from, fident(fs, held[1], s), fident(fs, f->length, s)})
    : fcall(fs, "field_gauss", s, 3, (Node*[]){from, fident(fs, held[1], s), f->size}), s);
  kid(out, fstore(fs, f->in, fident(fs, next, s), fbin(fs, "+", fidx(fs, f->in, fident(fs, next, s), s), laid, s), s));
}

/* A list of statements, as the lines they stand for. */
static void field_stmts(Fields* fs, Node** in, int n, Kids* out, int top) {
  int laws_end = -1;
  for (int i = 0; i < n; i++) if (is_law(in[i])) laws_end = i;
  for (int i = 0; i < n; i++) {
    Node* s = in[i];
    if (is_law(s)) {
      if (i == laws_end) step_laws(fs, in, n, out);
      continue;
    }
    if (gauss_source(fs, s)) { shaped_source(fs, s, out); continue; }
    Field* f = (s->kind == nkVar || s->kind == nkAssign) ? field_named(fs, s->str) : NULL;
    if (f && make_array(s->kids[0])) {                       /* its declaration */
      Node* c = s->kids[0];
      int named = 0;
      for (int k = 0; k < c->nkids; k++) if (c->kids[k]->kind == nkNamedArg) named = 1;
      if (f->period) {                                       /* the engine's places, stepped once through */
        c->str = "array";
        /* A sweep's stores show at its end, so a law sees its neighbours as a
         * sweep left them; a pair steps in place, each seeing the other fresh. */
        fkids(fs, c, f->reads ? 2 : 3, (Node*[]){f->size, f->init ? f->init : fnum(fs, 0, c), f->size});
      } else if (!named && c->nkids < 3) {
        Node* init = c->nkids == 2 ? c->kids[1] : fnum(fs, 0, c);
        fkids(fs, c, 3, (Node*[]){c->kids[0], init, c->kids[0]});
      }
      kid(out, s);
      Node* place = fnode(fs, nkLet, f->place, s);
      Node* index = fcall(fs, "pixel_index", s, 2, (Node*[]){f->size, fnum(fs, 1, s)});
      if (f->trails)
        index = fbin(fs, "mod", fbin(fs, "+", index, fbin(fs, "-", f->size, fnum(fs, 1, s), s), s), f->size, s);
      fkids(fs, place, 1, (Node*[]){index});
      kid(out, place);
      if (f->period) {
        if (f->length) {                                     /* there and back in a round */
          Node* length = fnode(fs, nkLet, f->length, s);
          fkids(fs, length, 1, (Node*[]){fbin(fs, "+", f->size, fnum(fs, f->extra, s), s)});
          kid(out, length);
        }
        Node* speed = fnode(fs, nkLet, f->speed, s);
        fkids(fs, speed, 1, (Node*[]){f->length ? fbin(fs, "*", fident(fs, f->length, s), fnum(fs, 2, s), s) : f->size});
        kid(out, speed);
        Node* rate = fnode(fs, nkLet, f->rate, s);           /* N^2: a wave goes round in one round */
        fkids(fs, rate, 1, (Node*[]){fbin(fs, "*", fident(fs, f->speed, s), fident(fs, f->speed, s), s)});
        kid(out, rate);
        Node* step = fnode(fs, nkLet, f->step, s);           /* a step, in rounds: N samples of T */
        fkids(fs, step, 1, (Node*[]){fbin(fs, "/", fbin(fs, "*", f->size, fident(fs, "dt", s), s), f->period, s)});
        kid(out, step);
        Node* grain = fnode(fs, nkLet, f->grain, s);         /* a push lands as a delta: a place's height, of its length */
        fkids(fs, grain, 1, (Node*[]){f->length ? fbin(fs, "/", fbin(fs, "*", fident(fs, f->length, s), fident(fs, "dt", s), s), f->period, s)
                                                : fident(fs, f->step, s)});
        kid(out, grain);
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
      fs->in_ring = f->period != NULL;
      Node* rhs = s->kids[0];
      if (f->period && rhs->kind == nkBinOp && rhs->num == 1.0 && !fs->failed) {        /* written `+=` */
        fs->failed = 1;
        snprintf(fs->err, fs->errlen, "`%s=` on a ring (%s:%d): say what changes, as its rate: `$%s' = ...`", rhs->str,
                 s->source, s->line, f->name);
      }
      Node* value = field_expr(fs, rhs);
      fs->in_ring = 0;
      field_store(fs, f, s, value, out);
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
      if (f) { f->place = fname(fs, "place", f->name, -1); f->stored = 1; }
    }
  }
}
/* Whether a store by index reaches a field from outside. */
static void find_pushes(Fields* fs, const Node* n) {
  if (!n) return;
  if (n->kind == nkDef) return;
  if (n->kind == nkIdxAssign && pushed_field(fs, n->kids[0])) {
    Field* f = pushed_field(fs, n->kids[0]);
    f->in = fname(fs, "pushes", f->name, -1);
    if (!f->pushed) f->pushed = n;
  }
  for (int i = 0; i < n->nkids; i++) find_pushes(fs, n->kids[i]);
}

/* What a law reads through `slope`. */
static void find_slopes(Fields* fs, const Node* n, Field* law) {
  if (!n || fs->failed) return;
  if (n->kind == nkCall && !strcmp(n->str, "slope") && n->nkids == 1 && bare_field(fs, n->kids[0])) {
    Field* f = bare_field(fs, n->kids[0]);
    if (law->reads && law->reads != f) field_fail(fs, n, "`slope` joins two fields: a law reads the slope of one partner");
    law->reads = f;
  }
  for (int i = 0; i < n->nkids; i++) find_slopes(fs, n->kids[i], law);
}
static int same_tree(const Node* a, const Node* b) {
  if (!a || !b) return a == b;
  if (a->kind != b->kind || a->num != b->num || strcmp(a->str, b->str) || a->nkids != b->nkids) return 0;
  for (int i = 0; i < a->nkids; i++) if (!same_tree(a->kids[i], b->kids[i])) return 0;
  return 1;
}
/* Two fields each read through the other's slope are a pair. */
static void find_pairs(Fields* fs, const Node* at) {
  for (int i = 0; i < fs->n; i++) {
    Field* f = &fs->f[i];
    Field* g = f->reads;
    if (!g) continue;
    char why[200];
    if (g == f || g->reads != f) {
      snprintf(why, sizeof why, "`slope($%s)` in `$%s'`'s law needs its partner: `$%s' = ... slope($%s) ...`",
               g->name, f->name, g->name, f->name);
      field_fail(fs, at, why);
    } else if (!same_tree(f->period, g->period) || !f->length != !g->length) {
      snprintf(why, sizeof why, "`$%s` and `$%s` are a pair: they share one period, on one ring or line", f->name, g->name);
      field_fail(fs, at, why);
    } else if (f->length && (f->ends[0] + g->ends[0] != 3 || f->ends[1] + g->ends[1] != 3)) {
      snprintf(why, sizeof why, "`$%s` and `$%s` are a pair: at each end one is fixed and the other free", f->name, g->name);
      field_fail(fs, at, why);
    } else if (g > f) g->trails = 1;
  }
}

/* Where a line's places stand. Past a fixed end the field is 0 a whole
 * place on, past a free one it mirrors, half a place on. A pair stands
 * staggered: its second a half place on from the first, and only the first's
 * ends say how long it is. */
static void place_lines(Fields* fs) {
  for (int i = 0; i < fs->n; i++) {
    Field* f = &fs->f[i];
    if (!f->length) continue;
    int l = f->ends[0] == 1, r = f->ends[1] == 1;
    if (f->reads) {
      Field* lead = f->trails ? f->reads : f;
      l = lead->ends[0] == 1; r = lead->ends[1] == 1;
      f->extra = (r ? 0 : -0.5) + (l ? 0 : 0.5);
      f->off = (l ? 0 : 0.5) + (f->trails ? 0.5 : 0);
    } else {
      f->extra = 0.5 * (l + r);
      f->off = l ? 1 : 0.5;
    }
  }
}

/* A law stands in a file's or a play's lines, once a field, for a ring. */
static void check_laws(Fields* fs, Node* n, int stands, char* said) {
  if (!n || fs->failed) return;
  if (is_law(n)) {
    Field* f = law_field(fs, n);
    const char* wrong = !stands ? "a law belongs in the file's lines or a play's, not in a def" :
                        !f ? "a rate is of a ring's field: `$x = ring(period = 1)`" :
                        said[f - fs->f] ? "one law a field: say all that changes it in one line" : NULL;
    if (wrong) {
      fs->failed = 1;
      snprintf(fs->err, fs->errlen, "`$%s` (%s:%d): %s", n->str, n->source, n->line, wrong);
      return;
    }
    said[f - fs->f] = 1;
    f->law = 1;
    find_slopes(fs, n->kids[0], f);
    if (!fs->pair_at) fs->pair_at = n;
  }
  if (n->kind == nkIdxAssign && n->kids[0]->kind == nkIdent && primed(n->kids[0]->str)) {
    if (!rate_of(fs, n->kids[0]->str)) field_fail(fs, n, "a rate is of a ring's field: `$x = ring(period = 1)`");
    else if (fs->in_def) field_fail(fs, n, "a source belongs in the file's lines or a play's, not in a def");
  }
  if (is_gauss(fs, n)) field_fail(fs, n, "`gauss(p, width)` is a shape: it belongs in a source's brackets, `$v'[gauss(p, width)] += force`");
  if (n->kind == nkIdxAssign && n->kids[0]->kind == nkIdent && primed(n->kids[0]->str) && is_gauss(fs, n->kids[1])) {
    for (int i = 0; i < n->kids[1]->nkids; i++) check_laws(fs, n->kids[1]->kids[i], 0, said);   /* the one place it stands */
    const Node* v = n->kids[2];
    check_laws(fs, v->kind == nkBinOp && v->num == 1.0 ? v->kids[1] : n->kids[2], 0, said);
    return;
  }
  if (n->kind == nkDef) {
    fs->in_def++;
    for (int i = 0; i < n->nkids; i++) check_laws(fs, n->kids[i], 0, said);
    fs->in_def--;
    return;
  }
  if (n->kind == nkPlay && n->kids[0] && n->kids[0]->kind == nkBlock) {
    char* own = arena_alloc(fs->a, fs->n + 1);
    memset(own, 0, fs->n + 1);
    for (int i = 0; i < n->kids[0]->nkids; i++) check_laws(fs, n->kids[0]->kids[i], 1, own);
    return;
  }
  for (int i = 0; i < n->nkids; i++) check_laws(fs, n->kids[i], 0, said);
}

static int fields_pass(Arena* a, Kids* kids, int from, char* err, int errlen) {
  Fields fs = {a, NULL, 0, 0, 0, err, errlen, 0};
  fs.f = arena_alloc(a, (kids->n + 1) * sizeof(Field));
  for (int i = from; i < kids->n; i++) if (kids->v[i]->kind == nkDef && !strcmp(kids->v[i]->str, "gauss")) fs.own_gauss = 1;
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
        Node* ends = ring_arg(s->kids[0], "ends", 99);
        if (make_line(s->kids[0])) {                          /* there and back in a period, so across in half */
          f->size = fcall(&fs, "field_places", s, 1, (Node*[]){fbin(&fs, "/", f->period, fnum(&fs, 2, s), s)});
          f->length = fname(&fs, "length", f->name, -1);
          if (!ends) field_fail(&fs, s, "a line says its ends: `line(period = 2, ends = fixed)`, or `ends = [fixed, free]`");
          else for (int e = 0; e < 2; e++) {
            const Node* end = ends->kind == nkArr && ends->nkids == 2 ? ends->kids[e] : ends;
            f->ends[e] = end->kind != nkIdent ? 0 : !strcmp(end->str, "fixed") ? 1 : !strcmp(end->str, "free") ? 2 : 0;
            if (!f->ends[e]) field_fail(&fs, s, "an end is `fixed` or `free`, and a line has two: `ends = [fixed, free]`");
          }
        } else {
          f->size = fcall(&fs, "field_places", s, 1, (Node*[]){f->period});
          if (ends) field_fail(&fs, s, "a ring has no ends: `line(period = 2, ends = fixed)` has");
        }
        f->place = fname(&fs, "place", f->name, -1);
        f->rate = fname(&fs, "rate", f->name, -1);
        f->step = fname(&fs, "step", f->name, -1);
        f->speed = fname(&fs, "speed", f->name, -1);
        f->grain = fname(&fs, "grain", f->name, -1);
      }
    }
  }
  find_fields(&fs, kids->v + from, kids->n - from);
  int kept = 0;                                              /* only what is assigned bare is a field */
  for (int i = 0; i < fs.n; i++) if (fs.f[i].place) fs.f[kept++] = fs.f[i];
  fs.n = kept;
  char* said = arena_alloc(a, fs.n + 1);
  memset(said, 0, fs.n + 1);
  for (int i = from; i < kids->n; i++) check_laws(&fs, kids->v[i], 1, said);
  if (fs.pair_at) find_pairs(&fs, fs.pair_at);
  place_lines(&fs);
  if (fs.failed) return -1;
  if (!fs.n) return 0;
  for (int i = from; i < kids->n; i++) find_pushes(&fs, kids->v[i]);
  for (int i = 0; i < fs.n; i++)
    if (fs.f[i].period && fs.f[i].pushed && !fs.f[i].law && !fs.f[i].stored) {
      char why[200];
      snprintf(why, sizeof why, "`$%s` has no law for a push to land in: `$%s' = ...`", fs.f[i].name, fs.f[i].name);
      field_fail(&fs, fs.f[i].pushed, why);
    }
  if (fs.failed) return -1;
  Kids out = {0};
  field_stmts(&fs, kids->v + from, kids->n - from, &out, 1);
  kids->n = from;
  for (int i = 0; i < out.n; i++) kid(kids, out.v[i]);
  free(out.v);
  return fs.failed ? -1 : 0;
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
  *kind = BUILD_PARSE;
  if (fields_pass(a, &kids, kept, err, errlen)) { free(kids.v); return NULL; }
  *kind = BUILD_OK;
  Node* prog = arena_alloc(a, sizeof(Node));
  prog->kind = nkBlock; prog->line = 1; prog->str = ""; prog->source = "";
  prog->nkids = kids.n;
  prog->kids = arena_alloc(a, (kids.n ? kids.n : 1) * sizeof(Node*));
  if (kids.n) memcpy(prog->kids, kids.v, kids.n * sizeof(Node*));
  free(kids.v);
  return prog;
}
