/* `import "lib/voice.aither"`: a library's defs and constant tables
 * spliced into the program. Paths are relative to the
 * importing file; a file imported twice, or in a cycle, is included once.
 * A library holds defs, constant lets and further imports: plays, state
 * and sound belong in the patch. */

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "imports.h"
#include "medium/system.h"

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
  int ends[2];                           /* a line's: 1 fixed, 2 free, 3 matched; a ring's are 0 */
  void* moves;                           /* the field its law says it moves at, `$x' = $v`, or NULL */
  double extra;                          /* a line: how much longer than its places */
  double off;                            /* ... and how far along its first place stands */
  const char* speed;                     /* the hidden let: places a round */
  const char* length;                    /* the hidden let, a line's: its length in places */
  const char* grain;                     /* the hidden let: a push's step */
  Node* width;                           /* line(width = p => ...): the lambda, else NULL */
  const char* table;                     /* the hidden array: width squared at each place, a pair's */
  int waves;                             /* a line whose period moves: held as two travelling waves */
  Node* wave_loss;                       /* ... the loss its law says, or NULL */
  const char* right;                     /* the hidden arrays: the wave going each way */
  const char* left;
  const char* head;                      /* the hidden let: this sample's cell */
  const char* cross;                     /* the hidden let: samples to cross it */
  int sheet;                             /* a sheet: `side` places a side, row after row */
  int side;
  const char* across;                    /* the hidden let, a sheet's: its length in places */
  int bar;                               /* a law reads four slopes of it, or moves it by them */
  const char* bend;                      /* the hidden let: what four slopes take */
  int per;                               /* a fine field: places a sample, else 0 */
  int sweep;                             /* ... and samples a sweep */
  const char* start;                     /* ... the hidden let: this sample's first place */
} Field;

#define WAVE_CELLS 131072                    /* a moving line's waves: 2.7 s across at 48 kHz */
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
static int make_sheet(const Node* n) { return n && n->kind == nkCall && !strcmp(n->str, "sheet"); }
static int make_ring(const Node* n) { return n && n->kind == nkCall && (!strcmp(n->str, "ring") || make_line(n) || make_sheet(n)); }
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

/* `slope(slope(...$x))`: how many deep, and the field; 0 if it isn't one. */
static int slopes(Fields* fs, const Node* n, Field** of) {
  int depth = 0;
  while (n && n->kind == nkCall && !strcmp(n->str, "slope") && n->nkids == 1) { depth++; n = n->kids[0]; }
  *of = depth ? bare_field(fs, n) : NULL;
  return *of ? depth : 0;
}

static Node* field_expr(Fields* fs, Node* n) {
  if (!n) return n;
  if (n->kind == nkIdent) {
    Field* moving = field_named(fs, n->str);
    if (moving && moving->waves) {
      field_fail(fs, n, "a line whose period moves is read at a place: `$v(p)`");
      return n;
    }
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
    if (f->waves) {
      Field* w = f->trails ? f->reads : f;
      return fcall(fs, "wave_read", n, 6, (Node*[]){fident(fs, w->right, n), fident(fs, w->left, n), fident(fs, w->head, n), fident(fs, w->cross, n),
                                                   field_expr(fs, n->kids[1]), fnum(fs, f->trails ? -1 : 1, n)});
    }
    if (f->sheet)
      return fcall(fs, "sheet_at", n, 6, (Node*[]){n->kids[0], field_expr(fs, n->kids[1]), fnum(fs, f->side, n), fident(fs, f->across, n),
                                                  fnum(fs, f->off, n), fnum(fs, f->ends[0], n)});
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
      snprintf(why, sizeof why, "a push on a ring is a change to its rate, a term of its law: `$%s' = ... + force * δ(p)`", f->name);
      field_fail(fs, n, why);
    } else if (!adds) {
      snprintf(why, sizeof why, "a push adds, a term of the law: `$%s%s = ... + force * δ(p)`", f->name, rate ? "'" : "");
      field_fail(fs, n, why);
    }
    if (fs->failed) return n;
    Node* at = f->sheet
      ? fcall(fs, "sheet_place", n, 4, (Node*[]){field_expr(fs, n->kids[1]), fnum(fs, f->side, n), fident(fs, f->across, n), fnum(fs, f->off, n)})
      : f->length
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
    if (f && !strcmp(n->str, "concentration")) {
      field_fail(fs, n, "`concentration($x)` is `-Δ$x` now");
      return n;
    }
    Field* of;
    int depth = slopes(fs, n, &of);
    if (depth == 3 || depth > 4) {
      field_fail(fs, n, "slopes come one, for a pair (∇), two, for a wave (Δ), or four, for a bar (ΔΔ)");
      return n;
    }
    if (depth == 1 && of->sheet) {
      field_fail(fs, n, "one slope (∇) needs a direction, and a sheet has two: Δ or ΔΔ");
      return n;
    }
    if (depth == 4) {
      if (of->sheet ? of->ends[0] != 1 : !of->length || of->ends[0] == 3 || of->ends[1] == 3) {
        field_fail(fs, n, "four slopes are a bar or a plate: a `line` with `fixed` or `free` ends, or a `sheet` with `fixed`");
        return n;
      }
      Node* inner = n->kids[0]->kids[0]->kids[0]->kids[0];
      if (of->sheet)
        return fbin(fs, "*", fcall(fs, "sheet_four", n, 3, (Node*[]){inner, fident(fs, of->place, n), fnum(fs, of->side, n)}), fident(fs, of->bend, n), n);
      return fbin(fs, "*", fcall(fs, "slope_four", n, 4, (Node*[]){inner, fident(fs, of->place, n), fnum(fs, of->ends[0], n), fnum(fs, of->ends[1], n)}),
                  fident(fs, of->bend, n), n);
    }
    if (depth == 2) {
      f = of;
      if (f->sheet)
        return at_speed(fs, f, fcall(fs, "sheet_twice", n, 4, (Node*[]){n->kids[0]->kids[0], fident(fs, f->place, n), fnum(fs, f->side, n), fnum(fs, f->ends[0], n)}), n);
      Node* word = at_speed(fs, f, fcall(fs, "slope_twice", n, 4, (Node*[]){n->kids[0]->kids[0], fident(fs, f->place, n), fnum(fs, f->ends[0], n), fnum(fs, f->ends[1], n)}), n);
      if (f->ends[0] != 3 && f->ends[1] != 3) return word;
      Field* v = f->moves;
      if (!v) {
        field_fail(fs, n, "a matched end takes what arrives as it moves: the field's law says it, `$x' = $v`");
        return n;
      }
      return fbin(fs, "-", word, fcall(fs, "field_drag", n, 5, (Node*[]){fident(fs, v->name, n), fident(fs, f->place, n), fident(fs, f->speed, n),
                                                                        fnum(fs, f->ends[0], n), fnum(fs, f->ends[1], n)}), n);
    }
    if (f && !strcmp(n->str, "slope")) {                      /* of a pair: where they meet, a half place between */
      if (!fs->law) {
        field_fail(fs, n, "∇ is a law's word: `∇$x` says how one of a pair changes the other");
        return n;
      }
      Node* word = f->trails                                  /* the leader reads behind it, the trailer the fresh place ahead */
        ? fcall(fs, "slope_behind", n, 4, (Node*[]){n->kids[0], fident(fs, fs->law->place, n), fnum(fs, f->ends[0], n), fident(fs, fs->law->name, n)})
        : fcall(fs, "slope_ahead", n, 5, (Node*[]){n->kids[0],
            f->per ? fidx(fs, f->name, fbin(fs, "mod", fbin(fs, "+", fident(fs, fs->law->place, n), fnum(fs, 1, n), n), f->size, n), n)
                   : fident(fs, f->now, n),
            fident(fs, fs->law->place, n), fnum(fs, f->ends[1], n), fident(fs, fs->law->name, n)});
      word = fbin(fs, "*", word, fident(fs, f->speed, n), n);
      if (fs->law->table)                                     /* flow over the width changes pressure, pressure over length flow */
        word = fbin(fs, fs->law->width ? "/" : "*", word, fidx(fs, fs->law->table, fident(fs, fs->law->place, n), n), n);
      return word;
    }
    Node* in = n->nkids == 1 ? n->kids[0] : NULL;
    f = in && in->kind == nkCall && in->nkids == 1 ? bare_field(fs, in->kids[0]) : NULL;
    if (f && f->sheet && (!strcmp(n->str, "slope") || !strcmp(n->str, "curl"))) {
      field_fail(fs, n, "a squeeze and a twist go along a line, not across a sheet");
      return n;
    }
    if (f && (f->ends[0] == 3 || f->ends[1] == 3) && (!strcmp(n->str, "slope") || !strcmp(n->str, "curl"))) {
      field_fail(fs, n, "a matched end is for two slopes or a pair: a squeeze and a twist go at their own speeds");
      return n;
    }
    if (f && !strcmp(n->str, "slope") && !strcmp(in->str, "convergence"))
      return at_speed(fs, f, fcall(fs, "slope_of_convergence", n, 4, (Node*[]){in->kids[0], fident(fs, f->place, n), fnum(fs, f->ends[0], n), fnum(fs, f->ends[1], n)}), n);
    if (f && !strcmp(n->str, "curl") && !strcmp(in->str, "curl"))
      return at_speed(fs, f, fcall(fs, "curl_of_curl", n, 4, (Node*[]){in->kids[0], fident(fs, f->place, n), fnum(fs, f->ends[0], n), fnum(fs, f->ends[1], n)}), n);
  }
  for (int i = 0; i < n->nkids; i++) n->kids[i] = field_expr(fs, n->kids[i]);
  return n;
}

