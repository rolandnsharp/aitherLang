/* aither to C (codegen.h). A port of codegen.nim, procedure for
 * procedure and in the same order, so that it writes the same C text for
 * every patch, temporaries' numbers included (make codegen-check). Nim
 * evaluates arguments left to right and C promises no order, so every
 * call that numbers a temporary or registers a region is sequenced into
 * its own statement. */

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codegen.h"
#include "dsp.h"

/* The most pool a voice may have, in slots: 512 MB. */
#define MAX_POOL_SLOTS (1L << 26)
#define MAX_PROF_SLOTS 1024

/* ------------------------------------------------------------- the basics */

typedef struct Scope Scope;
typedef struct Ctx Ctx;

/* One binding in a table: which fields mean what depends on the table. */
typedef struct Ent Ent;
struct Ent {
  const char* key;
  Ent* next;
  const char* a;      /* names, fns: the C text; stereo: l; arrays, larrays: sym; sarrays: base */
  const char* b;      /* stereo: r */
  long n;             /* arrays, larrays, sarrays: length; counters */
  long wo;            /* sarrays: where stores go */
  double v;           /* numlits */
  Node* node;         /* lambdas, invs; userDefs */
  Scope* sc;          /* invs */
  void* p;            /* pair-param caches */
};

struct Scope {
  Scope* parent;
  Ent* names;      /* name -> C expression */
  Ent* fns;        /* name -> the aither function it stands for */
  Ent* lambdas;    /* name -> nkLambda */
  Ent* stereo;     /* name -> (l, r) */
  Ent* arrays;     /* name -> (static table, length) */
  Ent* sarrays;    /* name -> (base, n, wo): state arrays */
  Ent* larrays;    /* name -> (C array, n): this sample's arrays */
  Ent* numlits;    /* name -> a compile-time number */
  Ent* invs;       /* name -> (block-invariant expression, its scope) */
};

#define NBUCKETS 1024
typedef struct { Ent* b[NBUCKETS]; int n; } Map;

typedef struct { char* s; size_t n, cap; } Buf;
typedef struct { const char** v; int n, cap; } Strs;

struct Ctx {
  Arena* a;
  jmp_buf jump;
  char* err;
  int errlen;
  Map user_defs;
  Strs top_var_names;
  Map top_var_set, top_lets, lets_done, let_lines;
  Strs play_names, unheard;
  int hoist_on;
  Strs hoist_decls, hoist_inits;
  Map hoist_seen;
  int in_hoist;
  Map assigned, play_set, stereo_lets;
  Map top_arrays;           /* a = sym, n = length */
  Buf array_decls;
  int tmp_counter;
  const char* patch_path;
  double sr;
  Region* regions; int nregions, cap_regions;
  Map type_counter, type_cum_size;
  Strs type_order;
  const char* scope;
  Map scope_counter;
  Strs def_stack;
  Map pair_vars;
  Node** var_decls; int nvar_decls, cap_var_decls;
  Map pair_param_cache;     /* p = int* flags, n = count */
  Map top_sarrays, top_larrays, top_sarray_names;
  int each_depth;
  Buf deferred;
  Map recursive, tail_checked, stereo_seen, pure_seen;
};

static void* al(Ctx* c, size_t n) { return arena_alloc(c->a, n); }

static const char* sdup(Ctx* c, const char* s) {
  size_t n = strlen(s);
  char* d = al(c, n + 1);
  memcpy(d, s, n);
  return d;
}

static const char* vfmt(Ctx* c, const char* fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  char* d = al(c, n + 1);
  vsnprintf(d, n + 1, fmt, ap);
  return d;
}

/* A formatted string in the arena. */
static const char* F(Ctx* c, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const char* s = vfmt(c, fmt, ap);
  va_end(ap);
  return s;
}

static void bput(Ctx* c, Buf* b, const char* s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    size_t cap = (b->n + n + 1) * 2;
    if (cap < 256) cap = 256;
    char* d = al(c, cap);
    if (b->n) memcpy(d, b->s, b->n);
    b->s = d; b->cap = cap;
  }
  memcpy(b->s + b->n, s, n);
  b->n += n;
  b->s[b->n] = 0;
}
static void bcat(Ctx* c, Buf* b, const char* s) { bput(c, b, s, strlen(s)); }
static void badd(Ctx* c, Buf* b, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  const char* s = vfmt(c, fmt, ap);
  va_end(ap);
  bcat(c, b, s);
}
static const char* bstr(Buf* b) { return b->n ? b->s : ""; }

static void spush(Ctx* c, Strs* x, const char* s) {
  if (x->n == x->cap) {
    int cap = x->cap ? x->cap * 2 : 16;
    const char** v = al(c, cap * sizeof(char*));
    if (x->n) memcpy(v, x->v, x->n * sizeof(char*));
    x->v = v; x->cap = cap;
  }
  x->v[x->n++] = s;
}
static int sfind(Strs* x, const char* s) {
  for (int i = 0; i < x->n; i++) if (!strcmp(x->v[i], s)) return i;
  return -1;
}

static int eq(const char* a, const char* b) { return !strcmp(a, b); }
static int in(const char* s, const char* const* list) {
  for (int i = 0; list[i]; i++) if (eq(s, list[i])) return 1;
  return 0;
}

/* ---- tables: a scope's are short lists, the context's are hashed ---- */

static Ent* lfind(Ent* l, const char* key) {
  for (; l; l = l->next) if (eq(l->key, key)) return l;
  return NULL;
}
static Ent* lput(Ctx* c, Ent** l, const char* key) {
  Ent* e = lfind(*l, key);
  if (e) return e;
  e = al(c, sizeof(Ent));
  e->key = key;
  e->next = *l;
  *l = e;
  return e;
}

static unsigned hsh(const char* s) {
  unsigned h = 2166136261u;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
  return h % NBUCKETS;
}
static Ent* hfind(Map* m, const char* key) { return lfind(m->b[hsh(key)], key); }
static int has(Map* m, const char* key) { return hfind(m, key) != NULL; }
static Ent* hput(Ctx* c, Map* m, const char* key) {
  Ent** l = &m->b[hsh(key)];
  Ent* e = lfind(*l, key);
  if (e) return e;
  m->n++;
  return lput(c, l, key);
}
static void incl(Ctx* c, Map* m, const char* key) { hput(c, m, key); }
static void excl(Map* m, const char* key) {
  Ent** l = &m->b[hsh(key)];
  for (; *l; l = &(*l)->next)
    if (eq((*l)->key, key)) { *l = (*l)->next; m->n--; return; }
}

/* ---- errors ---- */

static void fail(Ctx* c, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c->err, c->errlen, fmt, ap);
  va_end(ap);
  longjmp(c->jump, 1);
}

/* The node's place, for an error: "(source:line)", or "(line n)". */
static const char* err_loc(Ctx* c, const Node* n) {
  if (n->source[0] && n->line > 0) return F(c, "(%s:%d)", n->source, n->line);
  if (n->line > 0) return F(c, "(line %d)", n->line);
  return "";
}

/* A number as C text that reads back exactly, always a double. */
static const char* num_lit(Ctx* c, double v) {
  const char* s = F(c, "%#.17g", v);
  if (strchr(s, '.') || strchr(s, 'e') || strchr(s, 'E')) return s;
  return F(c, "%s.0", s);
}

/* Nim's `$` of a float, for messages: the shortest text that reads
 * back, with ".0" on a whole number. */
static const char* nim_float(Ctx* c, double v) {
  for (int p = 1; p <= 17; p++) {
    char b[64];
    snprintf(b, sizeof b, "%.*g", p, v);
    if (strtod(b, NULL) == v || p == 17) {
      if (!strchr(b, '.') && !strchr(b, 'e') && !strchr(b, 'n') && !strchr(b, 'i'))
        return F(c, "%s.0", b);
      return sdup(c, b);
    }
  }
  return "";
}

static Node* mknode(Ctx* c, NodeKind k, int line, const char* source) {
  Node* n = al(c, sizeof(Node));
  n->kind = k; n->line = line; n->str = ""; n->source = source ? source : "";
  return n;
}
static Node* mknum(Ctx* c, double v, int line, const char* source) {
  Node* n = mknode(c, nkNum, line, source);
  n->num = v;
  return n;
}
static Node* mkident(Ctx* c, const char* name, int line, const char* source) {
  Node* n = mknode(c, nkIdent, line, source);
  n->str = name;
  return n;
}
static void add_kid(Ctx* c, Node* n, Node* k) {
  Node** kids = al(c, (n->nkids + 1) * sizeof(Node*));
  if (n->nkids) memcpy(kids, n->kids, n->nkids * sizeof(Node*));
  kids[n->nkids++] = k;
  n->kids = kids;
}

static const char* const LIBM1[] = {"sin", "cos", "tan", "tanh", "exp", "log", "log2",
                                    "floor", "ceil", "sqrt", NULL};

/* The two entry points, after every patch's tick1. */
static const char* BLOCK_TEMPLATE =
"extern double n_input_l(void);\n"
"extern double n_input_r(void);\n"
"\n"
"void tick(VoiceState* s, double* outL, double* outR) {\n"
"  hoist_init(s);\n"
"  s->in_l = n_input_l(); s->in_r = n_input_r();\n"
"  tick1(s, outL, outR);\n"
"}\n"
"\n"
"void tick_block(VoiceState* s, long n, double* outL, double* outR) {\n"
"  const long np = s->n_parts;\n"
"  hoist_init(s);\n"
"  s->bad = 0; s->peak_raw = 0.0; s->stuck = 0;\n"
"  for (long i = 0; i < n; i++) {\n"
"    s->time_frac += s->dt;\n"
"    if (s->time_frac >= 1.0) { s->time_sec += 1.0; s->time_frac -= 1.0; }\n"
"    s->t = s->time_sec + s->time_frac;\n"
"    if (s->in_buf) { s->in_l = s->in_buf[2*i]; s->in_r = s->in_buf[2*i + 1]; }\n"
"    else { s->in_l = 0.0; s->in_r = 0.0; }\n"
"    double l, r;\n"
"    tick1(s, &l, &r);\n"
"    if (!(l == l) || !(r == r) || l > 1e6 || l < -1e6 || r > 1e6 || r < -1e6) {\n"
"      s->bad++; l = 0.0; r = 0.0;\n"
"    }\n"
"    double al = fabs(l), ar = fabs(r);\n"
"    if (al > s->peak_raw) s->peak_raw = al;\n"
"    if (ar > s->peak_raw) s->peak_raw = ar;\n"
"    /* the voice's fade, then the parts' gain ramps: for the next sample */\n"
"    s->fade_gain += s->fade_delta;\n"
"    if (s->fade_gain > 1.0) s->fade_gain = 1.0;\n"
"    if (s->fade_gain < 0.0) s->fade_gain = 0.0;\n"
"    if (s->fade_gain >= 1.0 && s->fade_delta > 0.0) s->fade_delta = 0.0;\n"
"    for (long p = 0; p < np; p++) {\n"
"      double d = s->part_fade_delta[p];\n"
"      if (d != 0.0) {\n"
"        double target = s->part_fade_target[p];\n"
"        double g = s->part_gains[p] + d;\n"
"        if ((d > 0.0 && g >= target) || (d < 0.0 && g <= target)) {\n"
"          s->part_gains[p] = target; s->part_fade_delta[p] = 0.0;\n"
"          if (s->part_park_pending[p] && target <= 0.0) {\n"
"            s->part_park[p] = 1.0; s->part_park_pending[p] = 0;\n"
"          }\n"
"        } else {\n"
"          s->part_gains[p] = g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);\n"
"        }\n"
"      }\n"
"    }\n"
"    const double g = s->fade_gain;\n"
"    outL[i] = l * g; outR[i] = r * g;\n"
"    /* per-part meters, after the voice fade like the voice's own */\n"
"    for (long p = 0; p < np; p++) {\n"
"      double a0 = fabs(s->part_out[2*p]), a1 = fabs(s->part_out[2*p + 1]);\n"
"      double a = (a0 > a1 ? a0 : a1) * g;\n"
"      double pk = s->part_peak[p] * s->peak_decay;\n"
"      s->part_peak[p] = pk > a ? pk : a;\n"
"      s->part_sq[p] = s->part_sq[p] * (1.0 - s->rms_alpha) + a * a * s->rms_alpha;\n"
"    }\n"
"  }\n"
"}\n";

/* ------------------------------------------------------- small helpers */

static void collect_assigned(Ctx* c, Node* n) {
  if (!n) return;
  if ((n->kind == nkAssign || n->kind == nkIdxAssign) && n->str[0]) incl(c, &c->assigned, n->str);
  for (int i = 0; i < n->nkids; i++) collect_assigned(c, n->kids[i]);
}

static void new_ctx(Ctx* c, Node* program) {
  c->sr = 48000.0;
  for (int i = 0; i < program->nkids; i++) {
    Node* n = program->kids[i];
    if (n->kind == nkDef) hput(c, &c->user_defs, n->str)->node = n;
  }
  collect_assigned(c, program);
}

static Node* user_def(Ctx* c, const char* name) {
  Ent* e = hfind(&c->user_defs, name);
  return e ? e->node : NULL;
}

/* A fixed-width placeholder for a region's offset, filled in after the
 * walk once each type's base is known. */
static const char* offset_placeholder(Ctx* c, int rid) { return F(c, "__OFF_%06d__", rid); }

/* Register one stateful call site; its placeholder. */
static const char* register_region(Ctx* c, const char* type_name, long size, const char* state_name) {
  Buf owner = {0};
  for (int i = 0; i < c->def_stack.n; i++) {
    if (i) bcat(c, &owner, ">");
    bcat(c, &owner, c->def_stack.v[i]);
  }
  if (state_name && state_name[0]) { bcat(c, &owner, "/$"); bcat(c, &owner, state_name); }
  const char* own = sdup(c, bstr(&owner));
  if (!has(&c->type_counter, type_name)) {
    hput(c, &c->type_counter, type_name)->n = 0;
    hput(c, &c->type_cum_size, type_name)->n = 0;
    spush(c, &c->type_order, type_name);
  }
  Ent* tc = hfind(&c->type_counter, type_name);
  Ent* ts = hfind(&c->type_cum_size, type_name);
  long per_type_idx = tc->n;
  long intra = ts->n;
  tc->n = per_type_idx + 1;
  ts->n = intra + size;
  int rid = c->nregions;
  const char* key = F(c, "%s | %s | %s", c->scope, type_name, own);
  Ent* sk = hput(c, &c->scope_counter, key);
  long scope_idx = sk->n;
  sk->n = scope_idx + 1;
  if (c->nregions == c->cap_regions) {
    int cap = c->cap_regions ? c->cap_regions * 2 : 64;
    Region* r = al(c, cap * sizeof(Region));
    if (c->nregions) memcpy(r, c->regions, c->nregions * sizeof(Region));
    c->regions = r; c->cap_regions = cap;
  }
  c->regions[c->nregions++] = (Region){sdup(c, type_name), (int)per_type_idx, c->scope,
                                       (int)scope_idx, own, intra, size};
  return offset_placeholder(c, rid);
}

static const char* fresh(Ctx* c, const char* prefix) {
  c->tmp_counter++;
  return F(c, "%s_%d", prefix, c->tmp_counter);
}

static Scope* push(Ctx* c, Scope* parent) {
  Scope* s = al(c, sizeof(Scope));
  s->parent = parent;
  return s;
}

/* Does the nearest binding of `name` bind it to anything at all? */
static int binds_any(Scope* s, const char* name) {
  return lfind(s->numlits, name) || lfind(s->names, name) || lfind(s->stereo, name) ||
         lfind(s->fns, name) || lfind(s->lambdas, name) || lfind(s->arrays, name) ||
         lfind(s->sarrays, name) || lfind(s->larrays, name);
}

/* The block-invariant expression `name` is bound to, if the nearest
 * binding of that name is one. */
static Ent* lookup_inv(Scope* sc, const char* name) {
  for (Scope* s = sc; s; s = s->parent) {
    Ent* e = lfind(s->invs, name);
    if (e) return e;
    if (binds_any(s, name)) return NULL;
  }
  return NULL;
}

/* The compile-time number `name` is bound to, if the nearest binding of
 * that name is one: a nearer binding to anything else shadows it. */
static int lookup_num_lit(Scope* sc, const char* name, double* v) {
  for (Scope* s = sc; s; s = s->parent) {
    Ent* e = lfind(s->numlits, name);
    if (e) { *v = e->v; return 1; }
    if (lfind(s->names, name) || lfind(s->stereo, name) || lfind(s->fns, name) ||
        lfind(s->lambdas, name) || lfind(s->arrays, name) || lfind(s->sarrays, name) ||
        lfind(s->larrays, name))
      return 0;
  }
  return 0;
}

static Ent* lookup_array(Ctx* c, Scope* sc, const char* name) {
  for (Scope* s = sc; s; s = s->parent) {
    Ent* e = lfind(s->arrays, name);
    if (e) return e;
  }
  return hfind(&c->top_arrays, name);
}

static Ent* lookup_sarray(Ctx* c, Scope* sc, const char* name) {
  for (Scope* s = sc; s; s = s->parent) {
    Ent* e = lfind(s->sarrays, name);
    if (e) return e;
  }
  return hfind(&c->top_sarrays, name);
}

static Ent* lookup_larray(Ctx* c, Scope* sc, const char* name) {
  for (Scope* s = sc; s; s = s->parent) {
    Ent* e = lfind(s->larrays, name);
    if (e) return e;
  }
  return hfind(&c->top_larrays, name);
}

/* A compile-time number: a literal, a name bound to one, len(array), or
 * arithmetic over those. */
static int const_value(Ctx* c, Scope* sc, Node* n, double* v) {
  if (!n) return 0;
  switch (n->kind) {
  case nkNum: *v = n->num; return 1;
  case nkIdent: return sc ? lookup_num_lit(sc, n->str, v) : 0;
  case nkCall:
    if (eq(n->str, "len") && n->nkids == 1 && n->kids[0]->kind == nkIdent) {
      const char* a = n->kids[0]->str;
      Ent* e;
      if ((e = lookup_sarray(c, sc, a))) { *v = (double)e->n; return 1; }
      if ((e = lookup_larray(c, sc, a))) { *v = (double)e->n; return 1; }
      if ((e = lookup_array(c, sc, a))) { *v = (double)e->n; return 1; }
    }
    return 0;
  case nkBinOp: {
    double a, b;
    if (!const_value(c, sc, n->kids[0], &a)) return 0;
    if (!const_value(c, sc, n->kids[1], &b)) return 0;
    if (eq(n->str, "+")) { *v = a + b; return 1; }
    if (eq(n->str, "-")) { *v = a - b; return 1; }
    if (eq(n->str, "*")) { *v = a * b; return 1; }
    if (eq(n->str, "/")) { if (b != 0.0) { *v = a / b; return 1; } return 0; }
    return 0;
  }
  case nkUnary:
    if (eq(n->str, "-")) {
      double a;
      if (const_value(c, sc, n->kids[0], &a)) { *v = -a; return 1; }
    }
    return 0;
  default: return 0;
  }
}
static int is_const(Ctx* c, Scope* sc, Node* n) { double v; return const_value(c, sc, n, &v); }

