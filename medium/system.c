/* The field core's way in (system.h; notes/one-system-2026-11-11.md, "The
 * boundary").
 *
 * The system is found, not marked: every field and every law, and whatever a
 * law uses that reads them. Everything else is outside. Each sample, after
 * all the patch's own lines (which so read the moment's start, wherever they
 * stand), the system takes one step, Strang split:
 *
 *   each medium turns half a sample on its own, exactly (dsp.c's modes);
 *   what the laws read of the media is read there, at the midpoint;
 *   the laws on plain state step by Gauss-Legendre on it, and each source's force,
 *     averaged over the step, kicks its medium;
 *   each medium turns the other half.
 *
 * Milestone 1 holds a `line` under a linear wave law as modes:
 *
 *   $x = line(period = T, ends = ...)    $v = line(period = T, ends = ...)
 *   $v' = A Δ$x + B $x + C $v + D Δ$v + E ΔΔ$x + force * δ(p) ...    $x' = $v + ...
 *
 * with A to E from outside (E, a bar, pinned at both ends), forces at a
 * place or spread, `δ(p, width)`, on either rate, plus laws on plain state,
 * all at the file's level. Anything else it leaves to the old lowering, untouched. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "system.h"
#include "../imports.h"

#define MODE_CAP 256               /* modes kept a medium, at most: the rest is left out, and said */

typedef struct { Node** v; int n, cap; } Vec;
static void push(Vec* k, Node* n) {
  if (k->n == k->cap) { k->cap = k->cap ? 2 * k->cap : 64; k->v = realloc(k->v, k->cap * sizeof(Node*)); }
  k->v[k->n++] = n;
}

typedef struct { Node* at; Node* width; Node* force; int minus, into; const char* contact; } Source;
/* into: 0 velocity, 1 place or a pair's first, 2 its partner. contact: a
 * force that reads its own field where it acts (a bow, a hammer's felt), the
 * name of the state its last root is kept in, or NULL. */

typedef struct {
  const char *x, *v;               /* the place and the velocity fields */
  const char *store, *tick;        /* its state array, and the state its steps are said into */
  Node *xdecl, *vdecl, *xlaw, *vlaw;
  Node* period;
  double T;
  int ends[2];
  Node *A, *B, *C, *D, *E;         /* the law's coefficients, outside expressions */
  int pair;                        /* a pair, ∇ each the other's: x the first field, v its partner */
  int axes;                        /* 1, or 2: a sheet, each axis's period T, a square, its edge all one end */
  int delay;                       /* a pair whose period moves, of equal loss: two travelling waves */
  Node* width;                     /* a horn: the pressure's width, p => ..., Webster's modes */
  Node* loss;                      /* its loss a round, g */
  Node* law[2][3];                 /* a pair's: each field's coefficients of ∇partner, itself, Δ itself */
  Source src[64];
  int nsrc;
} Medium;

typedef struct { const char* name; Node* law; Node* rate; } Plain;

typedef struct {
  Arena* a;
  char* err;
  int errlen;
  int no;                          /* the core can't hold this patch yet */
  Node** v;
  int n, from;
  Medium m[16];
  int nm;
  Plain p[64];
  int np;
  int made;
  Vec reads;                       /* the block's readings of the media, at the midpoint */
} Sys;

/* ----------------------------------------------------------------- trees */

static Node* mk(Sys* s, NodeKind k, const char* str, const Node* at) {
  Node* n = arena_alloc(s->a, sizeof(Node));
  memset(n, 0, sizeof *n);
  n->kind = k;
  n->str = str ? str : "";
  n->line = at ? at->line : 0;
  n->source = at ? at->source : "";
  return n;
}
static void kids(Sys* s, Node* n, int count, Node** k) {
  n->kids = arena_alloc(s->a, (count ? count : 1) * sizeof(Node*));
  for (int i = 0; i < count; i++) n->kids[i] = k[i];
  n->nkids = count;
}
static Node* num(Sys* s, double v, const Node* at) { Node* n = mk(s, nkNum, "", at); n->num = v; return n; }
static Node* ident(Sys* s, const char* name, const Node* at) { return mk(s, nkIdent, name, at); }
static Node* bin(Sys* s, const char* op, Node* l, Node* r, const Node* at) {
  Node* n = mk(s, nkBinOp, op, at);
  kids(s, n, 2, (Node*[]){l, r});
  return n;
}
static Node* call(Sys* s, const char* name, const Node* at, int count, Node** k) {
  Node* n = mk(s, nkCall, name, at);
  kids(s, n, count, k);
  return n;
}
static Node* let(Sys* s, const char* name, Node* value) {
  Node* n = mk(s, nkLet, name, value);
  kids(s, n, 1, (Node*[]){value});
  return n;
}
static Node* store(Sys* s, NodeKind k, const char* name, Node* value) {  /* `$name = value` */
  Node* n = mk(s, k, name, value);
  kids(s, n, 1, (Node*[]){value});
  return n;
}
static const char* fresh(Sys* s, const char* what) {
  char b[64];
  snprintf(b, sizeof b, "__%s_%d", what, s->made++);
  size_t l = strlen(b);
  char* d = arena_alloc(s->a, l + 1);
  memcpy(d, b, l + 1);
  return d;
}
static Node* copy(Sys* s, const Node* n) {
  if (!n) return NULL;
  Node* c = arena_alloc(s->a, sizeof(Node));
  *c = *n;
  if (n->nkids) {
    c->kids = arena_alloc(s->a, n->nkids * sizeof(Node*));
    for (int i = 0; i < n->nkids; i++) c->kids[i] = copy(s, n->kids[i]);
  }
  return c;
}
static int is(const Node* n, NodeKind k, const char* str) { return n && n->kind == k && !strcmp(n->str, str); }
static int primed(const char* name) { size_t l = strlen(name); return l && name[l - 1] == '\''; }
static const char* unprimed(Sys* s, const char* name) {
  size_t l = strlen(name) - 1;
  char* d = arena_alloc(s->a, l + 1);
  memcpy(d, name, l);
  d[l] = 0;
  return d;
}
static Node* arg(const Node* c, const char* name, int at) {
  for (int k = 0; k < c->nkids; k++)
    if (c->kids[k]->kind == nkNamedArg && !strcmp(c->kids[k]->str, name)) return c->kids[k]->kids[0];
  return at >= 0 && at < c->nkids && c->kids[at]->kind != nkNamedArg ? c->kids[at] : NULL;
}

/* -------------------------------------------------------------- the patch */

static Node* top_let(Sys* s, const char* name) {
  for (int i = s->from; i < s->n; i++)
    if (s->v[i]->kind == nkLet && !strcmp(s->v[i]->str, name) && s->v[i]->nkids == 1) return s->v[i];
  return NULL;
}
static int plain_number(Sys* s, const Node* n, double* v) {
  double a, b;
  if (!n) return 0;
  if (n->kind == nkNum) { *v = n->num; return 1; }
  if (n->kind == nkBinOp && n->nkids == 2 && plain_number(s, n->kids[0], &a) && plain_number(s, n->kids[1], &b)) {
    if (!strcmp(n->str, "+")) *v = a + b;
    else if (!strcmp(n->str, "-")) *v = a - b;
    else if (!strcmp(n->str, "*")) *v = a * b;
    else if (!strcmp(n->str, "/") && b != 0) *v = a / b;
    else return 0;
    return 1;
  }
  if (n->kind == nkIdent) { Node* l = top_let(s, n->str); return l && plain_number(s, l->kids[0], v); }
  return 0;
}
static Medium* medium_of(Sys* s, const char* field) {
  for (int i = 0; i < s->nm; i++) if (!strcmp(s->m[i].x, field) || !strcmp(s->m[i].v, field)) return &s->m[i];
  return NULL;
}
static Plain* plain_of(Sys* s, const char* name) {
  for (int i = 0; i < s->np; i++) if (!strcmp(s->p[i].name, name)) return &s->p[i];
  return NULL;
}
static int field_word(const Node* n) {             /* the words that only mean anything of a field */
  return n->kind == nkCall && (!strcmp(n->str, "slope") || !strcmp(n->str, "convergence") || !strcmp(n->str, "curl") ||
                               !strcmp(n->str, "gauss"));
}
/* Whether a tree reads the system: a field (bare, at a place, or through a
 * word), a law's plain state, or a let that does. */
