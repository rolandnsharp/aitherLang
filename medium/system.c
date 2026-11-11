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
 *   the laws on plain state step by RK4 on it, and each source's force,
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

#define MODE_CAP 256               /* modes kept a medium, at most: the rest is left out, and said */

typedef struct { Node** v; int n, cap; } Vec;
static void push(Vec* k, Node* n) {
  if (k->n == k->cap) { k->cap = k->cap ? 2 * k->cap : 64; k->v = realloc(k->v, k->cap * sizeof(Node*)); }
  k->v[k->n++] = n;
}

typedef struct { Node* at; Node* width; Node* force; int minus, into; } Source;   /* into: 0 velocity, 1 place or a pair's first, 2 its partner */

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
    push(&s->reads, let(s, name, call(s, vel ? (m->pair ? "medium_partner_at" : m->axes == 2 ? "medium_velocity_at2" : "medium_velocity_at")
                                                : m->axes == 2 ? "medium_at2" : "medium_at", n, 2,
                                      (Node*[]){ident(s, m->store, n), at})));
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

int system_pass(Arena* a, Node*** vp, int* np, int* capp, int from, char* err, int errlen) {
  (void)capp;
  const char* off = getenv("AITHER_CORE");
  if (off && !strcmp(off, "0")) return 0;
  Sys sys = {0}, *s = &sys;
  s->a = a; s->err = err; s->errlen = errlen; s->v = *vp; s->n = *np; s->from = from;

  /* What the patch has. Anything with a field below the file's level, or a
   * field this milestone doesn't hold, goes the old way. */
  int any_line = 0;
  for (int i = from; i < s->n; i++) {
    Node* t = s->v[i];
    if ((t->kind == nkDef || t->kind == nkPlay) && holds_field(t)) return 0;
    if (t->kind == nkVar && t->nkids == 1 && t->kids[0]->kind == nkCall) {
      const char* w = t->kids[0]->str;
      if (!strcmp(w, "line") || !strcmp(w, "ring") || !strcmp(w, "sheet")) any_line = 1;
    }
    if (t->kind == nkVar && t->nkids == 1 && t->kids[0]->kind == nkCall && !strcmp(t->kids[0]->str, "array")) {
      for (int k = from; k < s->n; k++)                /* an array assigned bare is a field of the old kind */
        if (s->v[k]->kind == nkAssign && !strcmp(s->v[k]->str, t->str)) return 0;
    }
  }
  if (!any_line) return 0;

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
    if (s->nm == 16) return 0;
    Medium* m = &s->m[s->nm++];
    memset(m, 0, sizeof *m);
    m->x = x; m->v = v; m->xdecl = xd; m->vdecl = vd; m->xlaw = t;
    Node *pe = arg(xd->kids[0], "period", 0), *pv = arg(vd->kids[0], "period", 0);
    if (strcmp(xd->kids[0]->str, vd->kids[0]->str)) return 0;
    m->axes = !strcmp(xd->kids[0]->str, "sheet") ? 2 : 1;
    if (m->axes == 2) {                                /* `field(period = [T, T])` arrives as sheet(T / sqrt 2): each axis's T */
      if (!(is(pe, nkBinOp, "/") && pe->kids[1]->kind == nkNum && pe->kids[1]->num == 1.4142135623730951) ||
          !(is(pv, nkBinOp, "/") && pv->kids[1]->kind == nkNum && pv->kids[1]->num == 1.4142135623730951)) return 0;
      pe = pe->kids[0]; pv = pv->kids[0];
    }
    double T, T2;
    if (!plain_number(s, pe, &T) || !plain_number(s, pv, &T2) || T != T2 || T <= 0) return 0;
    if (arg(xd->kids[0], "width", 99) || arg(vd->kids[0], "width", 99)) return 0;
    if (arg(xd->kids[0], "start", 99) || arg(vd->kids[0], "start", 99)) return 0;   /* a start: later */
    int round = !strcmp(xd->kids[0]->str, "ring");
    Node *ex = arg(xd->kids[0], "ends", 99), *ev = arg(vd->kids[0], "ends", 99);
    for (int e = 0; e < 2; e++) {
      m->ends[e] = round ? 0 : ends_of(ex, e);
      if (!round && (!m->ends[e] || ends_of(ev, e) != m->ends[e])) return 0;
    }
    if (m->axes == 2 && m->ends[0] != m->ends[1]) return 0;   /* a sheet's edge is one end all round */
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
    if (s->nm == 16) return 0;
    Medium* m = &s->m[s->nm++];
    memset(m, 0, sizeof *m);
    m->pair = 1;
    m->x = d->str; m->v = pd->str; m->xdecl = d; m->vdecl = pd; m->xlaw = law; m->vlaw = plaw;
    Node *pe = arg(d->kids[0], "period", 0), *pv = arg(pd->kids[0], "period", 0);
    double T, T2;
    /* A period that moves stays on delay lines, the old way: exact for a
     * uniform pair, and turning every mode anew each sample cost 30 times
     * as much (slide_line, 0.46 of a core against 0.015). */
    if (!plain_number(s, pe, &T) || !plain_number(s, pv, &T2) || T != T2 || T <= 0) return 0;
    if (arg(d->kids[0], "width", 99) || arg(pd->kids[0], "width", 99)) return 0;    /* a horn: later */
    if (arg(d->kids[0], "start", 99) || arg(pd->kids[0], "start", 99)) return 0;
    int round = !strcmp(d->kids[0]->str, "ring");
    Node *ex = arg(d->kids[0], "ends", 99), *ev = arg(pd->kids[0], "ends", 99);
    for (int e = 0; e < 2; e++) {
      m->ends[e] = round ? 0 : ends_of(ex, e);
      if (!round && (!m->ends[e] || ends_of(ev, e) != 3 - m->ends[e])) return 0;   /* a pair's ends: one fixed, the other free */
    }
    m->T = plain_number(s, pe, &T) ? T : 0;
    m->period = pe;
    for (int f = 0; f < 2; f++) {
      Node* coef[3] = {0};
      Node* lw = f ? plaw : law;
      linear_pair(s, f ? m->v : m->x, f ? m->x : m->v, inline_words(s, lw->kids[0], 0), num(s, 1, lw), coef);
      if (s->no) return 0;
      for (int c = 0; c < 3; c++) m->law[f][c] = coef[c] ? coef[c] : num(s, 0, lw);
    }
    m->store = fresh(s, "medium");
    m->tick = fresh(s, "medium_said");
  }
  if (!s->nm) return 0;
  for (int i = from; i < s->n; i++) {                  /* every line is a medium's */
    Node* d = s->v[i];
    if (d->kind == nkVar && d->nkids == 1 && d->kids[0]->kind == nkCall &&
        (!strcmp(d->kids[0]->str, "line") || !strcmp(d->kids[0]->str, "ring") || !strcmp(d->kids[0]->str, "sheet")) &&
        !medium_of(s, d->str))
      return 0;
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
      if (m->vlaw) return 0;
      m->vlaw = t;
      Node* coef[5] = {0};
      linear(s, m, inline_words(s, t->kids[0], 0), num(s, 1, t), coef);
      if (s->no) return 0;
      m->A = coef[0] ? coef[0] : num(s, 0, t);
      m->B = coef[1] ? coef[1] : num(s, 0, t);
      m->C = coef[2] ? coef[2] : num(s, 0, t);
      m->D = coef[3] ? coef[3] : num(s, 0, t);
      m->E = coef[4] ? coef[4] : num(s, 0, t);
      if (coef[4] && (m->ends[0] != 1 || m->ends[1] != 1 || m->axes == 2)) return 0;   /* a free bar's modes aren't sines; a plate: later */
      continue;
    }
    int declared = 0;                                  /* a law on plain state, declared as a number */
    for (int k = from; k < s->n; k++)
      if (s->v[k]->kind == nkVar && !strcmp(s->v[k]->str, base) && !(s->v[k]->kids[0]->kind == nkCall &&
          !strcmp(s->v[k]->kids[0]->str, "array"))) declared = 1;
    if (!declared || s->np == 64 || plain_of(s, base)) return 0;
    s->p[s->np++] = (Plain){base, t, t->kids[0]};
  }
  for (int i = 0; i < s->nm; i++) if (!s->m[i].vlaw) return 0;

  /* The sources: forces at places, on a medium's velocity. */
  for (int i = from; i < s->n; i++) {
    Node* t = s->v[i];
    if (t->kind != nkIdxAssign) continue;
    if (!is_source(t) || t->kids[0]->kind != nkIdent) return 0;
    const char* base = unprimed(s, t->kids[0]->str);
    Medium* m = medium_of(s, base);
    if (!m || m->nsrc == 64) return 0;
    Node *place = t->kids[1], *width = NULL;
    if (place->kind == nkCall && !strcmp(place->str, "gauss") && place->nkids == 2) { width = place->kids[1]; place = place->kids[0]; }
    Node* said = t->kids[2];
    int into = m->pair ? (!strcmp(base, m->x) ? 1 : 2) : !strcmp(base, m->x);
    m->src[m->nsrc++] = (Source){place, width, said->kids[1], !strcmp(said->str, "-"), into};
  }

  /* The block, after everything. */
  Vec block = {0}, kept = {0};
  const Node* at = s->v[s->n - 1];
  for (int i = 0; i < s->nm; i++) {
    Medium* m = &s->m[i];
    Node* turn = m->pair
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
      force[i][k] = inside(s, src->force, 0);
      place[i][k] = inside(s, src->at, 0);
      spread[i][k] = src->width ? inside(s, src->width, 0) : num(s, 0, src->at);
      if (reads_system(s, src->at, 0) || (src->width && reads_system(s, src->width, 0))) s->no = 1;
    }
  if (s->no) { free(block.v); free(s->reads.v); return 0; }
  for (int k = 0; k < s->reads.n; k++) push(&block, s->reads.v[k]);

  /* The laws on plain state, RK4 over the sample. */
  const char *now[64], *stage[64], *k1[64], *k2[64], *k3[64], *k4[64], *next[64];
  for (int j = 0; j < s->np; j++) now[j] = s->p[j].name;
  Node* dt = ident(s, "dt", at);
  if (s->np) {
    const char** ks[4] = {k1, k2, k3, k4};
    double step[4] = {0, 0.5, 0.5, 1};
    for (int g = 0; g < 4; g++) {
      if (g) for (int j = 0; j < s->np; j++) {
        stage[j] = fresh(s, "stage");
        push(&block, let(s, stage[j], bin(s, "+", ident(s, now[j], at),
                                          bin(s, "*", bin(s, "*", copy(s, dt), num(s, step[g], at), at), ident(s, ks[g - 1][j], at), at), at)));
      }
      for (int j = 0; j < s->np; j++) {
        ks[g][j] = fresh(s, "rate");
        push(&block, let(s, ks[g][j], staged(s, rate[j], g ? stage : now)));
      }
    }
    for (int j = 0; j < s->np; j++) {
      next[j] = fresh(s, "next");
      Node* sum = bin(s, "+", bin(s, "+", ident(s, k1[j], at), bin(s, "*", num(s, 2, at), ident(s, k2[j], at), at), at),
                      bin(s, "+", bin(s, "*", num(s, 2, at), ident(s, k3[j], at), at), ident(s, k4[j], at), at), at);
      push(&block, let(s, next[j], bin(s, "+", ident(s, now[j], at),
                                       bin(s, "*", bin(s, "/", copy(s, dt), num(s, 6, at), at), sum, at), at)));
    }
  }
  /* Each force, averaged over the step (as the plain state was and is),
   * kicks its medium. */
  for (int i = 0; i < s->nm; i++) {
    Medium* m = &s->m[i];
    for (int k = 0; k < m->nsrc; k++) {
      Node* f0 = force[i][k];
      Node* f = s->np ? bin(s, "*", num(s, 0.5, at), bin(s, "+", f0, staged(s, f0, next), at), at) : f0;
      Node* impulse = bin(s, "*", f, copy(s, dt), at);
      if (m->src[k].minus) impulse = bin(s, "*", impulse, num(s, -1, at), at);
      push(&block, store(s, nkAssign, m->tick, call(s, m->axes == 2 ? "medium_kick2" : "medium_kick", at, 5, (Node*[]){ident(s, m->store, at), impulse,
                                                                                       place[i][k], spread[i][k], num(s, m->src[k].into, at)})));
    }
  }
  for (int j = 0; j < s->np; j++) push(&block, store(s, nkAssign, now[j], ident(s, next[j], at)));
  for (int i = 0; i < s->nm; i++) {
    Medium* m = &s->m[i];
    Node* turn = m->pair
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
      push(&kept, store(s, nkVar, m->store, call(s, "array", t, 1, (Node*[]){num(s, 24 + 10 * MODE_CAP, t)})));
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
  if (s->no) { free(block.v); free(kept.v); free(s->reads.v); return 0; }
  for (int k = 0; k < block.n; k++) push(&kept, block.v[k]);
  free(block.v);
  free(s->reads.v);
  free(*vp);
  *vp = kept.v;
  *np = kept.n;
  if (capp) *capp = kept.cap;
  return 1;
}