/* `array(n, init, every)`: size and init, and `every` (NULL when not
 * given, or given as its default 0: not stepped). */
typedef struct { Node* pos[2]; int npos; Node* every; } ArrayArgs;
static ArrayArgs array_args(Node* call) {
  ArrayArgs r = {{0}, 0, NULL};
  for (int i = 0; i < call->nkids; i++) {
    Node* k = call->kids[i];
    if (i < 2) r.pos[r.npos++] = k;
    else if (!(k->kind == nkNum && k->num == 0.0)) r.every = k;
  }
  return r;
}

static int is_array_make(Node* n) {
  return n && n->kind == nkCall && eq(n->str, "array") && n->nkids >= 1 && n->nkids <= 3;
}

static Ent* lookup_stereo(Scope* sc, const char* name) {
  for (Scope* s = sc; s; s = s->parent) {
    Ent* e = lfind(s->stereo, name);
    if (e) return e;
  }
  return NULL;
}

static const char* lookup_fn(Scope* sc, const char* name) {
  for (Scope* s = sc; s; s = s->parent) {
    Ent* e = lfind(s->fns, name);
    if (e) return e->a;
  }
  return "";
}

static Node* lookup_lambda(Scope* sc, const char* name) {
  for (Scope* s = sc; s; s = s->parent) {
    Ent* e = lfind(s->lambdas, name);
    if (e) return e->node;
  }
  return NULL;
}

static const char* lookup(Scope* sc, const char* name) {
  for (Scope* s = sc; s; s = s->parent) {
    Ent* e = lfind(s->names, name);
    if (e) return e->a;
  }
  return "";
}

/* The nearest binding of `name` is a plain number, shadowing a file-level
 * pair of the same name. */
static int scalar_shadows(Scope* sc, const char* name) {
  for (Scope* s = sc; s; s = s->parent) {
    if (lfind(s->stereo, name)) return 0;
    if (lfind(s->names, name)) return 1;
  }
  return 0;
}

static const char* const BUILTIN_FNS[] = {
  "saw", "tri", "sqr", "phasor", "noise", "abs", "pow",
  "midi_cc", "midi_note", "midi_freq", "midi_gate",
  "midi_trig", "midi_voice_freq", "midi_voice_gate",
  "midi_voice_vel", "midi_voice_key", "midi_vel",
  "magnitude", "phase", "freq_shift", NULL};

/* Pair-returning builtins, and the reductions of a pair to a number. */
static const char* const PAIR_CALLS[] = {"cmul", "cdiv", "cscale", "rotate", "analytic",
                                         "phasor_pair", "conj", "input", NULL};
static const char* const PAIR_READERS[] = {"magnitude", "phase", NULL};

/* Whether argument j of a pair builtin (called with n) is a pair. */
static int takes_pair_at(const char* name, int j, int n) {
  if (eq(name, "rotate")) return !(n == 2 && j == 1);
  if (eq(name, "cscale")) return !(n == 2 && j == 0);
  if (eq(name, "analytic") || eq(name, "phasor_pair") || eq(name, "input")) return 0;
  return 1;
}

/* Helpers that process a signal without making its sides differ: given a
 * pair, they run once per side, each with its own state. */
static const char* const TRANSPARENT_HELPERS[] = {
  "lpf", "hpf", "bpf", "lp1", "hp1", "drive", "fold",
  "delay", "fbdelay", "tape", "reverb", "slew", "discharge",
  "pluck", "swell", "adsr", "gain", "prev", "tremolo", NULL};

/* Pair-returning defs that need a mono input. */
static const char* const MONO_ONLY_INPUT_DEFS[] = {"pan", NULL};

static const NativeSpec* native(const char* name) {
  for (int i = 0; dsp_natives[i].name; i++) if (eq(dsp_natives[i].name, name)) return &dsp_natives[i];
  return NULL;
}
static int native_arity(const NativeSpec* s) {
  int n = 1;
  for (const char* p = s->params; *p; p++) if (*p == ',') n++;
  return n;
}

static int is_value_name(Ctx* c, Scope* sc, const char* name) {
  if (lookup(sc, name)[0]) return 1;
  if (lookup_stereo(sc, name)) return 1;
  if (lookup_array(c, sc, name)) return 1;
  if (lookup_sarray(c, sc, name) || lookup_larray(c, sc, name)) return 1;
  if (has(&c->top_lets, name)) return 1;
  if (has(&c->top_var_set, name)) return 1;
  if (has(&c->pair_vars, name)) return 1;
  if (has(&c->play_set, name)) return 1;
  if (has(&c->stereo_lets, name)) return 1;
  return 0;
}

/* A function-valued name; a value of the same name shadows it. */
static int is_fn_name(Ctx* c, Scope* sc, const char* name) {
  if (is_value_name(c, sc, name)) return 0;
  return in(name, LIBM1) || in(name, BUILTIN_FNS) || user_def(c, name) ||
         native(name) || lookup_fn(sc, name)[0];
}

static const char* stereo_ref(Ctx* c, const char* name, const char* chan) {
  return F(c, "%s%s_%s", has(&c->play_set, name) ? "p_" : "l_", name, chan);
}

static const char* pair_var_field(Ctx* c, const char* name, int k) { return F(c, "%s__%d", name, k); }
static const char* pair_var_ref(Ctx* c, const char* name, int k) { return F(c, "s->v_%s__%d", name, k); }

static int param_index(Node* def, const char* name) {
  for (int i = 0; i < def->nparams; i++) if (eq(def->params[i], name)) return i;
  return -1;
}

/* Which params a def's body reads as a pair. */
static int* pair_params(Ctx* c, Node* def);

static int pp_is_pair(int* res, Node* def, Node* n) {
  if (!n) return 0;
  switch (n->kind) {
  case nkIdent: { int k = param_index(def, n->str); return k >= 0 && res[k]; }
  case nkArr: return n->nkids == 2;
  case nkCall: return in(n->str, PAIR_CALLS);
  case nkBinOp:
    if (!(eq(n->str, "+") || eq(n->str, "-") || eq(n->str, "*") || eq(n->str, "/"))) return 0;
    for (int i = 0; i < n->nkids; i++) if (pp_is_pair(res, def, n->kids[i])) return 1;
    return 0;
  case nkUnary: return eq(n->str, "-") && pp_is_pair(res, def, n->kids[0]);
  default: return 0;
  }
}

static void pp_mark(Ctx* c, int* res, Node* def, Node* n) {
  if (!n) return;
  switch (n->kind) {
  case nkIdx: {
    Node* a = n->kids[0];
    Node* i = n->kids[1];
    if (a->kind == nkIdent && i->kind == nkNum && ((int)i->num == 0 || (int)i->num == 1)) {
      int k = param_index(def, a->str);
      if (k >= 0) res[k] = 1;
    }
    break;
  }
  case nkBinOp:
    if (eq(n->str, "+") || eq(n->str, "-") || eq(n->str, "*") || eq(n->str, "/")) {
      int any = 0;
      for (int i = 0; i < n->nkids; i++) if (pp_is_pair(res, def, n->kids[i])) any = 1;
      if (any)
        for (int i = 0; i < n->nkids; i++)
          if (n->kids[i]->kind == nkIdent) {
            int k = param_index(def, n->kids[i]->str);
            if (k >= 0) res[k] = 1;
          }
    }
    break;
  case nkCall: {
    int* callee = NULL;
    int ncallee = 0;
    Node* cd = user_def(c, n->str);
    if (cd && !in(n->str, PAIR_CALLS) && !in(n->str, PAIR_READERS)) {
      callee = pair_params(c, cd);
      ncallee = cd->nparams;
    }
    for (int j = 0; j < n->nkids; j++) {
      Node* arg = n->kids[j];
      if (eq(n->str, def->str) && j < def->nparams && pp_is_pair(res, def, arg)) res[j] = 1;
      if (arg->kind != nkIdent) continue;
      int k = param_index(def, arg->str);
      if (k < 0) continue;
      if (((in(n->str, PAIR_CALLS) || in(n->str, PAIR_READERS)) && takes_pair_at(n->str, j, n->nkids)) ||
          (j < ncallee && callee[j]))
        res[k] = 1;
    }
    break;
  }
  default: break;
  }
  for (int i = 0; i < n->nkids; i++) pp_mark(c, res, def, n->kids[i]);
}

static int* pair_params(Ctx* c, Node* def) {
  Ent* e = hfind(&c->pair_param_cache, def->str);
  if (e) return e->p;
  int np = def->nparams;
  int* res = al(c, (np ? np : 1) * sizeof(int));
  hput(c, &c->pair_param_cache, def->str)->p = res;       /* against call cycles */
  int* before = al(c, (np ? np : 1) * sizeof(int));
  for (;;) {
    memcpy(before, res, np * sizeof(int));
    pp_mark(c, res, def, def->kids[0]);
    if (!memcmp(before, res, np * sizeof(int))) break;
  }
  return res;
}

static int has_pair_params(Ctx* c, const char* name) {
  Node* d = user_def(c, name);
  if (!d) return 0;
  int* p = pair_params(c, d);
  for (int i = 0; i < d->nparams; i++) if (p[i]) return 1;
  return 0;
}

/* ------------------------------------------------- expression codegen */

static const char* emit_expr(Ctx* c, Scope* sc, Node* n);
static const char* emit_block_expr(Ctx* c, Scope* sc, Node* blk);
static int refs_stereo(Ctx* c, Scope* sc, Node* n);
static const char* emit_idx_assign(Ctx* c, Scope* sc, Node* st);
static void declare_state_array(Ctx* c, Scope* sc, Node* st, Buf* pre, int top_level);
static void declare_local_array(Ctx* c, Scope* sc, const char* name, Node* rhs, Buf* pre, int top_level);

typedef struct { const char* l; const char* r; int stereo; } SV;
static SV emit_stereo(Ctx* c, Scope* sc, Node* n, Buf* pre);
static int is_stereo_return(Ctx* c, Node* n);

/* Inside a def body, `names` are those known to hold a pair. */
typedef struct NameSet { const char** v; int n; } NameSet;
static int ns_has(NameSet* s, const char* k) {
  for (int i = 0; i < s->n; i++) if (eq(s->v[i], k)) return 1;
  return 0;
}
/* A copy with room for `extra` more names. */
static NameSet ns_copy(Ctx* c, NameSet* s, int extra) {
  NameSet r = {al(c, (s->n + extra + 1) * sizeof(char*)), s->n};
  if (s->n) memcpy(r.v, s->v, s->n * sizeof(char*));
  return r;
}
static void ns_incl(NameSet* s, const char* k) { if (!ns_has(s, k)) s->v[s->n++] = k; }
static void ns_excl(NameSet* s, const char* k) {
  for (int i = 0; i < s->n; i++)
    if (eq(s->v[i], k)) { s->v[i] = s->v[--s->n]; return; }
}

static int stereo_in(Ctx* c, Node* n, NameSet* names) {
  if (!n) return 0;
  switch (n->kind) {
  case nkIdent: return ns_has(names, n->str);
  case nkBinOp: case nkUnary:
    for (int i = 0; i < n->nkids; i++) if (stereo_in(c, n->kids[i], names)) return 1;
    return 0;
  case nkBlock: {
    NameSet nm = ns_copy(c, names, n->nkids);
    for (int i = 0; i < n->nkids; i++) {
      Node* k = n->kids[i];
      if (i == n->nkids - 1) return stereo_in(c, k, &nm);
      if (k->kind == nkLet) {
        if (stereo_in(c, k->kids[0], &nm)) ns_incl(&nm, k->str);
        else ns_excl(&nm, k->str);
      } else if (k->kind == nkVar && stereo_in(c, k->kids[0], &nm)) {
        ns_incl(&nm, k->str);
      }
    }
    return 0;
  }
  case nkIf:
    return stereo_in(c, n->kids[1], names) || stereo_in(c, n->kids[2], names);
  default:
    return is_stereo_return(c, n);
  }
}

static int is_stereo_def(Ctx* c, Node* def) {
  if (has(&c->stereo_seen, def->str)) return 0;
  incl(c, &c->stereo_seen, def->str);
  NameSet names = {al(c, (def->nparams + 1) * sizeof(char*)), 0};
  int* pairs = pair_params(c, def);
  for (int i = 0; i < def->nparams; i++) if (pairs[i]) ns_incl(&names, def->params[i]);
  int r = stereo_in(c, def->kids[0], &names);
  excl(&c->stereo_seen, def->str);
  return r;
}

static int is_stereo_return(Ctx* c, Node* n) {
  if (!n) return 0;
  switch (n->kind) {
  case nkArr: return n->nkids == 2;
  case nkBlock: return n->nkids == 0 ? 0 : is_stereo_return(c, n->kids[n->nkids - 1]);
  case nkIf:
    return is_stereo_return(c, n->kids[1]) || (n->kids[2] && is_stereo_return(c, n->kids[2]));
  case nkCall:
    if (in(n->str, PAIR_CALLS)) return 1;
    { Node* d = user_def(c, n->str); return d && is_stereo_def(c, d); }
  default: return 0;
  }
}

/* Can a def be inlined twice, once per channel? Not with state in it. */
static int is_pure_for_stereo(Ctx* c, Node* n) {
  if (!n) return 1;
  switch (n->kind) {
  case nkVar: return 0;
  case nkCall:
    if (eq(n->str, "sum") || eq(n->str, "each") || eq(n->str, "array")) return 0;
    if (eq(n->str, "phasor") || eq(n->str, "phasor_pair") || eq(n->str, "noise") || eq(n->str, "midi_trig")) return 0;
    if (native(n->str)) return 0;
    {
      Node* d = user_def(c, n->str);
      if (d && !has(&c->pure_seen, n->str)) {
        incl(c, &c->pure_seen, n->str);
        int pure = is_pure_for_stereo(c, d->kids[0]);
        excl(&c->pure_seen, n->str);
        if (!pure) return 0;
      }
    }
    break;
  case nkLambda: return 0;
  default: break;
  }
  for (int i = 0; i < n->nkids; i++)
    if (n->kids[i] && !is_pure_for_stereo(c, n->kids[i])) return 0;
  return 1;
}

static const char* call_args(Ctx* c, Scope* sc, Node** kids, int nkids) {
  Buf b = {0};
  for (int i = 0; i < nkids; i++) {
    const char* e = emit_expr(c, sc, kids[i]);
    if (i) bcat(c, &b, ", ");
    bcat(c, &b, e);
  }
  return sdup(c, bstr(&b));
}

/* ---- hoisting: what can't change within a block runs once a block ---- */

static const char* const PURE_MATH[] = {"sin", "cos", "tan", "tanh", "exp", "log", "log2",
                                        "floor", "ceil", "sqrt", "pow", "min", "max", "clamp",
                                        "abs", "int", NULL};

static int is_invariant(Ctx* c, Scope* sc, Node* n) {
  if (!n) return 0;
  switch (n->kind) {
  case nkNum: return 1;
  case nkIdent: {
    if (eq(n->str, "PI") || eq(n->str, "TAU") || eq(n->str, "sr")) return 1;
    if (!sc) return 0;
    double v;
    if (lookup_num_lit(sc, n->str, &v)) return 1;
    return !has(&c->assigned, n->str) && lookup_inv(sc, n->str);
  }
  case nkBinOp: case nkUnary:
    for (int i = 0; i < n->nkids; i++) if (!is_invariant(c, sc, n->kids[i])) return 0;
    return 1;
  case nkIf:
    if (n->nkids != 3) return 0;
    for (int i = 0; i < 3; i++) if (!is_invariant(c, sc, n->kids[i])) return 0;
    return 1;
  case nkCall:
    if (!(in(n->str, PURE_MATH) || eq(n->str, "midi_cc")) || n->nkids == 0) return 0;
    for (int i = 0; i < n->nkids; i++) if (!is_invariant(c, sc, n->kids[i])) return 0;
    return 1;
  default: return 0;
  }
}

static int mentions(Node* n, const char* name) {
  if (!n) return 0;
  if (n->kind == nkIdent && eq(n->str, name)) return 1;
  for (int i = 0; i < n->nkids; i++) if (mentions(n->kids[i], name)) return 1;
  return 0;
}

/* Record that `name` is bound to a block-invariant expression. */
static void note_invariant(Ctx* c, Scope* target, const char* name, Node* expr, Scope* expr_sc) {
  if (c->hoist_on && expr && expr->kind != nkNum &&
      !(target == expr_sc && mentions(expr, name)) &&
      is_invariant(c, expr_sc, expr) && !is_const(c, expr_sc, expr)) {
    Ent* e = lput(c, &target->invs, name);
    e->node = expr;
    e->sc = expr_sc;
  }
}

/* A copy of `n` with every name spelled out: a compile-time number as
 * that number, an invariant name as its expression. */
static Node* spell(Ctx* c, Scope* sc, Node* n) {
  if (!n) return NULL;
  if (n->kind == nkIdent && !(eq(n->str, "PI") || eq(n->str, "TAU") || eq(n->str, "sr")) && sc) {
    double v;
    if (lookup_num_lit(sc, n->str, &v)) return mknum(c, v, n->line, n->source);
    Ent* inv = lookup_inv(sc, n->str);
    if (inv) return spell(c, inv->sc, inv->node);
  }
  Node* r = mknode(c, n->kind, n->line, n->source);
  r->str = n->str; r->num = n->num; r->params = n->params; r->defaults = n->defaults;
  r->nparams = n->nparams;
  if (n->nkids) {
    r->kids = al(c, n->nkids * sizeof(Node*));
    r->nkids = n->nkids;
    for (int i = 0; i < n->nkids; i++) r->kids[i] = spell(c, sc, n->kids[i]);
  }
  return r;
}

static const char* emit_invariant(Ctx* c, Scope* sc, Node* n) {
  Node* sp = spell(c, sc, n);
  int was = c->in_hoist;
  c->in_hoist = 1;
  const char* r = emit_expr(c, push(c, NULL), sp);
  c->in_hoist = was;
  return r;
}

/* A static double computed once from `cexpr`; its name. */
static const char* hoist(Ctx* c, const char* cexpr) {
  Ent* e = hfind(&c->hoist_seen, cexpr);
  if (e) return e->a;
  const char* name = F(c, "h_%d", c->hoist_decls.n);
  spush(c, &c->hoist_decls, F(c, "static double %s;\n", name));
  spush(c, &c->hoist_inits, F(c, "  %s = (%s);\n", name, cexpr));
  hput(c, &c->hoist_seen, cexpr)->a = name;
  return name;
}