static int reads_system(Sys* s, const Node* n, int depth) {
  if (!n || depth > 64) return 0;
  if (n->kind == nkIdent) {
    if (medium_of(s, n->str) || plain_of(s, n->str)) return 1;
    Node* l = top_let(s, n->str);
    return l && reads_system(s, l->kids[0], depth + 1);
  }
  if (field_word(n)) return 1;
  for (int i = 0; i < n->nkids; i++) if (reads_system(s, n->kids[i], depth)) return 1;
  return 0;
}
/* Whether it says a field bare, or a word of one: what can only stand in a law. */
static int says_field(Sys* s, const Node* n, int depth) {
  if (!n || depth > 64) return 0;
  if (n->kind == nkIdx && n->num == 2) return 0;    /* a field at a place is a value */
  if (n->kind == nkIdent) {
    if (medium_of(s, n->str)) return 1;
    Node* l = top_let(s, n->str);
    return l && says_field(s, l->kids[0], depth + 1);
  }
  if (field_word(n)) return 1;
  for (int i = 0; i < n->nkids; i++) if (says_field(s, n->kids[i], depth)) return 1;
  return 0;
}
/* A let that says a field is a name for the words (`let pull = Δ$x`): in a law
 * it is written where it's used. */
static Node* inline_words(Sys* s, const Node* n, int depth) {
  if (!n) return NULL;
  if (depth > 64) { s->no = 1; return copy(s, n); }
  if (n->kind == nkIdent && !medium_of(s, n->str)) {
    Node* l = top_let(s, n->str);
    if (l && says_field(s, l->kids[0], 0)) return inline_words(s, l->kids[0], depth + 1);
  }
  Node* c = copy(s, n);
  for (int i = 0; i < c->nkids; i++) c->kids[i] = inline_words(s, n->kids[i], depth);
  return c;
}

/* ------------------------------------------------------- a law, linear? */

static Node* add_to(Sys* s, Node* sum, Node* term, const Node* at) { return sum ? bin(s, "+", sum, term, at) : term; }

/* How many slopes deep a field is: the field, or -1. */
static int slopes(const Node* n, const char** of) {
  int k = 0;
  while (n->kind == nkCall && !strcmp(n->str, "slope") && n->nkids == 1) { n = n->kids[0]; k++; }
  if (n->kind != nkIdent) return -1;
  *of = n->str;
  return k;
}
/* What a field term is: Δ$x, $x, $v, Δ$v, ΔΔ$x (0..4), or -1. */
static int field_term(Medium* m, const Node* n) {
  const char* of;
  int k = slopes(n, &of);
  if (k < 0) return -1;
  int x = !strcmp(of, m->x), v = !strcmp(of, m->v);
  if (x && k == 0) return 1;
  if (v && k == 0) return 2;
  if (x && k == 2) return 0;
  if (v && k == 2) return 3;
  if (x && k == 4) return 4;
  return -1;
}
/* Split a law's right side into its four coefficients, each the sum of
 * `factor` times what multiplies its field term; anything else (a field
 * term squared, a slope of one alone, a force on the whole line) is beyond
 * milestone 1, and the patch goes the old way. */
static void linear(Sys* s, Medium* m, const Node* n, Node* factor, Node** coef) {
  if (s->no) return;
  if (n->kind == nkBinOp && (!strcmp(n->str, "+") || !strcmp(n->str, "-")) && n->nkids == 2) {
    linear(s, m, n->kids[0], factor, coef);
    linear(s, m, n->kids[1], !strcmp(n->str, "-") ? bin(s, "*", factor, num(s, -1, n), n) : factor, coef);
    return;
  }
  if (n->kind == nkUnary && !strcmp(n->str, "-")) { linear(s, m, n->kids[0], bin(s, "*", factor, num(s, -1, n), n), coef); return; }
  int k = field_term(m, n);
  if (k >= 0) { coef[k] = add_to(s, coef[k], factor, n); return; }
  if (n->kind == nkBinOp && !strcmp(n->str, "*") && n->nkids == 2) {
    int l = says_field(s, n->kids[0], 0), r = says_field(s, n->kids[1], 0);
    if (l != r && !reads_system(s, l ? n->kids[1] : n->kids[0], 0)) {
      linear(s, m, l ? n->kids[0] : n->kids[1], bin(s, "*", factor, copy(s, l ? n->kids[1] : n->kids[0]), n), coef);
      return;
    }
  }
  if (n->kind == nkBinOp && !strcmp(n->str, "/") && n->nkids == 2 && says_field(s, n->kids[0], 0) &&
      !reads_system(s, n->kids[1], 0)) {
    linear(s, m, n->kids[0], bin(s, "/", factor, copy(s, n->kids[1]), n), coef);
    return;
  }
  s->no = 1;
}

/* A pair's law, split: the coefficients of ∇partner, itself, Δ itself. */
static void linear_pair(Sys* s, const char* self, const char* partner, const Node* n, Node* factor, Node** coef) {
  if (s->no) return;
  if (n->kind == nkBinOp && (!strcmp(n->str, "+") || !strcmp(n->str, "-")) && n->nkids == 2) {
    linear_pair(s, self, partner, n->kids[0], factor, coef);
    linear_pair(s, self, partner, n->kids[1], !strcmp(n->str, "-") ? bin(s, "*", factor, num(s, -1, n), n) : factor, coef);
    return;
  }
  if (n->kind == nkUnary && !strcmp(n->str, "-")) { linear_pair(s, self, partner, n->kids[0], bin(s, "*", factor, num(s, -1, n), n), coef); return; }
  const char* of;
  int k = slopes(n, &of);
  if (k >= 0) {
    int at = k == 1 && !strcmp(of, partner) ? 0 : k == 0 && !strcmp(of, self) ? 1 : k == 2 && !strcmp(of, self) ? 2 : -1;
    if (at < 0) { s->no = 1; return; }
    coef[at] = add_to(s, coef[at], factor, n);
    return;
  }
  if (n->kind == nkBinOp && !strcmp(n->str, "*") && n->nkids == 2) {
    int l = says_field(s, n->kids[0], 0), r = says_field(s, n->kids[1], 0);
    if (l != r && !reads_system(s, l ? n->kids[1] : n->kids[0], 0)) {
      linear_pair(s, self, partner, l ? n->kids[0] : n->kids[1], bin(s, "*", factor, copy(s, l ? n->kids[1] : n->kids[0]), n), coef);
      return;
    }
  }
  if (n->kind == nkBinOp && !strcmp(n->str, "/") && n->nkids == 2 && says_field(s, n->kids[0], 0) && !reads_system(s, n->kids[1], 0)) {
    linear_pair(s, self, partner, n->kids[0], bin(s, "/", factor, copy(s, n->kids[1]), n), coef);
    return;
  }
  s->no = 1;
}
/* Whether a law's right side reads `partner` through one slope. */
static int reads_slope_of(Sys* s, const Node* n, const char* partner) {
  const char* of;
  if (slopes(n, &of) == 1 && !strcmp(of, partner)) return 1;
  for (int i = 0; i < n->nkids; i++) if (reads_slope_of(s, n->kids[i], partner)) return 1;
  return 0;
}
static int same_tree(const Node* a, const Node* b) {
  if (!a || !b) return a == b;
  if (a->kind != b->kind || strcmp(a->str, b->str) || a->num != b->num || a->nkids != b->nkids) return 0;
  for (int i = 0; i < a->nkids; i++) if (!same_tree(a->kids[i], b->kids[i])) return 0;
  return 1;
}

/* The field a source's force acts on: a wave's velocity, a pair's (a
 * horn's, travelling waves') first field or its partner. */