/* `each(count, j => body)`, for what happens at several places a sample. */
static void each_place(Fields* fs, int count, const char* j, Kids* body, const Node* at, Kids* out) {
  kid(body, fnum(fs, 0, at));
  Node* block = fnode(fs, nkBlock, "", at);
  fkids(fs, block, body->n, body->v);
  Node* lam = fnode(fs, nkLambda, "", at);
  fkids(fs, lam, 1, (Node*[]){block});
  lam->nparams = 1;
  lam->params = arena_alloc(fs->a, sizeof(char*));
  lam->params[0] = j;
  kid(out, fcall(fs, "each", at, 2, (Node*[]){fnum(fs, count, at), lam}));
}
/* The place j along from this sample's first. */
static Node* place_along(Fields* fs, Field* f, const char* j, const Node* at) {
  return fbin(fs, "mod", fbin(fs, "+", fident(fs, f->start, at), fident(fs, j, at), at), f->size, at);
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
static void find_reached(Fields* fs, const Node* n, char* reached, const Field* self) {
  if (!n) return;
  if (n->kind == nkCall && n->nkids == 1 && bare_field(fs, n->kids[0]) &&
      (!strcmp(n->str, "convergence") || !strcmp(n->str, "curl") || !strcmp(n->str, "slope")))
    if (bare_field(fs, n->kids[0]) != self) reached[bare_field(fs, n->kids[0]) - fs->f] = 1;   /* a law smoothing itself moves nothing else */
  for (int i = 0; i < n->nkids; i++) find_reached(fs, n->kids[i], reached, self);
}
/* A block's laws are one system, so the order they're written in means
 * nothing: they are stepped together, where the last of them stands. What
 * moves on what its neighbours do is stepped last, on the rates just found,
 * as force comes before motion. A pair read through `slope` staggers: the
 * one declared first steps at this place, the other a place behind it, so
 * each sees the other fresh on both sides. */
static int same_tree(const Node* a, const Node* b);
static Node* fstore(Fields* fs, const char* array, Node* at, Node* value, const Node* s);
static const char* flet(Fields* fs, const char* what, const char* name, int k, Node* value, const Node* s, Kids* out);
/* One step of a line's two waves: each reaches its far end, and comes back by
 * what that end does, having lost half a round's loss on the way. */
static void step_waves(Fields* fs, Field* f, const Node* s, Kids* out) {
  int k = fs->made++;
  double turn[2];
  for (int e = 0; e < 2; e++) turn[e] = f->ends[e] == 1 ? -1 : f->ends[e] == 2 ? 1 : 0;
  Node* loss = f->wave_loss ? field_expr(fs, f->wave_loss) : fnum(fs, 0, s);
  const char* keep = flet(fs, "keep", f->name, k, fcall(fs, "exp", s, 1, (Node*[]){fbin(fs, "*", loss, fnum(fs, -0.5, s), s)}), s, out);
  const char* to_right = flet(fs, "at_right", f->name, k,
    fcall(fs, "wave_tap", s, 3, (Node*[]){fident(fs, f->right, s), fident(fs, f->head, s), fident(fs, f->cross, s)}), s, out);
  const char* to_left = flet(fs, "at_left", f->name, k,
    fcall(fs, "wave_tap", s, 3, (Node*[]){fident(fs, f->left, s), fident(fs, f->head, s), fident(fs, f->cross, s)}), s, out);
  kid(out, fstore(fs, f->right, fident(fs, f->head, s),
    fbin(fs, "*", fident(fs, to_left, s), fbin(fs, "*", fident(fs, keep, s), fnum(fs, turn[0], s), s), s), s));
  kid(out, fstore(fs, f->left, fident(fs, f->head, s),
    fbin(fs, "*", fident(fs, to_right, s), fbin(fs, "*", fident(fs, keep, s), fnum(fs, turn[1], s), s), s), s));
}
/* A point source on a moving line: a quarter of its force a sample into each
 * wave where it passes, split between the two cells either side. */
static void lay_wave(Fields* fs, const char* array, const char* head, const char* lag, const char* amount, double sign, const Node* s, Kids* out) {
  int k = fs->made++;
  const char* whole = flet(fs, "lag", array, k, fcall(fs, "floor", s, 1, (Node*[]){fident(fs, lag, s)}), s, out);
  const char* part = flet(fs, "part", array, k, fbin(fs, "-", fident(fs, lag, s), fident(fs, whole, s), s), s, out);
  for (int e = 0; e < 2; e++) {
    Node* cell = fbin(fs, "-", fident(fs, head, s), fbin(fs, "+", fident(fs, whole, s), fnum(fs, e, s), s), s);
    cell = fbin(fs, "mod", fbin(fs, "+", cell, fnum(fs, WAVE_CELLS, s), s), fnum(fs, WAVE_CELLS, s), s);
    const char* at = flet(fs, "cell", array, fs->made++, cell, s, out);
    Node* share = e ? fident(fs, part, s) : fbin(fs, "-", fnum(fs, 1, s), fident(fs, part, s), s);
    Node* add = fbin(fs, "*", fbin(fs, "*", fident(fs, amount, s), share, s), fnum(fs, sign, s), s);
    kid(out, fstore(fs, array, fident(fs, at, s), fbin(fs, "+", fidx(fs, array, fident(fs, at, s), s), add, s), s));
  }
}
static void wave_source(Fields* fs, Field* f, const Node* s, Kids* out) {
  Field* w = f->trails ? f->reads : f;
  const Node* said = s->kids[2];
  int k = fs->made++;
  Node* force = field_expr(fs, said->kids[1]);
  if (!strcmp(said->str, "-")) force = fbin(fs, "*", force, fnum(fs, -1, s), s);
  const char* amount = flet(fs, "amount", f->name, k, fbin(fs, "*", force, fnum(fs, 0.25, s), s), s, out);
  const char* p = flet(fs, "point", f->name, k, fcall(fs, "clamp", s, 3, (Node*[]){field_expr(fs, s->kids[1]), fnum(fs, 0, s), fnum(fs, 1, s)}), s, out);
  Node* lag_r = fcall(fs, "max", s, 2, (Node*[]){fnum(fs, 1, s), fbin(fs, "*", fident(fs, p, s), fident(fs, w->cross, s), s)});
  Node* lag_l = fcall(fs, "max", s, 2, (Node*[]){fnum(fs, 1, s), fbin(fs, "*", fbin(fs, "-", fnum(fs, 1, s), fident(fs, p, s), s), fident(fs, w->cross, s), s)});
  lay_wave(fs, w->right, w->head, flet(fs, "lag_r", f->name, k, lag_r, s, out), amount, 1, s, out);
  lay_wave(fs, w->left, w->head, flet(fs, "lag_l", f->name, k, lag_l, s, out), amount, f->trails ? -1 : 1, s, out);
}
static void step_group(Fields* fs, Node** in, int n, Kids* out, const char* reached, const Field* with, int trails) {
  for (int last = 0; last < 2; last++)
    for (int k = 0; k < fs->n; k++) {
      if (reached[k] != last) continue;
      Field* f = &fs->f[k];
      if (f->waves) continue;
      if (with ? !f->per || !same_tree(f->period, with->period) || f->trails != trails : f->per != 0) continue;
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
static void step_laws(Fields* fs, Node** in, int n, Kids* out) {
  char* reached = arena_alloc(fs->a, fs->n + 1);
  memset(reached, 0, fs->n + 1);
  for (int i = 0; i < n; i++) if (is_law(in[i])) find_reached(fs, in[i]->kids[0], reached, law_field(fs, in[i]));
  step_group(fs, in, n, out, reached, NULL, 0);
  for (int k = 0; k < fs->n; k++) {
    Field* f = &fs->f[k];
    if (!f->waves || f->trails) continue;
    for (int i = 0; i < n; i++) if (is_law(in[i]) && law_field(fs, in[i]) == f) step_waves(fs, f, in[i], out);
  }
  char* done = arena_alloc(fs->a, fs->n + 1);
  memset(done, 0, fs->n + 1);
  for (int k = 0; k < fs->n; k++) {                          /* fine fields of a period step together, several places a sample */
    Field* f = &fs->f[k];
    if (!f->per || done[k]) continue;
    const Node* s = NULL;
    for (int i = 0; i < n; i++) if (is_law(in[i]) && law_field(fs, in[i]) && same_tree(law_field(fs, in[i])->period, f->period)) s = in[i];
    if (!s) continue;
    /* An `each` holds its stores to its end, so a pair takes two: the second
     * of it sees all the first has just done, a whole block at a time. */
    for (int trails = 0; trails < 2; trails++) {
      const char* j = fname(fs, "along", f->name, fs->made++);
      Kids body = {0};
      const char* outer[fs->n + 1];
      int any = 0;
      for (int g = 0; g < fs->n; g++) {
        Field* h = &fs->f[g];
        outer[g] = h->place;
        if (!h->per || !same_tree(h->period, f->period)) continue;
        done[g] = 1;
        any |= h->trails == trails;
        const char* inner = fname(fs, "at", h->name, fs->made++);
        Node* let = fnode(fs, nkLet, inner, s);
        fkids(fs, let, 1, (Node*[]){place_along(fs, h, j, s)});
        kid(&body, let);
        h->place = inner;
      }
      if (any) step_group(fs, in, n, &body, reached, f, trails);
      for (int g = 0; g < fs->n; g++) {
        fs->f[g].place = outer[g];
        if (fs->f[g].per && same_tree(fs->f[g].period, f->period)) fs->f[g].now = NULL;
      }
      if (any) each_place(fs, f->per, j, &body, s, out);
      free(body.v);
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
    Node* start = fnum(fs, 0, s);
    if (f->sheet && i == 0) {                                 /* a point on a sheet is one value */
      start = fnode(fs, nkArr, "()", s);
      fkids(fs, start, 2, (Node*[]){fnum(fs, 0, s), fnum(fs, 0, s)});
    }
    fkids(fs, v, 1, (Node*[]){start});
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
  Kids body = {0};                                           /* a fine field's, once a place this sample */
  Kids* lay = f->per ? &body : out;
  const char* j = f->per ? fname(fs, "along", f->name, fs->made++) : NULL;
  const char* here = f->per ? flet(fs, "at", f->name, fs->made++, place_along(fs, f, j, s), s, lay) : f->place;
  const char* caught = flet(fs, "caught", f->name, k,
    fbin(fs, "/", fbin(fs, "-", fident(fs, total, s), fidx(fs, then, fident(fs, here, s), s), s), f->period, s), s, lay);
  kid(lay, fstore(fs, then, fident(fs, here, s), fident(fs, total, s), s));
  const char* next = flet(fs, "next", f->name, k,
    fbin(fs, "mod", fbin(fs, "+", fident(fs, here, s), fnum(fs, f->per ? f->per : 1, s), s), f->size, s), s, lay);
  Node* along = f->sheet ? fnum(fs, 0, s) : f->length ? fbin(fs, "/", fbin(fs, "+", fident(fs, next, s), fnum(fs, f->off, s), s), fident(fs, f->length, s), s)
                          : fbin(fs, "/", fident(fs, next, s), f->size, s);
  Node* from = fbin(fs, "-", along, fident(fs, held[0], s), s);
  Node* shaped = fbin(fs, "*", fident(fs, caught, s), f->sheet
    ? fcall(fs, "sheet_gauss", s, 6, (Node*[]){fident(fs, next, s), fnum(fs, f->side, s), fident(fs, held[0], s), fident(fs, held[1], s),
                                              fident(fs, f->across, s), fnum(fs, f->off, s)})
    : f->length
    ? fcall(fs, "line_gauss", s, 3, (Node*[]){from, fident(fs, held[1], s), fident(fs, f->length, s)})
    : fcall(fs, "field_gauss", s, 3, (Node*[]){from, fident(fs, held[1], s), f->size}), s);
  Node* laid = fnode(fs, nkIf, "", s);                       /* nothing to lay, nothing to work out */
  fkids(fs, laid, 3, (Node*[]){fcall(fs, "field_striking", s, 1, (Node*[]){fident(fs, caught, s)}), shaped,
                               fbin(fs, "*", fident(fs, caught, s), fnum(fs, 0, s), s)});
  kid(lay, fstore(fs, f->in, fident(fs, next, s), fbin(fs, "+", fidx(fs, f->in, fident(fs, next, s), s), laid, s), s));
  if (f->per) {
    each_place(fs, f->per, j, &body, s, out);
    free(body.v);
  }
}

/* A list of statements, as the lines they stand for. */
/* Which call of a def a law came from: its lines are written out with names
 * ending in that call's number (`__v_string_3'`), -1 for the file's own. */
static int law_call(const Node* s) {
  const char* name = s->str;
  if (strncmp(name, "__", 2)) return -1;
  const char* u = strrchr(name, '_');
  return u && u[1] >= '0' && u[1] <= '9' ? atoi(u + 1) : -1;
}
static void field_stmts(Fields* fs, Node** in, int n, Kids* out, int top) {
  /* A block's laws are one system, stepped where the last of them stands;
   * each call of a def holding a field is a system of its own, stepped where
   * its own last law stands, so every call is the string a lone one is. */
  for (int i = 0; i < n; i++) {
    Node* s = in[i];
    if (is_law(s)) {
      int call = law_call(s), last = 1;
      for (int k = i + 1; k < n && last; k++) if (is_law(in[k]) && law_call(in[k]) == call) last = 0;
      if (last) {
        Node** mine = arena_alloc(fs->a, (n + 1) * sizeof(Node*));
        int m = 0;
        for (int k = 0; k <= i; k++) if (is_law(in[k]) && law_call(in[k]) == call) mine[m++] = in[k];
        step_laws(fs, mine, m, out);
      }
      continue;
    }
    if (s->kind == nkIdxAssign && pushed_field(fs, s->kids[0]) && pushed_field(fs, s->kids[0])->waves) {
      Field* w = pushed_field(fs, s->kids[0]);
      const Node* said = s->kids[2];
      if (is_gauss(fs, s->kids[1]))
        field_fail(fs, s, "a line whose period moves takes a point: a shorter blow is a sharper one, `force * δ(p)`");
      else if (!primed(s->kids[0]->str) || said->kind != nkBinOp || said->num != 1.0 || (strcmp(said->str, "+") && strcmp(said->str, "-")))
        field_fail(fs, s, "a push on a line is a change to its rate, a term of its law: `$v' = ... + force * δ(p)`");
      else wave_source(fs, w, s, out);
      continue;
    }
    if (gauss_source(fs, s)) { shaped_source(fs, s, out); continue; }
    Field* f = (s->kind == nkVar || s->kind == nkAssign) ? field_named(fs, s->str) : NULL;
    if (f && f->waves) {                                     /* its declaration: the first of the pair holds the waves */
      if (!f->trails) {
        for (int e = 0; e < 2; e++) {
          Node* buffer = fnode(fs, nkVar, e ? f->left : f->right, s);
          fkids(fs, buffer, 1, (Node*[]){fcall(fs, "array", s, 2, (Node*[]){fnum(fs, WAVE_CELLS, s), fnum(fs, 0, s)})});
          kid(out, buffer);
        }
        Node* head = fnode(fs, nkLet, f->head, s);
        fkids(fs, head, 1, (Node*[]){fcall(fs, "pixel_index", s, 2, (Node*[]){fnum(fs, WAVE_CELLS, s), fnum(fs, 1, s)})});
        kid(out, head);
        Node* cross = fnode(fs, nkLet, f->cross, s);       /* there and back in a period, so across in half */
        fkids(fs, cross, 1, (Node*[]){fcall(fs, "clamp", s, 3, (Node*[]){
          fbin(fs, "/", field_expr(fs, f->period), fbin(fs, "*", fident(fs, "dt", s), fnum(fs, 2, s), s), s),
          fnum(fs, 2, s), fnum(fs, WAVE_CELLS - 8, s)})});
        kid(out, cross);
      }
      continue;
    }
    if (f && make_array(s->kids[0])) {                       /* its declaration */
      Node* c = s->kids[0];
      int named = 0;
      for (int k = 0; k < c->nkids; k++) if (c->kids[k]->kind == nkNamedArg) named = 1;
      if (f->period) {                                       /* the engine's places, stepped once through */
        c->str = "array";
        /* A sweep's stores show at its end, so a law sees its neighbours as a
         * sweep left them; a pair steps in place, each seeing the other fresh. */
        fkids(fs, c, f->reads ? 2 : 3, (Node*[]){f->size, f->init ? f->init : fnum(fs, 0, c), f->per ? fnum(fs, f->sweep, c) : f->size});
      } else if (!named && c->nkids < 3) {
        Node* init = c->nkids == 2 ? c->kids[1] : fnum(fs, 0, c);
        fkids(fs, c, 3, (Node*[]){c->kids[0], init, c->kids[0]});
      }
      kid(out, s);
      Node* place = fnode(fs, nkLet, f->place, s);
      Node* index = f->per ? fbin(fs, "*", fcall(fs, "pixel_index", s, 2, (Node*[]){fnum(fs, f->sweep, s), fnum(fs, 1, s)}), fnum(fs, f->per, s), s)
                           : fcall(fs, "pixel_index", s, 2, (Node*[]){f->size, fnum(fs, 1, s)});
      if (f->trails)
        index = fbin(fs, "mod", fbin(fs, "+", index, fbin(fs, "-", f->size, fnum(fs, 1, s), s), s), f->size, s);
      fkids(fs, place, 1, (Node*[]){index});
      f->start = f->place;
      kid(out, place);
      if (f->period) {
        if (f->length) {                                     /* there and back in a round */
          Node* length = fnode(fs, nkLet, f->length, s);
          fkids(fs, length, 1, (Node*[]){fbin(fs, "+", f->size, fnum(fs, f->extra, s), s)});
          kid(out, length);
        }
        if (f->sheet) {
          Node* across = fnode(fs, nkLet, f->across, s);
          fkids(fs, across, 1, (Node*[]){fnum(fs, f->side + f->extra, s)});
          kid(out, across);
        }
        /* Four slopes are Δ twice: Δ here is speed^2 times two slopes of
         * places, so ΔΔ is speed^4 times four. A bar's or a plate's pitch is
         * its stiffness in the law (SPEC "Four slopes are a bar"). */
        if (f->sheet && f->bar) {                            /* a plate: speed across sqrt 2, pinned */
          Node* lc = fident(fs, f->across, s);
          Node* bend = fnode(fs, nkLet, f->bend, s);
          fkids(fs, bend, 1, (Node*[]){fbin(fs, "*", fbin(fs, "*", fbin(fs, "*", lc, lc, s), fbin(fs, "*", lc, lc, s), s), fnum(fs, 4, s), s)});
          kid(out, bend);
        }
        if (f->bar && !f->sheet) {                           /* a bar: speed twice its length */
          Node* lc = fident(fs, f->length, s);
          Node* bend = fnode(fs, nkLet, f->bend, s);
          fkids(fs, bend, 1, (Node*[]){fbin(fs, "*", fbin(fs, "*", fbin(fs, "*", lc, lc, s), fbin(fs, "*", lc, lc, s), s), fnum(fs, 16, s), s)});
          kid(out, bend);
        }
        Node* speed = fnode(fs, nkLet, f->speed, s);
        fkids(fs, speed, 1, (Node*[]){f->sheet ? fbin(fs, "*", fident(fs, f->across, s), fnum(fs, f->ends[0] == 1 ? sqrt(2) : 2, s), s)
                                     : f->length ? fbin(fs, "*", fident(fs, f->length, s), fnum(fs, 2, s), s) : f->size});
        kid(out, speed);
        Node* rate = fnode(fs, nkLet, f->rate, s);           /* N^2: a wave goes round in one round */
        fkids(fs, rate, 1, (Node*[]){fbin(fs, "*", fident(fs, f->speed, s), fident(fs, f->speed, s), s)});
        kid(out, rate);
        Node* step = fnode(fs, nkLet, f->step, s);           /* a step, in rounds: N samples of T */
        fkids(fs, step, 1, (Node*[]){fbin(fs, "/", fbin(fs, "*", f->per ? fnum(fs, f->sweep, s) : f->size, fident(fs, "dt", s), s), f->period, s)});
        kid(out, step);
        Node* grain = fnode(fs, nkLet, f->grain, s);         /* a push lands as a delta: a place's height, of its length */
        fkids(fs, grain, 1, (Node*[]){f->sheet ? fbin(fs, "/", fbin(fs, "*", fbin(fs, "*", fident(fs, f->across, s), fident(fs, f->across, s), s), fident(fs, "dt", s), s), f->period, s)
                                                : f->length ? fbin(fs, "/", fbin(fs, "*", fident(fs, f->length, s), fident(fs, "dt", s), s), f->period, s)
                                                : fident(fs, f->step, s)});
        kid(out, grain);
      }
      if (f->table) {                                        /* the width where each place stands: squared, an area */
        Field* w = f->width ? f : f->reads;
        const char* i = fname(fs, "index", f->name, -1);
        Node* lam = w->width;
        Node* along = fnode(fs, nkLet, lam->params[0], s);
        fkids(fs, along, 1, (Node*[]){fbin(fs, "/", fbin(fs, "+", fident(fs, i, s), fnum(fs, f->off, s), s),
                                           fbin(fs, "+", f->size, fnum(fs, f->extra, s), s), s)});
        const char* wide = fname(fs, "wide", f->name, -1);
        Node* say = fnode(fs, nkLet, wide, s);
        fkids(fs, say, 1, (Node*[]){lam->kids[0]});
        Node* block = fnode(fs, nkBlock, "", s);
        fkids(fs, block, 3, (Node*[]){along, say, fbin(fs, "*", fident(fs, wide, s), fident(fs, wide, s), s)});
        Node* rule = fnode(fs, nkLambda, "", s);
        fkids(fs, rule, 1, (Node*[]){block});
        rule->nparams = 1;
        rule->params = arena_alloc(fs->a, sizeof(char*));
        rule->params[0] = i;
        Node* table = fnode(fs, nkVar, f->table, s);
        fkids(fs, table, 1, (Node*[]){fcall(fs, "array", s, 2, (Node*[]){f->size, rule})});
        kid(out, table);
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
  Field* of;
  if (slopes(fs, n, &of) > 1) return;                        /* an even number lands back on the places */
  if (n->kind == nkCall && !strcmp(n->str, "slope") && n->nkids == 1 && bare_field(fs, n->kids[0])) {
    Field* f = bare_field(fs, n->kids[0]);
    if (law->reads && law->reads != f) field_fail(fs, n, "∇ joins two fields: a law reads the slope of one partner");
    law->reads = f;
  }
  for (int i = 0; i < n->nkids; i++) find_slopes(fs, n->kids[i], law);
}
static int same_tree(const Node* a, const Node* b) {
  if (a == b) return 1;
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
    if (f->sheet || g->sheet) {
      field_fail(fs, at, "one slope (∇) needs a direction, and a sheet has two: Δ or ΔΔ");
    } else if (g == f || g->reads != f) {
      snprintf(why, sizeof why, "`∇$%s` in `$%s'`'s law needs its partner: `$%s' = ... ∇$%s ...`",
               g->name, f->name, g->name, f->name);
      field_fail(fs, at, why);
    } else if (!same_tree(f->period, g->period) || !f->length != !g->length) {
      snprintf(why, sizeof why, "`$%s` and `$%s` are a pair: they share one period, on one ring or line", f->name, g->name);
      field_fail(fs, at, why);
    } else if (f->length && (!(f->ends[0] + g->ends[0] == 3 || f->ends[0] + g->ends[0] == 6) ||
                             !(f->ends[1] + g->ends[1] == 3 || f->ends[1] + g->ends[1] == 6) ||
                             (f->ends[0] == 3) != (g->ends[0] == 3) || (f->ends[1] == 3) != (g->ends[1] == 3))) {
      snprintf(why, sizeof why, "`$%s` and `$%s` are a pair: at each end one is fixed and the other free, or both matched", f->name, g->name);
      field_fail(fs, at, why);
    } else {
      if (f->width && g->width) {
        snprintf(why, sizeof why, "`$%s` and `$%s` are a pair: the one that is pressure says its width, the flow takes it", f->name, g->name);
        field_fail(fs, at, why);
      }
      if (f->width && (f->ends[0] == 3 || f->ends[1] == 3)) {
        snprintf(why, sizeof why, "`$%s`: a matched end is for a line of one width", f->name);
        field_fail(fs, at, why);
      }
      if (f->width || g->width) {
        f->table = fname(fs, "area", f->name, -1);
        g->table = fname(fs, "area", g->name, -1);
      }
      if (g > f) g->trails = 1;
    }
  }
  for (int i = 0; i < fs->n; i++)
    if (fs->f[i].width && !fs->f[i].reads) field_fail(fs, at, "a width is of a pair: pressure and flow, `$p' = -∇$u`, `$u' = -∇$p`");
}

/* Where a line's places stand. Past a fixed end the field is 0 a whole
 * place on, past a free one it mirrors, half a place on. A pair stands
 * staggered: its second a half place on from the first, and only the first's
 * ends say how long it is. */
static void place_lines(Fields* fs) {
  for (int i = 0; i < fs->n; i++) {
    Field* f = &fs->f[i];
    if (f->sheet) {
      f->extra = f->ends[0] == 1 ? 1 : 0;
      f->off = f->ends[0] == 1 ? 1 : 0.5;
    }
    if (!f->length) continue;
    int l = f->ends[0] == 1, r = f->ends[1] == 1;
    if (f->reads) {
      Field* lead = f->trails ? f->reads : f;
      l = lead->ends[0] == 1; r = lead->ends[1] != 2;
      f->extra = (r ? 0 : -0.5) + (l ? 0 : 0.5);
      f->off = (l ? 0 : 0.5) + (f->trails ? 0.5 : 0);
    } else {
      f->extra = 0.5 * (l + r);
      f->off = l ? 1 : 0.5;
    }
  }
}

/* A moving line's law: `-slope($i)`, less `$v * loss` if it has one. */
static int wave_law(Fields* fs, const Node* n, Field* self, Field* other, Node** loss) {
  Field* of;
  *loss = NULL;
  if (n->kind == nkBinOp && !strcmp(n->str, "-") && n->nkids == 2) {
    const Node* term = n->kids[1];
    if (term->kind != nkBinOp || strcmp(term->str, "*") || term->nkids != 2) return 0;
    if (bare_field(fs, term->kids[0]) == self) *loss = term->kids[1];
    else if (bare_field(fs, term->kids[1]) == self) *loss = term->kids[0];
    else return 0;
    n = n->kids[0];
  }
  if (n->kind != nkUnary || n->nkids != 1) return 0;
  return slopes(fs, n->kids[0], &of) == 1 && of == other;
}
static void check_waves(Fields* fs, Node** list, int count) {
  for (int i = 0; i < fs->n; i++) {
    Field* f = &fs->f[i];
    if (!f->waves || fs->failed) continue;
    const char* odd = "a line whose period moves is a pair, `$v' = -∇$i - $v * loss` and `$i' = -∇$v - $i * loss`: nothing else stays two travelling waves";
    Field* g = f->reads;
    if (!g || !g->waves) { field_fail(fs, list[0], odd); return; }
    Node* mine = NULL;
    const Node* at = NULL;
    for (int k = 0; k < count; k++)
      if (is_law(list[k]) && law_field(fs, list[k]) == f) {
        at = list[k];
        if (!wave_law(fs, list[k]->kids[0], f, g, &mine)) { field_fail(fs, at, odd); return; }
      }
    if (!at) { field_fail(fs, list[0], odd); return; }
    f->wave_loss = mine;
    if (f->width) field_fail(fs, at, "a flared line doesn't stay two travelling waves: its period is fixed");
  }
  for (int i = 0; i < fs->n; i++) {
    Field* f = &fs->f[i];
    if (!f->waves || !f->reads || fs->failed) continue;
    Field* g = f->reads;
    if (!!f->wave_loss != !!g->wave_loss || (f->wave_loss && !same_tree(f->wave_loss, g->wave_loss)))
      field_fail(fs, list[0], "a line whose period moves keeps its losses equal, `* loss` in both laws: out of balance it doesn't stay two travelling waves");
  }
}

/* A law stands in a file's or a play's lines, once a field, for a ring. */
static void check_laws(Fields* fs, Node* n, int stands, char* said) {
  if (!n || fs->failed) return;
  if (is_law(n)) {
    Field* f = law_field(fs, n);
    const char* wrong = !stands ? "a law belongs in the file's lines or a play's, not in a def" :
                        !f ? "a rate is of state or a field: `$x = 0` or `$x = field(period = 1, ends = round)`" :
                        said[f - fs->f] ? "one law a field: say all that changes it in one line" : NULL;
    if (wrong) {
      fs->failed = 1;
      snprintf(fs->err, fs->errlen, "`$%s` (%s:%d): %s", n->str, n->source, n->line, wrong);
      return;
    }
    said[f - fs->f] = 1;
    f->law = 1;
    find_slopes(fs, n->kids[0], f);
    if (bare_field(fs, n->kids[0])) f->moves = bare_field(fs, n->kids[0]);
    if (!fs->pair_at) fs->pair_at = n;
  }
  if (n->kind == nkIdxAssign && n->kids[0]->kind == nkIdent && primed(n->kids[0]->str)) {
    if (!rate_of(fs, n->kids[0]->str)) field_fail(fs, n, "δ is a place on a field, and this is one number: a source is on a field, `$x = field(period = 1, ends = round)`");
    else if (fs->in_def) field_fail(fs, n, "a source belongs in the file's lines or a play's, not in a def");
  }
  if (is_gauss(fs, n)) field_fail(fs, n, "a spread source is `δ(p, width)`, a term of a law: `$v' = ... + force * δ(p, width)`");
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

/* A law on plain state: one place, no neighbours, and nothing to take a
 * round from, so its rates are a second. It is lowered here, before anything
 * else sees it, to `$x = $x + rate * dt`. As with a field, the laws of a
 * block are one system and the order written means nothing: they step where
 * the last stands, in the order the state was declared, and one that says
 * only what it moves at, `$x' = $v`, last: force before motion. */
typedef struct { Node** at; int n; } Scope1;
static int plain_decl(const Node* s, const char* name, size_t len) {
  if (s->kind != nkVar || primed(s->str) || strlen(s->str) != len || strncmp(s->str, name, len)) return 0;
  return !make_array(s->kids[0]);
}
static int declared_at(Node** list, int n, const char* name, size_t len) {
  for (int i = 0; i < n; i++) if (plain_decl(list[i], name, len)) return i;
  return -1;
}
static void has_time(const Node* n, const char** which, const Node** where) {
  if (!n || *which) return;
  if (n->kind == nkIdent && (!strcmp(n->str, "dt") || !strcmp(n->str, "t"))) { *which = n->str; *where = n; return; }
  for (int i = 0; i < n->nkids; i++) has_time(n->kids[i], which, where);
}
static int plain_block(Arena* a, Node** list, int n, Scope1 file, char* err, int errlen);
static int plain_walk(Arena* a, Node* n, Scope1 file, char* err, int errlen) {
  if (!n) return 0;
  if ((n->kind == nkBlock || n->kind == nkDef) && plain_block(a, n->kids, n->nkids, file, err, errlen)) return -1;
  for (int i = 0; i < n->nkids; i++) if (plain_walk(a, n->kids[i], file, err, errlen)) return -1;
  return 0;
}
static int plain_block(Arena* a, Node** list, int n, Scope1 file, char* err, int errlen) {
  Node* laws[n + 1];
  int keys[n + 1], count = 0, last = -1;
  for (int i = 0; i < n; i++) {
    if (!(list[i]->kind == nkVar || list[i]->kind == nkAssign) || !primed(list[i]->str)) continue;
    size_t len = strlen(list[i]->str) - 1;
    int order = declared_at(list, n, list[i]->str, len);
    if (order < 0) {
      order = declared_at(file.at, file.n, list[i]->str, len);
      if (order < 0) continue;                               /* a field's, or nobody's: fields_pass says */
      order += n;
    }
    const char* time = NULL;
    const Node* where = NULL;
    has_time(list[i]->kids[0], &time, &where);
    if (time) {
      snprintf(err, errlen, "`%s` in a law (%s:%d): there is no time in a law, only what changes. Say the rate; "
               "what varies with time comes in by a let above it", time, where->source, where->line);
      return -1;
    }
    for (int k = 0; k < count; k++)
      if (!strcmp(laws[k]->str, list[i]->str)) {
        snprintf(err, errlen, "`$%s` (%s:%d): one law a state: say all that changes it in one line", list[i]->str,
                 list[i]->source, list[i]->line);
        return -1;
      }
    keys[count] = order + (list[i]->kids[0]->kind == nkIdent ? 100000 : 0);
    laws[count++] = list[i];
    last = i;
  }
  if (!count) return 0;
  for (int i = 1; i < count; i++)                            /* by declaration, what only moves last */
    for (int k = i; k > 0 && keys[k - 1] > keys[k]; k--) {
      int key = keys[k]; keys[k] = keys[k - 1]; keys[k - 1] = key;
      Node* law = laws[k]; laws[k] = laws[k - 1]; laws[k - 1] = law;
    }
  Fields fs = {0};
  fs.a = a;
  for (int k = 0; k < count; k++) {
    Node* s = laws[k];
    char* name = arena_alloc(a, strlen(s->str));
    memcpy(name, s->str, strlen(s->str) - 1);
    name[strlen(s->str) - 1] = 0;
    Node* step = fbin(&fs, "*", s->kids[0], fident(&fs, "dt", s), s);
    s->kids[0] = fbin(&fs, "+", fident(&fs, name, s), step, s);
    s->kind = nkAssign;
    s->str = name;
  }
  int w = 0, seen = 0;                                       /* the others keep their order; the laws go where the last stood */
  Node* rest[n + 1];
  for (int i = 0; i < n; i++) {
    int law = 0;
    for (int k = 0; k < count; k++) if (list[i] == laws[k]) law = 1;
    if (!law) rest[w++] = list[i];
    else if (i < last) seen++;
  }
  int before = last - seen;
  for (int i = 0, o = 0; i < n; i++) {
    if (i >= before && i < before + count) list[i] = laws[i - before];
    else list[i] = rest[o++];
  }
  return 0;
}
static int plain_laws(Arena* a, Kids* kids, int from, char* err, int errlen) {
  Scope1 file = {kids->v + from, kids->n - from};
  if (plain_block(a, kids->v + from, kids->n - from, (Scope1){NULL, 0}, err, errlen)) return -1;
  for (int i = from; i < kids->n; i++) if (plain_walk(a, kids->v[i], file, err, errlen)) return -1;
  return 0;
}

/* A period the patch states plainly, in numbers and file-level lets. */
static double field_sr = 48000.0;
void fields_rate(double sr) { if (sr > 0) field_sr = sr; }
double fields_sr(void) { return field_sr; }
static int plain_number(const Node* n, Kids* kids, int from, double* v) {
  double a, b;
  if (!n) return 0;
  if (n->kind == nkNum) { *v = n->num; return 1; }
  if (n->kind == nkBinOp && n->nkids == 2 && plain_number(n->kids[0], kids, from, &a) && plain_number(n->kids[1], kids, from, &b)) {
    if (!strcmp(n->str, "+")) *v = a + b;
    else if (!strcmp(n->str, "-")) *v = a - b;
    else if (!strcmp(n->str, "*")) *v = a * b;
    else if (!strcmp(n->str, "/") && b != 0) *v = a / b;
    else return 0;
    return 1;
  }
  if (n->kind == nkIdent)
    for (int i = from; i < kids->n; i++)
      if (kids->v[i]->kind == nkLet && !strcmp(kids->v[i]->str, n->str) && kids->v[i]->nkids == 1)
        return plain_number(kids->v[i]->kids[0], kids, from, v);
  return 0;
}
/* At a place a sample a field a push crosses in `across` seconds has
 * sqrt(across * sr / 4) places. Too few to hold a note, and it takes several
 * a sample instead: at least MIN_PLACES, the push going half a place a step,
 * M samples to a sweep of N = M * P. Finer costs P places a sample. */
#define MIN_PLACES 32
/* A sheet goes round no place in particular, so its first mode says how fast
 * a push goes. At a place a sample, a sweep is every place, and that is the
 * finest it can be for the same cost; too few a side, and it takes several a
 * sample, a sweep a divisor of its places, at 12 a side (8 for a plate). */
static void sheet_grain(Field* f, double period, int plate) {
  double reach = (plate ? 0.47 : f->ends[0] == 1 ? 0.354 : 0.25) * period * field_sr;
  int least = plate ? 8 : 12;
  int side = (int)((plate ? sqrt(sqrt(reach)) : cbrt(reach)) + 0.5);
  if (side >= least) {
    f->side = side;
    f->per = 1;
    f->sweep = side * side;
    return;
  }
  side = least;
  while (side > 4 && (plate ? side * side : side) > reach) side--;
  int most = (int)(reach / (plate ? side * side : side));
  if (most < 1) most = 1;
  int m = most;
  while (m > 1 && (side * side) % m) m--;
  f->side = side;
  f->per = side * side / m;
  f->sweep = m;
}
/* A bar: its highest mode, not a crossing, sets the step. At places of a
 * sample or fewer, N * N * M = 0.47 * sr * T holds that mode to about a
 * radian a step, with at least 16 places while the rate can give them. */
static void bar_grain(Field* f, double period) {
  double reach = 0.47 * period * field_sr;
  int whole = (int)(cbrt(reach) + 0.5);                      /* a place a sample, if that's enough */
  if (whole >= 16) {
    f->per = 1;
    f->sweep = whole;
    return;
  }
  int m = (int)(reach / 256);
  if (m < 1) m = 1;
  int p = (int)(sqrt(reach / m) / m + 0.5);
  f->per = p < 1 ? 1 : p;
  f->sweep = m;
}
static void fine_grain(Field* f, double across) {
  if (sqrt(0.25 * across * field_sr) + 0.5 >= MIN_PLACES) return;
  double reach = 0.5 * across * field_sr;                    /* places times samples a sweep */
  int m = (int)(reach / MIN_PLACES);
  if (m < 1) m = 1;
  int p = (int)(reach / m / m + 0.5);
  if (p < 1) p = 1;
  f->per = p;
  f->sweep = m;
}

/* A slope with a name: `let curve = slope(slope($x))`. A law is a line at each
 * place, so the name stands for the words, not for a value at one of them,
 * and goes into the laws as if written there; `slope(curve)` is a slope more. */
/* A field is a function of place, read `$v(p)`; an array is indexed,
 * `$a[i]`. Each spelling is held to its own. */
static void check_reads(Fields* fs, const Node* n) {
  if (!n || fs->failed) return;
  if (n->kind == nkIdx && n->kids[0]->kind == nkIdent) {
    Field* f = bare_field(fs, n->kids[0]);
    char why[300];
    if (n->num == 2 && !f) {
      snprintf(why, sizeof why, "`$%s(p)` reads a field at a place, and `$%s` is no field (a ring, a line, a sheet, or an array assigned bare): an array is indexed, `$%s[i]`",
               n->kids[0]->str, n->kids[0]->str, n->kids[0]->str);
      field_fail(fs, n, why);
    } else if (n->num != 2 && f) {
      snprintf(why, sizeof why, "a field is a function of place, read `$%s(p)`: brackets index an array", f->name);
      field_fail(fs, n, why);
    }
  }
  if (n->kind == nkIdxAssign && n->kids[2]->kind == nkBinOp && n->kids[2]->num == 1.0) {   /* `$x[p] += ...`: a store, not a read */
    for (int i = 0; i < 2; i++) check_reads(fs, n->kids[i]);
    check_reads(fs, n->kids[2]->kids[1]);
    return;
  }
  for (int i = 0; i < n->nkids; i++) check_reads(fs, n->kids[i]);
}
static Node* copy_tree(Fields* fs, const Node* n) {
  Node* c = arena_alloc(fs->a, sizeof(Node));
  *c = *n;
  if (n->nkids) {
    c->kids = arena_alloc(fs->a, n->nkids * sizeof(Node*));
    for (int i = 0; i < n->nkids; i++) c->kids[i] = copy_tree(fs, n->kids[i]);
  }
  return c;
}
static int says_slope(Fields* fs, const Node* n) {
  if (!n) return 0;
  Field* of;
  if (slopes(fs, n, &of)) return 1;
  for (int i = 0; i < n->nkids; i++) if (says_slope(fs, n->kids[i])) return 1;
  return 0;
}
static Node* name_out(Fields* fs, Node* n, Node** lets, int count) {
  if (!n) return n;
  if (n->kind == nkIdent)
    for (int i = 0; i < count; i++) if (!strcmp(lets[i]->str, n->str)) return copy_tree(fs, lets[i]->kids[0]);
  for (int i = 0; i < n->nkids; i++) n->kids[i] = name_out(fs, n->kids[i], lets, count);
  return n;
}
/* Whether a let reads a field here, bare: then in a law it is what it says at
 * each place, so it goes in as words too, and stays a let for the rest. */
static int reads_here(Fields* fs, const Node* n) {
  if (!n) return 0;
  if (n->kind == nkIdent && field_named(fs, n->str)) return 1;
  if (n->kind == nkIdx) return reads_here(fs, n->kids[1]);    /* `$x[p]`: at a point, not here */
  for (int i = 0; i < n->nkids; i++) if (reads_here(fs, n->kids[i])) return 1;
  return 0;
}
static void named_slopes(Fields* fs, Kids* kids, int from) {
  Node* lets[kids->n + 1];
  int count = 0, kept = from;
  for (int i = from; i < kids->n; i++) {
    Node* s = kids->v[i];
    if (s->kind == nkLet && s->nkids == 1 && s->str[0]) {
      s->kids[0] = name_out(fs, s->kids[0], lets, count);
      if (says_slope(fs, s->kids[0])) { lets[count++] = s; continue; }
      if (reads_here(fs, s->kids[0])) lets[count++] = s;
    }
    kids->v[kept++] = s;
  }
  kids->n = kept;
  if (!count) return;
  for (int i = from; i < kids->n; i++) {
    Node* s = kids->v[i];
    if (is_law(s)) s->kids[0] = name_out(fs, s->kids[0], lets, count);
    if (s->kind == nkPlay && s->kids[0] && s->kids[0]->kind == nkBlock)
      for (int k = 0; k < s->kids[0]->nkids; k++)
        if (is_law(s->kids[0]->kids[k])) s->kids[0]->kids[k]->kids[0] = name_out(fs, s->kids[0]->kids[k]->kids[0], lets, count);
  }
}

/* A def that holds a field: each call is its own field, so a call is written
 * out where it stands, its names made its own, in the file's lines. In a `sum`
 * of a known count, each turn is a call. */
static Node** def_lines(const Node* d, int* n) {
  Node* body = d->nkids ? d->kids[0] : NULL;
  if (body && body->kind == nkBlock) { *n = body->nkids; return body->kids; }
  *n = body ? 1 : 0;
  return d->kids;
}
static int field_def(const Node* d) {
  if (d->kind != nkDef) return 0;
  int n;
  Node** lines = def_lines(d, &n);
  for (int i = 0; i < n; i++)
    if (lines[i]->kind == nkVar && make_ring(lines[i]->kids[0])) return 1;
  return 0;
}
typedef struct { const char* from; const char* to; } Rename;
static const char* renamed(Fields* fs, const char* name, Rename* map, int count) {
  size_t n = strlen(name);
  int prime = n && name[n - 1] == '\'';
  if (prime) n--;
  for (int i = 0; i < count; i++)
    if (strlen(map[i].from) == n && !strncmp(map[i].from, name, n)) {
      if (!prime) return map[i].to;
      char* r = arena_alloc(fs->a, strlen(map[i].to) + 2);
      sprintf(r, "%s'", map[i].to);
      return r;
    }
  return name;
}
static void rename_tree(Fields* fs, Node* n, Rename* map, int count) {
  if (!n) return;
  if (n->kind == nkIdent || n->kind == nkVar || n->kind == nkAssign || n->kind == nkLet) n->str = renamed(fs, n->str, map, count);
  for (int i = 0; i < n->nkids; i++) rename_tree(fs, n->kids[i], map, count);
}
static void put_number(Node* n, const char* name, double v) {
  if (!n) return;
  if (n->kind == nkIdent && !strcmp(n->str, name)) { n->kind = nkNum; n->num = v; n->str = ""; return; }
  for (int i = 0; i < n->nkids; i++) put_number(n->kids[i], name, v);
}
static const Node* def_named(Kids* kids, int from, const char* name) {
  for (int i = from; i < kids->n; i++) if (field_def(kids->v[i]) && !strcmp(kids->v[i]->str, name)) return kids->v[i];
  return NULL;
}
static int calls_field_def(const Node* n, Kids* kids, int from) {
  if (!n) return 0;
  if (n->kind == nkCall && def_named(kids, from, n->str)) return 1;
  for (int i = 0; i < n->nkids; i++) if (calls_field_def(n->kids[i], kids, from)) return 1;
  return 0;
}
static Node* write_out(Fields* fs, Node* n, Kids* kids, int from, Kids* out) {
  if (!n || fs->failed || !calls_field_def(n, kids, from)) return n;
  if (n->kind == nkCall && !strcmp(n->str, "sum") && n->nkids == 2 && n->kids[0]->kind == nkNum &&
      n->kids[1]->kind == nkLambda && n->kids[1]->nparams == 1 && n->kids[1]->kids[0]->kind != nkBlock) {
    Node* total = NULL;
    for (int turn = 1; turn <= (int)n->kids[0]->num; turn++) {
      Node* one = copy_tree(fs, n->kids[1]->kids[0]);
      put_number(one, n->kids[1]->params[0], turn);
      one = write_out(fs, one, kids, from, out);
      total = total ? fbin(fs, "+", total, one, n) : one;
    }
    return total ? total : fnum(fs, 0, n);
  }
  if (n->kind == nkLambda) {
    field_fail(fs, n, "a def with a field is called in the file's lines, or in a `sum` with a known count and one line");
    return n;
  }
  for (int i = 0; i < n->nkids; i++) n->kids[i] = write_out(fs, n->kids[i], kids, from, out);
  const Node* d = n->kind == nkCall ? def_named(kids, from, n->str) : NULL;
  if (!d) return n;
  int k = fs->made++;
  int lines_n;
  Node** lines = def_lines(d, &lines_n);
  Rename map[d->nparams + lines_n + 1];
  int count = 0;
  char buf[256];
  for (int i = 0; i < d->nparams; i++) {
    Node* given = i < n->nkids ? n->kids[i] : d->defaults && d->defaults[i] ? copy_tree(fs, d->defaults[i]) : NULL;
    if (!given) {
      snprintf(buf, sizeof buf, "`%s` needs `%s`", d->str, d->params[i]);
      field_fail(fs, n, buf);
      return n;
    }
    const char* name = fname(fs, d->params[i], d->str, k);
    Node* let = fnode(fs, nkLet, name, n);
    fkids(fs, let, 1, (Node*[]){given});
    kid(out, let);
    map[count++] = (Rename){d->params[i], name};
  }
  for (int i = 0; i < lines_n; i++) {
    const Node* s = lines[i];
    if ((s->kind != nkVar && s->kind != nkLet && s->kind != nkAssign) || !s->str[0]) continue;
    size_t len = strlen(s->str) - primed(s->str);
    snprintf(buf, sizeof buf, "%.*s", (int)len, s->str);
    int known = 0;
    for (int m = 0; m < count; m++) if (!strcmp(map[m].from, buf)) known = 1;
    if (known) continue;
    char* from_name = arena_alloc(fs->a, len + 1);
    strcpy(from_name, buf);
    map[count++] = (Rename){from_name, fname(fs, from_name, d->str, k)};
  }
  Node* result = NULL;
  for (int i = 0; i < lines_n; i++) {
    Node* s = copy_tree(fs, lines[i]);
    rename_tree(fs, s, map, count);
    if (i == lines_n - 1) result = s;
    else kid(out, s);
  }
  const char* back = fname(fs, "result", d->str, k);
  Node* let = fnode(fs, nkLet, back, n);
  fkids(fs, let, 1, (Node*[]){result});
  kid(out, let);
  return fident(fs, back, n);
}
static int field_defs(Arena* a, Kids* kids, int from, char* err, int errlen) {
  int any = 0;
  for (int i = from; i < kids->n; i++) any |= field_def(kids->v[i]);
  if (!any) return 0;
  Fields fs = {a, NULL, 0, 0, 0, err, errlen, 0};
  Kids out = {0};
  for (int i = from; i < kids->n; i++) {
    Node* s = kids->v[i];
    if (field_def(s)) continue;
    if (s->kind == nkPlay && calls_field_def(s, kids, from)) field_fail(&fs, s, "a def with a field is called in the file's lines, not a play");
    else if (s->kind != nkDef && s->kind != nkLet && s->kind != nkVar && s->kind != nkAssign &&
             s->kind != nkIdxAssign && s->kind != nkImport && s->kind != nkPlay)
      s = write_out(&fs, s, kids, from, &out);              /* the file's expression: a `sum` may be it */
    else if (s->kind != nkDef)
      for (int k = 0; k < s->nkids; k++) s->kids[k] = write_out(&fs, s->kids[k], kids, from, &out);
    kid(&out, s);
  }
  kids->n = from;
  for (int i = 0; i < out.n; i++) kid(kids, out.v[i]);
  free(out.v);
  return fs.failed ? -1 : 0;
}

/* A field read through four slopes is a bar, and so is what its law moves. */
static void make_bar(Fields* fs, Field* f, Kids* kids, int from, const Node* at) {
  double period;
  if (f->bar) return;
  if (!f->period || !plain_number(f->period, kids, from, &period) || period <= 0) {
    field_fail(fs, at, "a bar's period has to be plain numbers and file-level lets, as its places come from it");
    return;
  }
  f->bar = 1;
  f->bend = fname(fs, "bend", f->name, -1);
  if (f->sheet) sheet_grain(f, period, 1);
  else bar_grain(f, period);
  f->size = fnum(fs, f->per * f->sweep, at);
}
static void bars_in(Fields* fs, const Node* n, Field* law, Kids* kids, int from, const Node* at) {
  Field* of;
  if (!n) return;
  if (slopes(fs, n, &of) == 4) {
    make_bar(fs, of, kids, from, at);
    if (law) make_bar(fs, law, kids, from, at);
    return;
  }
  for (int i = 0; i < n->nkids; i++) bars_in(fs, n->kids[i], law, kids, from, at);
}
static void find_bars(Fields* fs, const Node* s, Kids* kids, int from) {
  if (s->kind == nkPlay && s->kids[0] && s->kids[0]->kind == nkBlock) {
    for (int i = 0; i < s->kids[0]->nkids; i++) find_bars(fs, s->kids[0]->kids[i], kids, from);
    return;
  }
  if (is_law(s)) bars_in(fs, s->kids[0], law_field(fs, s), kids, from, s);
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
        if (make_sheet(s->kids[0])) {
          f->sheet = 1;
          f->across = fname(&fs, "across", f->name, -1);
          f->ends[0] = !ends || ends->kind != nkIdent ? 0 : !strcmp(ends->str, "fixed") ? 1 : !strcmp(ends->str, "free") ? 2 : 0;
          f->ends[1] = f->ends[0];
          if (!f->ends[0]) field_fail(&fs, s, "a sheet says its edge, all round: `ends = fixed` or `ends = free`");
        }
        f->width = ring_arg(s->kids[0], "width", 99);
        if (f->width && f->width->kind != nkLambda) field_fail(&fs, s, "a width says itself along the line: `width = p => 0.03 + 0.05 * p`");
        if (f->width && !make_line(s->kids[0])) field_fail(&fs, s, "a ring has one width: `line` can vary");
        if (f->sheet) {                                       /* sized below, when its first mode is known */
        } else if (make_line(s->kids[0])) {                   /* there and back in a period, so across in half */
          f->size = fcall(&fs, "field_places", s, 1, (Node*[]){fbin(&fs, "/", f->period, fnum(&fs, 2, s), s)});
          f->length = fname(&fs, "length", f->name, -1);
          if (!ends) field_fail(&fs, s, "a field says its ends: `field(period = 2, ends = fixed)`, `ends = [fixed, free]`, or `ends = round`");
          else for (int e = 0; e < 2; e++) {
            const Node* end = ends->kind == nkArr && ends->nkids == 2 ? ends->kids[e] : ends;
            f->ends[e] = end->kind != nkIdent ? 0 : !strcmp(end->str, "fixed") ? 1 : !strcmp(end->str, "free") ? 2 : !strcmp(end->str, "matched") ? 3 : 0;
            if (!f->ends[e]) field_fail(&fs, s, "an end is `fixed`, `free` or `matched`, and a line has two: `ends = [fixed, free]`");
          }
        } else {
          f->size = fcall(&fs, "field_places", s, 1, (Node*[]){f->period});
          if (ends) field_fail(&fs, s, "round has no ends: `field(period = 2, ends = fixed)` has");
        }
        f->place = fname(&fs, "place", f->name, -1);
        f->rate = fname(&fs, "rate", f->name, -1);
        f->step = fname(&fs, "step", f->name, -1);
        f->speed = fname(&fs, "speed", f->name, -1);
        f->grain = fname(&fs, "grain", f->name, -1);
        double period;
        if (plain_number(f->period, kids, from, &period) && period > 0) {
          if (f->sheet) {
            if (f->ends[0]) sheet_grain(f, period, 0);
            f->size = fnum(&fs, f->side * f->side, s);
          } else fine_grain(f, f->length ? period / 2 : period);
          if (f->per && !f->sheet) f->size = fnum(&fs, f->per * f->sweep, s);
        } else if (f->sheet) field_fail(&fs, s, "a sheet's period has to be plain numbers and file-level lets, as its places come from it");
        else if (f->length) {                                /* a period that moves */
          f->waves = 1;
          f->size = fnum(&fs, WAVE_CELLS, s);
          f->right = fname(&fs, "right", f->name, -1);
          f->left = fname(&fs, "left", f->name, -1);
          f->head = fname(&fs, "head", f->name, -1);
          f->cross = fname(&fs, "cross", f->name, -1);
        }
      }
    }
  }
  find_fields(&fs, kids->v + from, kids->n - from);
  int kept = 0;                                              /* only what is assigned bare is a field */
  for (int i = 0; i < fs.n; i++) if (fs.f[i].place) fs.f[kept++] = fs.f[i];
  fs.n = kept;
  for (int i = from; i < kids->n; i++) check_reads(&fs, kids->v[i]);
  named_slopes(&fs, kids, from);
  char* said = arena_alloc(a, fs.n + 1);
  memset(said, 0, fs.n + 1);
  for (int i = from; i < kids->n; i++) check_laws(&fs, kids->v[i], 1, said);
  for (int i = from; i < kids->n; i++) find_bars(&fs, kids->v[i], kids, from);
  for (int i = 0; i < fs.n; i++)
    if (fs.f[i].width && !fs.pair_at) field_fail(&fs, kids->v[from], "a width is of a pair: pressure and flow, `$p' = -∇$u`, `$u' = -∇$p`");
  if (fs.pair_at) find_pairs(&fs, fs.pair_at);
  check_waves(&fs, kids->v + from, kids->n - from);
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

/* A field's domain is its ends (notes/one-system-2026-11-11.md, "Domains,
 * without names"): `field(period = T, ends = round)` goes round, `ends =
 * fixed` or `[fixed, free]` has two ends, and a list of periods, one an
 * axis, is a sheet, each axis's period there and back as a line's. They
 * become the words the lowering knows (a sheet's period there is its first
 * mode's, the axis's over sqrt 2, a square as yet). Those words aren't a
 * patch's to write. */
static Node* dw_node(Arena* a, NodeKind k, const char* str, const Node* at) {
  Node* n = arena_alloc(a, sizeof(Node));
  memset(n, 0, sizeof *n);
  n->kind = k; n->str = str; n->line = at->line; n->source = at->source;
  return n;
}
static int dw_same(const Node* x, const Node* y) {
  if (x->kind != y->kind || x->nkids != y->nkids || strcmp(x->str, y->str) || (x->kind == nkNum && x->num != y->num)) return 0;
  for (int i = 0; i < x->nkids; i++) if (!dw_same(x->kids[i], y->kids[i])) return 0;
  return 1;
}
static int domain_words(Arena* a, Node* n, char* err, int errlen) {
  if (!n) return 0;
  if (n->kind == nkCall && (!strcmp(n->str, "ring") || !strcmp(n->str, "line") || !strcmp(n->str, "sheet"))) {
    snprintf(err, errlen, "a field's domain is its ends now (%s:%d): `field(period = 1, ends = round)` goes round, "
             "`field(period = 2, ends = [fixed, free])` has two ends, `field(period = [2, 2], ends = fixed)` is a sheet",
             n->source, n->line);
    return -1;
  }
  if (n->kind == nkCall && !strcmp(n->str, "field")) {
    int pi = -1, ei = -1;
    for (int k = 0; k < n->nkids; k++) {
      Node* c = n->kids[k];
      if (c->kind == nkNamedArg && !strcmp(c->str, "period")) pi = k;
      else if (c->kind == nkNamedArg && !strcmp(c->str, "ends")) ei = k;
      else if (c->kind != nkNamedArg && k == 0) pi = k;
    }
    Node** period = pi < 0 ? NULL : n->kids[pi]->kind == nkNamedArg ? &n->kids[pi]->kids[0] : &n->kids[pi];
    Node* ends = ei < 0 ? NULL : n->kids[ei]->kids[0];
    if (period && (*period)->kind == nkArr && strcmp((*period)->str, "()")) {
      Node* sides = *period;
      if (sides->nkids != 2 || !dw_same(sides->kids[0], sides->kids[1])) {
        snprintf(err, errlen, "a field of two axes is a square, as yet (%s:%d): `period = [2, 2]`", n->source, n->line);
        return -1;
      }
      if (ends && ends->kind == nkArr) {
        snprintf(err, errlen, "a sheet says its edge, all round, as yet (%s:%d): `ends = fixed` or `ends = free`", n->source, n->line);
        return -1;
      }
      Node* root2 = dw_node(a, nkNum, "", n);
      root2->num = 1.4142135623730951;
      Node* q = dw_node(a, nkBinOp, "/", n);
      q->kids = arena_alloc(a, 2 * sizeof(Node*));
      q->kids[0] = sides->kids[0]; q->kids[1] = root2; q->nkids = 2;
      *period = q;
      n->str = "sheet";
    } else if (ends && ends->kind == nkIdent && !strcmp(ends->str, "round")) {
      for (int k = ei; k < n->nkids - 1; k++) n->kids[k] = n->kids[k + 1];
      n->nkids--;
      n->str = "ring";
    } else {
      n->str = "line";
    }
  }
  for (int i = 0; i < n->nkids; i++) if (domain_words(a, n->kids[i], err, errlen)) return -1;
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
  int kept = 0;
  for (int i = 0; i < std->nkids; i++)
    if (!shadowed(kids.v[i], &kids, std->nkids)) kids.v[kept++] = kids.v[i];
  memmove(kids.v + kept, kids.v + std->nkids, (kids.n - std->nkids) * sizeof(Node*));
  kids.n -= std->nkids - kept;
  *kind = BUILD_PARSE;
  /* `field(...)`: a domain is its ends, not a name (domain_words). */
  for (int i = kept; i < kids.n; i++)
    if (domain_words(a, kids.v[i], err, errlen)) { free(kids.v); return NULL; }
  /* The field core takes the patch's system when it can hold it
   * (medium/system.c); what it can't hold yet goes the old way. */
  /* A def holding a field is written out first, each call its own, so the
   * core sees every call's field as a medium of its own. */
  if (field_defs(a, &kids, kept, err, errlen)) { free(kids.v); return NULL; }
  if (system_pass(a, &kids.v, &kids.n, &kids.cap, kept, err, errlen) < 0) { free(kids.v); return NULL; }
  if (plain_laws(a, &kids, kept, err, errlen) || fields_pass(a, &kids, kept, err, errlen)) { free(kids.v); return NULL; }
  *kind = BUILD_OK;
  Node* prog = arena_alloc(a, sizeof(Node));
  prog->kind = nkBlock; prog->line = 1; prog->str = ""; prog->source = "";
  prog->nkids = kids.n;
  prog->kids = arena_alloc(a, (kids.n ? kids.n : 1) * sizeof(Node*));
  if (kids.n) memcpy(prog->kids, kids.v, kids.n * sizeof(Node*));
  free(kids.v);
  return prog;
}