static const char* replace_all(Ctx* c, const char* s, const char* what, const char* with) {
  Buf b = {0};
  size_t wl = strlen(what);
  for (;;) {
    const char* p = strstr(s, what);
    if (!p) { bcat(c, &b, s); break; }
    bput(c, &b, s, p - s);
    bcat(c, &b, with);
    s = p + wl;
  }
  return sdup(c, bstr(&b));
}

/* A static array of n doubles filled once by `init_call` ($H: the array). */
static const char* hoist_arr(Ctx* c, int n, const char* init_call) {
  Ent* e = hfind(&c->hoist_seen, init_call);
  if (e) return e->a;
  const char* name = F(c, "h_%d", c->hoist_decls.n);
  spush(c, &c->hoist_decls, F(c, "static double %s[%d];\n", name, n));
  spush(c, &c->hoist_inits, F(c, "  %s;\n", replace_all(c, init_call, "$H", name)));
  hput(c, &c->hoist_seen, init_call)->a = name;
  return name;
}

/* `call` with $1, $2, ... standing for names[0], names[1], ... */
static const char* subst(Ctx* c, const char* call, const char** names, int n) {
  Buf b = {0};
  for (const char* p = call; *p;) {
    if (*p == '$' && p[1] >= '1' && p[1] <= '9') {
      int k = 0;
      p++;
      while (*p >= '0' && *p <= '9') k = k * 10 + (*p++ - '0');
      if (k >= 1 && k <= n) bcat(c, &b, names[k - 1]);
    } else {
      bput(c, &b, p, 1);
      p++;
    }
  }
  return sdup(c, bstr(&b));
}

/* A stateful native at its call site's region: the args into locals
 * first (one may move s->idx), then s->idx, then the call. */
static const char* native_at(Ctx* c, const char* off, const char** args, int nargs, const char* call) {
  Buf pre = {0};
  const char** names = al(c, (nargs ? nargs : 1) * sizeof(char*));
  for (int i = 0; i < nargs; i++) {
    const char* t = fresh(c, "na");
    badd(c, &pre, "double %s = (%s); ", t, args[i]);
    names[i] = t;
  }
  return F(c, "(({ %ss->idx = %s; %s; }))", bstr(&pre), off, subst(c, call, names, nargs));
}

/* An argument bound at compile time: a lambda, a function or an array. */
static int bind_by_name(Ctx* c, Scope* sc, Scope* inner, const char* p, Node* a, Buf* preface) {
  if (a->kind == nkLambda) { lput(c, &inner->lambdas, p)->node = a; return 1; }
  if (a->kind == nkIdent) {
    Node* lam = lookup_lambda(sc, a->str);
    if (lam) { lput(c, &inner->lambdas, p)->node = lam; return 1; }
  }
  if (a->kind == nkIdent && is_fn_name(c, sc, a->str)) {
    const char* resolved = lookup_fn(sc, a->str)[0] ? lookup_fn(sc, a->str) : a->str;
    lput(c, &inner->fns, p)->a = resolved;
    return 1;
  }
  if (a->kind == nkIdent && !lookup(sc, a->str)[0] && !lookup_stereo(sc, a->str)) {
    Ent* e;
    if ((e = lookup_sarray(c, sc, a->str))) {
      Ent* d = lput(c, &inner->sarrays, p);
      d->a = e->a; d->n = e->n; d->wo = e->wo;
      return 1;
    }
    if ((e = lookup_larray(c, sc, a->str))) {
      Ent* d = lput(c, &inner->larrays, p);
      d->a = e->a; d->n = e->n;
      return 1;
    }
    if ((e = lookup_array(c, sc, a->str))) {
      Ent* d = lput(c, &inner->arrays, p);
      d->a = e->a; d->n = e->n;
      return 1;
    }
  }
  if (a->kind == nkArr && a->nkids != 2) {
    int all_num = a->nkids > 0;
    for (int i = 0; i < a->nkids; i++) if (a->kids[i]->kind != nkNum) all_num = 0;
    if (all_num) {
      const char* sym = fresh(c, F(c, "arr_arg_%s", p));
      Buf items = {0};
      for (int i = 0; i < a->nkids; i++) {
        if (i) bcat(c, &items, ", ");
        bcat(c, &items, num_lit(c, a->kids[i]->num));
      }
      badd(c, &c->array_decls, "static const double %s[%d] = {%s};\n", sym, a->nkids, bstr(&items));
      Ent* d = lput(c, &inner->arrays, p);
      d->a = sym; d->n = a->nkids;
    } else {
      const char* tmp = fresh(c, F(c, "arg_%s", p));
      Buf sub = {0};
      declare_local_array(c, sc, tmp, a, &sub, 0);
      bcat(c, preface, bstr(&sub));
      Ent* src = lfind(sc->larrays, tmp);
      Ent* d = lput(c, &inner->larrays, p);
      d->a = src->a; d->n = src->n;
    }
    return 1;
  }
  return 0;
}

/* A def reached again while it is still being inlined. */
static void cycle_error(Ctx* c, Node* def, Node* call) {
  int start = sfind(&c->def_stack, def->str);
  Buf chain = {0};
  for (int i = start; i < c->def_stack.n; i++) { bcat(c, &chain, c->def_stack.v[i]); bcat(c, &chain, " -> "); }
  bcat(c, &chain, def->str);
  fail(c, "%s: %s comes back to itself through another def %s. A def can repeat by "
       "calling itself, but a cycle through other defs can't repeat yet",
       bstr(&chain), def->str, err_loc(c, call));
}

/* Does `n` index `name` anywhere? */
static int indexes(Node* n, const char* name) {
  if (!n) return 0;
  if (n->kind == nkIdx && n->kids[0]->kind == nkIdent && eq(n->kids[0]->str, name)) return 1;
  for (int i = 0; i < n->nkids; i++) if (indexes(n->kids[i], name)) return 1;
  return 0;
}

/* Which params take a pair argument whole. */
static int* takes_pair(Ctx* c, Node* def) {
  int* pp = pair_params(c, def);
  int* r = al(c, (def->nparams ? def->nparams : 1) * sizeof(int));
  for (int i = 0; i < def->nparams; i++) r[i] = pp[i] || indexes(def->kids[0], def->params[i]);
  return r;
}

static const char* emit_def_inline(Ctx* c, Scope* sc, Node* def, Node* call) {
  if (call->nkids != def->nparams)
    fail(c, "wrong arg count for %s: expected %d got %d %s", def->str, def->nparams, call->nkids, err_loc(c, call));
  if (sfind(&c->def_stack, def->str) >= 0) cycle_error(c, def, call);
  Scope* inner = push(c, sc);
  Buf preface = {0};
  int* pairs = pair_params(c, def);
  for (int i = 0; i < def->nparams; i++) {
    const char* p = def->params[i];
    Node* a = call->kids[i];
    if (pairs[i] || (indexes(def->kids[0], p) && refs_stereo(c, sc, a))) {
      Buf sub = {0};
      SV sv = emit_stereo(c, sc, a, &sub);
      const char* tl = fresh(c, F(c, "p_%s_l", p));
      const char* tr = fresh(c, F(c, "p_%s_r", p));
      bcat(c, &preface, bstr(&sub));
      badd(c, &preface, "double %s = (%s); double %s = (%s); ", tl, sv.l, tr, sv.r);
      Ent* e = lput(c, &inner->stereo, p);
      e->a = tl; e->b = tr;
      continue;
    }
    if (bind_by_name(c, sc, inner, p, a, &preface)) continue;
    const char* arg = emit_expr(c, sc, a);
    const char* tmp = fresh(c, F(c, "p_%s", p));
    badd(c, &preface, "double %s = (%s); ", tmp, arg);
    lput(c, &inner->names, p)->a = tmp;
    double v;
    if (const_value(c, sc, a, &v)) lput(c, &inner->numlits, p)->v = v;
    else note_invariant(c, inner, p, a, sc);
  }
  Node* body = def->kids[0];
  spush(c, &c->def_stack, def->str);
  const char* b = body->kind == nkBlock ? emit_block_expr(c, inner, body) : emit_expr(c, inner, body);
  c->def_stack.n--;
  return F(c, "(({ %sdouble __r = (%s); __r; }))", bstr(&preface), b);
}

/* `n` where one number is needed. */
static const char* scalar_value(Ctx* c, Scope* sc, Node* n, Buf* pre, const char* what) {
  if (!refs_stereo(c, sc, n)) return emit_expr(c, sc, n);
  SV v = emit_stereo(c, sc, n, pre);
  if (v.stereo)
    fail(c, "%s needs one number but gets a pair — pick a side with [0] / [1], or make it "
         "pair state: $x = [0, 0] %s", what, err_loc(c, n));
  return v.l;
}

/* Operands of a pair operation, flattened to numbers, each into a temp. */
static const char** pair_operands(Ctx* c, Scope* sc, Node* n, Buf* pre, int want, const char* usage) {
  const char** r = al(c, (2 * n->nkids + 1) * sizeof(char*));
  int k = 0;
  for (int i = 0; i < n->nkids; i++) {
    Node* kid = n->kids[i];
    if (refs_stereo(c, sc, kid)) {
      SV v = emit_stereo(c, sc, kid, pre);
      const char* tl = fresh(c, F(c, "%s_a", n->str));
      badd(c, pre, "  double %s = (%s);\n", tl, v.l);
      r[k++] = tl;
      if (v.stereo) {
        const char* tr = fresh(c, F(c, "%s_a", n->str));
        badd(c, pre, "  double %s = (%s);\n", tr, v.r);
        r[k++] = tr;
      }
    } else {
      const char* t = fresh(c, F(c, "%s_a", n->str));
      const char* e = emit_expr(c, sc, kid);
      badd(c, pre, "  double %s = (%s);\n", t, e);
      r[k++] = t;
    }
  }
  if (k != want)
    fail(c, "%s takes %s, got %d%s%s", n->str, usage, k, k == 1 ? " number " : " numbers ", err_loc(c, n));
  return r;
}

/* `target = rhs` for a pair: both sides into temps before either is stored. */
static const char* assign_pair(Ctx* c, Scope* sc, const char* tl0, const char* tr0, Node* rhs) {
  Buf pre = {0};
  SV v = emit_stereo(c, sc, rhs, &pre);
  const char* tl = fresh(c, "pa_l");
  const char* tr = fresh(c, "pa_r");
  badd(c, &pre, "  double %s = (%s);\n  double %s = (%s);\n  %s = %s;\n  %s = %s;\n",
       tl, v.l, tr, v.r, tl0, tl, tr0, tr);
  return sdup(c, bstr(&pre));
}

/* `$z[0] = expr` / `$z[1] = expr`: one side of a pair. */
static const char* assign_pair_side(Ctx* c, Scope* sc, Node* st) {
  Node* base = st->kids[0];
  Node* idx = st->kids[1];
  if (base->kind == nkIdent && idx->kind == nkNum && ((int)idx->num == 0 || (int)idx->num == 1)) {
    int k = (int)idx->num;
    const char* target = "";
    Ent* pair = lookup_stereo(sc, base->str);
    if (pair) target = k == 0 ? pair->a : pair->b;
    else if (has(&c->pair_vars, base->str)) target = pair_var_ref(c, base->str, k);
    if (target[0]) {
      Buf pre = {0};
      const char* rhs = scalar_value(c, sc, st->kids[2], &pre, F(c, "$%s[%d]", base->str, k));
      badd(c, &pre, "  %s = (%s);\n", target, rhs);
      return sdup(c, bstr(&pre));
    }
  }
  fail(c, "only one side of pair state can be assigned by index ($z[0] = ... or $z[1] = ...) %s",
       err_loc(c, st));
  return NULL;
}

/* The size of `array(N, init)`: a compile-time integer. */
static int array_make_size(Ctx* c, Scope* sc, Node* call) {
  Node* narg = call->kids[0];
  double v;
  if (!const_value(c, sc, narg, &v))
    fail(c, "array: the size must be a compile-time integer %s", err_loc(c, narg));
  long n = (long)v;
  if ((double)n != v || n < 1 || n > 1000000)
    fail(c, "array: the size must be a whole number from 1 to 1000000, got %s %s",
         nim_float(c, v), err_loc(c, narg));
  return (int)n;
}

/* `$f = array(N, init)`: a state array, a flag then the elements;
 * `every = M` steps it, a second generation landing every M samples. */
static void declare_state_array(Ctx* c, Scope* sc, Node* st, Buf* pre, int top_level) {
  Node* call = st->kids[0];
  ArrayArgs args = array_args(call);
  int n = array_make_size(c, sc, call);
  long every = 0;
  if (args.every) {
    double v;
    if (!const_value(c, sc, args.every, &v) || (double)(long)v != v || v < 0)
      fail(c, "array: every must be a whole number of samples (0 for none), known when the "
           "patch compiles %s", err_loc(c, args.every));
    every = (long)v;
  }
  int gens = every > 0 ? 2 : 1;
  const char* off = register_region(c, "sarr", 1 + gens * n, st->str);
  Buf init_pre = {0};
  const char* init = args.npos == 2 ? scalar_value(c, sc, args.pos[1], &init_pre, F(c, "$%s", st->str)) : "0.0";
  long wo = every > 0 ? n : 0;
  const char* base;
  if (top_level) {
    Ent* e = hput(c, &c->top_sarrays, st->str);
    e->a = off; e->n = n; e->wo = wo;
    base = off;
  } else {
    const char* b = fresh(c, "sa");
    badd(c, pre, "  long %s = %s;\n", b, off);
    Ent* e = lput(c, &sc->sarrays, st->str);
    e->a = b; e->n = n; e->wo = wo;
    base = b;
  }
  badd(c, pre, "  if (s->pool[%s] == 0.0) {\n%s    double _v = (%s);\n"
       "    for (long _k = 0; _k < %d; ++_k) s->pool[%s + 1 + _k] = _v;\n"
       "    s->pool[%s] = 1.0;\n  }\n", base, bstr(&init_pre), init, gens * n, base, base);
  if (every > 0)
    badd(c, pre, "  if (((long)floor(s->t * s->sr + 0.5) - 1) %% %ld == 0)\n"
         "    for (long _k = 0; _k < %d; ++_k) s->pool[%s + 1 + _k] = s->pool[%s + 1 + %d + _k];\n",
         every, n, base, base, n);
}

/* `let a = [e1, ..., eN]` with runtime values, or `let a = array(N, init)`. */
static void declare_local_array(Ctx* c, Scope* sc, const char* name, Node* rhs, Buf* pre, int top_level) {
  const char* sym = fresh(c, F(c, "la_%s", name));
  if (rhs->kind == nkArr) {
    badd(c, pre, "  double %s[%d];\n", sym, rhs->nkids);
    for (int k = 0; k < rhs->nkids; k++) {
      const char* v = scalar_value(c, sc, rhs->kids[k], pre, F(c, "%s[%d]", name, k));
      badd(c, pre, "  %s[%d] = (%s);\n", sym, k, v);
    }
    Ent* e = top_level ? hput(c, &c->top_larrays, name) : lput(c, &sc->larrays, name);
    e->a = sym; e->n = rhs->nkids;
  } else {
    ArrayArgs args = array_args(rhs);
    if (args.every)
      fail(c, "every steps a state array, which keeps its values: $%s = array(...), not let %s",
           name, err_loc(c, rhs));
    int n = array_make_size(c, sc, rhs);
    const char* init = args.npos == 2 ? scalar_value(c, sc, args.pos[1], pre, name) : "0.0";
    badd(c, pre, "  double %s[%d];\n  { double _v = (%s); for (long _k = 0; _k < %d; ++_k) %s[_k] = _v; }\n",
         sym, n, init, n, sym);
    Ent* e = top_level ? hput(c, &c->top_larrays, name) : lput(c, &sc->larrays, name);
    e->a = sym; e->n = n;
  }
}

/* `name[i] = v`: an element store. */
static const char* emit_idx_assign(Ctx* c, Scope* sc, Node* st) {
  Node* base = st->kids[0];
  if (base->kind == nkIdent) {
    Ent* sa = lookup_sarray(c, sc, base->str);
    if (sa) {
      Buf pre = {0};
      const char* ix = emit_expr(c, sc, st->kids[1]);
      const char* v = scalar_value(c, sc, st->kids[2], &pre, F(c, "$%s[i]", base->str));
      const char* it = fresh(c, "ai");
      const char* vt = fresh(c, "av");
      badd(c, &pre, "  long %s = (long)(%s); double %s = (%s);\n", it, ix, vt, v);
      const char* store = F(c, "  if (%s >= 0 && %s < %ld) s->pool[%s + 1 + %ld + %s] = %s;\n",
                            it, it, sa->n, sa->a, sa->wo, it, vt);
      if (c->each_depth > 0) {
        bcat(c, &c->deferred, store);
        return sdup(c, bstr(&pre));
      }
      bcat(c, &pre, store);
      return sdup(c, bstr(&pre));
    }
    Ent* la = lookup_larray(c, sc, base->str);
    if (la) {
      Buf pre = {0};
      const char* ix = emit_expr(c, sc, st->kids[1]);
      const char* v = scalar_value(c, sc, st->kids[2], &pre, F(c, "%s[i]", base->str));
      badd(c, &pre, "  { long _i = (long)(%s) %% %ld; %s[_i < 0 ? _i + %ld : _i] = (%s); }\n",
           ix, la->n, la->a, la->n, v);
      return sdup(c, bstr(&pre));
    }
    if (lookup_array(c, sc, base->str))
      fail(c, "%s is a constant table; to change elements declare it with runtime values or "
           "array(N) %s", base->str, err_loc(c, st));
  }
  return assign_pair_side(c, sc, st);
}

static int all_nums(Node* a) {
  for (int i = 0; i < a->nkids; i++) if (a->kids[i]->kind != nkNum) return 0;
  return 1;
}