static const char* driven_field(Medium* m, int into) {
  if (!m->pair) return into == 0 ? m->v : m->x;
  return into == 2 ? m->v : m->x;
}
static int is_reading(const Node* n, const char* field, const Node* place) {
  return n->kind == nkIdx && n->num == 2 && n->kids[0]->kind == nkIdent && !strcmp(n->kids[0]->str, field) &&
         same_tree(n->kids[1], place);
}
/* Whether a force reads the field it drives, at the place it acts: a contact. */
static int reads_own(Medium* m, const char* field, const Node* place, const Node* n) {
  (void)m;
  if (!n) return 0;
  if (is_reading(n, field, place)) return 1;
  for (int i = 0; i < n->nkids; i++) if (reads_own(m, field, place, n->kids[i])) return 1;
  return 0;
}
static Node* reading_as(Sys* s, const Node* n, const char* field, const Node* place, const char* name) {
  if (!n) return NULL;
  if (is_reading(n, field, place)) return ident(s, name, n);
  Node* c = copy(s, n);
  for (int i = 0; i < c->nkids; i++) c->kids[i] = reading_as(s, n->kids[i], field, place, name);
  return c;
}

/* A copy with each name in `from` read as the one in `to`. */
static Node* subst_names(Sys* s, const Node* n, const char** from, const char** to, int count) {
  if (!n) return NULL;
  if (n->kind == nkIdent) for (int k = 0; k < count; k++) if (!strcmp(n->str, from[k])) return ident(s, to[k], n);
  Node* c = copy(s, n);
  for (int i = 0; i < c->nkids; i++) c->kids[i] = subst_names(s, n->kids[i], from, to, count);
  return c;
}

/* A copy of `n` with `name` (a lambda's parameter) read as `value`. */
static Node* substitute(Sys* s, const Node* n, const char* name, Node* value) {
  if (!n) return NULL;
  if (n->kind == nkIdent && !strcmp(n->str, name)) return copy(s, value);
  Node* c = copy(s, n);
  for (int i = 0; i < c->nkids; i++) c->kids[i] = substitute(s, n->kids[i], name, value);
  return c;
}

/* ------------------------------------------- what the block computes */

/* A copy for the block: lets that read the system written in where they're
 * used, so each is what it is at the midpoint, and every reading of a medium
 * a name the block has read once there. */
static Node* inside(Sys* s, const Node* n, int depth) {
  if (!n) return NULL;
  if (depth > 64) { s->no = 1; return copy(s, n); }
  if (n->kind == nkIdx && n->num == 2 && n->kids[0]->kind == nkIdent && medium_of(s, n->kids[0]->str)) {
    Medium* m = medium_of(s, n->kids[0]->str);
    Node* at = inside(s, n->kids[1], depth);
    if (reads_system(s, n->kids[1], 0)) s->no = 1;   /* a reading where the system says: later */
    const char* name = fresh(s, "read");
    int vel = !strcmp(n->kids[0]->str, m->v);
    push(&s->reads, let(s, name, m->delay || m->width
      ? call(s, m->width ? "medium_horn_at" : "medium_line_at", n, 3, (Node*[]){ident(s, m->store, n), at, num(s, vel ? 2 : 1, n)})
      : call(s, vel ? (m->pair ? "medium_partner_at" : m->axes == 2 ? "medium_velocity_at2" : "medium_velocity_at")
                    : m->axes == 2 ? "medium_at2" : "medium_at", n, 2, (Node*[]){ident(s, m->store, n), at})));
    return ident(s, name, n);
  }
  if (n->kind == nkIdent && !medium_of(s, n->str) && !plain_of(s, n->str)) {
    Node* l = top_let(s, n->str);
    if (l && reads_system(s, l->kids[0], 0)) return inside(s, l->kids[0], depth + 1);
  }
  if (n->kind == nkIdent && medium_of(s, n->str)) { s->no = 1; return copy(s, n); }
  if (n->kind == nkLambda || n->kind == nkBlock) {
    if (reads_system(s, n, 0)) s->no = 1;
    return copy(s, n);
  }
  Node* c = copy(s, n);
  for (int i = 0; i < c->nkids; i++) c->kids[i] = inside(s, n->kids[i], depth);
  return c;
}
/* The same with each plain state read as `names` holds it in this stage. */
static Node* staged(Sys* s, const Node* n, const char** names) {
  if (!n) return NULL;
  if (n->kind == nkIdent) {
    Plain* p = plain_of(s, n->str);
    if (p) return ident(s, names[p - s->p], n);
  }
  Node* c = copy(s, n);
  for (int i = 0; i < c->nkids; i++) c->kids[i] = staged(s, n->kids[i], names);
  return c;
}
/* Outside, a reading of a medium is the medium at the moment's start. */
static Node* outside(Sys* s, Node* n) {
  if (!n) return n;
  if (n->kind == nkIdx && n->num == 2 && n->kids[0]->kind == nkIdent && medium_of(s, n->kids[0]->str)) {
    Medium* m = medium_of(s, n->kids[0]->str);
    int vel = !strcmp(n->kids[0]->str, m->v);
    if (m->delay || m->width)
      return call(s, m->width ? "medium_horn_at" : "medium_line_at", n, 3,
                  (Node*[]){ident(s, m->store, n), outside(s, n->kids[1]), num(s, vel ? 2 : 1, n)});
    return call(s, vel ? (m->pair ? "medium_partner_at" : m->axes == 2 ? "medium_velocity_at2" : "medium_velocity_at")
                       : m->axes == 2 ? "medium_at2" : "medium_at", n, 2,
                (Node*[]){ident(s, m->store, n), outside(s, n->kids[1])});
  }
  if (n->kind == nkIdent && medium_of(s, n->str)) { s->no = 1; return n; }
  if (field_word(n)) { s->no = 1; return n; }
  for (int i = 0; i < n->nkids; i++) n->kids[i] = outside(s, n->kids[i]);
  return n;
}

/* ------------------------------------------------------------------ pass */

static int is_law(const Node* n) { return (n->kind == nkVar || n->kind == nkAssign) && primed(n->str); }
static int is_source(const Node* n) { return n->kind == nkIdxAssign && n->num == 3; }
static int holds_field(const Node* n) {
  if (!n) return 0;
  if (n->kind == nkCall && (!strcmp(n->str, "ring") || !strcmp(n->str, "line") || !strcmp(n->str, "sheet"))) return 1;
  if ((n->kind == nkVar || n->kind == nkAssign) && primed(n->str)) return 1;
  for (int i = 0; i < n->nkids; i++) if (holds_field(n->kids[i])) return 1;
  return 0;
}
static int ends_of(const Node* e, int which) {
  const Node* end = e && e->kind == nkArr && e->nkids == 2 ? e->kids[which] : e;
  if (!end || end->kind != nkIdent) return 0;
  return !strcmp(end->str, "fixed") ? 1 : !strcmp(end->str, "free") ? 2 : 0;   /* matched: later */
}

/* Why a patch goes the old way, said when AITHER_CORE_SAY=1: the line here
 * that turned it away. */
static int said_no(int line) {
  const char* say = getenv("AITHER_CORE_SAY");
  if (say && !strcmp(say, "1")) fprintf(stderr, "[aither] the field core leaves this patch to the old way (medium/system.c:%d)\n", line);
  return 0;
}

/* The names an expression reads (idents, not the functions it calls),
 * each once, leaving out `but` and any lambda's own parameters. */
static void free_names(const Node* n, const char* but, const char** out, int* nn, int cap) {
  if (!n) return;
  if (n->kind == nkIdent && strcmp(n->str, but)) {
    for (int i = 0; i < *nn; i++) if (!strcmp(out[i], n->str)) return;
    if (*nn < cap) out[(*nn)++] = n->str;
    return;
  }
  if (n->kind == nkLambda) {
    int before = *nn;
    for (int i = 0; i < n->nkids; i++) free_names(n->kids[i], but, out, nn, cap);
    for (int i = before; i < *nn; i++)
      for (int q = 0; q < n->nparams; q++)
        if (!strcmp(out[i], n->params[q])) { out[i] = out[--*nn]; i--; break; }
    return;
  }
  for (int i = 0; i < n->nkids; i++) free_names(n->kids[i], but, out, nn, cap);
}

int system_pass(Arena* a, Node*** vp, int* np, int* capp, int from, char* err, int errlen) {
  (void)capp;
  const char* off = getenv("AITHER_CORE");
  if (off && !strcmp(off, "0")) return said_no(__LINE__);
  Sys sys = {0}, *s = &sys;
  s->a = a; s->err = err; s->errlen = errlen; s->v = *vp; s->n = *np; s->from = from;

  /* What the patch has. Anything with a field below the file's level, or a
   * field this milestone doesn't hold, goes the old way. */
  int any_line = 0;
  for (int i = from; i < s->n; i++) {
    Node* t = s->v[i];
    if (t->kind == nkPlay && holds_field(t)) return said_no(__LINE__);      /* a def's fields are written out before this (field_defs) */
    if (t->kind == nkDef && holds_field(t)) return said_no(__LINE__);
    if (t->kind == nkVar && t->nkids == 1 && t->kids[0]->kind == nkCall) {
      const char* w = t->kids[0]->str;
      if (!strcmp(w, "line") || !strcmp(w, "ring") || !strcmp(w, "sheet")) any_line = 1;
    }
    if (t->kind == nkVar && t->nkids == 1 && t->kids[0]->kind == nkCall && !strcmp(t->kids[0]->str, "array")) {
      for (int k = from; k < s->n; k++)                /* an array assigned bare is a field of the old kind */
        if (s->v[k]->kind == nkAssign && !strcmp(s->v[k]->str, t->str)) return said_no(__LINE__);
    }
  }
  if (!any_line) return said_no(__LINE__);

  /* The media: a place and its velocity, `$x' = $v`, on one line. */
  for (int i = from; i < s->n; i++) {
    Node* t = s->v[i];
    if (!is_law(t) || t->kids[0]->kind != nkIdent) continue;
    const char* x = unprimed(s, t->str);
    const char* v = t->kids[0]->str;
    Node *xd = NULL, *vd = NULL;
    for (int k = from; k < s->n; k++) {
      Node* d = s->v[k];
      if (d->kind == nkVar && d->nkids == 1 && d->kids[0]->kind == nkCall &&
          (!strcmp(d->kids[0]->str, "line") || !strcmp(d->kids[0]->str, "ring") || !strcmp(d->kids[0]->str, "sheet"))) {
        if (!strcmp(d->str, x)) xd = d;
        if (!strcmp(d->str, v)) vd = d;
      }
    }
    if (!xd || !vd) continue;
    if (s->nm == 16) return said_no(__LINE__);
    Medium* m = &s->m[s->nm++];
    memset(m, 0, sizeof *m);
    m->x = x; m->v = v; m->xdecl = xd; m->vdecl = vd; m->xlaw = t;
    Node *pe = arg(xd->kids[0], "period", 0), *pv = arg(vd->kids[0], "period", 0);
    if (strcmp(xd->kids[0]->str, vd->kids[0]->str)) return said_no(__LINE__);
    m->axes = !strcmp(xd->kids[0]->str, "sheet") ? 2 : 1;
    if (m->axes == 2) {                                /* `field(period = [T, T])` arrives as sheet(T / sqrt 2): each axis's T */
      if (!(is(pe, nkBinOp, "/") && pe->kids[1]->kind == nkNum && pe->kids[1]->num == 1.4142135623730951) ||
          !(is(pv, nkBinOp, "/") && pv->kids[1]->kind == nkNum && pv->kids[1]->num == 1.4142135623730951)) return said_no(__LINE__);
      pe = pe->kids[0]; pv = pv->kids[0];
    }
    double T, T2;
    if (!plain_number(s, pe, &T) || !plain_number(s, pv, &T2) || T != T2 || T <= 0) return said_no(__LINE__);
    if (arg(xd->kids[0], "width", 99) || arg(vd->kids[0], "width", 99)) return said_no(__LINE__);
    if (arg(xd->kids[0], "start", 99) || arg(vd->kids[0], "start", 99)) return said_no(__LINE__);   /* a start: later */
    int round = !strcmp(xd->kids[0]->str, "ring");
    Node *ex = arg(xd->kids[0], "ends", 99), *ev = arg(vd->kids[0], "ends", 99);
    for (int e = 0; e < 2; e++) {
      m->ends[e] = round ? 0 : ends_of(ex, e);
      if (!round && (!m->ends[e] || ends_of(ev, e) != m->ends[e])) return said_no(__LINE__);
    }
    if (m->axes == 2 && m->ends[0] != m->ends[1]) return said_no(__LINE__);   /* a sheet's edge is one end all round */
    m->T = T;
    m->period = pe;
    m->store = fresh(s, "medium");
    m->tick = fresh(s, "medium_said");
  }
  /* The pairs: two fields, each's law reading the other's slope, on one
   * line or ring, the second's ends the others of the first's, one period. */
  for (int i = from; i < s->n; i++) {
    Node* d = s->v[i];
    if (!(d->kind == nkVar && d->nkids == 1 && d->kids[0]->kind == nkCall &&
          (!strcmp(d->kids[0]->str, "line") || !strcmp(d->kids[0]->str, "ring"))) || medium_of(s, d->str)) continue;
    Node *law = NULL, *plaw = NULL, *pd = NULL;
    for (int k = from; k < s->n; k++)
      if (is_law(s->v[k]) && !strcmp(unprimed(s, s->v[k]->str), d->str)) law = s->v[k];
    if (!law) continue;
    for (int k = i + 1; k < s->n && !pd; k++) {
      Node* e = s->v[k];
      if (!(e->kind == nkVar && e->nkids == 1 && e->kids[0]->kind == nkCall && !strcmp(e->kids[0]->str, d->kids[0]->str)) ||
          medium_of(s, e->str)) continue;
      Node* el = NULL;
      for (int q = from; q < s->n; q++)
        if (is_law(s->v[q]) && !strcmp(unprimed(s, s->v[q]->str), e->str)) el = s->v[q];
      if (el && reads_slope_of(s, inline_words(s, law->kids[0], 0), e->str) &&
          reads_slope_of(s, inline_words(s, el->kids[0], 0), d->str)) { pd = e; plaw = el; }
    }
    if (!pd) continue;
    if (s->nm == 16) return said_no(__LINE__);
    Medium* m = &s->m[s->nm++];
    memset(m, 0, sizeof *m);
    m->pair = 1;
    m->x = d->str; m->v = pd->str; m->xdecl = d; m->vdecl = pd; m->xlaw = law; m->vlaw = plaw;
    Node *pe = arg(d->kids[0], "period", 0), *pv = arg(pd->kids[0], "period", 0);
    double T, T2;
    /* A period that moves: two travelling waves, delay lines, exact for a
     * uniform pair, where turning every mode anew each sample cost 30 times
     * as much (slide_line, 0.46 of a core against 0.015). */
    int moves = !(plain_number(s, pe, &T) && plain_number(s, pv, &T2));
    if (moves ? !same_tree(pe, pv) || reads_system(s, pe, 0) : (T != T2 || T <= 0)) return said_no(__LINE__);
    m->delay = moves;
    if (arg(pd->kids[0], "width", 99)) return said_no(__LINE__);    /* the width is the pressure's, the first field's */
    m->width = arg(d->kids[0], "width", 99);
    if (m->width && (m->width->kind != nkLambda || m->width->nparams != 1)) return said_no(__LINE__);
    if (arg(d->kids[0], "start", 99) || arg(pd->kids[0], "start", 99)) return said_no(__LINE__);
    int round = !strcmp(d->kids[0]->str, "ring");
    Node *ex = arg(d->kids[0], "ends", 99), *ev = arg(pd->kids[0], "ends", 99);
    for (int e = 0; e < 2; e++) {
      m->ends[e] = round ? 0 : ends_of(ex, e);
      if (!round && (!m->ends[e] || ends_of(ev, e) != 3 - m->ends[e])) return said_no(__LINE__);   /* a pair's ends: one fixed, the other free */
    }
    m->T = plain_number(s, pe, &T) ? T : 0;
    m->period = pe;
    for (int f = 0; f < 2; f++) {
      Node* coef[3] = {0};
      Node* lw = f ? plaw : law;
      linear_pair(s, f ? m->v : m->x, f ? m->x : m->v, inline_words(s, lw->kids[0], 0), num(s, 1, lw), coef);
      if (s->no) return said_no(__LINE__);
      for (int c = 0; c < 3; c++) m->law[f][c] = coef[c] ? coef[c] : num(s, 0, lw);
    }
    if (m->width) {                                    /* a horn: a fixed period, no Δ on the flow */
      double du;
      if (m->delay || round || !plain_number(s, m->law[1][2], &du) || du != 0) return said_no(__LINE__);
    }
    if (m->delay) {                                    /* two waves: -∇ each, one loss for both, no Δ */
      double c0, c1, d0, d1;
      if (round || !plain_number(s, m->law[0][0], &c0) || !plain_number(s, m->law[1][0], &c1) || c0 != -1 || c1 != -1 ||
          !plain_number(s, m->law[0][2], &d0) || !plain_number(s, m->law[1][2], &d1) || d0 != 0 || d1 != 0) return said_no(__LINE__);
      double g0, g1;
      int both = plain_number(s, m->law[0][1], &g0) && plain_number(s, m->law[1][1], &g1);
      if (both ? g0 != g1 : !same_tree(m->law[0][1], m->law[1][1])) return said_no(__LINE__);
      m->loss = bin(s, "*", copy(s, m->law[0][1]), num(s, -1, law), law);
    }
    m->store = fresh(s, "medium");
    m->tick = fresh(s, "medium_said");
  }
  if (!s->nm) return said_no(__LINE__);
  for (int i = from; i < s->n; i++) {                  /* every line is a medium's */
    Node* d = s->v[i];
    if (d->kind == nkVar && d->nkids == 1 && d->kids[0]->kind == nkCall &&
        (!strcmp(d->kids[0]->str, "line") || !strcmp(d->kids[0]->str, "ring") || !strcmp(d->kids[0]->str, "sheet")) &&
        !medium_of(s, d->str))
      return said_no(__LINE__);
  }

  /* The laws: each medium's velocity law, linear; laws on plain state. */
  for (int i = from; i < s->n; i++) {
    Node* t = s->v[i];
    if (!is_law(t)) continue;
    const char* base = unprimed(s, t->str);
    Medium* m = medium_of(s, base);
    if (m && m->pair) continue;                        /* split above */
    if (m && !strcmp(base, m->x)) continue;
    if (m) {
      if (m->vlaw) return said_no(__LINE__);
      m->vlaw = t;
      Node* coef[5] = {0};
      linear(s, m, inline_words(s, t->kids[0], 0), num(s, 1, t), coef);
      if (s->no) return said_no(__LINE__);
      m->A = coef[0] ? coef[0] : num(s, 0, t);
      m->B = coef[1] ? coef[1] : num(s, 0, t);
      m->C = coef[2] ? coef[2] : num(s, 0, t);
      m->D = coef[3] ? coef[3] : num(s, 0, t);
      m->E = coef[4] ? coef[4] : num(s, 0, t);
      if (coef[4] && (m->ends[0] != 1 || m->ends[1] != 1)) return said_no(__LINE__);   /* a free bar's modes aren't sines: the grid, as yet */
      continue;
    }
    int declared = 0;                                  /* a law on plain state, declared as a number */
    for (int k = from; k < s->n; k++)
      if (s->v[k]->kind == nkVar && !strcmp(s->v[k]->str, base) && !(s->v[k]->kids[0]->kind == nkCall &&
          !strcmp(s->v[k]->kids[0]->str, "array"))) declared = 1;
    if (!declared || s->np == 64 || plain_of(s, base)) return said_no(__LINE__);
    s->p[s->np++] = (Plain){base, t, t->kids[0]};
  }
  for (int i = 0; i < s->nm; i++) if (!s->m[i].vlaw) return said_no(__LINE__);

  /* The sources: forces at places, on a medium's velocity. */
  for (int i = from; i < s->n; i++) {
    Node* t = s->v[i];
    if (t->kind != nkIdxAssign) continue;
    if (!is_source(t) || t->kids[0]->kind != nkIdent) return said_no(__LINE__);
    const char* base = unprimed(s, t->kids[0]->str);
    Medium* m = medium_of(s, base);
    if (!m || m->nsrc == 64) return said_no(__LINE__);
    Node *place = t->kids[1], *width = NULL;
    if (place->kind == nkCall && !strcmp(place->str, "gauss") && place->nkids == 2) { width = place->kids[1]; place = place->kids[0]; }
    Node* said = t->kids[2];
    int into = m->pair ? (!strcmp(base, m->x) ? 1 : 2) : !strcmp(base, m->x);
    if ((m->delay || m->width) && width) return said_no(__LINE__);     /* spread on travelling waves or a horn: later */
    m->src[m->nsrc++] = (Source){place, width, said->kids[1], !strcmp(said->str, "-"), into, NULL};
    if (reads_own(m, driven_field(m, into), place, said->kids[1]))
      m->src[m->nsrc - 1].contact = fresh(s, "contact");
  }

  /* The block, after everything. */
  Vec block = {0}, kept = {0}, solvers = {0};
  const Node* at = s->v[s->n - 1];
  for (int i = 0; i < s->nm; i++) {
    Medium* m = &s->m[i];
    if (m->delay) continue;                            /* travelling waves take their whole sample after the kicks */
    Node* turn = m->width
      ? call(s, "medium_horn_turn", at, 11, (Node*[]){
          ident(s, m->store, at), num(s, MODE_CAP, at), inside(s, m->period, 0),
          inside(s, m->law[0][0], 0), inside(s, m->law[0][1], 0), inside(s, m->law[0][2], 0),
          inside(s, m->law[1][0], 0), inside(s, m->law[1][1], 0),
          num(s, m->ends[0], at), num(s, m->ends[1], at), bin(s, "/", ident(s, "dt", at), num(s, 2, at), at)})
      : m->pair
      ? call(s, "medium_pair_turn", at, 12, (Node*[]){
          ident(s, m->store, at), num(s, MODE_CAP, at), inside(s, m->period, 0),
          inside(s, m->law[0][0], 0), inside(s, m->law[0][1], 0), inside(s, m->law[0][2], 0),
          inside(s, m->law[1][0], 0), inside(s, m->law[1][1], 0), inside(s, m->law[1][2], 0),
          num(s, m->ends[0], at), num(s, m->ends[1], at), bin(s, "/", ident(s, "dt", at), num(s, 2, at), at)})
      : call(s, "medium_turn", at, 13, (Node*[]){
          ident(s, m->store, at), num(s, MODE_CAP, at), num(s, m->T, at),
          inside(s, m->A, 0), inside(s, m->B, 0), inside(s, m->C, 0), inside(s, m->D, 0), inside(s, m->E, 0),
          num(s, m->ends[0], at), num(s, m->ends[1], at),
          num(s, m->axes == 2 ? m->ends[0] : -1, at), num(s, m->axes == 2 ? m->ends[1] : -1, at),
          bin(s, "/", ident(s, "dt", at), num(s, 2, at), at)});
    push(&block, store(s, nkAssign, m->tick, turn));
  }
  /* What the laws and forces read, at the midpoint. */
  Node* rate[64];
  for (int j = 0; j < s->np; j++) rate[j] = inside(s, s->p[j].rate, 0);
  Node* force[16][64];
  Node* place[16][64];
  Node* spread[16][64];
  for (int i = 0; i < s->nm; i++)
    for (int k = 0; k < s->m[i].nsrc; k++) {
      Source* src = &s->m[i].src[k];
      /* a contact's own reading is the unknown it is solved for, not a reading at the midpoint */
      force[i][k] = inside(s, src->contact ? reading_as(s, src->force, driven_field(&s->m[i], src->into), src->at, src->contact)
                                           : src->force, 0);
      place[i][k] = inside(s, src->at, 0);
      spread[i][k] = src->width ? inside(s, src->width, 0) : num(s, 0, src->at);
      if (reads_system(s, src->at, 0) || (src->width && reads_system(s, src->width, 0))) s->no = 1;
    }
  if (s->no) { free(block.v); free(s->reads.v); return said_no(__LINE__); }
  for (int k = 0; k < s->reads.n; k++) push(&block, s->reads.v[k]);

  /* The laws on plain state, by 4th-order Gauss-Legendre over the sample,
   * as everywhere (imports.c, plain_block): two implicit stages, eight
   * fixed-point turns from an explicit guess, the media held at the midpoint. */
  const char *now[64], *y1[64], *y2[64], *f1[64], *f2[64], *next[64];
  for (int j = 0; j < s->np; j++) now[j] = s->p[j].name;
  Node* dt = ident(s, "dt", at);
  if (s->np) {
    const double r3 = sqrt(3.0) / 6.0, c1 = 0.5 - r3, c2 = 0.5 + r3, a11 = 0.25, a12 = 0.25 - r3, a21 = 0.25 + r3, a22 = 0.25;
    const int turns = 8;
    #define GL_STEP(base, coef, rate) bin(s, "+", ident(s, (base), at), bin(s, "*", bin(s, "*", num(s, (coef), at), copy(s, dt), at), (rate), at), at)
    const char* f0[64];
    for (int j = 0; j < s->np; j++) { f0[j] = fresh(s, "rate"); push(&block, let(s, f0[j], staged(s, rate[j], now))); }
    for (int j = 0; j < s->np; j++) {
      y1[j] = fresh(s, "stage"); push(&block, let(s, y1[j], GL_STEP(now[j], c1, ident(s, f0[j], at))));
      y2[j] = fresh(s, "stage"); push(&block, let(s, y2[j], GL_STEP(now[j], c2, ident(s, f0[j], at))));
    }
    for (int it = 0; it <= turns; it++) {
      for (int j = 0; j < s->np; j++) {
        f1[j] = fresh(s, "rate"); push(&block, let(s, f1[j], staged(s, rate[j], y1)));
        f2[j] = fresh(s, "rate"); push(&block, let(s, f2[j], staged(s, rate[j], y2)));
      }
      if (it == turns) break;
      const char *n1[64], *n2[64];
      for (int j = 0; j < s->np; j++) {
        n1[j] = fresh(s, "stage");
        push(&block, let(s, n1[j], bin(s, "+", GL_STEP(now[j], a11, ident(s, f1[j], at)),
                                       bin(s, "*", bin(s, "*", num(s, a12, at), copy(s, dt), at), ident(s, f2[j], at), at), at)));
        n2[j] = fresh(s, "stage");
        push(&block, let(s, n2[j], bin(s, "+", GL_STEP(now[j], a21, ident(s, f1[j], at)),
                                       bin(s, "*", bin(s, "*", num(s, a22, at), copy(s, dt), at), ident(s, f2[j], at), at), at)));
      }
      for (int j = 0; j < s->np; j++) { y1[j] = n1[j]; y2[j] = n2[j]; }
    }
    for (int j = 0; j < s->np; j++) {
      next[j] = fresh(s, "next");
      push(&block, let(s, next[j], GL_STEP(now[j], 0.5, bin(s, "+", ident(s, f1[j], at), ident(s, f2[j], at), at))));
    }
    #undef GL_STEP
  }
  /* Travelling waves take their whole sample now, so their forces land on
   * the sample just made, and the first sample's too (a kick needs the
   * crossing its turn sets). */
  for (int i = 0; i < s->nm; i++) {
    Medium* m = &s->m[i];
    if (!m->delay) continue;
    push(&block, store(s, nkAssign, m->tick, call(s, "medium_line_turn", at, 6, (Node*[]){
      ident(s, m->store, at), inside(s, m->period, 0), inside(s, m->loss, 0),
      num(s, m->ends[0], at), num(s, m->ends[1], at), ident(s, "dt", at)})));
  }
  /* Each force, averaged over the step as the method's own quadrature does
   * (the mean of it at the two stages), kicks its medium. */
  for (int i = 0; i < s->nm; i++) {
    Medium* m = &s->m[i];
    for (int k = 0; k < m->nsrc; k++) {
      if (m->src[k].contact) continue;                 /* contacts are solved after every other force has landed */
      Node* f0 = force[i][k];
      Node* f = s->np ? bin(s, "*", num(s, 0.5, at), bin(s, "+", staged(s, f0, y1), staged(s, f0, y2), at), at) : f0;
      Node* impulse = bin(s, "*", f, copy(s, dt), at);
      if (m->src[k].minus) impulse = bin(s, "*", impulse, num(s, -1, at), at);
      Node* kick = m->delay || m->width
        ? call(s, m->width ? "medium_horn_kick" : "medium_line_kick", at, 4,
               (Node*[]){ident(s, m->store, at), impulse, place[i][k], num(s, m->src[k].into, at)})
        : call(s, m->axes == 2 ? "medium_kick2" : "medium_kick", at, 5, (Node*[]){ident(s, m->store, at), impulse,
                                                                              place[i][k], spread[i][k], num(s, m->src[k].into, at)});
      push(&block, store(s, nkAssign, m->tick, kick));
    }
  }
  /* A contact, a force that reads its own field where it acts (a bow's
   * friction, a hammer's felt), is solved within the step: u, the field there
   * once the force has acted, is u0 (there with every other force in) plus the
   * medium's admittance there times the force's impulse, u = u0 + Y dt f(u).
   * Y is measured: a unit impulse kicked in, read, and kicked back out, exact
   * for a medium that is linear. Newton's method, its slope by a difference,
   * from the last sample's root, so a bow stuck stays stuck until the force
   * can't hold it, and slips until it can: stick and slip, and their hysteresis. */
  for (int i = 0; i < s->nm; i++) {
    Medium* m = &s->m[i];
    for (int k = 0; k < m->nsrc; k++) {
      Source* src = &m->src[k];
      if (!src->contact) continue;
      const char* field = driven_field(m, src->into);
      Node* read_node = mk(s, nkIdx, "", at);
      kids(s, read_node, 2, (Node*[]){ident(s, field, at), copy(s, place[i][k])});
      read_node->num = 2;
      #define CONTACT_KICK(imp) (m->delay || m->width \
        ? call(s, m->width ? "medium_horn_kick" : "medium_line_kick", at, 4, (Node*[]){ident(s, m->store, at), (imp), copy(s, place[i][k]), num(s, src->into, at)}) \
        : call(s, m->axes == 2 ? "medium_kick2" : "medium_kick", at, 5, (Node*[]){ident(s, m->store, at), (imp), copy(s, place[i][k]), copy(s, spread[i][k]), num(s, src->into, at)}))
      const char* u0 = fresh(s, "contact_from");
      push(&block, let(s, u0, outside(s, copy(s, read_node))));
      push(&block, store(s, nkAssign, m->tick, CONTACT_KICK(num(s, 1, at))));
      const char* up = fresh(s, "contact_kicked");
      push(&block, let(s, up, outside(s, copy(s, read_node))));
      push(&block, store(s, nkAssign, m->tick, CONTACT_KICK(num(s, -1, at))));
      const char* Y = fresh(s, "admittance");
      push(&block, let(s, Y, bin(s, "*", bin(s, "-", ident(s, up, at), ident(s, u0, at), at), copy(s, dt), at)));   /* Y dt */
      double sign = src->minus ? -1.0 : 1.0;
      Node* f0 = force[i][k];
      /* the force at u: f0 with the contact's state read as u (and averaged over the stages, as the others are) */
      #define FORCE_AT(uname) ({ const char* from_[1] = {src->contact}; const char* to_[1] = {(uname)}; \
        Node* g_ = subst_names(s, f0, from_, to_, 1); \
        s->np ? bin(s, "*", num(s, 0.5, at), bin(s, "+", staged(s, g_, y1), staged(s, g_, y2), at), at) : g_; })
      const char* u = src->contact;                    /* the last root, a state */
      /* the velocity the force meets is the step's mean, halfway through its
       * kick: before it, u0; after, u0 + Y dt f. Matched to the after, a stuck
       * bow holds a still string (the reads swing half a kick either side). */
      Node* cs = bin(s, "*", ident(s, Y, at), num(s, 0.5 * sign, at), at);
      /* G(x) = x - u0 - Y dt sign f(x), zero at the root; named, so each is one let */
      #define G_OF(xname) bin(s, "-", bin(s, "-", ident(s, (xname), at), ident(s, u0, at), at), \
                                  bin(s, "*", copy(s, cs), FORCE_AT(xname), at), at)
      #define G_AT(xname) ({ const char* gx_ = fresh(s, "contact_g"); push(&block, let(s, gx_, G_OF(xname))); gx_; })
      #define IF(c, a_, b_) ({ Node* if_ = mk(s, nkIf, "", at); kids(s, if_, 3, (Node*[]){(c), (a_), (b_)}); if_; })
      #define LET(name, v) ({ const char* n_ = fresh(s, name); push(&block, let(s, n_, (v))); n_; })
      #define ID(n) ident(s, (n), at)
      /* From the last root, toward where G says the root is (down if G > 0):
       * the nearest root that way, so the contact keeps its state, stuck or
       * slipping, while that state's root lasts, and jumps only when it's gone.
       * The friction curve's fall makes three roots in the middle (McIntyre,
       * Schumacher & Woodhouse 1983); Newton's method alone wanders among them.
       * First a step out, half again the Newton step at first and eight times
       * as far each time (to 8^8), until G changes sign; then regula falsi in
       * the bracket, the Illinois way (an end kept twice has its G halved),
       * every other step a halving, so the bracket at least halves each two,
       * until G is within 10^-14 (40 steps at most). */
      const char* r = LET("contact_r", ID(u));
      const char* g0 = G_AT(r);
      const char* re = LET("contact_re", bin(s, "+", ID(r), num(s, 1e-7, at), at));
      const char* ge = G_AT(re);
      const char* dir = LET("contact_dir", IF(bin(s, ">", ID(g0), num(s, 0, at), at), num(s, -1, at), num(s, 1, at)));
      Node* slope = bin(s, "/", bin(s, "-", ID(ge), ID(g0), at), num(s, 1e-7, at), at);
      Node* newton = bin(s, "/", call(s, "abs", at, 1, (Node*[]){ID(g0)}), call(s, "abs", at, 1, (Node*[]){slope}), at);
      const char* step = LET("contact_step", call(s, "max", at, 2, (Node*[]){
        IF(bin(s, ">", copy(s, slope), num(s, 1e-12, at), at), bin(s, "*", num(s, 1.5, at), newton, at),
           call(s, "abs", at, 1, (Node*[]){ID(g0)})), num(s, 1e-12, at)}));
      /* The search is a loop, a `def rec` of the patch's own, each repeat one
       * trial point and one evaluation of the force, stopping when it's done:
       * a step out from the last root while G keeps its sign, then the
       * bracket closed. The force's names come in as parameters. */
      const char* names[64]; int nn = 0;
      free_names(f0, src->contact, names, &nn, 64);
      const char* fixed_[] = {u0, Y, r, g0, dir, step};
      enum { NF = 6 };
      const char* loop = fresh(s, "contact_solve");
      const char* P[] = {"__cs_ph", "__cs_a", "__cs_ga", "__cs_b", "__cs_gb", "__cs_reach", "__cs_k"};
      enum { NP = 7 };
      int total = NP + NF + nn;
      Node* def = mk(s, nkDef, loop, at);
      def->is_rec = 1;
      def->nparams = total;
      def->params = arena_alloc(s->a, total * sizeof(char*));
      def->defaults = arena_alloc(s->a, total * sizeof(Node*));
      for (int q = 0; q < NP; q++) def->params[q] = P[q];
      for (int q = 0; q < NF; q++) def->params[NP + q] = fixed_[q];
      for (int q = 0; q < nn; q++) def->params[NP + NF + q] = names[q];
      #define PI(q) ident(s, P[q], at)
      #define REPEAT(...) ({ Node* args_[] = {__VA_ARGS__}; Node* all_[total]; \
        for (int q = 0; q < NP; q++) all_[q] = args_[q]; \
        for (int q = 0; q < NF; q++) all_[NP + q] = ident(s, fixed_[q], at); \
        for (int q = 0; q < nn; q++) all_[NP + NF + q] = ident(s, names[q], at); \
        call(s, loop, at, total, all_); })
      Vec body = {0};
      Node* odd = bin(s, "==", bin(s, "-", PI(6), bin(s, "*", num(s, 2, at), call(s, "floor", at, 1, (Node*[]){bin(s, "/", PI(6), num(s, 2, at), at)}), at), at), num(s, 1, at), at);
      Node* span = bin(s, "-", PI(4), PI(2), at);
      Node* falsi = IF(bin(s, "==", copy(s, span), num(s, 0, at), at), PI(3),
                       bin(s, "-", PI(3), bin(s, "/", bin(s, "*", PI(4), bin(s, "-", PI(3), PI(1), at), at), span, at), at));
      Node* trial = IF(bin(s, "==", PI(0), num(s, 0, at), at),
                       bin(s, "+", ID(r), bin(s, "*", bin(s, "*", ID(dir), ID(step), at), PI(5), at), at),
                       IF(copy(s, odd), bin(s, "*", num(s, 0.5, at), bin(s, "+", PI(1), PI(3), at), at), falsi));
      Node* tiny_g = bin(s, "<=", call(s, "abs", at, 1, (Node*[]){PI(4)}),
                         bin(s, "*", num(s, 1e-14, at), bin(s, "+", num(s, 1, at), call(s, "abs", at, 1, (Node*[]){PI(3)}), at), at), at);
      Node* tiny_b = bin(s, "<=", call(s, "abs", at, 1, (Node*[]){bin(s, "-", PI(3), PI(1), at)}),
                         bin(s, "*", num(s, 1e-13, at), bin(s, "+", num(s, 1, at), call(s, "abs", at, 1, (Node*[]){PI(3)}), at), at), at);
      Node* done_e = bin(s, "and", bin(s, "==", PI(0), num(s, 1, at), at),
                         bin(s, "or", bin(s, "or", tiny_g, tiny_b, at), bin(s, ">=", PI(6), num(s, 40, at), at), at), at);
      push(&body, let(s, "__cs_done", done_e));
      push(&body, let(s, "__cs_t", trial));
      Node* g_t = bin(s, "-", bin(s, "-", ident(s, "__cs_t", at), ID(u0), at),
                      bin(s, "*", bin(s, "*", ID(Y), num(s, 0.5 * sign, at), at), FORCE_AT("__cs_t"), at), at);
      push(&body, let(s, "__cs_gt", IF(ID("__cs_done"), num(s, 0, at), g_t)));
      Node* crossed = bin(s, "<=", bin(s, "*", ID("__cs_gt"), ID(g0), at), num(s, 0, at), at);
      Node* out = IF(crossed,
        REPEAT(num(s, 1, at), PI(1), PI(2), ID("__cs_t"), ID("__cs_gt"), PI(5), num(s, 0, at)),
        IF(bin(s, ">=", PI(6), num(s, 8, at), at), ID("__cs_t"),
           REPEAT(num(s, 0, at), ID("__cs_t"), ID("__cs_gt"), PI(3), PI(4), bin(s, "*", PI(5), num(s, 8, at), at),
                  bin(s, "+", PI(6), num(s, 1, at), at))));
      Node* kept = bin(s, ">", bin(s, "*", ID("__cs_gt"), PI(4), at), num(s, 0, at), at);
      Node* in = REPEAT(num(s, 1, at), IF(copy(s, kept), PI(1), PI(3)),
                        IF(kept, IF(copy(s, odd), PI(2), bin(s, "*", num(s, 0.5, at), PI(2), at)), PI(4)),
                        ID("__cs_t"), ID("__cs_gt"), PI(5), bin(s, "+", PI(6), num(s, 1, at), at));
      push(&body, IF(ID("__cs_done"), PI(3), IF(bin(s, "==", PI(0), num(s, 0, at), at), out, in)));
      Node* blk = mk(s, nkBlock, "", at);
      kids(s, blk, body.n, body.v);
      free(body.v);
      kids(s, def, 1, (Node*[]){blk});
      push(&solvers, def);
      const char* cur = LET("contact_u", IF(bin(s, "==", ID(g0), num(s, 0, at), at), ID(r),
        REPEAT(num(s, 0, at), ID(r), ID(g0), ID(r), ID(g0), num(s, 1, at), num(s, 0, at))));
      #undef PI
      #undef REPEAT
      #undef G_AT
      #undef G_OF
      #undef IF
      #undef LET
      #undef ID
      const char* ff = fresh(s, "contact_force");
      push(&block, let(s, ff, FORCE_AT(cur)));
      push(&block, store(s, nkAssign, m->tick, CONTACT_KICK(bin(s, "*", bin(s, "*", ident(s, ff, at), num(s, sign, at), at), copy(s, dt), at))));
      push(&block, store(s, nkAssign, u, ident(s, cur, at)));
      #undef CONTACT_KICK
      #undef FORCE_AT
    }
  }
  for (int j = 0; j < s->np; j++) push(&block, store(s, nkAssign, now[j], ident(s, next[j], at)));
  for (int i = 0; i < s->nm; i++) {
    Medium* m = &s->m[i];
    if (m->delay) continue;                            /* it took its sample before the kicks */
    Node* turn = m->width
      ? call(s, "medium_horn_turn", at, 11, (Node*[]){
          ident(s, m->store, at), num(s, MODE_CAP, at), inside(s, m->period, 0),
          inside(s, m->law[0][0], 0), inside(s, m->law[0][1], 0), inside(s, m->law[0][2], 0),
          inside(s, m->law[1][0], 0), inside(s, m->law[1][1], 0),
          num(s, m->ends[0], at), num(s, m->ends[1], at), bin(s, "/", ident(s, "dt", at), num(s, 2, at), at)})
      : m->pair
      ? call(s, "medium_pair_turn", at, 12, (Node*[]){
          ident(s, m->store, at), num(s, MODE_CAP, at), inside(s, m->period, 0),
          inside(s, m->law[0][0], 0), inside(s, m->law[0][1], 0), inside(s, m->law[0][2], 0),
          inside(s, m->law[1][0], 0), inside(s, m->law[1][1], 0), inside(s, m->law[1][2], 0),
          num(s, m->ends[0], at), num(s, m->ends[1], at), bin(s, "/", ident(s, "dt", at), num(s, 2, at), at)})
      : call(s, "medium_turn", at, 13, (Node*[]){
          ident(s, m->store, at), num(s, MODE_CAP, at), num(s, m->T, at),
          inside(s, m->A, 0), inside(s, m->B, 0), inside(s, m->C, 0), inside(s, m->D, 0), inside(s, m->E, 0),
          num(s, m->ends[0], at), num(s, m->ends[1], at),
          num(s, m->axes == 2 ? m->ends[0] : -1, at), num(s, m->axes == 2 ? m->ends[1] : -1, at),
          bin(s, "/", ident(s, "dt", at), num(s, 2, at), at)});
    push(&block, store(s, nkAssign, m->tick, turn));
  }

  /* The patch's own lines, outside: the media's declarations become their
   * state, their laws and the sources go, each reading of a medium is of its
   * state at the moment's start, and the lets that only name field words go
   * (they were written into the laws). */
  for (int i = 0; i < from; i++) push(&kept, s->v[i]);
  for (int i = from; i < s->n; i++) {
    Node* t = s->v[i];
    Medium* m = (t->kind == nkVar) ? medium_of(s, t->str) : NULL;
    if (m && t == m->xdecl) {
      for (int k = 0; k < m->nsrc; k++)                /* each contact's last root */
        if (m->src[k].contact) push(&kept, store(s, nkVar, m->src[k].contact, num(s, 0, t)));
      if (m->width) {                                  /* a horn's state, its width at 513 places laid in by the array's rule */
        int off = 24 + 7 * MODE_CAP, size = off + 513 + 2 * MODE_CAP * 1025;
        const char* i_ = fresh(s, "place");
        Node* where = bin(s, "/", bin(s, "-", ident(s, i_, t), num(s, off, t), t), num(s, 512, t), t);
        Node* w = substitute(s, m->width->kids[0], m->width->params[0], where);
        Node* in = bin(s, "and", bin(s, ">=", ident(s, i_, t), num(s, off, t), t), bin(s, "<=", ident(s, i_, t), num(s, off + 512, t), t), t);
        Node* cond = mk(s, nkIf, "", t);
        kids(s, cond, 3, (Node*[]){in, w, num(s, 0, t)});
        Node* rule = mk(s, nkLambda, "", t);
        kids(s, rule, 1, (Node*[]){cond});
        rule->nparams = 1;
        rule->params = arena_alloc(s->a, sizeof(char*));
        rule->params[0] = i_;
        push(&kept, store(s, nkVar, m->store, call(s, "array", t, 2, (Node*[]){num(s, size, t), rule})));
      } else
      push(&kept, store(s, nkVar, m->store, call(s, "array", t, 1, (Node*[]){num(s, m->delay ? 24 + 2 * 131072 : 24 + 10 * MODE_CAP, t)})));
      push(&kept, store(s, nkVar, m->tick, num(s, 0, t)));
      continue;
    }
    if (m && t == m->vdecl) continue;
    if (is_law(t) || is_source(t)) {
      const char* base = unprimed(s, is_law(t) ? t->str : t->kids[0]->str);
      if (medium_of(s, base) || plain_of(s, base)) continue;
    }
    if (t->kind == nkLet && t->nkids == 1 && says_field(s, t->kids[0], 0)) continue;
    push(&kept, outside(s, copy(s, t)));          /* a copy: the patch is untouched until it's taken */
  }
  if (s->no) { free(block.v); free(kept.v); free(s->reads.v); return said_no(__LINE__); }
  for (int k = 0; k < solvers.n; k++) push(&kept, solvers.v[k]);   /* the contacts' searches, defs of the patch's own */
  free(solvers.v);
  for (int k = 0; k < block.n; k++) push(&kept, block.v[k]);
  free(block.v);
  /* What the mode cap leaves out is said, not hidden: a field slow against
   * the rate has more modes under Nyquist than it keeps, and is band-limited
   * to the highest it keeps. */
  /* Counted from the law itself (a plate's modes are spaced as m^2 + n^2,
   * a membrane's as their root), when its coefficients are plain numbers. */
  for (int i = 0; i < s->nm; i++) {
    Medium* m = &s->m[i];
    double A, B, E, T = m->T;
    if (m->delay || m->pair || T <= 0 || !plain_number(s, m->A, &A) || !plain_number(s, m->B, &B) ||
        !plain_number(s, m->E, &E)) continue;
    double ny = M_PI * fields_sr(), o = m->ends[0] == m->ends[1] ? (m->ends[0] == 1 ? 1.0 : 0.0) : 0.5;
    if (m->ends[0] == 0) o = 0.0;
    long under = 0, kept_at = -1;
    double band = 0, w[1 << 14];
    int nw = 0;
    for (long j1 = 0; j1 < 4096; j1++) {
      double k1 = (m->ends[0] == 0 ? (double)((j1 + 1) / 2) : j1 + o) * M_PI;
      long seen = 0;
      for (long j2 = 0; j2 < (m->axes == 2 ? 4096 : 1); j2++) {
        double k2 = m->axes == 2 ? (j2 + o) * M_PI : 0.0, l = k1 * k1 + k2 * k2;
        double w2 = (4.0 * A * l - B - 16.0 * E * l * l) / (T * T);
        if (w2 > ny * ny) break;
        under++; seen++;
        if (nw < (1 << 14)) w[nw++] = sqrt(w2 > 0 ? w2 : 0);
      }
      if (!seen && j1 > 0) break;
    }
    if (under > MODE_CAP) {                            /* the cap-th lowest: the band it keeps */
      for (int a2 = 0; a2 < nw && a2 <= MODE_CAP; a2++)
        for (int b2 = a2 + 1; b2 < nw; b2++) if (w[b2] < w[a2]) { double t2 = w[a2]; w[a2] = w[b2]; w[b2] = t2; }
      kept_at = MODE_CAP - 1;
      band = w[kept_at] / (2.0 * M_PI);
      fprintf(stderr, "[aither] $%s keeps %d of its %ld modes under Nyquist: band-limited to about %.0f Hz\n",
              m->x, MODE_CAP, under, band);
    }
  }
  free(s->reads.v);
  free(*vp);
  *vp = kept.v;
  *np = kept.n;
  if (capp) *capp = kept.cap;
  return 1;
}