/* One non-final statement of a def, lambda or stereo-def body. */
static void emit_local_stmt(Ctx* c, Scope* sc, Node* st, Buf* pre) {
  switch (st->kind) {
  case nkLet: {
    Node* rhs = st->kids[0];
    if ((rhs->kind == nkArr && rhs->nkids != 2 && rhs->nkids > 0 && !all_nums(rhs)) || is_array_make(rhs)) {
      declare_local_array(c, sc, st->str, rhs, pre, 0);
    } else if (rhs->kind == nkArr && rhs->nkids > 0 && all_nums(rhs)) {
      const char* sym = fresh(c, F(c, "arr_def_%s", st->str));
      const char** items = al(c, rhs->nkids * sizeof(char*));
      Buf list = {0};
      for (int i = 0; i < rhs->nkids; i++) {
        items[i] = num_lit(c, rhs->kids[i]->num);
        if (i) bcat(c, &list, ", ");
        bcat(c, &list, items[i]);
      }
      badd(c, &c->array_decls, "static const double %s[%d] = {%s};\n", sym, rhs->nkids, bstr(&list));
      Ent* e = lput(c, &sc->arrays, st->str);
      e->a = sym; e->n = rhs->nkids;
      if (rhs->nkids == 2) {
        const char* tl = fresh(c, F(c, "l_%s_l", st->str));
        const char* tr = fresh(c, F(c, "l_%s_r", st->str));
        badd(c, pre, "  double %s = %s;\n  double %s = %s;\n", tl, items[0], tr, items[1]);
        Ent* s2 = lput(c, &sc->stereo, st->str);
        s2->a = tl; s2->b = tr;
      }
    } else if (refs_stereo(c, sc, rhs)) {
      SV v = emit_stereo(c, sc, rhs, pre);
      if (v.stereo) {
        const char* tl = fresh(c, F(c, "l_%s_l", st->str));
        const char* tr = fresh(c, F(c, "l_%s_r", st->str));
        badd(c, pre, "  double %s = (%s);\n  double %s = (%s);\n", tl, v.l, tr, v.r);
        Ent* e = lput(c, &sc->stereo, st->str);
        e->a = tl; e->b = tr;
      } else {
        const char* t = fresh(c, F(c, "l_%s", st->str));
        badd(c, pre, "  double %s = (%s);\n", t, v.l);
        lput(c, &sc->names, st->str)->a = t;
      }
    } else {
      const char* t = fresh(c, F(c, "l_%s", st->str));
      const char* v = emit_expr(c, sc, rhs);
      badd(c, pre, "  double %s = (%s);\n", t, v);
      lput(c, &sc->names, st->str)->a = t;
      double cv;
      if (const_value(c, sc, rhs, &cv)) lput(c, &sc->numlits, st->str)->v = cv;
      else note_invariant(c, sc, st->str, rhs, sc);
    }
    break;
  }
  case nkVar:
    if (is_array_make(st->kids[0])) {
      declare_state_array(c, sc, st, pre, 0);
    } else if (is_stereo_return(c, st->kids[0])) {
      /* Pair state: a "pvar" region, the flag then the two sides. */
      const char* flag = fresh(c, "pf");
      const char* off = register_region(c, "pvar", 3, st->str);
      badd(c, pre, "  long %s = %s;\n", flag, off);
      Buf init_pre = {0};
      SV v = emit_stereo(c, sc, st->kids[0], &init_pre);
      badd(c, pre, "  if (s->pool[%s] == 0.0) {\n%s    s->pool[%s + 1] = (%s);\n"
           "    s->pool[%s + 2] = (%s);\n    s->pool[%s] = 1.0;\n  }\n",
           flag, bstr(&init_pre), flag, v.l, flag, v.r, flag);
      Ent* e = lput(c, &sc->stereo, st->str);
      e->a = F(c, "s->pool[%s + 1]", flag);
      e->b = F(c, "s->pool[%s + 2]", flag);
    } else {
      /* `$x` in a def: a "var" region, a flag and the value. */
      const char* flag = fresh(c, "vf");
      const char* val = fresh(c, "vs");
      const char* off = register_region(c, "var", 2, st->str);
      badd(c, pre, "  long %s = %s;\n  long %s = (%s) + 1;\n", flag, off, val, off);
      Buf init_pre = {0};
      const char* init = scalar_value(c, sc, st->kids[0], &init_pre, F(c, "$%s", st->str));
      badd(c, pre, "  if (s->pool[%s] == 0.0) {\n%s    s->pool[%s] = (%s);\n    s->pool[%s] = 1.0;\n  }\n",
           flag, bstr(&init_pre), val, init, flag);
      lput(c, &sc->names, st->str)->a = F(c, "s->pool[%s]", val);
    }
    break;
  case nkAssign: {
    Ent* pair = lookup_stereo(sc, st->str);
    if (pair) {
      bcat(c, pre, assign_pair(c, sc, pair->a, pair->b, st->kids[0]));
    } else {
      const char* target = lookup(sc, st->str);
      if (!target[0]) fail(c, "unknown assignment target: %s %s", st->str, err_loc(c, st));
      const char* rhs = scalar_value(c, sc, st->kids[0], pre, F(c, "$%s", st->str));
      badd(c, pre, "  %s = (%s);\n", target, rhs);
    }
    break;
  }
  case nkIdxAssign:
    bcat(c, pre, emit_idx_assign(c, sc, st));
    break;
  default:
    if (refs_stereo(c, sc, st)) {
      SV v = emit_stereo(c, sc, st, pre);
      badd(c, pre, "  (void)(%s);\n  (void)(%s);\n", v.l, v.r);
    } else {
      const char* e = emit_expr(c, sc, st);
      badd(c, pre, "  (void)(%s);\n", e);
    }
  }
}

/* A def's body: its statements, then the expression returned. */
static const char* emit_block_expr(Ctx* c, Scope* sc, Node* blk) {
  Buf pre = {0};
  int last = blk->nkids - 1;
  for (int i = 0; i < blk->nkids; i++) {
    Node* s = blk->kids[i];
    if (i == last) {
      const char* v = scalar_value(c, sc, s, &pre, "the result");
      return F(c, "({ %sdouble __r = (%s); __r; })", bstr(&pre), v);
    }
    emit_local_stmt(c, sc, s, &pre);
  }
  return "0.0";
}

/* ---- recursion: a def that calls itself compiles to a loop ---- */

typedef struct {
  Node* def;
  const char** vl; const char** vr;    /* each parameter's loop variable(s); "" when bound by name */
  int* pairs;
  int pair;
  const char* rl; const char* rr;
} RecLoop;

static Node* self_call_walk(const char* name, Node* n) {
  if (!n) return NULL;
  if (n->kind == nkCall && eq(n->str, name)) return n;
  for (int i = 0; i < n->nkids; i++) {
    Node* f = self_call_walk(name, n->kids[i]);
    if (f) return f;
  }
  return NULL;
}

static void mark_recursion(Ctx* c) {
  for (int b = 0; b < NBUCKETS; b++)
    for (Ent* e = c->user_defs.b[b]; e; e = e->next) {
      Node* def = e->node;
      Node* call = self_call_walk(def->str, def->kids[0]);
      if (!call) continue;
      if (!def->is_rec)
        fail(c, "%s calls itself %s. Write def rec %s to make it repeat. (A def can't call the "
             "one it replaces: a wrapper needs a name of its own.)", def->str, err_loc(c, call), def->str);
      incl(c, &c->recursive, def->str);
    }
}

static const char* c_literal(Ctx* c, const char* s) {
  Buf b = {0};
  bcat(c, &b, "\"");
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') bcat(c, &b, "\\");
    bput(c, &b, s, 1);
  }
  bcat(c, &b, "\"");
  return sdup(c, bstr(&b));
}

static void check_repeats_walk(Ctx* c, Node* def, Node* n, int tail) {
  if (!n) return;
  switch (n->kind) {
  case nkCall:
    if (eq(n->str, def->str) && !tail)
      fail(c, "%s calls itself inside an expression %s. A def repeats only when the call is the "
           "whole result of a branch: if done then x else %s(...). Carry the rest of the work in "
           "a parameter instead, like def fib(n, a = 0, b = 1): if n == 0 then a else fib(n - 1, b, a + b)",
           def->str, err_loc(c, n), def->str);
    for (int i = 0; i < n->nkids; i++) check_repeats_walk(c, def, n->kids[i], 0);
    break;
  case nkIf:
    check_repeats_walk(c, def, n->kids[0], 0);
    check_repeats_walk(c, def, n->kids[1], tail);
    check_repeats_walk(c, def, n->kids[2], tail);
    break;
  case nkBlock:
    for (int i = 0; i < n->nkids; i++) check_repeats_walk(c, def, n->kids[i], tail && i == n->nkids - 1);
    break;
  default:
    for (int i = 0; i < n->nkids; i++) check_repeats_walk(c, def, n->kids[i], 0);
  }
}

static void check_repeats(Ctx* c, Node* def) {
  if (has(&c->tail_checked, def->str)) return;
  check_repeats_walk(c, def, def->kids[0], 1);
  incl(c, &c->tail_checked, def->str);
}

static void binds_walk(Node* n, NameSet* names) {
  if (!n) return;
  if (n->kind == nkLet || n->kind == nkVar) ns_incl(names, n->str);
  for (int i = 0; i < n->nkids; i++) binds_walk(n->kids[i], names);
}
static int count_nodes(Node* n) {
  if (!n) return 0;
  int k = 1;
  for (int i = 0; i < n->nkids; i++) k += count_nodes(n->kids[i]);
  return k;
}
static void changing_walk(Node* def, Node* n, NameSet* rebound, int* res) {
  if (!n) return;
  if (n->kind == nkCall && eq(n->str, def->str))
    for (int i = 0; i < n->nkids; i++) {
      Node* a = n->kids[i];
      if (i < def->nparams && !(a->kind == nkIdent && eq(a->str, def->params[i]) && !ns_has(rebound, a->str)))
        res[i] = 1;
    }
  for (int i = 0; i < n->nkids; i++) changing_walk(def, n->kids[i], rebound, res);
}
/* The parameters some repeat changes. */
static int* changing_params(Ctx* c, Node* def) {
  int* res = al(c, (def->nparams ? def->nparams : 1) * sizeof(int));
  NameSet rebound = {al(c, (count_nodes(def->kids[0]) + 1) * sizeof(char*)), 0};
  binds_walk(def->kids[0], &rebound);
  changing_walk(def, def->kids[0], &rebound, res);
  return res;
}

static void emit_local_stmt(Ctx* c, Scope* sc, Node* st, Buf* pre);

/* The tail of a recursive def's body, as statements inside the loop. */
static void emit_tail(Ctx* c, Scope* sc, RecLoop* lp, Node* n, Buf* o) {
  switch (n->kind) {
  case nkBlock: {
    Scope* inner = push(c, sc);
    for (int i = 0; i < n->nkids; i++) {
      if (i == n->nkids - 1) emit_tail(c, inner, lp, n->kids[i], o);
      else emit_local_stmt(c, inner, n->kids[i], o);
    }
    return;
  }
  case nkIf: {
    const char* cond = scalar_value(c, sc, n->kids[0], o, "the condition");
    badd(c, o, "  if ((%s) != 0.0) {\n", cond);
    emit_tail(c, sc, lp, n->kids[1], o);
    bcat(c, o, "  } else {\n");
    Node* els = n->kids[2] ? n->kids[2] : mknum(c, 0.0, n->line, n->source);
    emit_tail(c, sc, lp, els, o);
    bcat(c, o, "  }\n");
    return;
  }
  case nkCall:
    if (eq(n->str, lp->def->str)) {
      Node* def = lp->def;
      if (n->nkids != def->nparams)
        fail(c, "wrong arg count for %s: expected %d got %d %s", def->str, def->nparams, n->nkids, err_loc(c, n));
      Buf assigns = {0};
      for (int i = 0; i < n->nkids; i++) {
        Node* a = n->kids[i];
        const char* p = def->params[i];
        if (!lp->vl[i][0]) {
          if (!(a->kind == nkIdent && eq(a->str, p)))
            fail(c, "%s passes a different array or function as '%s' %s: an array or function "
                 "parameter is passed on unchanged when a def repeats", def->str, p, err_loc(c, n));
          continue;
        }
        if (lp->pairs[i]) {
          if (a->kind == nkIdent) {
            Ent* st = lookup_stereo(sc, a->str);
            if (eq(st ? st->a : "", lp->vl[i])) continue;
          }
          SV sv = emit_stereo(c, sc, a, o);
          const char* tl = fresh(c, F(c, "next_%s_l", p));
          const char* tr = fresh(c, F(c, "next_%s_r", p));
          badd(c, o, "  double %s = (%s); double %s = (%s);\n", tl, sv.l, tr, sv.r);
          badd(c, &assigns, "  %s = %s; %s = %s;\n", lp->vl[i], tl, lp->vr[i], tr);
        } else {
          if (a->kind == nkIdent && eq(lookup(sc, a->str), lp->vl[i])) continue;
          const char* x = scalar_value(c, sc, a, o, F(c, "%s's %s", def->str, p));
          const char* t = fresh(c, F(c, "next_%s", p));
          badd(c, o, "  double %s = (%s);\n", t, x);
          badd(c, &assigns, "  %s = %s;\n", lp->vl[i], t);
        }
      }
      bcat(c, o, bstr(&assigns));
      bcat(c, o, "  continue;\n");
      return;
    }
    break;
  default: break;
  }
  if (lp->pair) {
    SV v = emit_stereo(c, sc, n, o);
    badd(c, o, "  %s = (%s); %s = (%s); break;\n", lp->rl, v.l, lp->rr, v.r);
  } else {
    const char* v = scalar_value(c, sc, n, o, F(c, "the result of %s", lp->def->str));
    badd(c, o, "  %s = (%s); break;\n", lp->rl, v);
  }
}

/* A call to a recursive def: the loop's statements, and its result. */
typedef struct { const char* stmts; const char* l; const char* r; } Loop;
static Loop emit_loop(Ctx* c, Scope* sc, Node* def, Node* call, int pair) {
  if (call->nkids != def->nparams)
    fail(c, "wrong arg count for %s: expected %d got %d %s", def->str, def->nparams, call->nkids, err_loc(c, call));
  if (sfind(&c->def_stack, def->str) >= 0) cycle_error(c, def, call);
  check_repeats(c, def);
  int* changing = changing_params(c, def);
  RecLoop lp = {def, NULL, NULL, pair_params(c, def), pair, NULL, NULL};
  lp.vl = al(c, (def->nparams + 1) * sizeof(char*));
  lp.vr = al(c, (def->nparams + 1) * sizeof(char*));
  Scope* inner = push(c, sc);
  Buf o = {0};
  for (int i = 0; i < def->nparams; i++) {
    const char* p = def->params[i];
    Node* a = call->kids[i];
    if (lp.pairs[i]) {
      SV sv = emit_stereo(c, sc, a, &o);
      const char* vl = fresh(c, F(c, "rec_%s_l", p));
      const char* vr = fresh(c, F(c, "rec_%s_r", p));
      badd(c, &o, "  double %s = (%s); double %s = (%s);\n", vl, sv.l, vr, sv.r);
      Ent* e = lput(c, &inner->stereo, p);
      e->a = vl; e->b = vr;
      lp.vl[i] = vl; lp.vr[i] = vr;
      continue;
    }
    Buf sub = {0};
    if (bind_by_name(c, sc, inner, p, a, &sub)) {
      bcat(c, &o, bstr(&sub));
      lp.vl[i] = ""; lp.vr[i] = "";
      continue;
    }
    const char* v = scalar_value(c, sc, a, &o, F(c, "%s's %s", def->str, p));
    const char* vn = fresh(c, F(c, "rec_%s", p));
    badd(c, &o, "  double %s = (%s);\n", vn, v);
    lput(c, &inner->names, p)->a = vn;
    if (!changing[i]) {
      double cv;
      if (const_value(c, sc, a, &cv)) lput(c, &inner->numlits, p)->v = cv;
      else note_invariant(c, inner, p, a, sc);
    }
    lp.vl[i] = vn; lp.vr[i] = "";
  }
  lp.rl = fresh(c, F(c, "rec_%s", def->str));
  lp.rr = pair ? fresh(c, F(c, "rec_%s_r", def->str)) : lp.rl;
  const char* steps = fresh(c, "steps");
  const char* give_up = pair ? F(c, "%s = %s = 0.0 / 0.0;", lp.rl, lp.rr) : F(c, "%s = 0.0 / 0.0;", lp.rl);
  const char* where = c_literal(c, F(c, "%s %s", def->str, err_loc(c, def)));
  if (pair) badd(c, &o, "  double %s, %s;\n", lp.rl, lp.rr);
  else badd(c, &o, "  double %s;\n", lp.rl);
  badd(c, &o, "  long %s = 0;\n", steps);
  badd(c, &o, "  if (s->stuck) { %s } else for (;;) {\n", give_up);
  badd(c, &o, "  if ((++%s & 65535) == 0 && n_overrun((void*)s, %s)) { %s break; }\n", steps, where, give_up);
  spush(c, &c->def_stack, def->str);
  int depth = c->each_depth;
  Buf deferred = c->deferred;
  c->each_depth = 0;
  c->deferred = (Buf){0};
  emit_tail(c, inner, &lp, def->kids[0], &o);
  c->each_depth = depth;
  c->deferred = deferred;
  c->def_stack.n--;
  bcat(c, &o, "  }\n");
  return (Loop){sdup(c, bstr(&o)), lp.rl, lp.rr};
}

/* phasor(freq): one slot, advanced and wrapped. */
static const char* emit_phasor(Ctx* c, Scope* sc, Node* freq) {
  const char* step;
  if (c->hoist_on && is_invariant(c, sc, freq)) step = hoist(c, F(c, "(%s) / s->sr", emit_invariant(c, sc, freq)));
  else step = F(c, "(%s) / s->sr", emit_expr(c, sc, freq));
  const char* slot = fresh(c, "ph");
  const char* off = register_region(c, "phasor", 1, NULL);
  return F(c, "(({ long %s = %s; s->pool[%s] = fmod(s->pool[%s] + %s, 1.0); "
           "if (s->pool[%s] < 0.0) s->pool[%s] += 1.0; s->pool[%s]; }))",
           slot, off, slot, slot, step, slot, slot, slot);
}

/* noise(): xorshift32 in a slot, seeded from where the call is (its
 * statement, its defs, which noise() there), so each call site is its
 * own stream, the same on every run. */
static const char* emit_noise(Ctx* c) {
  const char* slot = fresh(c, "nz");
  const char* off = register_region(c, "noise", 1, NULL);
  Region* r = &c->regions[c->nregions - 1];
  unsigned seed = 2166136261u;
  const char* id = F(c, "%s|%s|%d", r->scope, r->owner, r->scope_idx);
  for (const char* p = id; *p; p++) seed = (seed ^ (unsigned char)*p) * 16777619u;
  if (seed == 0) seed = 2463534242u;
  return F(c, "(({ long %s = %s; unsigned int r = (unsigned int)s->pool[%s]; if (r == 0) r = %uu; "
           "r ^= r << 13; r ^= r >> 17; r ^= r << 5; s->pool[%s] = (double)r; "
           "((double)r / 4294967295.0) * 2.0 - 1.0; }))", slot, off, slot, seed, slot);
}

/* The slots one call site of a native claims. */
static long native_slot_size(Ctx* c, const char* name, Node** kids) {
  const NativeSpec* spec = native(name);
  if (spec->buf_arg < 0) return spec->slots;
  Node* a = kids[spec->buf_arg];
  if (a->kind != nkNum) {
    /* the parameter's name, from the comma-separated list */
    const char* p = spec->params;
    for (int i = 0; i < spec->buf_arg; i++) p = strchr(p, ',') + 1;
    const char* e = strchr(p, ',');
    const char* pname = e ? F(c, "%.*s", (int)(e - p), p) : p;
    fail(c, "%s %s must be a numeric literal %s", name, pname, err_loc(c, a));
  }
  long n = (long)(a->num * c->sr);
  return spec->slots + (n > 1 ? n : 1);
}

static const char* const KIND_NAMES[] = {
  "nkNum", "nkIdent", "nkBinOp", "nkUnary", "nkCall", "nkIf",
  "nkVar", "nkLet", "nkDef", "nkAssign", "nkArr", "nkIdx", "nkIdxAssign", "nkBlock",
  "nkPlay", "nkLambda", "nkImport", "nkStr", "nkNamedArg"};

static const char* const HOISTED_MATH[] = {"exp", "log", "log2", "pow", "sin", "cos", "tan", "tanh", "sqrt", NULL};
static const char* const MIDI_FNS[] = {"midi_cc", "midi_note", "midi_trig", "midi_freq",
                                       "midi_gate", "midi_voice_freq", "midi_voice_gate",
                                       "midi_voice_vel", "midi_voice_key", "midi_vel", NULL};

static const char* emit_expr(Ctx* c, Scope* sc, Node* n) {
  switch (n->kind) {
  case nkNum:
    return num_lit(c, n->num);
  case nkIdent: {
    const char* s = n->str;
    if (eq(s, "t")) return "s->t";
    if (eq(s, "sr")) return "s->sr";
    if (eq(s, "dt")) return "s->dt";
    if (eq(s, "start_t")) return "s->start_t";
    if (eq(s, "PI")) return "3.14159265358979323846";
    if (eq(s, "TAU")) return "(2.0 * 3.14159265358979323846)";
    const char* scoped = lookup(sc, s);
    Ent* table = lookup_array(c, sc, s);
    if (scoped[0]) return scoped;
    if (table && table->n != 2)
      fail(c, "%s is a table of %ld numbers, but one number is needed here — index it (%s[i]) "
           "or hand it to wave %s", s, table->n, s, err_loc(c, n));
    if (lookup_stereo(sc, s) || has(&c->stereo_lets, s) || has(&c->pair_vars, s) || has(&c->play_set, s))
      fail(c, "%s is a pair, but one number is needed here — pick a side with %s[0] / %s[1], "
           "or use mono(%s) %s", s, s, s, s, err_loc(c, n));
    if (has(&c->top_lets, s)) {
      if (!has(&c->lets_done, s)) {
        Ent* ll = hfind(&c->let_lines, s);
        fail(c, "'%s' is used before its `let` on line %ld: move that let above this line %s",
             s, ll ? ll->n : 0L, err_loc(c, n));
      }
      return F(c, "l_%s", s);
    }
    if (has(&c->top_var_set, s)) return F(c, "s->v_%s", s);
    fail(c, "unknown identifier: %s %s", s, err_loc(c, n));
    return NULL;
  }
  case nkUnary: {
    const char* v = emit_expr(c, sc, n->kids[0]);
    if (eq(n->str, "-")) return F(c, "(-(%s))", v);
    if (eq(n->str, "not")) return F(c, "((%s) == 0.0 ? 1.0 : 0.0)", v);
    fail(c, "bad unary: %s %s", n->str, err_loc(c, n));
    return NULL;
  }
  case nkBinOp: {
    const char* a = emit_expr(c, sc, n->kids[0]);
    const char* b = emit_expr(c, sc, n->kids[1]);
    const char* op = n->str;
    if (eq(op, "+")) return F(c, "((%s) + (%s))", a, b);
    if (eq(op, "-")) return F(c, "((%s) - (%s))", a, b);
    if (eq(op, "*")) return F(c, "((%s) * (%s))", a, b);
    if (eq(op, "/")) return F(c, "((%s) == 0.0 ? 0.0 : (%s) / (%s))", b, a, b);
    if (eq(op, "mod")) return F(c, "((%s) == 0.0 ? 0.0 : (%s) - floor((%s) / (%s)) * (%s))", b, a, a, b, b);
    if (eq(op, "==")) return F(c, "(((%s) == (%s)) ? 1.0 : 0.0)", a, b);
    if (eq(op, "!=")) return F(c, "(((%s) != (%s)) ? 1.0 : 0.0)", a, b);
    if (eq(op, "<")) return F(c, "(((%s) < (%s)) ? 1.0 : 0.0)", a, b);
    if (eq(op, ">")) return F(c, "(((%s) > (%s)) ? 1.0 : 0.0)", a, b);
    if (eq(op, "<=")) return F(c, "(((%s) <= (%s)) ? 1.0 : 0.0)", a, b);
    if (eq(op, ">=")) return F(c, "(((%s) >= (%s)) ? 1.0 : 0.0)", a, b);
    /* `and` and `or` don't short-circuit: both sides always run, so a
     * `prev(g)` on the right keeps stepping. */
    if (eq(op, "and")) return F(c, "((((%s) != 0.0) & ((%s) != 0.0)) ? 1.0 : 0.0)", a, b);
    if (eq(op, "or")) return F(c, "((((%s) != 0.0) | ((%s) != 0.0)) ? 1.0 : 0.0)", a, b);
    fail(c, "bad binop: %s %s", op, err_loc(c, n));
    return NULL;
  }
  case nkIf: {
    const char* cond = emit_expr(c, sc, n->kids[0]);
    const char* thn = emit_expr(c, sc, n->kids[1]);
    const char* els = n->kids[2] ? emit_expr(c, sc, n->kids[2]) : "0.0";
    return F(c, "((%s) != 0.0 ? (%s) : (%s))", cond, thn, els);
  }
  case nkCall: {
    /* Costly pure maths over invariant inputs: once a block. */
    if (c->hoist_on && !c->in_hoist && in(n->str, HOISTED_MATH) && !user_def(c, n->str) &&
        !lookup_fn(sc, n->str)[0] && !lookup_lambda(sc, n->str) && is_invariant(c, sc, n) &&
        !is_const(c, sc, n))
      return hoist(c, emit_invariant(c, sc, n));
    /* A lambda-valued param: inline its body. */
    Node* lam = lookup_lambda(sc, n->str);
    if (lam) {
      if (n->nkids < lam->nparams)
        fail(c, "lambda '%s' takes %d args, got %d %s", n->str, lam->nparams, n->nkids, err_loc(c, n));
      Scope* inner = push(c, sc);
      Buf preface = {0};
      for (int i = 0; i < lam->nparams; i++) {
        const char* p = lam->params[i];
        const char* arg = emit_expr(c, sc, n->kids[i]);
        const char* tmp = fresh(c, F(c, "p_%s", p));
        badd(c, &preface, "double %s = (%s); ", tmp, arg);
        lput(c, &inner->names, p)->a = tmp;
        double cv;
        if (const_value(c, sc, n->kids[i], &cv)) lput(c, &inner->numlits, p)->v = cv;
        else note_invariant(c, inner, p, n->kids[i], sc);
      }
      Node* body = lam->kids[0];
      const char* b = body->kind == nkBlock ? emit_block_expr(c, inner, body) : emit_expr(c, inner, body);
      return F(c, "(({ %sdouble __r = (%s); __r; }))", bstr(&preface), b);
    }
    const char* resolved = lookup_fn(sc, n->str);
    const char* name = resolved[0] ? resolved : n->str;
    if (eq(name, "sum")) {
      if (n->nkids != 2) fail(c, "sum takes 2 args: sum(N, fn) %s", err_loc(c, n));
      Node* narg = n->kids[0];
      Node* lamarg = n->kids[1];
      double v;
      if (!const_value(c, sc, narg, &v))
        fail(c, "sum: first arg must be a compile-time-constant integer (a literal, a name bound "
             "to one, or len(array)) %s", err_loc(c, narg));
      long N = (long)v;
      if ((double)N != v) fail(c, "sum: first arg must be an integer, got %s %s", nim_float(c, v), err_loc(c, narg));
      if (N < 0) fail(c, "sum: first arg must be non-negative, got %ld %s", N, err_loc(c, narg));
      if (lamarg->kind != nkLambda)
        fail(c, "sum: second arg must be a lambda like `n => expr`, got %s %s", KIND_NAMES[lamarg->kind], err_loc(c, lamarg));
      if (lamarg->nparams != 1) fail(c, "sum: lambda must take exactly one parameter %s", err_loc(c, lamarg));
      if (N == 0) return "0.0";
      const char* param = lamarg->params[0];
      Node* body = lamarg->kids[0];
      Buf terms = {0};
      for (long k = 1; k <= N; k++) {
        Scope* iter = push(c, sc);
        lput(c, &iter->names, param)->a = num_lit(c, (double)k);
        lput(c, &iter->numlits, param)->v = (double)k;
        const char* b = body->kind == nkBlock ? emit_block_expr(c, iter, body) : emit_expr(c, iter, body);
        if (k > 1) bcat(c, &terms, " + ");
        bcat(c, &terms, b);
      }
      return F(c, "(%s)", bstr(&terms));
    }
    if (eq(name, "each")) {
      if (n->nkids != 2 || n->kids[1]->kind != nkLambda || n->kids[1]->nparams != 1)
        fail(c, "each takes a count and a one-parameter lambda: each(N, i => ...) %s", err_loc(c, n));
      Node* narg = n->kids[0];
      double v;
      if (!const_value(c, sc, narg, &v))
        fail(c, "each: the count must be a compile-time integer (a literal, a name bound to one, "
             "or len(array)) %s", err_loc(c, narg));
      long N = (long)v;
      if ((double)N != v || N < 0)
        fail(c, "each: the count must be a non-negative integer, got %s %s", nim_float(c, v), err_loc(c, narg));
      Node* l = n->kids[1];
      Buf saved = c->deferred;
      c->deferred = (Buf){0};
      c->each_depth++;
      Buf body = {0};
      for (long k = 0; k < N; k++) {
        Scope* iter = push(c, sc);
        lput(c, &iter->names, l->params[0])->a = num_lit(c, (double)k);
        lput(c, &iter->numlits, l->params[0])->v = (double)k;
        Node* b = l->kids[0];
        if (b->kind == nkBlock) {
          for (int i = 0; i < b->nkids - 1; i++) emit_local_stmt(c, iter, b->kids[i], &body);
          Node* last = b->kids[b->nkids - 1];
          if (!(last->kind == nkNum && last->num == 0.0)) {
            const char* e = emit_expr(c, iter, last);
            badd(c, &body, "  (void)(%s);\n", e);
          }
        } else {
          const char* e = emit_expr(c, iter, b);
          badd(c, &body, "  (void)(%s);\n", e);
        }
      }
      c->each_depth--;
      const char* commits = sdup(c, bstr(&c->deferred));
      c->deferred = saved;
      return F(c, "(({\n%s%s  0.0; }))", bstr(&body), commits);
    }
    if (eq(name, "len")) {
      if (n->nkids == 1 && n->kids[0]->kind == nkIdent) {
        const char* a = n->kids[0]->str;
        Ent* e;
        if ((e = lookup_sarray(c, sc, a))) return num_lit(c, (double)e->n);
        if ((e = lookup_larray(c, sc, a))) return num_lit(c, (double)e->n);
        if ((e = lookup_array(c, sc, a))) return num_lit(c, (double)e->n);
      }
      fail(c, "len takes the name of an array %s", err_loc(c, n));
    }
    if (eq(name, "array"))
      fail(c, "array(N) declares an array: `$f = array(N)` for state that lasts, `let a = array(N)` "
           "for this sample's scratch %s", err_loc(c, n));
    if (eq(name, "phasor")) {
      if (n->nkids != 1) fail(c, "phasor takes 1 arg %s", err_loc(c, n));
      return emit_phasor(c, sc, n->kids[0]);
    }
    if (eq(name, "noise")) return emit_noise(c);
    if (in(name, LIBM1) && n->nkids == 1) return F(c, "%s(%s)", name, emit_expr(c, sc, n->kids[0]));
    if (eq(name, "abs") && n->nkids == 1) return F(c, "fabs(%s)", emit_expr(c, sc, n->kids[0]));
    if ((eq(name, "saw") || eq(name, "tri") || eq(name, "sqr")) && n->nkids == 1)
      return F(c, "shape_%s(%s)", name, emit_expr(c, sc, n->kids[0]));
    if (eq(name, "pow") && n->nkids == 2) return F(c, "pow(%s)", call_args(c, sc, n->kids, n->nkids));
    if ((eq(name, "min") || eq(name, "max")) && n->nkids >= 2) {
      const char* acc = emit_expr(c, sc, n->kids[0]);
      for (int i = 1; i < n->nkids; i++) {
        const char* k = emit_expr(c, sc, n->kids[i]);
        acc = F(c, "f%s(%s, %s)", name, acc, k);
      }
      return acc;
    }
    if (eq(name, "clamp") && n->nkids == 3) {
      const char* v = emit_expr(c, sc, n->kids[0]);
      const char* lo = emit_expr(c, sc, n->kids[1]);
      const char* hi = emit_expr(c, sc, n->kids[2]);
      return F(c, "fmin(fmax(%s, %s), %s)", v, lo, hi);
    }
    if (eq(name, "int") && n->nkids == 1) return F(c, "((double)(long)(%s))", emit_expr(c, sc, n->kids[0]));
    if (in(name, PAIR_CALLS))
      fail(c, "%s returns a pair — pick a side with `%s(...)[0]` / `[1]`, bind it with `let p = "
           "%s(...)`, or pass it to `magnitude` / `phase` %s", name, name, name, err_loc(c, n));
    if (in(name, PAIR_READERS)) {
      Buf pre = {0};
      const char** a = pair_operands(c, sc, n, &pre, 2, "one pair (or two numbers)");
      const char* v = eq(name, "magnitude") ? F(c, "sqrt(%s * %s + %s * %s)", a[0], a[0], a[1], a[1])
                                            : F(c, "atan2(%s, %s)", a[1], a[0]);
      return F(c, "(({\n%s  double __m = %s; __m; }))", bstr(&pre), v);
    }
    if (eq(name, "freq_shift") && n->nkids == 2) {
      const char* args[2];
      args[0] = emit_expr(c, sc, n->kids[0]);
      args[1] = emit_expr(c, sc, n->kids[1]);
      const char* off = register_region(c, "freq_shift", 33, NULL);
      return native_at(c, off, args, 2, "n_freq_shift((DspState*)s, $1, $2)");
    }
    if (eq(name, "freq_shift")) fail(c, "freq_shift(signal, hz) takes exactly 2 args %s", err_loc(c, n));
    if (eq(name, "midi_cc") && n->nkids == 1) return F(c, "n_midi_cc((int)(%s))", emit_expr(c, sc, n->kids[0]));
    if (eq(name, "midi_note") && n->nkids == 1) return F(c, "n_midi_note((int)(%s))", emit_expr(c, sc, n->kids[0]));
    if (eq(name, "midi_freq") && n->nkids == 0) return "n_midi_freq()";
    if (eq(name, "midi_gate") && n->nkids == 0) return "n_midi_gate()";
    if (eq(name, "midi_trig") && n->nkids == 1) {
      const char* off = register_region(c, "midi_trig", 1, NULL);
      const char* args[1] = {emit_expr(c, sc, n->kids[0])};
      return native_at(c, off, args, 1, "n_midi_trig((DspState*)s, (int)($1))");
    }
    if (eq(name, "midi_voice_freq") && n->nkids == 1) return F(c, "n_midi_voice_freq((int)(%s))", emit_expr(c, sc, n->kids[0]));
    if (eq(name, "midi_voice_gate") && n->nkids == 1) return F(c, "n_midi_voice_gate((int)(%s))", emit_expr(c, sc, n->kids[0]));
    if (eq(name, "midi_voice_vel") && n->nkids == 1) return F(c, "n_midi_voice_vel((int)(%s))", emit_expr(c, sc, n->kids[0]));
    if (eq(name, "midi_voice_key") && n->nkids == 1) return F(c, "n_midi_voice_key((int)(%s))", emit_expr(c, sc, n->kids[0]));
    if (eq(name, "midi_vel") && n->nkids == 0) return "n_midi_vel()";
    if (in(name, MIDI_FNS)) fail(c, "%s: wrong arg count %s", name, err_loc(c, n));
    if (eq(name, "wave")) {
      if (n->nkids != 2) fail(c, "wave takes 2 args %s", err_loc(c, n));
      const char* freq = emit_expr(c, sc, n->kids[0]);
      Node* arr = n->kids[1];
      const char* sym;
      long length;
      if (arr->kind == nkIdent) {
        Ent* found = lookup_array(c, sc, arr->str);
        if (!found)
          fail(c, "wave: %s must be a constant table of numeric literals%s %s", arr->str,
               lookup_larray(c, sc, arr->str) ? " (it holds runtime values)" : "", err_loc(c, arr));
        sym = found->a;
        length = found->n;
      } else if (arr->kind == nkArr) {
        sym = fresh(c, "arr");
        length = arr->nkids;
        Buf items = {0};
        for (int i = 0; i < arr->nkids; i++) {
          if (arr->kids[i]->kind != nkNum) fail(c, "wave array must be numeric literals only %s", err_loc(c, arr->kids[i]));
          if (i) bcat(c, &items, ", ");
          bcat(c, &items, num_lit(c, arr->kids[i]->num));
        }
        badd(c, &c->array_decls, "static const double %s[%ld] = {%s};\n", sym, length, bstr(&items));
      } else {
        fail(c, "wave: second arg must be an array literal or a let binding to one %s", err_loc(c, arr));
        return NULL;
      }
      long size = native_slot_size(c, "wave", n->kids);
      const char* off = register_region(c, "wave", size, NULL);
      const char* args[1] = {freq};
      return native_at(c, off, args, 1, F(c, "n_wave((DspState*)s, $1, (double*)%s, %ld)", sym, length));
    }
    /* Hoisted natives: the coefficients once, the `_c` step per sample. */
    if (c->hoist_on && (eq(name, "lp1") || eq(name, "hp1")) && n->nkids == 2 && is_invariant(c, sc, n->kids[1])) {
      const char* a = hoist(c, F(c, "n_lp1_coef(%s, s->sr)", emit_invariant(c, sc, n->kids[1])));
      const char* off = register_region(c, name, native_slot_size(c, name, n->kids), NULL);
      const char* args[1] = {emit_expr(c, sc, n->kids[0])};
      return native_at(c, off, args, 1, F(c, "n_%s_c((DspState*)s, $1, %s)", name, a));
    }
    if (c->hoist_on && (eq(name, "lpf") || eq(name, "hpf") || eq(name, "bpf") || eq(name, "notch")) &&
        n->nkids == 3 && is_invariant(c, sc, n->kids[1]) && is_invariant(c, sc, n->kids[2])) {
      const char* k1 = emit_invariant(c, sc, n->kids[1]);
      const char* k2 = emit_invariant(c, sc, n->kids[2]);
      const char* h = hoist_arr(c, 4, F(c, "n_svf_coef(%s, %s, s->sr, $H)", k1, k2));
      const char* off = register_region(c, name, native_slot_size(c, name, n->kids), NULL);
      const char* args[1] = {emit_expr(c, sc, n->kids[0])};
      return native_at(c, off, args, 1, F(c, "n_%s_c((DspState*)s, $1, %s)", name, h));
    }
    if (c->hoist_on && eq(name, "reverb") && n->nkids == 3 && is_invariant(c, sc, n->kids[1])) {
      const char* h = hoist_arr(c, 4, F(c, "n_reverb_coef(%s, s->sr, $H)", emit_invariant(c, sc, n->kids[1])));
      const char* off = register_region(c, name, native_slot_size(c, name, n->kids), NULL);
      const char* args[2];
      args[0] = emit_expr(c, sc, n->kids[0]);
      args[1] = emit_expr(c, sc, n->kids[2]);
      return native_at(c, off, args, 2, F(c, "n_reverb_c((DspState*)s, $1, $2, %s)", h));
    }
    /* Native DSP calls, each at its region. */
    const NativeSpec* spec = native(name);
    if (spec && !eq(name, "wave")) {
      int arity = native_arity(spec);
      if (n->nkids != arity) fail(c, "%s takes %d args, got %d %s", name, arity, n->nkids, err_loc(c, n));
      long size = native_slot_size(c, name, n->kids);
      const char* off = register_region(c, name, size, NULL);
      const char** args = al(c, n->nkids * sizeof(char*));
      Buf slots = {0};
      for (int i = 0; i < n->nkids; i++) {
        args[i] = emit_expr(c, sc, n->kids[i]);
        badd(c, &slots, i ? ", $%d" : "$%d", i + 1);
      }
      return native_at(c, off, args, n->nkids, F(c, "n_%s((DspState*)s, %s)", name, bstr(&slots)));
    }
    Node* def = user_def(c, name);
    if (def) {
      if (has(&c->recursive, name)) {
        Loop lp = emit_loop(c, sc, def, n, 0);
        return F(c, "(({\n%s  %s; }))", lp.stmts, lp.l);
      }
      return emit_def_inline(c, sc, def, n);
    }
    fail(c, "unknown function: %s %s", name, err_loc(c, n));
    return NULL;
  }
  case nkArr:
    fail(c, "array value can't appear here %s — only numeric literals for wave() or top-level stereo return",
         err_loc(c, n));
    return NULL;
  case nkStr:
    fail(c, "a string is only an import path here %s", err_loc(c, n));
    return NULL;
  case nkImport:
    fail(c, "import belongs at the top of the file %s", err_loc(c, n));
    return NULL;
  case nkLambda:
    fail(c, "lambda `%s => ...` can only appear as a builtin argument (e.g. sum(N, n => expr)), "
         "not as a standalone value %s", n->nparams > 0 ? n->params[0] : "_", err_loc(c, n));
    return NULL;
  case nkIdx: {
    Node* a = n->kids[0];
    Node* i = n->kids[1];
    if (a->kind == nkIdent) {
      Ent* sa = lookup_sarray(c, sc, a->str);
      if (sa) {
        const char* ix = emit_expr(c, sc, i);
        return F(c, "(({ long _i = (long)(%s); (_i >= 0 && _i < %ld) ? s->pool[%s + 1 + _i] : 0.0; }))",
                 ix, sa->n, sa->a);
      }
      Ent* la = lookup_larray(c, sc, a->str);
      if (la) {
        const char* ix = emit_expr(c, sc, i);
        return F(c, "(({ long _i = (long)(%s) %% %ld; %s[_i < 0 ? _i + %ld : _i]; }))", ix, la->n, la->a, la->n);
      }
      Ent* found = lookup_array(c, sc, a->str);
      if (found) {
        const char* ix = emit_expr(c, sc, i);
        return F(c, "(({ long _i = (long)(%s) %% %ld; %s[_i < 0 ? _i + %ld : _i]; }))",
                 ix, found->n, found->a, found->n);
      }
    }
    if (a->kind == nkIdent && i->kind == nkNum && ((int)i->num == 0 || (int)i->num == 1)) {
      int k = (int)i->num;
      Ent* st = lookup_stereo(sc, a->str);
      if (st) return k == 0 ? st->a : st->b;
      if (has(&c->pair_vars, a->str)) return pair_var_ref(c, a->str, k);
      if (has(&c->play_set, a->str) || has(&c->stereo_lets, a->str)) return stereo_ref(c, a->str, k == 0 ? "l" : "r");
    }
    /* Any other index on a pair wraps at 2, as a table's at its length. */
    Ent* bound = a->kind == nkIdent ? lookup_stereo(sc, a->str) : NULL;
    if (bound || refs_stereo(c, sc, a)) {
      Buf pre = {0};
      const char* l = bound ? bound->a : "";
      const char* r = bound ? bound->b : "";
      if (!bound) {
        SV v = emit_stereo(c, sc, a, &pre);
        l = v.l; r = v.r;
      }
      const char* ix = emit_expr(c, sc, i);
      return F(c, "(({\n%s  long _i = (long)(%s) %% 2; double __pick = (_i == 0) ? (%s) : (%s); __pick; }))",
               bstr(&pre), ix, l, r);
    }
    if (a->kind == nkArr && i->kind == nkNum) {
      int k = (int)i->num;
      if (k < 0 || k >= a->nkids) fail(c, "array index out of range %s", err_loc(c, n));
      return emit_expr(c, sc, a->kids[k]);
    }
    if (i->kind == nkNum && ((int)i->num == 0 || (int)i->num == 1) && refs_stereo(c, sc, a)) {
      Buf pre = {0};
      SV v = emit_stereo(c, sc, a, &pre);
      return F(c, "(({\n%s  double __pick = (%s); __pick; }))", bstr(&pre), (int)i->num == 0 ? v.l : v.r);
    }
    fail(c, "indexing non-constant arrays not supported %s", err_loc(c, n));
    return NULL;
  }
  default:
    fail(c, "cannot emit expression node: %s %s", KIND_NAMES[n->kind], err_loc(c, n));
    return NULL;
  }
}

static int is_shape_or_math(const char* name) {
  static const char* const xs[] = {"pow", "min", "max", "clamp", "int", "saw", "tri", "sqr", "abs", NULL};
  return in(name, LIBM1) || in(name, xs);
}

static SV emit_stereo(Ctx* c, Scope* sc, Node* n, Buf* pre) {
  /* One value (no pair in it): emit it once, into a temp when it may
   * have side effects, so both channels read the same sample. */
  if (!refs_stereo(c, sc, n)) {
    if (n->kind == nkNum) {
      const char* s = num_lit(c, n->num);
      return (SV){s, s, 0};
    }
    if (n->kind == nkIdent) {
      if (is_fn_name(c, sc, n->str)) return (SV){n->str, n->str, 0};
      const char* s = emit_expr(c, sc, n);
      return (SV){s, s, 0};
    }
    const char* s = emit_expr(c, sc, n);
    const char* tmp = fresh(c, "sst");
    badd(c, pre, "  double %s = (%s);\n", tmp, s);
    return (SV){tmp, tmp, 0};
  }
  switch (n->kind) {
  case nkIdent: {
    Ent* st = lookup_stereo(sc, n->str);
    if (st) return (SV){st->a, st->b, 1};
    if (has(&c->pair_vars, n->str)) return (SV){pair_var_ref(c, n->str, 0), pair_var_ref(c, n->str, 1), 1};
    if (has(&c->play_set, n->str) || has(&c->stereo_lets, n->str))
      return (SV){stereo_ref(c, n->str, "l"), stereo_ref(c, n->str, "r"), 1};
    fail(c, "internal: nkIdent '%s' flagged as stereo by refsStereo but no stereo binding found %s",
         n->str, err_loc(c, n));
    break;
  }
  case nkArr: {
    if (n->nkids == 2) {
      const char* l = emit_expr(c, sc, n->kids[0]);
      const char* r = emit_expr(c, sc, n->kids[1]);
      return (SV){l, r, 1};
    }
    fail(c, "array literal with %d elements in stereo context (only [L,R] supported) %s", n->nkids, err_loc(c, n));
    break;
  }
  case nkIdx: {
    Node* base = n->kids[0];
    Node* idx = n->kids[1];
    if (base->kind == nkIdent && idx->kind == nkNum && ((int)idx->num == 0 || (int)idx->num == 1)) {
      int k = (int)idx->num;
      Ent* st = lookup_stereo(sc, base->str);
      if (st) { const char* p = k == 0 ? st->a : st->b; return (SV){p, p, 0}; }
      if (has(&c->pair_vars, base->str)) { const char* p = pair_var_ref(c, base->str, k); return (SV){p, p, 0}; }
      if (has(&c->play_set, base->str) || has(&c->stereo_lets, base->str)) {
        const char* p = stereo_ref(c, base->str, k == 0 ? "l" : "r");
        return (SV){p, p, 0};
      }
    }
    fail(c, "stereo-indexed expression must be base[0] or base[1] %s", err_loc(c, n));
    break;
  }
  case nkBinOp: {
    SV a = emit_stereo(c, sc, n->kids[0], pre);
    SV b = emit_stereo(c, sc, n->kids[1], pre);
    const char* op = n->str;
    const char* fmt;
    if (eq(op, "+")) fmt = "((%s) + (%s))";
    else if (eq(op, "-")) fmt = "((%s) - (%s))";
    else if (eq(op, "*")) fmt = "((%s) * (%s))";
    else if (eq(op, "/")) {
      return (SV){F(c, "((%s) == 0.0 ? 0.0 : (%s) / (%s))", b.l, a.l, b.l),
                  F(c, "((%s) == 0.0 ? 0.0 : (%s) / (%s))", b.r, a.r, b.r), 1};
    } else {
      fail(c, "binop '%s' not allowed on stereo values %s", op, err_loc(c, n));
      return (SV){0};
    }
    return (SV){F(c, fmt, a.l, b.l), F(c, fmt, a.r, b.r), 1};
  }
  case nkUnary: {
    SV a = emit_stereo(c, sc, n->kids[0], pre);
    if (eq(n->str, "-")) return (SV){F(c, "(-(%s))", a.l), F(c, "(-(%s))", a.r), 1};
    fail(c, "unary '%s' not allowed on stereo %s", n->str, err_loc(c, n));
    break;
  }
  case nkIf: {
    /* The condition is one number, into a temp so it runs once. */
    const char* cond = emit_expr(c, sc, n->kids[0]);
    const char* ct = fresh(c, "ifc");
    badd(c, pre, "  double %s = (%s);\n", ct, cond);
    SV thn = emit_stereo(c, sc, n->kids[1], pre);
    SV els = n->kids[2] ? emit_stereo(c, sc, n->kids[2], pre) : (SV){"0.0", "0.0", 0};
    return (SV){F(c, "((%s) != 0.0 ? (%s) : (%s))", ct, thn.l, els.l),
                F(c, "((%s) != 0.0 ? (%s) : (%s))", ct, thn.r, els.r), 1};
  }
  case nkCall: {
    if (in(n->str, PAIR_CALLS)) {
      const char* f = n->str;
      if (eq(f, "cmul")) {
        const char** a = pair_operands(c, sc, n, pre, 4, "two pairs (or four numbers)");
        return (SV){F(c, "(%s * %s - %s * %s)", a[0], a[2], a[1], a[3]),
                    F(c, "(%s * %s + %s * %s)", a[0], a[3], a[1], a[2]), 1};
      }
      if (eq(f, "cdiv")) {
        const char** a = pair_operands(c, sc, n, pre, 4, "two pairs (or four numbers)");
        const char* den = fresh(c, "cdiv_den");
        badd(c, pre, "  double %s = %s * %s + %s * %s;\n", den, a[2], a[2], a[3], a[3]);
        return (SV){F(c, "(%s == 0.0 ? 0.0 : (%s * %s + %s * %s) / %s)", den, a[0], a[2], a[1], a[3], den),
                    F(c, "(%s == 0.0 ? 0.0 : (%s * %s - %s * %s) / %s)", den, a[1], a[2], a[0], a[3], den), 1};
      }
      if (eq(f, "cscale")) {
        const char** a = pair_operands(c, sc, n, pre, 3, "a number and a pair (or three numbers)");
        return (SV){F(c, "(%s * %s)", a[0], a[1]), F(c, "(%s * %s)", a[0], a[2]), 1};
      }
      if (eq(f, "rotate")) {
        const char** a = pair_operands(c, sc, n, pre, 3, "a pair and an angle (or three numbers)");
        const char* cs = fresh(c, "rot_c");
        const char* sn = fresh(c, "rot_s");
        badd(c, pre, "  double %s = cos(%s);\n", cs, a[2]);
        badd(c, pre, "  double %s = sin(%s);\n", sn, a[2]);
        return (SV){F(c, "(%s * %s - %s * %s)", a[0], cs, a[1], sn),
                    F(c, "(%s * %s + %s * %s)", a[0], sn, a[1], cs), 1};
      }
      if (eq(f, "conj")) {
        const char** a = pair_operands(c, sc, n, pre, 2, "one pair (or two numbers)");
        return (SV){a[0], F(c, "(-%s)", a[1]), 1};
      }
      if (eq(f, "analytic")) {
        const char** a = pair_operands(c, sc, n, pre, 1, "one signal");
        const char* sig = a[0];
        const char* off_a = register_region(c, "hilbert_a", 16, NULL);
        const char* off_b = register_region(c, "hilbert_b", 16, NULL);
        const char* re = fresh(c, "an_re");
        const char* im = fresh(c, "an_im");
        badd(c, pre, "  s->idx = %s;\n", off_a);
        badd(c, pre, "  double %s = n_hilbert_a((DspState*)s, %s);\n", re, sig);
        badd(c, pre, "  s->idx = %s;\n", off_b);
        badd(c, pre, "  double %s = -n_hilbert_b((DspState*)s, %s);\n", im, sig);
        return (SV){re, im, 1};
      }
      if (eq(f, "phasor_pair")) {
        const char** a = pair_operands(c, sc, n, pre, 1, "a frequency");
        const char* off = register_region(c, "phasor_pair", 1, NULL);
        const char* ph = fresh(c, "ppair_ph");
        badd(c, pre, "  s->pool[%s] = fmod(s->pool[%s] + (%s) / s->sr, 1.0);\n", off, off, a[0]);
        badd(c, pre, "  if (s->pool[%s] < 0.0) s->pool[%s] += 1.0;\n", off, off);
        badd(c, pre, "  double %s = s->pool[%s] * (2.0 * 3.14159265358979323846);\n", ph, off);
        return (SV){F(c, "cos(%s)", ph), F(c, "sin(%s)", ph), 1};
      }
      if (eq(f, "input")) {
        pair_operands(c, sc, n, pre, 0, "no arguments");
        return (SV){"s->in_l", "s->in_r", 1};
      }
    }
    /* A pair-returning def (haas, pan): inlined once, its two channels
     * caught in temps. */
    Node* sdef = user_def(c, n->str);
    if (sdef && is_stereo_def(c, sdef)) {
      if (in(n->str, MONO_ONLY_INPUT_DEFS) && n->nkids > 0 && refs_stereo(c, sc, n->kids[0]))
        fail(c, "%s takes a mono signal — collapse the input via mono(x) or pick a channel via "
             "x[0] / x[1] before piping %s", n->str, err_loc(c, n));
      Node* def = sdef;
      if (n->nkids != def->nparams)
        fail(c, "wrong arg count for %s: expected %d got %d %s", n->str, def->nparams, n->nkids, err_loc(c, n));
      if (has(&c->recursive, n->str)) {
        Loop lp = emit_loop(c, sc, def, n, 1);
        bcat(c, pre, lp.stmts);
        return (SV){lp.l, lp.r, 1};
      }
      if (sfind(&c->def_stack, def->str) >= 0) cycle_error(c, def, n);
      Scope* inner = push(c, sc);
      const char* tl = fresh(c, F(c, "st_%s_l", n->str));
      const char* tr = fresh(c, F(c, "st_%s_r", n->str));
      Buf blk = {0};
      badd(c, &blk, "  double %s, %s;\n", tl, tr);
      bcat(c, &blk, "  {\n");
      for (int i = 0; i < def->nparams; i++) {
        const char* p = def->params[i];
        Node* a = n->kids[i];
        if (a->kind == nkIdent && is_fn_name(c, sc, a->str)) {
          const char* resolved = lookup_fn(sc, a->str)[0] ? lookup_fn(sc, a->str) : a->str;
          lput(c, &inner->fns, p)->a = resolved;
          continue;
        }
        Buf sub = {0};
        SV sv = emit_stereo(c, sc, a, &sub);
        bcat(c, pre, bstr(&sub));
        if (sv.stereo || pair_params(c, def)[i]) {
          const char* pl = fresh(c, F(c, "p_%s_l", p));
          const char* pr = fresh(c, F(c, "p_%s_r", p));
          badd(c, &blk, "    double %s = (%s);\n", pl, sv.l);
          badd(c, &blk, "    double %s = (%s);\n", pr, sv.r);
          Ent* e = lput(c, &inner->stereo, p);
          e->a = pl; e->b = pr;
        } else {
          const char* t = fresh(c, F(c, "p_%s", p));
          badd(c, &blk, "    double %s = (%s);\n", t, sv.l);
          lput(c, &inner->names, p)->a = t;
          if (a->kind == nkNum) lput(c, &inner->numlits, p)->v = a->num;
          else if (a->kind == nkIdent) {
            double v;
            if (lookup_num_lit(sc, a->str, &v)) lput(c, &inner->numlits, p)->v = v;
          }
          if (!lfind(inner->numlits, p)) note_invariant(c, inner, p, a, sc);
        }
      }
      Node* body = def->kids[0];
      Node** stmts = body->kind == nkBlock ? body->kids : &def->kids[0];
      int nst = body->kind == nkBlock ? body->nkids : 1;
      spush(c, &c->def_stack, def->str);
      for (int si = 0; si < nst; si++) {
        Node* st = stmts[si];
        if (si == nst - 1 && st->kind != nkLet && st->kind != nkVar && st->kind != nkAssign && st->kind != nkIdxAssign) {
          Buf sub = {0};
          SV v = emit_stereo(c, inner, st, &sub);
          bcat(c, &blk, bstr(&sub));
          badd(c, &blk, "    %s = (%s);\n", tl, v.l);
          badd(c, &blk, "    %s = (%s);\n", tr, v.r);
        } else {
          emit_local_stmt(c, inner, st, &blk);
        }
      }
      c->def_stack.n--;
      bcat(c, &blk, "  }\n");
      bcat(c, pre, bstr(&blk));
      return (SV){tl, tr, 1};
    }
    /* A def that reads some params as pairs: nothing to split when every
     * pair lands on one of those, so it runs once. */
    int* pairs = NULL;
    int npairs = 0;
    Node* udef = user_def(c, n->str);
    if (udef) { pairs = takes_pair(c, udef); npairs = udef->nparams; }
    int any_pair = 0;
    for (int i = 0; i < npairs; i++) if (pairs[i]) any_pair = 1;
    if (any_pair) {
      int per_channel = 0;
      for (int i = 0; i < n->nkids; i++)
        if (i < npairs && !pairs[i] && refs_stereo(c, sc, n->kids[i])) per_channel = 1;
      if (!per_channel) {
        const char* v = emit_expr(c, sc, n);
        const char* tmp = fresh(c, "pp");
        badd(c, pre, "  double %s = (%s);\n", tmp, v);
        return (SV){tmp, tmp, 0};
      }
    }
    /* Otherwise split per channel: each side a call of its own. */
    SV* argv = al(c, (n->nkids + 1) * sizeof(SV));
    int any_stereo = 0;
    for (int i = 0; i < n->nkids; i++) {
      argv[i] = emit_stereo(c, sc, n->kids[i], pre);
      if (argv[i].stereo) any_stereo = 1;
    }
    if (!any_stereo)
      fail(c, "internal: nkCall '%s' flagged as stereo by refsStereo but no arg resolved to stereo %s",
           n->str, err_loc(c, n));
    const char* name = n->str;
    int can_split = is_shape_or_math(name) || in(name, TRANSPARENT_HELPERS) ||
                    (udef && is_pure_for_stereo(c, udef->kids[0]));
    if (!can_split) {
      const char* who = "x";
      for (int i = 0; i < n->nkids; i++)
        if (argv[i].stereo && n->kids[i]->kind == nkIdent) { who = n->kids[i]->str; break; }
      fail(c, "stateful call '%s' receives a stereo value %s — split L/R first via %s[0] / %s[1], "
           "or collapse it with mono(%s)", name, err_loc(c, n), who, who, who);
    }
    Scope* inl = push(c, sc);
    Scope* inr = push(c, sc);
    Node* cl = mknode(c, nkCall, n->line, n->source);
    Node* cr = mknode(c, nkCall, n->line, n->source);
    cl->str = n->str; cr->str = n->str;
    for (int i = 0; i < n->nkids; i++) {
      Node* k = n->kids[i];
      if (i < npairs && pairs[i] && argv[i].stereo) {
        const char* tmp = fresh(c, "spp");
        Ent* e = lput(c, &inl->stereo, tmp); e->a = argv[i].l; e->b = argv[i].r;
        e = lput(c, &inr->stereo, tmp); e->a = argv[i].l; e->b = argv[i].r;
        add_kid(c, cl, mkident(c, tmp, n->line, n->source));
        add_kid(c, cr, mkident(c, tmp, n->line, n->source));
      } else if (argv[i].stereo) {
        const char* tl = fresh(c, "stl");
        const char* tr = fresh(c, "str");
        lput(c, &inl->names, tl)->a = argv[i].l;
        lput(c, &inr->names, tr)->a = argv[i].r;
        add_kid(c, cl, mkident(c, tl, n->line, n->source));
        add_kid(c, cr, mkident(c, tr, n->line, n->source));
      } else if (k->kind == nkNum || k->kind == nkIdent) {
        /* kept as written: delay and friends read a literal to size a buffer */
        add_kid(c, cl, k);
        add_kid(c, cr, k);
      } else {
        const char* tmp = fresh(c, "ssa");
        lput(c, &inl->names, tmp)->a = argv[i].l;
        lput(c, &inr->names, tmp)->a = argv[i].l;
        add_kid(c, cl, mkident(c, tmp, n->line, n->source));
        add_kid(c, cr, mkident(c, tmp, n->line, n->source));
      }
    }
    const char* le = emit_expr(c, inl, cl);
    const char* re = emit_expr(c, inr, cr);
    return (SV){le, re, 1};
  }
  default: {
    const char* s = emit_expr(c, sc, n);
    return (SV){s, s, 0};
  }
  }
  return (SV){0};
}

/* Does a subtree hold a pair (a play, a stereo let, a scope's pair, a
 * pair-returning def)? */
static int refs_stereo(Ctx* c, Scope* sc, Node* n) {
  if (!n) return 0;
  switch (n->kind) {
  case nkIdent:
    if (sc && scalar_shadows(sc, n->str)) return 0;
    if (has(&c->play_set, n->str) || has(&c->stereo_lets, n->str)) return 1;
    if (has(&c->pair_vars, n->str)) return 1;
    if (sc && lookup_stereo(sc, n->str)) return 1;
    return 0;
  case nkArr: return n->nkids == 2;
  case nkLambda: return 0;
  case nkIdx: {
    Node* a = n->kids[0];
    Node* i = n->kids[1];
    if (a->kind == nkIdent && (lookup_sarray(c, sc, a->str) || lookup_larray(c, sc, a->str)))
      return refs_stereo(c, sc, i);
    if (a->kind == nkIdent && i->kind == nkNum && ((int)i->num == 0 || (int)i->num == 1)) {
      int base = (!sc || !scalar_shadows(sc, a->str)) &&
                 (has(&c->play_set, a->str) || has(&c->stereo_lets, a->str) || has(&c->pair_vars, a->str) ||
                  (sc && lookup_stereo(sc, a->str)));
      if (base) return 0;
    }
    return refs_stereo(c, sc, i);
  }
  case nkCall: {
    if (in(n->str, PAIR_CALLS)) return 1;
    if (in(n->str, PAIR_READERS)) return 0;
    if (eq(n->str, "wave")) return n->nkids > 0 && refs_stereo(c, sc, n->kids[0]);
    Node* def = user_def(c, n->str);
    if (def) {
      if (is_stereo_def(c, def)) return 1;
      int* pairs = takes_pair(c, def);
      for (int i = 0; i < n->nkids; i++) {
        if (i < def->nparams && pairs[i]) continue;
        if (n->kids[i] && refs_stereo(c, sc, n->kids[i])) return 1;
      }
      return 0;
    }
    for (int i = 0; i < n->nkids; i++) if (n->kids[i] && refs_stereo(c, sc, n->kids[i])) return 1;
    return 0;
  }
  default:
    for (int i = 0; i < n->nkids; i++) if (n->kids[i] && refs_stereo(c, sc, n->kids[i])) return 1;
    return 0;
  }
}

/* ------------------------------------------------- top-level emission */

/* A statement's Region.scope: what it binds, else its role. */
static const char* stmt_scope(Ctx* c, Node* s, int is_final) {
  switch (s->kind) {
  case nkLet: return F(c, "let %s", s->str);
  case nkVar: case nkAssign: case nkIdxAssign: return F(c, "set %s", s->str);
  case nkPlay: return F(c, "play %s", s->str);
  default: return is_final ? "out" : "expr";
  }
}

/* A file-level `$x = init`; the first declaration wins. */
static void declare_var(Ctx* c, Node* n) {
  if (has(&c->play_set, n->str))
    fail(c, "'%s' is the name of a play block — choose a different variable name %s", n->str, err_loc(c, n));
  if (has(&c->top_var_set, n->str) || has(&c->pair_vars, n->str) || has(&c->top_sarray_names, n->str)) return;
  if (c->nvar_decls == c->cap_var_decls) {
    int cap = c->cap_var_decls ? c->cap_var_decls * 2 : 32;
    Node** v = al(c, cap * sizeof(Node*));
    if (c->nvar_decls) memcpy(v, c->var_decls, c->nvar_decls * sizeof(Node*));
    c->var_decls = v; c->cap_var_decls = cap;
  }
  c->var_decls[c->nvar_decls++] = n;
  if (is_array_make(n->kids[0])) {
    incl(c, &c->top_sarray_names, n->str);
  } else if (is_stereo_return(c, n->kids[0])) {
    incl(c, &c->pair_vars, n->str);
    spush(c, &c->top_var_names, pair_var_field(c, n->str, 0));
    spush(c, &c->top_var_names, pair_var_field(c, n->str, 1));
  } else {
    incl(c, &c->top_var_set, n->str);
    spush(c, &c->top_var_names, n->str);
  }
}

static void scan_vars(Ctx* c, Node* n) {
  if (!n) return;
  if (n->kind == nkVar) declare_var(c, n);
  else if (n->kind == nkPlay) {
    Node* body = n->kids[0];
    if (body->kind == nkBlock) for (int i = 0; i < body->nkids; i++) scan_vars(c, body->kids[i]);
    else scan_vars(c, body);
  }
}

/* A file-level `$x = init` lazy-inits where it is declared; only the
 * first declaration of a name inits. */
static const char* var_init(Ctx* c, Scope* sc, Node* st) {
  int first = 0;
  for (int i = 0; i < c->nvar_decls; i++) if (c->var_decls[i] == st) first = 1;
  if (!first) return "";
  Buf init_pre = {0};
  if (has(&c->top_sarray_names, st->str)) {
    declare_state_array(c, sc, st, &init_pre, 1);
    return sdup(c, bstr(&init_pre));
  }
  if (has(&c->pair_vars, st->str)) {
    int i0 = sfind(&c->top_var_names, pair_var_field(c, st->str, 0));
    int i1 = i0 + 1;
    SV v = emit_stereo(c, sc, st->kids[0], &init_pre);
    return F(c, "  if (!(s->inited[%d] & 1)) {\n%s    %s = (%s);\n    %s = (%s);\n"
             "    s->inited[%d] |= 1; s->inited[%d] |= 1;\n  }\n",
             i0, bstr(&init_pre), pair_var_ref(c, st->str, 0), v.l, pair_var_ref(c, st->str, 1), v.r, i0, i1);
  }
  int i = sfind(&c->top_var_names, st->str);
  const char* init = scalar_value(c, sc, st->kids[0], &init_pre, F(c, "$%s", st->str));
  return F(c, "  if (!(s->inited[%d] & 1)) {\n%s    s->v_%s = (%s); s->inited[%d] |= 1;\n  }\n",
           i, bstr(&init_pre), st->str, init, i);
}

static const char* escape_prof(Ctx* c, const char* s) {
  Buf b = {0};
  for (; *s; s++) {
    if (*s == '\\') bcat(c, &b, "\\\\");
    else if (*s == '"') bcat(c, &b, "\\\"");
    else bput(c, &b, s, 1);
  }
  return sdup(c, bstr(&b));
}

static void mention(Ctx* c, Node* n, Map* plays, Map* reached) {
  if (!n) return;
  if (n->kind == nkIdent && has(plays, n->str) && !has(reached, n->str)) {
    incl(c, reached, n->str);
    mention(c, hfind(plays, n->str)->node->kids[0], plays, reached);
  }
  for (int i = 0; i < n->nkids; i++) mention(c, n->kids[i], plays, reached);
}

static const char* emit_program(Ctx* c, Node* program) {
  for (int i = 0; i < program->nkids; i++)
    if (program->kids[i]->kind == nkPlay) incl(c, &c->play_set, program->kids[i]->str);
  for (int i = 0; i < program->nkids; i++) {
    Node* s = program->kids[i];
    if (s->kind == nkVar) declare_var(c, s);
    else if (s->kind == nkLet) {
      if (has(&c->play_set, s->str))
        fail(c, "'%s' is the name of a play block — choose a different variable name %s", s->str, err_loc(c, s));
      incl(c, &c->top_lets, s->str);
      if (!has(&c->let_lines, s->str)) hput(c, &c->let_lines, s->str)->n = s->line;
    } else if (s->kind == nkPlay) {
      Node* body = s->kids[0];
      if (body->kind == nkBlock) for (int k = 0; k < body->nkids; k++) scan_vars(c, body->kids[k]);
      else scan_vars(c, body);
    }
  }

  Buf pre = {0};
  bcat(c, &pre, "#include <math.h>\n");
  bcat(c, &pre, "typedef struct DspState DspState;\n");
  for (int i = 0; dsp_natives[i].name; i++) {
    const NativeSpec* sp = &dsp_natives[i];
    if (eq(sp->name, "wave")) continue;
    badd(c, &pre, "extern double n_%s(DspState*", sp->name);
    for (int k = 0; k < native_arity(sp); k++) bcat(c, &pre, ",double");
    bcat(c, &pre, ");\n");
  }
  bcat(c, &pre,
    "extern double n_lp1_coef(double,double);\n"
    "extern double n_lp1_c(DspState*,double,double);\n"
    "extern double n_hp1_c(DspState*,double,double);\n"
    "extern void n_svf_coef(double,double,double,double*);\n"
    "extern double n_lpf_c(DspState*,double,const double*);\n"
    "extern double n_hpf_c(DspState*,double,const double*);\n"
    "extern double n_bpf_c(DspState*,double,const double*);\n"
    "extern double n_notch_c(DspState*,double,const double*);\n"
    "extern void n_reverb_coef(double,double,double*);\n"
    "extern double n_reverb_c(DspState*,double,double,const double*);\n"
    "extern double n_wave(DspState*,double,double*,int);\n"
    "extern double n_hilbert_a(DspState*,double);\n"
    "extern double n_hilbert_b(DspState*,double);\n"
    "extern double n_freq_shift(DspState*,double,double);\n"
    "extern double shape_saw(double);\n"
    "extern double shape_tri(double);\n"
    "extern double shape_sqr(double);\n"
    "extern double n_midi_cc(int);\n"
    "extern double n_midi_note(int);\n"
    "extern double n_midi_freq(void);\n"
    "extern double n_midi_gate(void);\n"
    "extern double n_midi_trig(DspState*,int);\n"
    "extern double n_midi_voice_freq(int);\n"
    "extern double n_midi_voice_gate(int);\n"
    "extern double n_midi_voice_vel(int);\n"
    "extern double n_midi_voice_key(int);\n"
    "extern double n_midi_vel(void);\n"
    "extern double n_input_l(void);\n"
    "extern double n_input_r(void);\n"
    "extern long n_overrun(void*,const char*);\n"
    "extern double n_now(void);\n"
    "typedef struct {\n"
    "  double* pool;\n"
    "  long   pool_slots;\n"
    "  long   idx;\n"
    "  char   overflow;\n"
    "  double sr;\n"
    "  double t, dt, start_t;\n"
    "  double* part_gains;\n"
    "  double* part_out;\n"
    "  double* part_park;\n"
    "  double* part_fade_delta;\n"
    "  double* part_fade_target;\n"
    "  unsigned char* part_park_pending;\n"
    "  double* part_peak;\n"
    "  double* part_sq;\n"
    "  double* in_buf;\n"
    "  double  in_l, in_r;\n"
    "  double  fade_gain, fade_delta;\n"
    "  double  time_sec, time_frac;\n"
    "  double  peak_decay, rms_alpha;\n"
    "  double  peak_raw;\n"
    "  long    bad;\n"
    "  long    n_parts;\n"
    "  double  deadline;\n"
    "  long    stuck;\n"
    "  const char* stuck_at;\n"
    "  double* prof;\n"
    "  long    profiling;\n"
    "  long    prof_ticks;\n");
  for (int i = 0; i < c->top_var_names.n; i++) badd(c, &pre, "  double v_%s;\n", c->top_var_names.v[i]);
  badd(c, &pre, "  unsigned char inited[%d];\n", c->top_var_names.n > 1 ? c->top_var_names.n : 1);
  bcat(c, &pre, "} VoiceState;\n\n");

  Buf body = {0};
  bcat(c, &body, "static void tick1(VoiceState* s, double* outL, double* outR) {\n");
  bcat(c, &body, "  s->idx = 0;\n");
  bcat(c, &body, "  double prof_last = 0.0;\n");
  bcat(c, &body, "  if (s->profiling) { prof_last = n_now(); s->prof_ticks++; }\n");
  Strs prof_names = {0};
  Scope* top = push(c, NULL);

  int final_idx = -1;
  for (int i = program->nkids - 1; i >= 0; i--) {
    Node* s = program->kids[i];
    int is_each = s->kind == nkCall && eq(s->str, "each");
    if (s->kind != nkDef && s->kind != nkVar && s->kind != nkLet && s->kind != nkAssign &&
        s->kind != nkIdxAssign && s->kind != nkPlay && s->kind != nkImport && !is_each) {
      final_idx = i;
      break;
    }
  }

  for (int i = 0; i < program->nkids; i++) {
    Node* s = program->kids[i];
    const char* src = s->source[0] ? s->source : c->patch_path;
    if (src[0] && s->line > 0) badd(c, &body, "#line %d \"%s\"\n", s->line, src);
    c->scope = stmt_scope(c, s, i == final_idx);
    if (i > 0 && program->kids[i - 1]->kind == nkLet) incl(c, &c->lets_done, program->kids[i - 1]->str);
    switch (s->kind) {
    case nkDef: break;
    case nkImport:
      fail(c, "import \"%s\" was not resolved — the program was built without import expansion %s",
           s->str, err_loc(c, s));
      break;
    case nkVar: bcat(c, &body, var_init(c, top, s)); break;
    case nkLet: {
      Node* rhs = s->kids[0];
      if ((rhs->kind == nkArr && rhs->nkids != 2 && rhs->nkids > 0 && !all_nums(rhs)) || is_array_make(rhs)) {
        declare_local_array(c, top, s->str, rhs, &body, 1);
      } else if (rhs->kind == nkArr && all_nums(rhs)) {
        const char* sym = F(c, "arr_%s", s->str);
        const char** items = al(c, (rhs->nkids + 1) * sizeof(char*));
        Buf list = {0};
        for (int k = 0; k < rhs->nkids; k++) {
          items[k] = num_lit(c, rhs->kids[k]->num);
          if (k) bcat(c, &list, ", ");
          bcat(c, &list, items[k]);
        }
        badd(c, &c->array_decls, "static const double %s[%d] = {%s};\n", sym, rhs->nkids, bstr(&list));
        Ent* e = hput(c, &c->top_arrays, s->str);
        e->a = sym; e->n = rhs->nkids;
        if (rhs->nkids == 2) {
          badd(c, &body, "  double l_%s_l = %s;\n", s->str, items[0]);
          badd(c, &body, "  double l_%s_r = %s;\n", s->str, items[1]);
          incl(c, &c->stereo_lets, s->str);
        }
      } else if (refs_stereo(c, top, rhs)) {
        Buf p2 = {0};
        SV v = emit_stereo(c, top, rhs, &p2);
        bcat(c, &body, bstr(&p2));
        badd(c, &body, "  double l_%s_l = (%s);\n", s->str, v.l);
        badd(c, &body, "  double l_%s_r = (%s);\n", s->str, v.r);
        incl(c, &c->stereo_lets, s->str);
      } else {
        const char* v = emit_expr(c, top, rhs);
        badd(c, &body, "  double l_%s = (%s);\n", s->str, v);
        if (rhs->kind == nkNum) lput(c, &top->numlits, s->str)->v = rhs->num;
        else if (rhs->kind == nkIdent) {
          double nv;
          if (lookup_num_lit(top, rhs->str, &nv)) lput(c, &top->numlits, s->str)->v = nv;
        }
        if (!lfind(top->numlits, s->str)) note_invariant(c, top, s->str, rhs, top);
      }
      break;
    }
    case nkAssign:
      if (has(&c->pair_vars, s->str)) {
        bcat(c, &body, assign_pair(c, top, pair_var_ref(c, s->str, 0), pair_var_ref(c, s->str, 1), s->kids[0]));
      } else {
        const char* target;
        if (has(&c->top_var_set, s->str)) target = F(c, "s->v_%s", s->str);
        else if (has(&c->top_lets, s->str)) target = F(c, "l_%s", s->str);
        else { fail(c, "unknown assign: %s %s", s->str, err_loc(c, s)); target = ""; }
        Buf p2 = {0};
        const char* rhs = scalar_value(c, top, s->kids[0], &p2, F(c, "$%s", s->str));
        bcat(c, &body, bstr(&p2));
        badd(c, &body, "  %s = (%s);\n", target, rhs);
      }
      break;
    case nkIdxAssign:
      bcat(c, &body, emit_idx_assign(c, top, s));
      break;
    case nkPlay: {
      int part = c->play_names.n;
      spush(c, &c->play_names, s->str);
      incl(c, &c->play_set, s->str);
      Scope* inner = push(c, top);
      const char* nm = s->str;
      badd(c, &body, "  double p_%s_l, p_%s_r;\n", nm, nm);
      badd(c, &body, "  if (s->part_park && s->part_park[%d] != 0.0) {\n    p_%s_l = 0.0; p_%s_r = 0.0;\n"
           "    if (s->part_out) { s->part_out[%d] = 0.0; s->part_out[%d] = 0.0; }\n  } else {\n",
           part, nm, nm, 2 * part, 2 * part + 1);
      Node* bn = s->kids[0];
      Node** stmts = bn->kind == nkBlock ? bn->kids : &s->kids[0];
      int nst = bn->kind == nkBlock ? bn->nkids : 1;
      for (int si = 0; si < nst; si++) {
        Node* st = stmts[si];
        c->scope = F(c, "play %s/%s", nm, stmt_scope(c, st, si == nst - 1));
        if (si == nst - 1 && st->kind != nkLet && st->kind != nkVar && st->kind != nkAssign && st->kind != nkIdxAssign) {
          Buf sub = {0};
          SV v = emit_stereo(c, inner, st, &sub);
          bcat(c, &body, bstr(&sub));
          badd(c, &body, "    p_%s_l = (%s) * s->part_gains[%d];\n", nm, v.l, part);
          badd(c, &body, "    p_%s_r = (%s) * s->part_gains[%d];\n", nm, v.r, part);
          badd(c, &body, "    if (s->part_out) { s->part_out[%d] = p_%s_l; s->part_out[%d] = p_%s_r; }\n",
               2 * part, nm, 2 * part + 1, nm);
          continue;
        }
        switch (st->kind) {
        case nkLet: {
          if (has(&c->play_set, st->str))
            fail(c, "'%s' is the name of a play block — choose a different local variable name %s",
                 st->str, err_loc(c, st));
          Node* rhs = st->kids[0];
          int numeric = rhs->kind == nkArr && rhs->nkids > 0 && all_nums(rhs);
          if (numeric) {
            const char* sym = fresh(c, F(c, "arr_%s_%s", nm, st->str));
            Buf list = {0};
            for (int k = 0; k < rhs->nkids; k++) {
              if (k) bcat(c, &list, ", ");
              bcat(c, &list, num_lit(c, rhs->kids[k]->num));
            }
            badd(c, &c->array_decls, "static const double %s[%d] = {%s};\n", sym, rhs->nkids, bstr(&list));
            Ent* e = lput(c, &inner->arrays, st->str);
            e->a = sym; e->n = rhs->nkids;
          }
          if ((rhs->kind == nkArr && rhs->nkids != 2 && rhs->nkids > 0 && !numeric) || is_array_make(rhs)) {
            declare_local_array(c, inner, st->str, rhs, &body, 0);
          } else if (numeric && rhs->nkids != 2) {
            /* a table, nothing more */
          } else if (refs_stereo(c, inner, st->kids[0])) {
            Buf sub = {0};
            SV v = emit_stereo(c, inner, st->kids[0], &sub);
            bcat(c, &body, bstr(&sub));
            badd(c, &body, "    double l_%s_l = (%s);\n", st->str, v.l);
            badd(c, &body, "    double l_%s_r = (%s);\n", st->str, v.r);
            incl(c, &c->stereo_lets, st->str);
          } else {
            const char* v = emit_expr(c, inner, st->kids[0]);
            const char* tmp = F(c, "l_%s", st->str);
            badd(c, &body, "    double %s = (%s);\n", tmp, v);
            lput(c, &inner->names, st->str)->a = tmp;
            double cv;
            if (const_value(c, inner, st->kids[0], &cv)) lput(c, &inner->numlits, st->str)->v = cv;
            else note_invariant(c, inner, st->str, st->kids[0], inner);
          }
          break;
        }
        case nkVar:
          bcat(c, &body, var_init(c, inner, st));
          break;
        case nkAssign:
          if (has(&c->pair_vars, st->str)) {
            bcat(c, &body, assign_pair(c, inner, pair_var_ref(c, st->str, 0), pair_var_ref(c, st->str, 1), st->kids[0]));
          } else {
            const char* target;
            if (has(&c->top_var_set, st->str)) target = F(c, "s->v_%s", st->str);
            else if (lookup(inner, st->str)[0]) target = lookup(inner, st->str);
            else { fail(c, "unknown assignment target '%s' %s", st->str, err_loc(c, st)); target = ""; }
            Buf sub = {0};
            const char* rhs = scalar_value(c, inner, st->kids[0], &sub, F(c, "$%s", st->str));
            bcat(c, &body, bstr(&sub));
            badd(c, &body, "    %s = (%s);\n", target, rhs);
          }
          break;
        case nkIdxAssign:
          bcat(c, &body, emit_idx_assign(c, inner, st));
          break;
        default:
          if (refs_stereo(c, inner, st)) {
            Buf sub = {0};
            SV v = emit_stereo(c, inner, st, &sub);
            bcat(c, &body, bstr(&sub));
            badd(c, &body, "    (void)(%s); (void)(%s);\n", v.l, v.r);
          } else {
            const char* v = emit_expr(c, inner, st);
            badd(c, &body, "    (void)(%s);\n", v);
          }
        }
      }
      bcat(c, &body, "  }\n");
      break;
    }
    default:
      if (i == final_idx) {
        Buf p2 = {0};
        SV v = emit_stereo(c, top, s, &p2);
        bcat(c, &body, bstr(&p2));
        badd(c, &body, "  *outL = (%s); *outR = (%s);\n", v.l, v.r);
      } else if (refs_stereo(c, top, s)) {
        Buf p2 = {0};
        SV v = emit_stereo(c, top, s, &p2);
        bcat(c, &body, bstr(&p2));
        badd(c, &body, "  (void)(%s); (void)(%s);\n", v.l, v.r);
      } else {
        const char* v = emit_expr(c, top, s);
        badd(c, &body, "  (void)(%s);\n", v);
      }
    }
    if (s->kind != nkDef && s->kind != nkImport) {
      const char* loc = (s->source[0] && !eq(s->source, c->patch_path))
                          ? F(c, "%s %d", s->source, s->line) : F(c, "line %d", s->line);
      if (prof_names.n < MAX_PROF_SLOTS) spush(c, &prof_names, F(c, "%s (%s)", stmt_scope(c, s, i == final_idx), loc));
      badd(c, &body, "  if (s->profiling) { double prof_now = n_now(); s->prof[%d] += prof_now - prof_last; "
           "prof_last = prof_now; }\n", prof_names.n - 1);
    }
  }
  /* The parts the output reaches, by name, and so the ones it doesn't. */
  c->unheard = (Strs){0};
  if (final_idx >= 0 && c->play_names.n > 0) {
    Map plays = {{0}, 0}, reached = {{0}, 0};
    for (int i = 0; i < program->nkids; i++)
      if (program->kids[i]->kind == nkPlay) hput(c, &plays, program->kids[i]->str)->node = program->kids[i];
    mention(c, program->kids[final_idx], &plays, &reached);
    for (int i = 0; i < c->play_names.n; i++)
      if (!has(&reached, c->play_names.v[i])) spush(c, &c->unheard, c->play_names.v[i]);
  }
  /* No final expression: a file of plays plays them all. */
  if (final_idx < 0 && c->play_names.n > 0) {
    bcat(c, &body, "  *outL = ");
    for (int i = 0; i < c->play_names.n; i++) badd(c, &body, i ? " + p_%s_l" : "p_%s_l", c->play_names.v[i]);
    bcat(c, &body, ";\n  *outR = ");
    for (int i = 0; i < c->play_names.n; i++) badd(c, &body, i ? " + p_%s_r" : "p_%s_r", c->play_names.v[i]);
    bcat(c, &body, ";\n");
  }
  bcat(c, &body, "}\n\n");
  bcat(c, &body, "const char* const prof_names[] = {");
  for (int i = 0; i < prof_names.n; i++) badd(c, &body, i ? ", \"%s\"" : "\"%s\"", escape_prof(c, prof_names.v[i]));
  if (prof_names.n == 0) bcat(c, &body, "0");
  bcat(c, &body, "};\n");
  badd(c, &body, "const long prof_count = %d;\n\n", prof_names.n);
  bcat(c, &body, BLOCK_TEMPLATE);

  Buf hoistc = {0};
  for (int i = 0; i < c->hoist_decls.n; i++) bcat(c, &hoistc, c->hoist_decls.v[i]);
  bcat(c, &hoistc, "static void hoist_init(VoiceState* s) {\n");
  for (int i = 0; i < c->hoist_inits.n; i++) bcat(c, &hoistc, c->hoist_inits.v[i]);
  bcat(c, &hoistc, "  (void)s;\n}\n\n");

  return F(c, "%s%s%s%s", bstr(&pre), bstr(&c->array_decls), bstr(&hoistc), bstr(&body));
}

/* Each type's base in first-seen order, every region's absolute offset,
 * the placeholders filled in, and the pool the patch needs. */
static const char* finalise_layout(Ctx* c, const char* src) {
  long base = 0;
  long* bases = al(c, (c->type_order.n + 1) * sizeof(long));
  for (int i = 0; i < c->type_order.n; i++) {
    bases[i] = base;
    base += hfind(&c->type_cum_size, c->type_order.v[i])->n;
  }
  if (base + POOL_OVERFLOW_SLOTS > MAX_POOL_SLOTS)
    fail(c, "patch needs %ld MB of state (delays, tapes, reverbs, arrays); a voice may have %ld MB",
         base * 8 / 1000000, MAX_POOL_SLOTS * 8 / 1000000);
  for (int r = 0; r < c->nregions; r++) {
    int t = sfind(&c->type_order, c->regions[r].type_name);
    c->regions[r].offset = bases[t] + c->regions[r].offset;
  }
  Buf out = {0};
  const char* p = src;
  for (;;) {
    const char* q = strstr(p, "__OFF_");
    if (!q) { bcat(c, &out, p); break; }
    bput(c, &out, p, q - p);
    int rid = atoi(q + 6);
    badd(c, &out, "%ld", c->regions[rid].offset);
    p = q + 6 + 6 + 2;                 /* __OFF_ + six digits + __ */
  }
  badd(c, &out, "\nlong voice_pool_slots(void) { return %ld; }\n", base + POOL_OVERFLOW_SLOTS);
  return sdup(c, bstr(&out));
}

/* A default stands on its own: numbers, PI/TAU, operators and calls. */
static void check_default(Ctx* c, Node* def, Node* e) {
  switch (e->kind) {
  case nkNum: break;
  case nkIdent:
    if (!eq(e->str, "PI") && !eq(e->str, "TAU"))
      fail(c, "default for a parameter of %s can't use the name '%s': a default is numbers, PI/TAU, "
           "operators and calls %s", def->str, e->str, err_loc(c, e));
    break;
  case nkCall: case nkBinOp: case nkUnary: case nkIf:
    for (int i = 0; i < e->nkids; i++) if (e->kids[i]) check_default(c, def, e->kids[i]);
    break;
  default:
    fail(c, "default for a parameter of %s must be an expression %s", def->str, err_loc(c, e));
  }
}

/* Builtins' parameters besides the natives', as SPEC names them. */
static const char* builtin_params_of(const char* name) {
  const NativeSpec* s = native(name);
  if (s) return s->params;
  if (eq(name, "phasor")) return "freq";
  if (eq(name, "pow") || eq(name, "min") || eq(name, "max")) return "a,b";
  if (eq(name, "clamp")) return "x,l,h";
  if (eq(name, "array")) return "n,init=0,every=0";
  return NULL;
}

/* A call's arguments in parameter order: positional, then named, then
 * the defaults of the rest (NULL: required). */
static void reorder_args(Ctx* c, Node* n, const char* what, const char** params, Node** defaults, int np) {
  Node** args = al(c, (np + 1) * sizeof(Node*));
  int pos = 0, seen_named = 0;
  for (int i = 0; i < n->nkids; i++) {
    Node* k = n->kids[i];
    if (k->kind == nkNamedArg) {
      seen_named = 1;
      int j = -1;
      for (int q = 0; q < np; q++) if (eq(params[q], k->str)) { j = q; break; }
      if (j < 0) {
        Buf all = {0};
        for (int q = 0; q < np; q++) { if (q) bcat(c, &all, ", "); bcat(c, &all, params[q]); }
        fail(c, "%s has no parameter '%s' (it takes %s) %s", what, k->str, bstr(&all), err_loc(c, k));
      }
      if (args[j]) fail(c, "%s: '%s' given twice %s", what, k->str, err_loc(c, k));
      args[j] = k->kids[0];
    } else {
      if (seen_named) fail(c, "%s: positional arguments go before named ones %s", what, err_loc(c, k));
      if (pos >= np) fail(c, "wrong arg count for %s: expected %d got %d %s", what, np, n->nkids, err_loc(c, n));
      args[pos++] = k;
    }
  }
  for (int i = 0; i < np; i++)
    if (!args[i]) {
      Node* d = defaults ? defaults[i] : NULL;
      if (!d) fail(c, "%s: missing '%s' %s", what, params[i], err_loc(c, n));
      args[i] = d;
    }
  n->kids = args;
  n->nkids = np;
}

typedef struct Shadow { const char* name; struct Shadow* next; } Shadow;
static int shadowed(Shadow* s, const char* name) {
  for (; s; s = s->next) if (eq(s->name, name)) return 1;
  return 0;
}

/* Every call made positional, defs' and builtins' alike, so the rest of
 * codegen sees one argument per parameter. `shadow`: parameter names in
 * scope, which call the parameter rather than a def. */
static void normalize_calls(Ctx* c, Node* n, Shadow* shadow) {
  if (!n) return;
  Shadow* inner = shadow;
  if (n->kind == nkDef || n->kind == nkLambda)
    for (int i = 0; i < n->nparams; i++) {
      Shadow* s = al(c, sizeof(Shadow));
      s->name = n->params[i]; s->next = inner; inner = s;
    }
  for (int i = 0; i < n->nkids; i++) normalize_calls(c, n->kids[i], inner);
  if (n->kind == nkDef && n->defaults)
    for (int i = 0; i < n->nparams; i++) if (n->defaults[i]) check_default(c, n, n->defaults[i]);
  if (n->kind != nkCall || shadowed(shadow, n->str)) return;
  int any_named = 0;
  for (int i = 0; i < n->nkids; i++) if (n->kids[i]->kind == nkNamedArg) any_named = 1;
  Node* def = user_def(c, n->str);
  if (!def) {
    const char* known = builtin_params_of(n->str);
    int nk = 0;
    if (known) { nk = 1; for (const char* p = known; *p; p++) if (*p == ',') nk++; }
    if (!any_named && (!known || n->nkids >= nk)) return;
    if (!known) fail(c, "%s takes its arguments by position %s", n->str, err_loc(c, n));
    const char** params = al(c, nk * sizeof(char*));
    Node** defaults = al(c, nk * sizeof(Node*));
    const char* p = known;
    for (int i = 0; i < nk; i++) {
      const char* e = strchr(p, ',');
      int len = e ? (int)(e - p) : (int)strlen(p);
      const char* item = F(c, "%.*s", len, p);
      const char* eqs = strchr(item, '=');
      if (eqs) {
        params[i] = F(c, "%.*s", (int)(eqs - item), item);
        defaults[i] = mknum(c, strtod(eqs + 1, NULL), 0, "");
      } else {
        params[i] = item;
      }
      p = e ? e + 1 : p + len;
    }
    reorder_args(c, n, n->str, params, defaults, nk);
    return;
  }
  if (!any_named && n->nkids == def->nparams) return;
  reorder_args(c, n, def->str, def->params, def->defaults, def->nparams);
}

int generate_full(Arena* a, Node* program, const char* patch_path, double sr,
                  int hoist_on, Generated* out, char* err, int errlen) {
  Ctx* c = calloc(1, sizeof(Ctx));
  c->a = a; c->err = err; c->errlen = errlen;
  if (setjmp(c->jump)) { free(c); return -1; }
  new_ctx(c, program);
  c->hoist_on = hoist_on;
  c->patch_path = patch_path ? patch_path : "";
  c->sr = sr;
  c->scope = "";
  normalize_calls(c, program, NULL);
  mark_recursion(c);
  const char* src = emit_program(c, program);
  src = finalise_layout(c, src);
  out->csrc = src;
  out->var_names = c->top_var_names.v; out->nvars = c->top_var_names.n;
  out->part_names = c->play_names.v; out->nparts = c->play_names.n;
  out->unheard = c->unheard.v; out->nunheard = c->unheard.n;
  out->regions = c->regions; out->nregions = c->nregions;
  free(c);
  return 0;
}
