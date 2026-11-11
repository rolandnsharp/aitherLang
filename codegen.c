/* aither to C (codegen.h). C promises no order for evaluating a
 * call's arguments, so every call that numbers a temporary or registers
 * a region is sequenced into its own statement: the C written for a
 * patch is the same on every build. */

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codegen.h"
#include "pool.h"
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
  const char* c2;     /* stereo, a pair of pairs (n == 4): its second half */
  const char* d2;
  long n;             /* arrays, larrays, sarrays: length; counters */
  long wo;            /* sarrays: where stores go */
  int wd;             /* sarrays, larrays: numbers an element (0: one); xbufs: rows */
  int edged;          /* larrays made by array(): 0 past the ends, not cycling */
  int lad;            /* stereo, sarrays, larrays: values on the ladder, (a, b) */
  double v;           /* numlits */
  Node* node;         /* lambdas, invs; userDefs; arrays: the table's literal */
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
  int isolated;    /* a def's root: sees its arguments, not the file's lets */
};

#define NBUCKETS 1024
typedef struct { Ent* b[NBUCKETS]; int n; } Map;

typedef struct { char* s; size_t n, cap; } Buf;
typedef struct { const char** v; int n, cap; } Strs;

/* A unit: a top-level statement, or one statement of a play (`last`: the
 * play's final one, which gives its sound). What it reads and writes of
 * the file's shared names. */
typedef struct { Node* st; Node* play; int last; } Unit;
typedef struct { Map reads, writes; } Use;

struct Ctx {
  Arena* a;
  jmp_buf jump;
  char* err;
  int errlen;
  Map user_defs;
  Strs top_var_names;
  Map top_var_set, top_lets, lets_done, let_lines;
  Scope* top;               /* the file's scope: its constants */
  Strs play_names, unheard, apart;
  int hoist_on;
  Strs hoist_decls, hoist_inits;
  Map hoist_seen;
  int in_hoist;
  Map assigned, play_set, stereo_lets, quad_lets;
  Map turners;              /* "<scope>/<let>" -> a = cos side, b = sin side */
  Node* blk; int blk_i;     /* the block being emitted, and the statement */
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
  Scope* hoisted;      /* hoisted invariants' names, for spelled expressions */
  int loop_nv;         /* inside a looped sum (poly's voices): its count; else 0 */
  Map pair_vars;
  Map quad_vars;                       /* file-level $vars that are pairs of pairs (in pair_vars too) */
  Map lad_vars, lad_lets;              /* file-level $vars and lets that are values on the ladder */
  Map const_quads;                     /* file-level lets of a pair of pairs of number literals: node, the literal */
  Node** var_decls; int nvar_decls, cap_var_decls;
  Map pair_param_cache;     /* p = int* flags, n = count */
  Map top_sarrays, top_larrays, top_sarray_names;
  Map file_vars;            /* every file-level `$x`, known from the start */
  int each_depth;
  Buf deferred;
  Map recursive, tail_checked, stereo_seen, pure_seen;
  /* Tasks: the units, each one's task, and the buffers between tasks. */
  Unit* units; Use* use; int nunits;
  int* unit_task; int ntasks;
  int* fine_task; int nfine; /* the finest split, before clustering: what `aither profile` predicts from */
  Strs glue;                /* why parts were kept in one task, for `check` */
  double* ucost;            /* each unit's estimated cost */
  Map unit_costs;           /* def name -> its body's estimated cost (v) */
  int cur_unit;             /* the unit being emitted; -1 outside them */
  Map writers;              /* shared name -> p = int[] writer units, n = how many */
  Map xbufs;                /* "name#writer unit" -> n = first buffer (-1: not yet), pairs */
  int nbufs;
  int nplays;
  Scope* play_scope;        /* the play being emitted: its own lets' scope */
  const char* play_name;
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

static const char* const LIBM1[] = {"sin", "cos", "tan", "tanh", "exp", "log",
                                    "floor", "ceil", "sqrt", "erf", NULL};

/* The two entry points, after every patch's tick1. */
/* The drivers. Both run the tasks (run_tasks) over a block of
 * buffers: tick one sample (render, try, the tests), tick_block n of
 * them in runs of AITHER_MAXN (the engine). Before the tasks: the clock,
 * the input, each part's gain and park flag a sample at a time, the
 * gain ramps stepped as they always were; after: the guard, the voice's
 * fade and the meters, in the order they were. */
static const char* BLOCK_TEMPLATE =
"extern double n_input_l(void);\n"
"extern double n_input_r(void);\n"
"\n"
"void tick(VoiceState* s, double* outL, double* outR) {\n"
"  enum { S = 2 };\n"
"  double B[AITHER_NB * S];\n"
"  hoist_init(s);\n"
"  s->in_l = n_input_l(); s->in_r = n_input_r();\n"
"  B[XB_T * S] = s->step; B[XB_IN_L * S] = s->in_l; B[XB_IN_R * S] = s->in_r;\n"
"  for (long p = 0; p < AITHER_NP; p++) {\n"
"    B[(XB_GAIN + p) * S] = s->part_gains[p];\n"
"    B[(XB_PARK + p) * S] = s->part_park ? s->part_park[p] : 0.0;\n"
"  }\n"
"  if (s->profiling) s->prof_ticks++;\n"
"  run_tasks(s, 1, S, B);\n"
"  outs(S, B, 0, outL, outR);\n"
"  if (s->part_out)\n"
"    for (long p = 0; p < 2 * AITHER_NP; p++) s->part_out[p] = B[(XB_PART + p) * S];\n"
"}\n"
"\n"
"void tick_block(VoiceState* s, long n, double* outL, double* outR) {\n"
"  const long np = s->n_parts;\n"
"  enum { S = AITHER_MAXN + 1 };\n"
"  double B[AITHER_NB * S];\n"
"  hoist_init(s);\n"
"  s->bad = 0; s->peak_raw = 0.0; s->stuck = 0;\n"
"  for (long b0 = 0; b0 < n; b0 += AITHER_MAXN) {\n"
"    const long m = n - b0 < AITHER_MAXN ? n - b0 : AITHER_MAXN;\n"
"    for (long k = 0; k < m; k++) {\n"
"      if (s->in_buf) { s->in_l = s->in_buf[2*(b0 + k)]; s->in_r = s->in_buf[2*(b0 + k) + 1]; }\n"
"      else { s->in_l = 0.0; s->in_r = 0.0; }\n"
"      B[XB_T * S + k] = s->step; s->step += 1.0;\n"
"      B[XB_IN_L * S + k] = s->in_l; B[XB_IN_R * S + k] = s->in_r;\n"
"      for (long p = 0; p < AITHER_NP; p++) {\n"
"        B[(XB_GAIN + p) * S + k] = s->part_gains[p];\n"
"        B[(XB_PARK + p) * S + k] = s->part_park ? s->part_park[p] : 0.0;\n"
"      }\n"
"      /* the parts' gain ramps: for the next sample */\n"
"      for (long p = 0; p < np; p++) {\n"
"        double d = s->part_fade_delta[p];\n"
"        if (d != 0.0) {\n"
"          double target = s->part_fade_target[p];\n"
"          double g = s->part_gains[p] + d;\n"
"          if ((d > 0.0 && g >= target) || (d < 0.0 && g <= target)) {\n"
"            s->part_gains[p] = target; s->part_fade_delta[p] = 0.0;\n"
"            if (s->part_park_pending[p] && target <= 0.0) {\n"
"              s->part_park[p] = 1.0; s->part_park_pending[p] = 0;\n"
"            }\n"
"          } else {\n"
"            s->part_gains[p] = g < 0.0 ? 0.0 : (g > 1.0 ? 1.0 : g);\n"
"          }\n"
"        }\n"
"      }\n"
"    }\n"
"    if (s->profiling) s->prof_ticks += m;\n"
"    if (AITHER_NT < 2 || !s->par ||\n"
"        !s->par(s, AITHER_NT, aither_task_fns, aither_task_waits, aither_task_feeds_at, aither_task_feeds, m, S, B))\n"
"      run_tasks(s, m, S, B);\n"
"    for (long k = 0; k < m; k++) {\n"
"      double l, r;\n"
"      outs(S, B, k, &l, &r);\n"
"      if (s->raw_out) { s->raw_out[2*(b0 + k)] = l; s->raw_out[2*(b0 + k) + 1] = r; }\n"
"      if (s->part_buf)\n"
"        for (long p = 0; p < 2 * np; p++) s->part_buf[(b0 + k) * 2 * np + p] = B[(XB_PART + p) * S + k];\n"
"      if (!(l == l) || !(r == r) || l > 1e6 || l < -1e6 || r > 1e6 || r < -1e6) {\n"
"        s->bad++; l = 0.0; r = 0.0;\n"
"      }\n"
"      double al = fabs(l), ar = fabs(r);\n"
"      if (al > s->peak_raw) s->peak_raw = al;\n"
"      if (ar > s->peak_raw) s->peak_raw = ar;\n"
"      /* the voice's fade: for the next sample */\n"
"      s->fade_gain += s->fade_delta;\n"
"      if (s->fade_gain > 1.0) s->fade_gain = 1.0;\n"
"      if (s->fade_gain < 0.0) s->fade_gain = 0.0;\n"
"      if (s->fade_gain >= 1.0 && s->fade_delta > 0.0) s->fade_delta = 0.0;\n"
"      const double g = s->fade_gain;\n"
"      outL[b0 + k] = l * g; outR[b0 + k] = r * g;\n"
"      /* per-part meters, after the voice fade like the voice's own */\n"
"      for (long p = 0; p < np; p++) {\n"
"        double a0 = fabs(B[(XB_PART + 2*p) * S + k]), a1 = fabs(B[(XB_PART + 2*p + 1) * S + k]);\n"
"        double a = (a0 > a1 ? a0 : a1) * g;\n"
"        double pk = s->part_peak[p] * s->peak_decay;\n"
"        s->part_peak[p] = pk > a ? pk : a;\n"
"        s->part_sq[p] = s->part_sq[p] * (1.0 - s->rms_alpha) + a * a * s->rms_alpha;\n"
"      }\n"
"    }\n"
"    if (s->part_out && m > 0)\n"
"      for (long p = 0; p < 2 * AITHER_NP; p++) s->part_out[p] = B[(XB_PART + p) * S + m - 1];\n"
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

/* Whether sc is inside a def's body: its chain ends at the def's root,
 * not at the file. A lambda's body is not: it runs in the scope that
 * wrote it, wherever it is called. */
static int in_def(Scope* sc) {
  while (sc && sc->parent) sc = sc->parent;
  return sc && sc->isolated;
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
  return hfind(&c->top_arrays, name);           /* a constant: seen everywhere */
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
  return in_def(sc) ? NULL : hfind(&c->top_larrays, name);
}

/* A compile-time number: a literal, a name bound to one, len(array), or
 * arithmetic over those. */
/* How far a push goes in a field's step, in places, at its period: a
 * quarter, so a knob can take it four times faster before the step goes
 * unstable at one place a step. A ring of period T has sqrt(this T sr)
 * places. */
#define FIELD_STEP 0.25

static int const_value(Ctx* c, Scope* sc, Node* n, double* v) {
  if (!n) return 0;
  switch (n->kind) {
  case nkNum: *v = n->num; return 1;
  case nkIdent: {
    if (!sc) return 0;
    if (lookup_num_lit(sc, n->str, v)) return 1;
    /* A def sees the file's constants: a name the def doesn't bind is
     * the file's, as emit_ident reads it. */
    if (!in_def(sc) || !c->top) return 0;
    for (Scope* s = sc; s; s = s->parent)
      if (binds_any(s, n->str)) return 0;
    Ent* e = lfind(c->top->numlits, n->str);
    if (!e) return 0;
    *v = e->v;
    return 1;
  }
  case nkCall:
    if (eq(n->str, "field_places") && n->nkids == 1) {   /* the engine's choice for a period, at this rate */
      double period;
      if (!const_value(c, sc, n->kids[0], &period)) return 0;
      *v = fmax(4.0, floor(sqrt(FIELD_STEP * period * c->sr) + 0.5));
      return 1;
    }
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
    /* as emit_binop computes them */
    if (eq(n->str, "^")) {
      long k = (long)b;
      if ((double)k == b && k >= 0 && k <= 4) { double r = 1.0; if (k) { r = a; for (long j = 1; j < k; j++) r = r * a; } *v = r; return 1; }
      double r = pow(a, b);
      if (!isfinite(r)) return 0;
      *v = r; return 1;
    }
    if (eq(n->str, "mod")) { *v = b == 0.0 ? 0.0 : a - floor(a / b) * b; return 1; }
    if (eq(n->str, "==")) { *v = a == b; return 1; }
    if (eq(n->str, "!=")) { *v = a != b; return 1; }
    if (eq(n->str, "<")) { *v = a < b; return 1; }
    if (eq(n->str, ">")) { *v = a > b; return 1; }
    if (eq(n->str, "<=")) { *v = a <= b; return 1; }
    if (eq(n->str, ">=")) { *v = a >= b; return 1; }
    if (eq(n->str, "and")) { *v = (a != 0.0) & (b != 0.0); return 1; }
    if (eq(n->str, "or")) { *v = (a != 0.0) | (b != 0.0); return 1; }
    return 0;
  }
  case nkUnary:
    if (eq(n->str, "-")) {
      double a;
      if (const_value(c, sc, n->kids[0], &a)) { *v = -a; return 1; }
    }
    return 0;
  case nkIdx: {
    /* A constant table at a compile-time index, wrapped as the emitted
     * read wraps it. */
    double ix;
    if (n->kids[0]->kind != nkIdent || !const_value(c, sc, n->kids[1], &ix)) return 0;
    Ent* e = lookup_array(c, sc, n->kids[0]->str);
    if (!e || !e->node || e->n <= 0) return 0;
    long i = (long)ix % e->n;
    if (i < 0) i += e->n;
    return const_value(c, sc, e->node->kids[i], v);
  }
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

static Ent* lookup_lambda_ent(Scope* sc, const char* name) {
  for (Scope* s = sc; s; s = s->parent) {
    Ent* e = lfind(s->lambdas, name);
    if (e) return e;
  }
  return NULL;
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

static const char* const BUILTIN_FNS[] = {
  "saw", "tri", "sqr", "phasor", "noise", "abs", "pow",
  "midi_cc", "midi_note", "midi_freq", "midi_gate",
  "midi_trig", "midi_voice_freq", "midi_voice_gate",
  "midi_voice_vel", "midi_voice_key", "midi_vel",
  "midi_voice_cc", "midi_voice_press", "midi_voice_bend", "midi_voice_on",
  "amplitude", "phase", "axis", NULL};

/* Pair-returning builtins, and the reductions of a pair to a number. */
static const char* const PAIR_CALLS[] = {"analytic", "conj", "input", NULL};
static const char* const PAIR_READERS[] = {"amplitude", "phase", NULL};

/* Whether argument j of a pair builtin (called with n) is a pair. */
static int takes_pair_at(const char* name, int j, int n) {
  if (eq(name, "analytic") || eq(name, "input")) return 0;
  return 1;
}

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
  if (has(&c->top_var_set, name) || has(&c->file_vars, name)) return 1;
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


static const char* pair_var_field(Ctx* c, const char* name, int k) { return F(c, "%s__%d", name, k); }
static const char* pair_var_ref(Ctx* c, const char* name, int k) { return F(c, "s->v_%s__%d", name, k); }

/* Which params a def's body reads as a pair. Its own `$x`s and lets are
 * followed too: one read as a pair makes what it is made of a pair, its
 * first value and what is put in it, so `$z = start` then `rotate($z,
 * ..)` makes start a pair. */
static int* pair_params(Ctx* c, Node* def);

typedef struct {
  Node* def;
  int n;              /* params, then locals */
  const char** names;
} Pp;

static int pp_index(Pp* pp, const char* name) {
  for (int i = 0; i < pp->n; i++) if (eq(pp->names[i], name)) return i;
  return -1;
}

static int pp_is_pair(int* res, Pp* pp, Node* n) {
  if (!n) return 0;
  switch (n->kind) {
  case nkIdent: { int k = pp_index(pp, n->str); return k >= 0 && res[k]; }
  case nkArr: return n->nkids == 2;
  case nkCall: return in(n->str, PAIR_CALLS);
  case nkBinOp:
    if (!(eq(n->str, "+") || eq(n->str, "-") || eq(n->str, "*") || eq(n->str, "/") || eq(n->str, "^"))) return 0;
    for (int i = 0; i < n->nkids; i++) if (pp_is_pair(res, pp, n->kids[i])) return 1;
    return 0;
  case nkUnary: return eq(n->str, "-") && pp_is_pair(res, pp, n->kids[0]);
  default: return 0;
  }
}

/* Every parameter met through arithmetic in n (+ - * / and negation,
 * however nested): in `left + right - me * 2`, left and right meet the
 * pair me * 2 too. A pair param given a number fills both sides, which is
 * what broadcasting the number would have done, so marking is safe. */
static void pp_mark_arith(int* res, Pp* pp, Node* n) {
  if (!n) return;
  if (n->kind == nkIdent) { int k = pp_index(pp, n->str); if (k >= 0) res[k] = 1; return; }
  if ((n->kind == nkBinOp && (eq(n->str, "+") || eq(n->str, "-") || eq(n->str, "*") || eq(n->str, "/") || eq(n->str, "^"))) ||
      (n->kind == nkUnary && eq(n->str, "-")))
    for (int i = 0; i < n->nkids; i++) pp_mark_arith(res, pp, n->kids[i]);
}

static void pp_mark(Ctx* c, int* res, Pp* pp, Node* n) {
  if (!n) return;
  Node* def = pp->def;
  switch (n->kind) {
  case nkIdx: {
    Node* a = n->kids[0];
    Node* i = n->kids[1];
    if (a->kind == nkIdent && i->kind == nkNum && ((int)i->num == 0 || (int)i->num == 1)) {
      int k = pp_index(pp, a->str);
      if (k >= 0) res[k] = 1;
    }
    break;
  }
  case nkBinOp:
    if (eq(n->str, "+") || eq(n->str, "-") || eq(n->str, "*") || eq(n->str, "/") || eq(n->str, "^")) {
      int any = 0;
      for (int i = 0; i < n->nkids; i++) if (pp_is_pair(res, pp, n->kids[i])) any = 1;
      if (any) pp_mark_arith(res, pp, n);
    }
    break;
  case nkVar: case nkLet: case nkAssign: {
    /* a local read as a pair: what it is made of is one */
    int k = pp_index(pp, n->str);
    if (k >= def->nparams && n->nkids) {
      if (res[k]) pp_mark_arith(res, pp, n->kids[0]);
      else if (pp_is_pair(res, pp, n->kids[0])) res[k] = 1;
    }
    break;
  }
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
      if (eq(n->str, def->str) && j < def->nparams && pp_is_pair(res, pp, arg)) res[j] = 1;
      int wants = ((in(n->str, PAIR_CALLS) || in(n->str, PAIR_READERS)) && takes_pair_at(n->str, j, n->nkids)) ||
                  (j < ncallee && callee[j]);
      if (arg->kind != nkIdent) { if (wants) pp_mark_arith(res, pp, arg); continue; }
      int k = pp_index(pp, arg->str);
      if (k >= 0 && wants) res[k] = 1;
    }
    break;
  }
  default: break;
  }
  for (int i = 0; i < n->nkids; i++) pp_mark(c, res, pp, n->kids[i]);
}

static void pp_locals(Ctx* c, Pp* pp, Node* n, int* cap) {
  if (!n || n->kind == nkLambda || n->kind == nkDef) return;
  if ((n->kind == nkVar || n->kind == nkLet) && pp_index(pp, n->str) < 0) {
    if (pp->n == *cap) {
      *cap *= 2;
      const char** nm = al(c, *cap * sizeof(char*));
      memcpy(nm, pp->names, pp->n * sizeof(char*));
      pp->names = nm;
    }
    pp->names[pp->n++] = n->str;
  }
  for (int i = 0; i < n->nkids; i++) pp_locals(c, pp, n->kids[i], cap);
}

static int* pair_params(Ctx* c, Node* def) {
  Ent* e = hfind(&c->pair_param_cache, def->str);
  if (e) return e->p;
  int np = def->nparams;
  Pp pp = {def, np, NULL};
  int cap = np + 8;
  pp.names = al(c, cap * sizeof(char*));
  for (int i = 0; i < np; i++) pp.names[i] = def->params[i];
  pp_locals(c, &pp, def->kids[0], &cap);
  int n = pp.n;
  int* res = al(c, (n ? n : 1) * sizeof(int));
  hput(c, &c->pair_param_cache, def->str)->p = res;       /* against call cycles */
  int* before = al(c, (n ? n : 1) * sizeof(int));
  for (;;) {
    memcpy(before, res, n * sizeof(int));
    pp_mark(c, res, &pp, def->kids[0]);
    if (!memcmp(before, res, n * sizeof(int))) break;
  }
  return res;
}

/* ------------------------------------------------- expression codegen
 *
 * Every value is a pair, its two sides: `l` and `r`, each a pure C
 * expression (no side effects: whatever has state is a statement in `pre`
 * that leaves its result in a temporary, so a side may be read twice).
 * A number is a pair whose sides are known equal (width 1): it is
 * computed once. A function given a pair where it takes a number runs
 * once per side, each side with its own state; a def's own pair
 * parameters (pair_params) take the pair whole. Only `if` is lazy: a
 * branch's statements run only when it is taken. */

typedef struct {
  int w;                                 /* how many numbers: 1, 2 or 4 */
  union {
    const char* x[4];                    /* each number's C text */
    struct { const char *l, *r, *l2, *r2; };
  };
  int lad;                               /* written (a, b): on the ladder (notes/ladder-2026-11-06.md).
                                            Until [a, b] becomes places, the old pair sits beside it */
} V;
static int is_pair(V v) { return v.w > 1; }
static int is_quad(V v) { return v.w == 4; }

static V mono(const char* s) { return (V){1, {{s, s, NULL, NULL}}}; }
static V vpair(const char* l, const char* r) { return (V){2, {{l, r, NULL, NULL}}}; }

/* A pair of pairs (notes/nested-pair-2026-11-05.md): a pair whose sides
 * are pairs, Hamilton's quaternion. Its first half is (l, r), its
 * second (l2, r2); is_pair holds for it too, so code that takes it for a
 * pair must look at is_quad first, and code that doesn't know pairs of
 * pairs refuses them (no_quad) rather than drop a half. */
static V vquad(const char* a, const char* b, const char* c2, const char* d) {
  return (V){4, {{a, b, c2, d}}};
}
/* Half of four numbers: a place of `[z1, z2]`, or a value's half. */
static V half_of(V v, int k) {
  V h = k ? vpair(v.l2, v.r2) : vpair(v.l, v.r);
  h.lad = 1;
  return h;
}

/* Where a pair of pairs isn't handled yet the compiler says so, and says
 * the rule is a stand-in: these are new, and what they should do in each
 * place is learnt from patches that use them. */
#define QUAD_STANDIN "This is a stand-in, not a decision: if you expected something here, say what, and " \
  "the rule can change (notes/places-and-values-2026-11-06.md)."
/* ---- shapes in errors: what a value is, and what it was written as ---- */

static const char* shape_of(V v) {
  if (v.lad) return is_quad(v) ? "one of Hamilton's numbers" : "a complex number";
  return is_quad(v) ? "two places of complex numbers" : is_pair(v) ? "two places" : "a number";
}

/* An expression as the patch wrote it, near enough to recognise (its
 * spacing normalised, the inside of a long one elided). */
static void text_of(Ctx* c, Buf* b, Node* n, int depth) {
  if (!n) return;
  if (depth > 4 || b->n > 60) { bcat(c, b, "..."); return; }
  switch (n->kind) {
  case nkNum: badd(c, b, "%.15g", n->num); return;
  case nkIdent: bcat(c, b, n->str); return;
  case nkStr: badd(c, b, "\"%s\"", n->str); return;
  case nkUnary: badd(c, b, "%s", eq(n->str, "-") ? "-" : F(c, "%s ", n->str)); text_of(c, b, n->kids[0], depth + 1); return;
  case nkBinOp:
    for (int i = 0; i < 2; i++) {
      int paren = n->kids[i] && n->kids[i]->kind == nkBinOp;
      if (i) badd(c, b, " %s ", n->str);
      if (paren) bcat(c, b, "(");
      text_of(c, b, n->kids[i], depth + 1);
      if (paren) bcat(c, b, ")");
    }
    return;
  case nkCall: case nkArr: {
    int round = n->kind == nkCall || n->str[0] == '(';
    if (n->kind == nkCall) bcat(c, b, n->str);
    bcat(c, b, round ? "(" : "[");
    for (int i = 0; i < n->nkids; i++) { if (i) bcat(c, b, ", "); text_of(c, b, n->kids[i], depth + 1); }
    bcat(c, b, round ? ")" : "]");
    return;
  }
  case nkIdx: text_of(c, b, n->kids[0], depth + 1); bcat(c, b, "["); text_of(c, b, n->kids[1], depth + 1); bcat(c, b, "]"); return;
  case nkNamedArg: badd(c, b, "%s = ", n->str); text_of(c, b, n->kids[0], depth + 1); return;
  case nkIf: bcat(c, b, "if "); text_of(c, b, n->kids[0], depth + 1); bcat(c, b, " then ..."); return;
  case nkLambda: badd(c, b, "%s => ...", n->nparams ? n->params[0] : "_"); return;
  default: bcat(c, b, "..."); return;
  }
}
static const char* written(Ctx* c, Node* n) {
  Buf b = {0};
  text_of(c, &b, n, 0);
  return b.n ? bstr(&b) : "it";
}
/* `x`, a pair: a value as written, and its shape. */
static const char* given(Ctx* c, Node* n, V v) { return F(c, "`%s`, %s", written(c, n), shape_of(v)); }
/* What a call was given, argument by argument. */
static const char* givens(Ctx* c, Node** kids, V* a, int n) {
  Buf b = {0};
  for (int i = 0; i < n; i++) badd(c, &b, "%s%s", i ? (i == n - 1 ? ", and " : "; ") : "", given(c, kids[i], a[i]));
  return bstr(&b);
}

static void no_quad(Ctx* c, V v, const char* what, Node* at) {
  if (is_quad(v))
    fail(c, "%s doesn't take four numbers yet, and is given %s %s. " QUAD_STANDIN, what,
         at ? given(c, at, v) : "one", at ? err_loc(c, at) : "");
}
/* Number k of a value: a number is every one. */
static const char* at(V v, int k) { return v.w == 1 ? v.l : v.x[k]; }
/* An array's element width, or a crossing's rows: 1, 2 or 4. */
static int ew(Ent* e) { return e->wd > 1 ? e->wd : 1; }
/* A value from its w numbers. */
static V vwide(int w, const char** x) {
  return w == 4 ? vquad(x[0], x[1], x[2], x[3]) : w == 2 ? vpair(x[0], x[1]) : mono(x[0]);
}
/* An array's element, from its w numbers. */
static V elem_value(Ent* e, int w, const char** x) {
  V v = vwide(w, x);
  v.lad = e->lad && w > 1;
  return v;
}
/* The same, as `proto` is: on the ladder or not. */
static V vlike(V proto, int w, const char** x) {
  V r = vwide(w, x);
  r.lad = proto.lad && w > 1;
  return r;
}
/* On the ladder at width w: what is narrower sits inside, the rest zero. */
static V embed(V v, int w) {
  const char* x[4];
  for (int k = 0; k < w; k++) x[k] = k < v.w ? v.x[k] : "0.0";
  V r = vwide(w, x);
  r.lad = w > 1;
  return r;
}
/* Two places (notes/places-and-values-2026-11-06.md): `[a, b]`, width 2,
 * or `[z1, z2]`, width 4, not on the ladder: what is on it is one value. */
static int is_field(V v) { return v.w > 1 && !v.lad; }
/* What is at place k: a value or a number is everywhere. */
static V place(V v, int k) {
  if (!is_field(v)) return v;
  return v.w == 2 ? mono(v.x[k]) : half_of(v, k);
}
/* Two places from what is at each: numbers, or complex numbers. */
static V two_places(Ctx* c, V a, V b, Node* at) {
  if (a.w == 1 && b.w == 1) return vpair(a.l, b.l);
  if (a.w == 4 || b.w == 4)
    fail(c, "two places of Hamilton's numbers aren't built yet %s: nothing has needed them, so if this is what "
         "you mean, say so", at ? err_loc(c, at) : "");
  V p = embed(a, 2), q = embed(b, 2);
  return vquad(p.l, p.r, q.l, q.r);
}
/* Whether a shape (w numbers, on the ladder or not) can hold v: a number or
 * a value goes everywhere, and the narrower sits inside. */
static int fits(V v, int w, int lad) {
  if (lad) return !is_field(v) && v.w <= w;
  return w == 2 ? v.w == 1 || (is_field(v) && v.w == 2) : v.w <= 2 || is_field(v);
}
/* v in that shape. A number in two places of numbers stays one: `at` reads
 * it at both. */
static V as_shape(Ctx* c, V v, int w, int lad) {
  if (lad) return embed(v, w);
  if (w == 2) return v;
  V p = embed(place(v, 0), 2), q = embed(place(v, 1), 2);
  return vquad(p.l, p.r, q.l, q.r);
}
/* The shape that holds them all, as `if` and `sum` need. */
static void common_shape(Ctx* c, V* v, long n, int* w, int* lad, Node* at) {
  int field = 0, per = 1;
  for (long k = 0; k < n; k++) {
    if (is_field(v[k])) field = 1;
    int here = is_field(v[k]) ? v[k].w / 2 : v[k].w;
    if (here > per) per = here;
  }
  if (field && per == 4) two_places(c, mono(""), vquad("", "", "", ""), at);
  *w = field ? 2 * per : per;
  *lad = !field && per > 1;
}
/* `(a, b)` as written. */
static int is_value_lit(Node* n) { return n->kind == nkArr && n->str && n->str[0] == '('; }
/* `[[a, b], [c, d]]` of number literals. */
static int quad_literal(Node* n) {
  if (n->kind != nkArr || n->nkids != 2) return 0;
  for (int h = 0; h < 2; h++) {
    Node* x = n->kids[h];
    if (x->kind != nkArr || x->nkids != 2 || x->kids[0]->kind != nkNum || x->kids[1]->kind != nkNum) return 0;
  }
  return 1;
}

static V emit(Ctx* c, Scope* sc, Node* n, Buf* pre);
static V emit_block(Ctx* c, Scope* sc, Node* blk, Buf* pre);
static void emit_local_stmt(Ctx* c, Scope* sc, Node* st, Buf* pre);
static const char* emit_idx_assign(Ctx* c, Scope* sc, Node* st);
static void declare_state_array(Ctx* c, Scope* sc, Node* st, Buf* pre, int top_level);
static void declare_local_array(Ctx* c, Scope* sc, const char* name, Node* rhs, Buf* pre, int top_level);

/* A name set, for the recursion checks. */
typedef struct NameSet { const char** v; int n; } NameSet;
static int ns_has(NameSet* s, const char* k) {
  for (int i = 0; i < s->n; i++) if (eq(s->v[i], k)) return 1;
  return 0;
}
static void ns_incl(NameSet* s, const char* k) { if (!ns_has(s, k)) s->v[s->n++] = k; }

/* A text read more than once: a temporary unless it is already one word. */
static int is_simple(const char* s) {
  for (; *s; s++) if (*s == '(' || *s == ' ') return 0;
  return 1;
}
static const char* once(Ctx* c, Buf* pre, const char* s) {
  if (is_simple(s)) return s;
  const char* t = fresh(c, "v");
  badd(c, pre, "  double %s = (%s);\n", t, s);
  return t;
}

/* A value fixed now: each side not already one word into a temporary,
 * so statements that come after (a store into a state array, in a sum
 * or each body) can't change what it reads. */
static V capture(Ctx* c, Buf* pre, V v) {
  const char* x[4];
  for (int k = 0; k < v.w; k++) x[k] = once(c, pre, v.x[k]);
  return vlike(v, v.w, x);
}

/* Operands left to right, as written: when one needs statements, the
 * ones before it are captured first. */
static void emit_seq(Ctx* c, Scope* sc, Node** kids, int n, V* out, Buf* pre) {
  for (int i = 0; i < n; i++) {
    Buf own = {0};
    out[i] = emit(c, sc, kids[i], &own);
    if (own.n) for (int k = 0; k < i; k++) out[k] = capture(c, pre, out[k]);
    bcat(c, pre, bstr(&own));
  }
}

/* `n` where one number is needed. */
static const char* emit1(Ctx* c, Scope* sc, Node* n, Buf* pre, const char* what) {
  V v = emit(c, sc, n, pre);
  if (is_quad(v))
    fail(c, "%s needs one number but gets %s, `%s` — name the number it means %s", what, shape_of(v), written(c, n), err_loc(c, n));
  if (is_pair(v))
    fail(c, "%s needs one number but gets %s, `%s` — name the part it means: %s %s", what, shape_of(v), written(c, n),
         v.lad ? "let (a, b) = ..." : "let [l, r] = ...", err_loc(c, n));
  return v.l;
}

/* ---- hoisting: what can't change within a block runs once a block ---- */

static const char* const MIDI_HELD[] = {"midi_cc", "midi_note", "midi_freq", "midi_gate", "midi_vel",
                                        "midi_voice_freq", "midi_voice_gate", "midi_voice_vel",
                                        "midi_voice_key", "midi_voice_cc", "midi_voice_press",
                                        "midi_voice_bend", "midi_voice_on", NULL};
static const char* const PURE_MATH[] = {"sin", "cos", "tan", "tanh", "exp", "log",
                                        "floor", "ceil", "sqrt", "pow", "min", "max", "clamp",
                                        "abs", "int", "erf", NULL};

static int is_invariant(Ctx* c, Scope* sc, Node* n);

/* A call to a def that is one expression of its parameters: not `rec`,
 * not shadowed by a parameter or lambda of that name, given every
 * argument. A def sees only its arguments, so such a def is invariant
 * exactly when its arguments are and its expression is over them
 * (`def bandf(i): 50 * pow(2, i / 8)` with i a number), and the hoist
 * computes it once a block like any other maths. NULL otherwise. */
/* The pure defs being read through right now, innermost last: a def
 * already among them is a cycle (ping -> pong -> ping), which is an
 * error the emitter reports, so it isn't pure here. */
static Node* pure_path[64];
static int pure_depth;

static Node* pure_def(Ctx* c, Scope* sc, Node* n) {
  if (!n || n->kind != nkCall || lookup_fn(sc, n->str)[0] || lookup_lambda(sc, n->str)) return NULL;
  Node* d = user_def(c, n->str);
  if (!d || d->is_rec || !d->kids || !d->kids[0] || d->kids[0]->kind == nkBlock ||
      n->nkids != d->nparams || pure_depth >= 64)
    return NULL;
  for (int i = 0; i < pure_depth; i++) if (pure_path[i] == d) return NULL;
  return d;
}

/* The scope a pure def's expression is read in: each parameter bound to
 * its argument, a compile-time number as that number, anything else as
 * the invariant expression it is in the caller's scope. NULL when an
 * argument isn't invariant. */
static Scope* def_arg_scope(Ctx* c, Scope* sc, Node* call, Node* d) {
  Scope* ds = push(c, NULL);
  for (int i = 0; i < d->nparams; i++) {
    Node* arg = call->kids[i];
    double v;
    if (const_value(c, sc, arg, &v)) { lput(c, &ds->numlits, d->params[i])->v = v; continue; }
    if (!is_invariant(c, sc, arg)) return NULL;
    Ent* e = lput(c, &ds->invs, d->params[i]);
    e->node = arg;
    e->sc = sc;
  }
  return ds;
}

static int is_invariant(Ctx* c, Scope* sc, Node* n) {
  if (!n) return 0;
  switch (n->kind) {
  case nkNum: return 1;
  case nkIdent: {
    if (eq(n->str, "PI") || eq(n->str, "TAU") || eq(n->str, "sr") || eq(n->str, "dt")) return 1;
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
  case nkIdx: return is_const(c, sc, n);
  case nkCall:
    /* Maths over invariants, and what MIDI holds: a knob, a note and a
     * voice's note, gate, velocity and key change between blocks as far
     * as a block can tell (the input thread's writes are read once a
     * block; offline, tick reruns hoist_init every sample). midi_trig is
     * not: it consumes its edge. */
    Node* d = sc ? pure_def(c, sc, n) : NULL;
    if (d) {
      Scope* ds = def_arg_scope(c, sc, n, d);
      if (!ds) return 0;
      pure_path[pure_depth++] = d;
      int r = is_invariant(c, ds, d->kids[0]);
      pure_depth--;
      return r;
    }
    if (in(n->str, MIDI_HELD))
      for (int i = 0; i < n->nkids; i++) { if (!is_invariant(c, sc, n->kids[i])) return 0; }
    else if (!in(n->str, PURE_MATH) || n->nkids == 0) return 0;
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

static const char* hoist(Ctx* c, const char* cexpr);
static const char* emit_spelled(Ctx* c, Node* sp, Node* at);

/* A copy of `n` with every name spelled out: a compile-time number as
 * that number, an invariant name as its expression. An invariant name
 * whose expression is more than a number is hoisted once on its own and
 * spelled as that, so a value many hoisted expressions share (a knob's
 * morph, a voice's frequency) is computed once a block and written
 * once, not again inside each: the same arithmetic, so the same bits. */
static Node* spell(Ctx* c, Scope* sc, Node* n) {
  if (!n) return NULL;
  double cv;
  if (n->kind == nkIdx && const_value(c, sc, n, &cv)) return mknum(c, cv, n->line, n->source);
  if (n->kind == nkIdent && !(eq(n->str, "PI") || eq(n->str, "TAU") || eq(n->str, "sr") || eq(n->str, "dt")) && sc) {
    double v;
    if (lookup_num_lit(sc, n->str, &v)) return mknum(c, v, n->line, n->source);
    Ent* inv = lookup_inv(sc, n->str);
    if (inv) {
      Node* body = spell(c, inv->sc, inv->node);
      if (body->kind == nkNum || body->kind == nkIdent) return body;
      const char* cx = emit_spelled(c, body, n);
      if (!cx) return body;                       /* a pair: spelled out, as before */
      const char* h = hoist(c, cx);
      Node* id = mknode(c, nkIdent, n->line, n->source);
      id->str = F(c, "@%s", h);                   /* not a name a patch can write */
      if (!c->hoisted) c->hoisted = push(c, NULL);
      lput(c, &c->hoisted->names, id->str)->a = h;
      return id;
    }
  }
  Node* d = sc ? pure_def(c, sc, n) : NULL;       /* a pure def's call: its expression, spelled out */
  if (d) {
    Scope* ds = def_arg_scope(c, sc, n, d);
    if (ds) {
      pure_path[pure_depth++] = d;
      Node* r = spell(c, ds, d->kids[0]);
      pure_depth--;
      return r;
    }
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

/* An invariant expression as hoist_init computes it: numbers, knobs and
 * pure maths, so one expression and no statements. */
static const char* emit_invariant(Ctx* c, Scope* sc, Node* n) {
  Node* sp = spell(c, sc, n);
  const char* r = emit_spelled(c, sp, n);
  if (!r) fail(c, "internal: an invariant expression needed statements %s", err_loc(c, n));
  return r;
}

/* A spelled expression as one C expression, or NULL for a pair or one
 * that needs statements. */
static const char* emit_spelled(Ctx* c, Node* sp, Node* at) {
  (void)at;
  int was = c->in_hoist;
  c->in_hoist = 1;
  Buf p = {0};
  V v = emit(c, push(c, c->hoisted), sp, &p);
  c->in_hoist = was;
  if (p.n || is_pair(v)) return NULL;
  return v.l;
}

/* A static double computed once from `cexpr`; its name. */
static const char* hoist(Ctx* c, const char* cexpr) {
  Ent* e = hfind(&c->hoist_seen, cexpr);
  if (e) return e->a;
  if (c->loop_nv && strstr(cexpr, "aither_vk")) {
    /* It depends on the voice: one a voice, filled once a block. */
    const char* arr = F(c, "h_%d", c->hoist_decls.n);
    spush(c, &c->hoist_decls, F(c, "AITHER_SHARED double %s[%d];\n", arr, c->loop_nv));
    spush(c, &c->hoist_inits, F(c, "  for (long aither_vk = 0; aither_vk < %d; aither_vk++) %s[aither_vk] = (%s);\n",
                                c->loop_nv, arr, cexpr));
    const char* at = F(c, "%s[aither_vk]", arr);
    hput(c, &c->hoist_seen, cexpr)->a = at;
    return at;
  }
  const char* name = F(c, "h_%d", c->hoist_decls.n);
  spush(c, &c->hoist_decls, F(c, "AITHER_SHARED double %s;\n", name));
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
  if (c->loop_nv && strstr(init_call, "aither_vk")) {
    const char* arr = F(c, "h_%d", c->hoist_decls.n);
    spush(c, &c->hoist_decls, F(c, "AITHER_SHARED double %s[%d][%d];\n", arr, c->loop_nv, n));
    const char* at = F(c, "%s[aither_vk]", arr);
    spush(c, &c->hoist_inits, F(c, "  for (long aither_vk = 0; aither_vk < %d; aither_vk++) %s;\n",
                                c->loop_nv, replace_all(c, init_call, "$H", at)));
    hput(c, &c->hoist_seen, init_call)->a = at;
    return at;
  }
  const char* name = F(c, "h_%d", c->hoist_decls.n);
  spush(c, &c->hoist_decls, F(c, "AITHER_SHARED double %s[%d];\n", name, n));
  spush(c, &c->hoist_inits, F(c, "  %s;\n", replace_all(c, init_call, "$H", name)));
  hput(c, &c->hoist_seen, init_call)->a = name;
  return name;
}

/* ------------------------------------------------- poly's voices as a loop
 * A sum whose index only picks a MIDI voice slot (poly's) is written as
 * one C loop over the voices, not eight copies (notes/loops-2026-10-30.md).
 * The language doesn't change; the C is smaller, so cc -O2 lands sooner.
 * Each voice keeps its own state: voice 1's regions are claimed as the
 * body is emitted, then each later voice's copies in the same order the
 * unrolled sum claimed them, so the layout, a reload's matching and the
 * noise seeds are the unrolled ones; voice k's offset is voice 1's plus k
 * times its type's span. What depends on the voice and is hoisted (a
 * partial's turn) is an array filled once a block. The voices add in the
 * same order, voice 1 first, so the bits are the unrolled sum's. A clock
 * turner's per-block check is one a voice too. AITHER_LOOP=0 unrolls every
 * sum. */

static int loops_off(void) {
  static int off = -1;
  if (off < 0) { const char* e = getenv("AITHER_LOOP"); off = e && eq(e, "0"); }
  return off;
}

static const char* const VOICE_SLOT[] = {"midi_voice_freq", "midi_voice_gate", "midi_voice_vel",
                                         "midi_voice_key", "midi_voice_press", "midi_voice_bend",
                                         "midi_voice_cc", "midi_voice_on", NULL};

/* Is `name` read only as the slot of a voice call (midi_voice_freq(name),
 * midi_voice_cc(name, k))? */
static int only_voice_slot(Node* n, Node* parent, const char* name) {
  if (!n) return 1;
  if (n->kind == nkIdent && eq(n->str, name))
    return parent && parent->kind == nkCall && in(parent->str, VOICE_SLOT) && parent->kids[0] == n &&
           (parent->nkids == 1 || (parent->nkids == 2 && eq(parent->str, "midi_voice_cc")));
  if ((n->kind == nkLet || n->kind == nkVar) && n->str && eq(n->str, name)) return 0;
  if (n->kind == nkLambda)
    for (int i = 0; i < n->nparams; i++) if (eq(n->params[i], name)) return 0;
  for (int i = 0; i < n->nkids; i++) if (!only_voice_slot(n->kids[i], n, name)) return 0;
  return 1;
}

static int can_loop(Ctx* c, Node* body, const char* param, long N) {
  return N >= 2 && !c->loop_nv && !loops_off() && mentions(body, param) && only_voice_slot(body, NULL, param);
}

/* Claim a copy of `src` as the next voice's, as register_region would
 * have for the unrolled copy. */
static int copy_region(Ctx* c, int src_rid) {
  Region src = c->regions[src_rid];
  Ent* tc = hfind(&c->type_counter, src.type_name);
  Ent* ts = hfind(&c->type_cum_size, src.type_name);
  long per_type_idx = tc->n, intra = ts->n;
  tc->n = per_type_idx + 1;
  ts->n = intra + src.size;
  Ent* sk = hput(c, &c->scope_counter, F(c, "%s | %s | %s", src.scope, src.type_name, src.owner));
  long scope_idx = sk->n;
  sk->n = scope_idx + 1;
  if (c->nregions == c->cap_regions) {
    int cap = c->cap_regions ? c->cap_regions * 2 : 64;
    Region* r = al(c, cap * sizeof(Region));
    if (c->nregions) memcpy(r, c->regions, c->nregions * sizeof(Region));
    c->regions = r; c->cap_regions = cap;
  }
  c->regions[c->nregions++] = (Region){src.type_name, (int)per_type_idx, src.scope, (int)scope_idx,
                                       src.owner, intra, src.size};
  return c->nregions - 1;
}

static unsigned noise_seed(Ctx* c, int rid) {
  Region* r = &c->regions[rid];
  unsigned seed = 2166136261u;
  const char* id = F(c, "%s|%s|%d", r->scope, r->owner, r->scope_idx);
  for (const char* p = id; *p; p++) seed = (seed ^ (unsigned char)*p) * 16777619u;
  return seed ? seed : 2463534242u;
}

/* Is this C text a number, nothing more (parentheses around it allowed)? */
static int c_number(const char* t) {
  while (*t == '(' || *t == ' ') t++;
  if (*t == '-') t++;
  if (!(*t >= '0' && *t <= '9')) return 0;
  char* end;
  strtod(t, &end);
  while (*end == ')' || *end == ' ') end++;
  return *end == 0;
}

static V emit_block(Ctx* c, Scope* sc, Node* b, Buf* pre);

static V emit_voice_loop(Ctx* c, Scope* sc, Node* call, Node* body, const char* param, long N, Buf* pre) {
  int r0 = c->nregions;
  const char* idx = "((double)(aither_vk + 1))";
  if (!c->hoisted) c->hoisted = push(c, NULL);
  lput(c, &c->hoisted->names, "@vk")->a = idx;
  Scope* iter = push(c, sc);
  lput(c, &iter->names, param)->a = idx;            /* the slot, at run time */
  Node* vk = mknode(c, nkIdent, call->line, call->source);
  vk->str = "@vk";
  Ent* inv = lput(c, &iter->invs, param);          /* and steady over a block, as a voice's note is */
  inv->node = vk;
  inv->sc = iter;
  c->loop_nv = (int)N;
  int h0 = c->hoist_inits.n;           /* per-block code the body adds (a clock turner's) */
  Buf own = {0};
  V t = body->kind == nkBlock ? emit_block(c, iter, body, &own) : emit(c, iter, body, &own);
  c->loop_nv = 0;
  int r1 = c->nregions, nr = r1 - r0;
  /* Each type's span in one voice: voice k's region is voice 1's plus k spans. */
  const char** types = al(c, (nr + 1) * sizeof(char*));
  long* span = al(c, (nr + 1) * sizeof(long));
  int nt = 0;
  for (int r = r0; r < r1; r++) {
    int k = 0;
    while (k < nt && !eq(types[k], c->regions[r].type_name)) k++;
    if (k == nt) { types[nt] = c->regions[r].type_name; span[nt++] = 0; }
    span[k] += c->regions[r].size;
  }
  int* copies = al(c, ((long)N * nr + 1) * sizeof(int));     /* copies[v * nr + i]: voice v's region i */
  for (int i = 0; i < nr; i++) copies[i] = r0 + i;
  for (long v = 1; v < N; v++)
    for (int i = 0; i < nr; i++) copies[v * nr + i] = copy_region(c, r0 + i);
  no_quad(c, t, "a poly voice (the voice loop)", call);
  const char* code = bstr(&own);
  const char* tl = t.l;
  const char* tr = t.r;
  for (int i = 0; i < nr; i++) {
    int r = r0 + i, k = 0;
    while (!eq(types[k], c->regions[r].type_name)) k++;
    const char* ph = offset_placeholder(c, r);
    const char* at = F(c, "(%s + aither_vk * %ldL)", ph, span[k]);
    code = replace_all(c, code, ph, at);
    tl = replace_all(c, tl, ph, at);
    tr = replace_all(c, tr, ph, at);
    for (int h = h0; h < c->hoist_inits.n; h++)
      if (strstr(c->hoist_inits.v[h], ph)) c->hoist_inits.v[h] = replace_all(c, c->hoist_inits.v[h], ph, at);
    if (eq(c->regions[r].type_name, "noise")) {
      Buf seeds = {0};
      for (long v = 0; v < N; v++) badd(c, &seeds, "%s%uu", v ? ", " : "", noise_seed(c, copies[v * nr + i]));
      const char* arr = F(c, "nzs_%d", r);
      spush(c, &c->hoist_decls, F(c, "static const unsigned int %s[%ld] = {%s};\n", arr, N, bstr(&seeds)));
      code = replace_all(c, code, F(c, "__SEED_%06d__", r), F(c, "%s[aither_vk]", arr));
    }
  }
  const char* al_ = fresh(c, "voices_l");
  const char* ar_ = fresh(c, "voices_r");
  badd(c, pre, "  double %s = 0.0, %s = 0.0;\n  for (long aither_vk = 0; aither_vk < %ld; aither_vk++) {\n%s",
       al_, ar_, N, code);
  if (is_pair(t))
    badd(c, pre, "  if (aither_vk == 0) { %s = (%s); %s = (%s); } else { %s += (%s); %s += (%s); }\n  }\n",
         al_, tl, ar_, tr, al_, tl, ar_, tr);
  else
    badd(c, pre, "  if (aither_vk == 0) %s = (%s); else %s += (%s);\n  }\n", al_, tl, al_, tl);
  return is_pair(t) ? vpair(al_, ar_) : mono(al_);
}

/* A stateful native at its call site's region: its offset passed as the
 * call's second argument (no cursor shared between parts on different
 * cores). Its result in a temporary. */
static const char* native_call(Ctx* c, Buf* pre, const char* off, const char* call) {
  const char* t = fresh(c, "nv");
  badd(c, pre, "  double %s = %s;\n", t, replace_all(c, call, "(DspState*)s", F(c, "(DspState*)s, (long)(%s)", off)));
  return t;
}

/* An argument bound at compile time: a lambda, a function or an array. */
static int bind_by_name(Ctx* c, Scope* sc, Scope* inner, const char* p, Node* a, Buf* preface) {
  if (a->kind == nkLambda) {
    Ent* e = lput(c, &inner->lambdas, p);
    e->node = a;
    e->sc = sc;
    return 1;
  }
  if (a->kind == nkIdent) {
    Ent* lam = lookup_lambda_ent(sc, a->str);
    if (lam) {
      Ent* e = lput(c, &inner->lambdas, p);
      e->node = lam->node;
      e->sc = lam->sc;
      return 1;
    }
  }
  if (a->kind == nkIdent && is_fn_name(c, sc, a->str)) {
    const char* resolved = lookup_fn(sc, a->str)[0] ? lookup_fn(sc, a->str) : a->str;
    lput(c, &inner->fns, p)->a = resolved;
    return 1;
  }
  /* An array by name; two numbers at file level are a pair, a value
   * (indexed, a pair reads as the two-entry table anyway). */
  if (a->kind == nkIdent && !lookup(sc, a->str)[0] && !lookup_stereo(sc, a->str) &&
      !has(&c->stereo_lets, a->str)) {
    Ent* e;
    if ((e = lookup_sarray(c, sc, a->str))) {
      Ent* d = lput(c, &inner->sarrays, p);
      d->a = e->a; d->n = e->n; d->wo = e->wo; d->wd = e->wd; d->lad = e->lad;
      return 1;
    }
    if ((e = lookup_larray(c, sc, a->str))) {
      Ent* d = lput(c, &inner->larrays, p);
      d->a = e->a; d->n = e->n; d->wd = e->wd; d->edged = e->edged; d->lad = e->lad;
      return 1;
    }
    if ((e = lookup_array(c, sc, a->str))) {
      Ent* d = lput(c, &inner->arrays, p);
      d->a = e->a; d->n = e->n; d->node = e->node;
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
      d->a = sym; d->n = a->nkids; d->node = a;
    } else {
      const char* tmp = fresh(c, F(c, "arg_%s", p));
      declare_local_array(c, sc, tmp, a, preface, 0);
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

/* Does `n` index `name` anywhere, directly or through a let that names
 * it again (`let [a, b] = name` indexes its hidden let)? */
static int indexes_in(Node* n, const char* name, Node* body) {
  if (!n) return 0;
  if (n->kind == nkIdx && n->kids[0]->kind == nkIdent && eq(n->kids[0]->str, name)) return 1;
  if (n->kind == nkLet && n->kids[0] && n->kids[0]->kind == nkIdent && eq(n->kids[0]->str, name) &&
      !eq(n->str, name) && indexes_in(body, n->str, body)) return 1;
  for (int i = 0; i < n->nkids; i++) if (indexes_in(n->kids[i], name, body)) return 1;
  return 0;
}
static int indexes(Node* n, const char* name) { return indexes_in(n, name, n); }

/* Which params take a pair whole: those the body reads as a pair, and
 * those it indexes (a pair indexes like a two-entry table). */
static int* takes_pair(Ctx* c, Node* def) {
  int* pp = pair_params(c, def);
  int* r = al(c, (def->nparams ? def->nparams : 1) * sizeof(int));
  for (int i = 0; i < def->nparams; i++) r[i] = pp[i] || indexes(def->kids[0], def->params[i]);
  return r;
}

/* A file-level `$x` that is a pair or a pair of pairs. */
static V file_var(Ctx* c, const char* name) {
  const char* x[4];
  int w = has(&c->quad_vars, name) ? 4 : 2;
  for (int k = 0; k < w; k++) x[k] = pair_var_ref(c, name, k);
  V v = vwide(w, x);
  v.lad = has(&c->lad_vars, name);
  return v;
}

/* A name for a pair or a pair of pairs in a scope, and its value back. */
static void bind_wide(Ctx* c, Scope* sc, const char* name, V v) {
  Ent* e = lput(c, &sc->stereo, name);
  e->a = v.l; e->b = v.r; e->c2 = v.l2; e->d2 = v.r2; e->n = v.w; e->lad = v.lad;
}
static V wide_value(Ent* e) {
  V v = e->n == 4 ? vquad(e->a, e->b, e->c2, e->d2) : vpair(e->a, e->b);
  v.lad = e->lad;
  return v;
}

/* Bind a value to a parameter: a pair as two temporaries, a pair of
 * pairs as four, a number as one, with its compile-time value or
 * invariance noted. */
static void bind_value(Ctx* c, Scope* inner, const char* p, V v, int whole, Node* arg, Scope* sc, Buf* pre) {
  if (is_quad(v) || whole || v.lad) {    /* a value is always taken whole */
    int w = is_quad(v) ? 4 : 2;
    const char* t[4];
    for (int k = 0; k < w; k++) {
      t[k] = fresh(c, F(c, "p_%s_%d", p, k));
      badd(c, pre, "  double %s = (%s);\n", t[k], at(v, k));
    }
    bind_wide(c, inner, p, vlike(v, w, t));
    return;
  }
  const char* t = fresh(c, F(c, "p_%s", p));
  badd(c, pre, "  double %s = (%s);\n", t, v.l);
  lput(c, &inner->names, p)->a = t;
  double cv;
  if (arg && const_value(c, sc, arg, &cv)) lput(c, &inner->numlits, p)->v = cv;
  else if (arg) note_invariant(c, inner, p, arg, sc);
}

/* A def inlined. Given a pair where it takes a number, it runs once per
 * side, each side its own state; else once. */
static V emit_def(Ctx* c, Scope* sc, Node* def, Node* call, Buf* pre) {
  if (call->nkids != def->nparams)
    fail(c, "wrong arg count for %s: expected %d got %d %s", def->str, def->nparams, call->nkids, err_loc(c, call));
  if (sfind(&c->def_stack, def->str) >= 0) cycle_error(c, def, call);
  int* whole = takes_pair(c, def);
  int np = def->nparams;
  Scope* names = push(c, NULL);            /* what binds by name: lambdas, functions, arrays */
  names->isolated = 1;                     /* the def sees its arguments, not the caller's names */
  V* args = al(c, (np + 1) * sizeof(V));
  int* bound = al(c, (np + 1) * sizeof(int));
  int split = 0;
  for (int i = 0; i < np; i++) {
    if (bind_by_name(c, sc, names, def->params[i], call->kids[i], pre)) { bound[i] = 1; continue; }
    Buf own = {0};
    args[i] = emit(c, sc, call->kids[i], &own);
    if (own.n) for (int k = 0; k < i; k++) if (!bound[k]) args[k] = capture(c, pre, args[k]);
    bcat(c, pre, bstr(&own));
    if (is_field(args[i]) && !whole[i]) split = 2;
  }
  Node* body = def->kids[0];
  if (!split) {
    Scope* inner = push(c, names);
    /* Taken whole only when given a pair, or indexed: a number stays one
     * number, and broadcasting does the rest where it meets a pair. */
    for (int i = 0; i < np; i++)
      if (!bound[i])
        bind_value(c, inner, def->params[i], args[i],
                   whole[i] && (is_pair(args[i]) || indexes(body, def->params[i])), call->kids[i], sc, pre);
    spush(c, &c->def_stack, def->str);
    V r = body->kind == nkBlock ? emit_block(c, inner, body, pre) : emit(c, inner, body, pre);
    c->def_stack.n--;
    return r;
  }
  /* Once a place, each run its own state. */
  V res[2];
  for (int k = 0; k < 2; k++) {
    Scope* inner = push(c, names);
    for (int i = 0; i < np; i++) {
      if (bound[i]) continue;
      V a = whole[i] ? args[i] : place(args[i], k);
      bind_value(c, inner, def->params[i], a, whole[i], is_field(args[i]) ? NULL : call->kids[i], sc, pre);
    }
    spush(c, &c->def_stack, def->str);
    V r = body->kind == nkBlock ? emit_block(c, inner, body, pre) : emit(c, inner, body, pre);
    c->def_stack.n--;
    if (is_field(r))
      fail(c, "%s gives %s, and given %s it runs once a place %s: places of places aren't a shape", def->str,
           shape_of(r), givens(c, call->kids, args, np), err_loc(c, call));
    res[k] = r;
  }
  return two_places(c, res[0], res[1], call);
}

/* Operands of a pair operation, flattened to numbers, each a temporary:
 * a pair gives its two sides, a number itself. */
static const char** pair_operands(Ctx* c, Scope* sc, Node* n, Buf* pre, int want, const char* usage) {
  const char** r = al(c, (2 * n->nkids + 1) * sizeof(char*));
  V* got = al(c, (n->nkids + 1) * sizeof(V));
  int k = 0;
  for (int i = 0; i < n->nkids; i++) {
    V v = emit(c, sc, n->kids[i], pre);
    got[i] = v;
    no_quad(c, v, n->str, n->kids[i]);
    const char* tl = fresh(c, F(c, "%s_a", n->str));
    badd(c, pre, "  double %s = (%s);\n", tl, v.l);
    r[k++] = tl;
    if (is_pair(v)) {
      const char* tr = fresh(c, F(c, "%s_a", n->str));
      badd(c, pre, "  double %s = (%s);\n", tr, v.r);
      r[k++] = tr;
    }
  }
  if (k != want)
    fail(c, "%s takes %s, and is given %s: %d%s%s", n->str, usage, n->nkids ? givens(c, n->kids, got, n->nkids) : "nothing",
         k, k == 1 ? " number " : " numbers ", err_loc(c, n));
  return r;
}

/* `$name = rhs` for a pair or a pair of pairs: every number into a temp
 * before any is stored; a number fills them all. */
static const char* assign_wide(Ctx* c, Scope* sc, const char* name, V target, Node* rhs) {
  Buf pre = {0};
  V v = emit(c, sc, rhs, &pre);
  if (!fits(v, target.w, target.lad))
    fail(c, "$%s is %s, and is given %s %s: a state keeps the shape of its first value", name, shape_of(target),
         given(c, rhs, v), err_loc(c, rhs));
  v = as_shape(c, v, target.w, target.lad);
  const char* tmp[4];
  for (int k = 0; k < target.w; k++) {
    tmp[k] = fresh(c, "av");
    badd(c, &pre, "  double %s = (%s);\n", tmp[k], at(v, k));
  }
  for (int k = 0; k < target.w; k++) badd(c, &pre, "  %s = %s;\n", target.x[k], tmp[k]);
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
    if ((pair && pair->lad) || has(&c->lad_vars, base->str))
      fail(c, "$%s is a value, and a value has no places to assign %s: assign the whole, $%s = (a, b)",
           base->str, err_loc(c, st), base->str);
    if ((pair && pair->n == 4) || has(&c->quad_vars, base->str))
      fail(c, "$%s holds two places of values, and one can't be assigned by index yet %s: assign the whole. "
           QUAD_STANDIN, base->str, err_loc(c, st));
    if (pair) target = k == 0 ? pair->a : pair->b;
    else if (has(&c->pair_vars, base->str)) target = pair_var_ref(c, base->str, k);
    if (target[0]) {
      Buf pre = {0};
      const char* rhs = emit1(c, sc, st->kids[2], &pre, F(c, "$%s[%d]", base->str, k));
      badd(c, &pre, "  %s = (%s);\n", target, rhs);
      return sdup(c, bstr(&pre));
    }
  }
  fail(c, "only one of two places can be assigned by index ($s[0] = ... or $s[1] = ...) %s",
       err_loc(c, st));
  return NULL;
}

/* The size of `array(N, init)`: a compile-time integer. */
static int array_make_size(Ctx* c, Scope* sc, Node* call) {
  Node* narg = call->kids[0];
  double v;
  if (!const_value(c, sc, narg, &v)) {
    if (narg->kind == nkCall && eq(narg->str, "field_places"))
      fail(c, "a ring's period has to be known when the patch compiles, as its places are, and is given `%s` %s",
           written(c, narg->kids[0]), err_loc(c, narg));
    fail(c, "array: the size must be a compile-time integer %s", err_loc(c, narg));
  }
  long n = (long)v;
  if ((double)n != v || n < 1 || n > 1000000)
    fail(c, "array: the size must be a whole number from 1 to 1000000, got %s %s",
         nim_float(c, v), err_loc(c, narg));
  return (int)n;
}

/* `$f = array(N, init)`: a state array, a flag then the elements;
 * `every = M` steps it, a second generation landing every M samples. */
/* `array(n, i => rule)`: element k of an array made by its rule, the
 * index bound as `each` binds it (a compile-time number). */
static int is_rule(Node* init) { return init && init->kind == nkLambda; }

static V emit_rule_element(Ctx* c, Scope* sc, Node* rule, long k, Buf* pre) {
  if (rule->nparams != 1)
    fail(c, "array(n, i => ...): the rule takes one parameter, the index %s", err_loc(c, rule));
  Scope* iter = push(c, sc);
  lput(c, &iter->names, rule->params[0])->a = num_lit(c, (double)k);
  lput(c, &iter->numlits, rule->params[0])->v = (double)k;
  Node* b = rule->kids[0];
  if (b->kind != nkBlock) return emit(c, iter, b, pre);
  for (int i = 0; i < b->nkids - 1; i++) emit_local_stmt(c, iter, b->kids[i], pre);
  return emit(c, iter, b->kids[b->nkids - 1], pre);
}

/* Every element of a rule, its code into `pre`; its width in `w`: 1, 2
 * if any is a pair, 4 if any is a pair of pairs (a number fills them). */
static V* emit_rule(Ctx* c, Scope* sc, Node* rule, int n, Buf* pre, int* w, int* lad) {
  V* vs = al(c, (n + 1) * sizeof(V));
  int pair = 0, quad = 0;
  *lad = 0;
  for (int k = 0; k < n; k++) {
    vs[k] = emit_rule_element(c, sc, rule, k, pre);
    if (is_quad(vs[k])) quad = 1; else if (is_pair(vs[k])) pair = 1;
    *lad |= vs[k].lad;
  }
  if (*lad) {
    *w = quad ? 4 : 2;
    for (int k = 0; k < n; k++) vs[k] = embed(vs[k], *w);
    return vs;
  }
  if (quad && pair)
    fail(c, "array: the rule makes elements of two widths %s: an array's elements are one shape. " QUAD_STANDIN,
         err_loc(c, rule));
  *w = quad ? 4 : pair ? 2 : 1;
  return vs;
}

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
  Buf init_pre = {0};
  /* The initial value decides each element's shape: a pair makes an
   * array of pairs (two slots an element), as `$z = [0, 0]` makes pair
   * state. */
  /* A rule is evaluated on the sample the array is born, as a constant
   * init is: a field's starting shape. */
  Node* rule = args.npos == 2 && is_rule(args.pos[1]) ? args.pos[1] : NULL;
  V* rv = NULL;
  int w = 1;
  V iv = mono("0.0");
  int lad = 0;
  if (rule) rv = emit_rule(c, sc, rule, n, &init_pre, &w, &lad);
  else {
    if (args.npos == 2) iv = emit(c, sc, args.pos[1], &init_pre);
    w = iv.w;
    lad = iv.lad;
  }
  const char* off = register_region(c, w == 4 ? "sarr4" : w == 2 ? "sarr2" : "sarr", 1 + gens * n * w, st->str);
  long wo = every > 0 ? (long)n * w : 0;
  const char* base;
  if (top_level) {
    Ent* e = hput(c, &c->top_sarrays, st->str);
    e->a = off; e->n = n; e->wo = wo; e->wd = w; e->lad = lad;
    base = off;
  } else {
    const char* b = fresh(c, "sa");
    badd(c, pre, "  long %s = %s;\n", b, off);
    Ent* e = lput(c, &sc->sarrays, st->str);
    e->a = b; e->n = n; e->wo = wo; e->wd = w; e->lad = lad;
    base = b;
  }
  if (rule) {
    Buf fill = {0};
    for (int g = 0; g < gens; g++)
      for (int k = 0; k < n; k++) {
        long pos = (long)(g * n + k) * w;
        for (int j = 0; j < w; j++) badd(c, &fill, "    s->pool[%s + %ld] = (%s);\n", base, 1 + pos + j, at(rv[k], j));
      }
    badd(c, pre, "  if (s->pool[%s] == 0.0) {\n%s%s    s->pool[%s] = 1.0;\n  }\n", base, bstr(&init_pre), bstr(&fill), base);
  } else {
    Buf first = {0};
    for (int j = 0; j < w; j++) badd(c, &first, "%s(%s)", j ? ", " : "", at(iv, j));
    badd(c, pre, "  if (s->pool[%s] == 0.0) {\n%s    double _q[%d] = {%s};\n"
         "    for (long _k = 0; _k < %d; ++_k) for (int _j = 0; _j < %d; ++_j) s->pool[%s + 1 + %d * _k + _j] = _q[_j];\n"
         "    s->pool[%s] = 1.0;\n  }\n", base, bstr(&init_pre), w, bstr(&first), gens * n, w, base, w, base);
  }
  if (every > 0)
    badd(c, pre, "  if ((long)aither_step %% %ld == 0)\n"
         "    for (long _k = 0; _k < %d; ++_k) s->pool[%s + 1 + _k] = s->pool[%s + 1 + %d + _k];\n",
         every, n * w, base, base, n * w);
}

/* `let a = [e1, ..., eN]` with runtime values, or `let a = array(N, init)`. */
static void declare_local_array(Ctx* c, Scope* sc, const char* name, Node* rhs, Buf* pre, int top_level) {
  const char* sym = fresh(c, F(c, "la_%s", name));
  if (rhs->kind == nkArr) {
    badd(c, pre, "  double %s[%d];\n", sym, rhs->nkids);
    for (int k = 0; k < rhs->nkids; k++) {
      const char* v = emit1(c, sc, rhs->kids[k], pre, F(c, "%s[%d]", name, k));
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
    if (args.npos == 2 && is_rule(args.pos[1])) {
      int w, lad;
      V* rv = emit_rule(c, sc, args.pos[1], n, pre, &w, &lad);
      badd(c, pre, "  double %s[%d];\n", sym, w * n);
      for (int k = 0; k < n; k++)
        for (int j = 0; j < w; j++) badd(c, pre, "  %s[%d] = (%s);\n", sym, w * k + j, at(rv[k], j));
      Ent* e = top_level ? hput(c, &c->top_larrays, name) : lput(c, &sc->larrays, name);
      e->a = sym; e->n = n; e->wd = w; e->edged = 1; e->lad = lad;
      return;
    }
    V iv = args.npos == 2 ? emit(c, sc, args.pos[1], pre) : mono("0.0");
    Buf first = {0};
    for (int j = 0; j < iv.w; j++) badd(c, &first, "%s(%s)", j ? ", " : "", iv.x[j]);
    badd(c, pre, "  double %s[%d];\n  { double _q[%d] = {%s}; for (long _k = 0; _k < %d; ++_k) "
         "for (int _j = 0; _j < %d; ++_j) %s[%d * _k + _j] = _q[_j]; }\n",
         sym, iv.w * n, iv.w, bstr(&first), n, iv.w, sym, iv.w);
    Ent* e = top_level ? hput(c, &c->top_larrays, name) : lput(c, &sc->larrays, name);
    e->a = sym; e->n = n; e->wd = iv.w; e->edged = 1; e->lad = iv.lad;
  }
}

/* What goes into an element w numbers wide: a number fills it; a pair
 * of pairs into pairs, or a pair into pairs of pairs, is a stand-in error. */
static V elem_fit(Ctx* c, V v, Ent* e, const char* name, Node* at) {
  int w = ew(e);
  if (!fits(v, w, e->lad)) {
    V held = vwide(w, (const char*[]){"", "", "", ""});
    held.lad = e->lad;
    fail(c, "%s holds %s, and is given %s %s: an array keeps its first value's shape", name, shape_of(held),
         given(c, at->kids[2], v), err_loc(c, at));
  }
  return as_shape(c, v, w, e->lad);
}

/* `name[i] = v`: an element store. */
static const char* emit_idx_assign(Ctx* c, Scope* sc, Node* st) {
  Node* base = st->kids[0];
  if (base->kind == nkIdent) {
    Ent* sa = lookup_sarray(c, sc, base->str);
    if (sa) {
      Buf pre = {0};
      const char* ix = emit1(c, sc, st->kids[1], &pre, "an index");
      const char* it = fresh(c, "ai");
      const char* store;
      int w = ew(sa);
      if (w > 1) {                         /* every number of the element; a number fills them */
        V v = elem_fit(c, emit(c, sc, st->kids[2], &pre), sa, base->str, st);
        badd(c, &pre, "  long %s = (long)(%s);\n", it, ix);
        Buf put = {0};
        for (int j = 0; j < w; j++) {
          const char* vt = fresh(c, "av");
          badd(c, &pre, "  double %s = (%s);\n", vt, at(v, j));
          badd(c, &put, " s->pool[%s + %d + %ld + %d * %s] = %s;", sa->a, 1 + j, sa->wo, w, it, vt);
        }
        store = F(c, "  if (%s >= 0 && %s < %ld) {%s }\n", it, it, sa->n, bstr(&put));
      } else {
        const char* v = emit1(c, sc, st->kids[2], &pre, F(c, "$%s[i]", base->str));
        const char* vt = fresh(c, "av");
        badd(c, &pre, "  long %s = (long)(%s); double %s = (%s);\n", it, ix, vt, v);
        store = F(c, "  if (%s >= 0 && %s < %ld) s->pool[%s + 1 + %ld + %s] = %s;\n",
                  it, it, sa->n, sa->a, sa->wo, it, vt);
      }
      if (c->each_depth > 0) bcat(c, &c->deferred, store);
      else bcat(c, &pre, store);
      return sdup(c, bstr(&pre));
    }
    Ent* la = lookup_larray(c, sc, base->str);
    if (la && la->edged) {              /* made by array(): a store past either end is dropped */
      Buf pre = {0};
      const char* ix = emit1(c, sc, st->kids[1], &pre, "an index");
      int w = ew(la);
      if (w > 1) {
        V v = elem_fit(c, emit(c, sc, st->kids[2], &pre), la, base->str, st);
        Buf q = {0};
        for (int j = 0; j < w; j++) badd(c, &q, "%s(%s)", j ? ", " : "", at(v, j));
        badd(c, &pre, "  { long _i = (long)(%s); double _q[%d] = {%s}; if (_i >= 0 && _i < %ld) "
             "for (int _j = 0; _j < %d; ++_j) %s[%d * _i + _j] = _q[_j]; }\n", ix, w, bstr(&q), la->n, w, la->a, w);
        return sdup(c, bstr(&pre));
      }
      const char* v = emit1(c, sc, st->kids[2], &pre, F(c, "%s[i]", base->str));
      badd(c, &pre, "  { long _i = (long)(%s); double _v = (%s); if (_i >= 0 && _i < %ld) %s[_i] = _v; }\n",
           ix, v, la->n, la->a);
      return sdup(c, bstr(&pre));
    }
    if (la) {
      Buf pre = {0};
      const char* ix = emit1(c, sc, st->kids[1], &pre, "an index");
      int w = ew(la);
      if (w > 1) {
        V v = elem_fit(c, emit(c, sc, st->kids[2], &pre), la, base->str, st);
        Buf q = {0};
        for (int j = 0; j < w; j++) badd(c, &q, "%s(%s)", j ? ", " : "", at(v, j));
        badd(c, &pre, "  { long _i = (long)(%s) %% %ld; if (_i < 0) _i += %ld; double _q[%d] = {%s}; "
             "for (int _j = 0; _j < %d; ++_j) %s[%d * _i + _j] = _q[_j]; }\n", ix, la->n, la->n, w, bstr(&q), w, la->a, w);
        return sdup(c, bstr(&pre));
      }
      const char* v = emit1(c, sc, st->kids[2], &pre, F(c, "%s[i]", base->str));
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

/* A table of numbers, as static data; its symbol. */
static const char* static_table(Ctx* c, const char* sym, Node* arr, const char** items) {
  Buf list = {0};
  for (int i = 0; i < arr->nkids; i++) {
    const char* x = num_lit(c, arr->kids[i]->num);
    if (items) items[i] = x;
    if (i) bcat(c, &list, ", ");
    bcat(c, &list, x);
  }
  badd(c, &c->array_decls, "static const double %s[%d] = {%s};\n", sym, arr->nkids, bstr(&list));
  return sym;
}

/* Is `name` read only as cos(name) or sin(name) in n? A rebinding of it
 * counts as a read, to be safe. */
static int trig_only(Ctx* c, Node* n, Node* parent, const char* name) {
  if (!n) return 1;
  if (n->kind == nkIdent && eq(n->str, name))
    return parent && parent->kind == nkCall && parent->nkids == 1 &&
           (eq(parent->str, "sin") || eq(parent->str, "cos")) && !user_def(c, parent->str);
  if ((n->kind == nkLet || n->kind == nkVar) && n->str && eq(n->str, name)) return 0;
  if (n->kind == nkLambda)
    for (int i = 0; i < n->nparams; i++) if (eq(n->params[i], name)) return 0;
  for (int i = 0; i < n->nkids; i++) if (!trig_only(c, n->kids[i], n, name)) return 0;
  return 1;
}

/* `let name = rhs` in a scope (a def, lambda or play body): a table, an
 * array of this sample's numbers, a pair, or a number. */
/* ---------------------------------------------------------------- turning
 * sin(TAU * phasor(f)) with f steady over a block is a point turning by a
 * fixed angle each sample: a pair in the pool, turned by [cos, sin] of
 * TAU f / sr, which is computed once a block. Four multiplies a sample
 * instead of a sine, and its length is put back to 1 every sample (one
 * Newton step), so it never drifts. Where f changes within a block (FM,
 * vibrato) the phasor and the sine stay. */

/* The frequency of `TAU * phasor(f)` (either order), if f is steady over
 * a block; else NULL. */
static int turning_off(void) {
  static int off = -1;               /* AITHER_TURN=0: every sine by sin() */
  if (off < 0) { const char* e = getenv("AITHER_TURN"); off = e && eq(e, "0"); }
  return off;
}

static Node* turning_freq(Ctx* c, Scope* sc, Node* a) {
  if (turning_off() || !c->hoist_on || c->in_hoist || !a || a->kind != nkBinOp || !eq(a->str, "*")) return NULL;
  Node *x = a->kids[0], *y = a->kids[1], *ph = NULL;
  if (x->kind == nkIdent && eq(x->str, "TAU") && y->kind == nkCall) ph = y;
  else if (y->kind == nkIdent && eq(y->str, "TAU") && x->kind == nkCall) ph = x;
  if (!ph || !eq(ph->str, "phasor") || ph->nkids != 1 || user_def(c, "phasor") ||
      lookup_fn(sc, "phasor")[0] || user_def(c, "TAU"))
    return NULL;
  return is_invariant(c, sc, ph->kids[0]) ? ph->kids[0] : NULL;
}

/* A turning point at `freq`: this sample's cos and sin sides. */
static void emit_turner(Ctx* c, Scope* sc, Node* freq, Buf* pre, const char** re, const char** im) {
  const char* f = emit_invariant(c, sc, freq);
  const char* cs = hoist(c, F(c, "cos(2.0 * 3.14159265358979323846 * (%s) / s->sr)", f));
  const char* sn = hoist(c, F(c, "sin(2.0 * 3.14159265358979323846 * (%s) / s->sr)", f));
  const char* off = register_region(c, "turner", 3, NULL);
  const char* o = fresh(c, "tn");
  const char* r = fresh(c, "tn_cos");
  const char* i = fresh(c, "tn_sin");
  badd(c, pre, "  long %s = %s;\n  n_turn(&s->pool[%s], %s, %s);\n", o, off, o, cs, sn);
  badd(c, pre, "  double %s = s->pool[%s], %s = s->pool[%s + 1];\n", r, o, i, o);
  *re = r; *im = i;
}

/* A sine of the clock, sin(k t + c) with k and c steady over a block, is
 * a point turning by k / sr each sample as well. Its phase is the
 * clock's, not its own, so the point is set from the patch's own sin and
 * cos of the argument as written whenever it could have moved off the
 * turning: on its first sample, when k or c changed (hoist_init compares
 * them with the last block's: a knob, `aither sweep`), and every 256
 * samples so rounding can't build up. Between, it turns. Never merely
 * because a block began, so tick and tick_block stay the same to the
 * bit, at any block size. */

/* `t` or `sweep`, the built-in, not a name the patch binds. */
static int is_clock(Ctx* c, Scope* sc, Node* n) {
  if (n->kind != nkIdent || !(eq(n->str, "t") || eq(n->str, "sweep"))) return 0;
  for (Scope* x = sc; x; x = x->parent)
    if (lfind(x->names, n->str) || lfind(x->stereo, n->str) || lfind(x->arrays, n->str) ||
        lfind(x->larrays, n->str) || lfind(x->sarrays, n->str))
      return 0;
  return !has(&c->stereo_lets, n->str) && !has(&c->play_set, n->str) && !has(&c->pair_vars, n->str) &&
         !(has(&c->top_lets, n->str) && has(&c->lets_done, n->str)) && !has(&c->top_var_set, n->str) &&
         !has(&c->file_vars, n->str);
}

/* How fast `n` moves with the clock, as C, if it is k t + c with k and c
 * steady over a block (built of t, sweep, a * x, x / a, x + y, x - y,
 * -x, a steady); "0.0" if it doesn't move; NULL if it isn't such. */
static const char* clock_rate(Ctx* c, Scope* sc, Node* n) {
  if (is_clock(c, sc, n)) return "1.0";
  if (is_invariant(c, sc, n)) return "0.0";
  if (n->kind == nkUnary && eq(n->str, "-") && n->nkids == 1) {
    const char* r = clock_rate(c, sc, n->kids[0]);
    return r ? F(c, "(-(%s))", r) : NULL;
  }
  if (n->kind != nkBinOp || n->nkids != 2) return NULL;
  Node *a = n->kids[0], *b = n->kids[1];
  if (eq(n->str, "+") || eq(n->str, "-")) {
    const char *ra = clock_rate(c, sc, a), *rb = ra ? clock_rate(c, sc, b) : NULL;
    return rb ? F(c, "(%s %s %s)", ra, n->str, rb) : NULL;
  }
  if (eq(n->str, "*")) {
    if (is_invariant(c, sc, a)) {
      const char* rb = clock_rate(c, sc, b);
      return rb ? F(c, "(%s * %s)", emit_invariant(c, sc, a), rb) : NULL;
    }
    if (is_invariant(c, sc, b)) {
      const char* ra = clock_rate(c, sc, a);
      return ra ? F(c, "(%s * %s)", ra, emit_invariant(c, sc, b)) : NULL;
    }
    return NULL;
  }
  if (eq(n->str, "/") && is_invariant(c, sc, b)) {    /* x / 0 is 0, as everywhere */
    const char* ra = clock_rate(c, sc, a);
    const char* d = emit_invariant(c, sc, b);
    return ra ? F(c, "((%s) == 0.0 ? 0.0 : %s / (%s))", d, ra, d) : NULL;
  }
  return NULL;
}

/* Where `n` is at t = 0, as C, for an `n` clock_rate accepted: c of k t + c. */
static const char* clock_offset(Ctx* c, Scope* sc, Node* n) {
  if (is_clock(c, sc, n)) return eq(n->str, "t") ? "0.0" : "(-(s->sweep_origin / s->sr))";
  if (is_invariant(c, sc, n)) return emit_invariant(c, sc, n);
  if (n->kind == nkUnary) return F(c, "(-(%s))", clock_offset(c, sc, n->kids[0]));
  Node *a = n->kids[0], *b = n->kids[1];
  if (eq(n->str, "+") || eq(n->str, "-"))
    return F(c, "(%s %s %s)", clock_offset(c, sc, a), n->str, clock_offset(c, sc, b));
  if (eq(n->str, "*"))
    return is_invariant(c, sc, a) ? F(c, "(%s * %s)", emit_invariant(c, sc, a), clock_offset(c, sc, b))
                                  : F(c, "(%s * %s)", clock_offset(c, sc, a), emit_invariant(c, sc, b));
  const char* d = emit_invariant(c, sc, b);
  return F(c, "((%s) == 0.0 ? 0.0 : %s / (%s))", d, clock_offset(c, sc, a), d);
}

/* The point of sin(arg) and cos(arg), arg moving at `rate`: this
 * sample's cos and sin sides. Its region: the point, and the samples
 * left before it is set again (0: now; hoist_init zeroes it when k or c
 * moved). */
static void emit_clock_turner(Ctx* c, Scope* sc, Node* arg, const char* rate, Buf* pre,
                              const char** re, const char** im) {
  const char* k = hoist(c, rate);
  const char* k0 = hoist(c, clock_offset(c, sc, arg));
  const char* cs = hoist(c, F(c, "cos(%s / s->sr)", k));
  const char* sn = hoist(c, F(c, "sin(%s / s->sr)", k));
  const char* off = register_region(c, "clock_turner", 3, NULL);
  const char* was_k = fresh(c, "ck_was_k");
  const char* was_k0 = fresh(c, "ck_was_k0");
  if (c->loop_nv) {
    /* In a loop of voices, each voice's: its offset gets the voice's
     * stride when the loop is done (emit_voice_loop). */
    spush(c, &c->hoist_decls, F(c, "AITHER_SHARED double %s[%d], %s[%d];\n", was_k, c->loop_nv, was_k0, c->loop_nv));
    spush(c, &c->hoist_inits, F(c, "  for (long aither_vk = 0; aither_vk < %d; aither_vk++) {\n"
                                "  if (!(%s == %s[aither_vk] && %s == %s[aither_vk])) s->pool[%s + 2] = 0.0;\n"
                                "  %s[aither_vk] = %s; %s[aither_vk] = %s;\n  }\n",
                                c->loop_nv, k, was_k, k0, was_k0, off, was_k, k, was_k0, k0));
  } else
    spush(c, &c->hoist_decls, F(c, "AITHER_SHARED double %s, %s;\n", was_k, was_k0));
  if (!c->loop_nv)
    spush(c, &c->hoist_inits, F(c, "  if (!(%s == %s && %s == %s)) s->pool[%s + 2] = 0.0;\n  %s = %s; %s = %s;\n",
                                k, was_k, k0, was_k0, off, was_k, k, was_k0, k0));
  const char* o = fresh(c, "ck");
  const char* r = fresh(c, "ck_cos");
  const char* i = fresh(c, "ck_sin");
  const char* left = fresh(c, "ck_left");
  const char* av = fresh(c, "ck_a");
  Buf at = {0};
  const char* a = emit1(c, sc, arg, &at, "sin's argument");
  badd(c, pre, "  long %s = %s;\n  double %s, %s, %s = s->pool[%s + 2];\n  if (%s <= 0.0) {\n%s",
       o, off, r, i, left, o, left, bstr(&at));
  badd(c, pre, "    double %s = (%s);\n    %s = cos(%s); %s = sin(%s); s->pool[%s + 2] = 255.0;\n",
       av, a, r, av, i, av, o);
  badd(c, pre, "    s->pool[%s] = %s; s->pool[%s + 1] = %s;\n  } else {\n", o, r, o, i);
  badd(c, pre, "    n_turn_clock(&s->pool[%s], %s, %s);\n    %s = s->pool[%s]; %s = s->pool[%s + 1];\n  }\n",
       o, cs, sn, r, o, i, o);
  *re = r; *im = i;
}

/* The turning point a `let` of `TAU * phasor(f)` made, if `name` means it. */
static Ent* turner_across(Ctx* c, const char* name);

static Ent* find_turner(Ctx* c, Scope* sc, const char* name) {
  for (Scope* s = sc; s; s = s->parent) {
    if (lfind(s->names, name)) {
      if (s == c->play_scope && c->cur_unit >= 0 && !c->in_hoist) {
        Ent* x = turner_across(c, name);
        if (x) return x;
      }
      return hfind(&c->turners, F(c, "%p/%s", (void*)s, name));
    }
    if (lfind(s->stereo, name) || lfind(s->numlits, name) || lfind(s->fns, name) ||
        lfind(s->lambdas, name) || lfind(s->arrays, name) || lfind(s->larrays, name) ||
        lfind(s->sarrays, name))
      return NULL;
  }
  return NULL;
}

static void emit_let(Ctx* c, Scope* sc, const char* name, Node* rhs, Buf* pre, const char* cname) {
  int numeric = rhs->kind == nkArr && rhs->nkids > 0 && all_nums(rhs) && !is_value_lit(rhs);
  if ((rhs->kind == nkArr && rhs->nkids != 2 && rhs->nkids > 0 && !numeric) || is_array_make(rhs)) {
    declare_local_array(c, sc, name, rhs, pre, 0);
    return;
  }
  if (numeric) {
    const char** items = al(c, rhs->nkids * sizeof(char*));
    Ent* e = lput(c, &sc->arrays, name);
    e->a = static_table(c, fresh(c, F(c, "arr_%s", name)), rhs, items);
    e->n = rhs->nkids;
    e->node = rhs;
    if (rhs->nkids != 2) return;
    /* two numbers are a pair as well */
    Ent* s2 = lput(c, &sc->stereo, name);
    s2->a = items[0]; s2->b = items[1];
    return;
  }
  /* A steady turning (`let a = TAU * phasor(f)`) read only through
   * cos(a) and sin(a), the rest of its block: the turner is all that is
   * heard, so the phasor isn't kept. */
  if (!cname && c->blk) {
    Node* tf = turning_freq(c, sc, rhs);
    int only = tf != NULL;
    for (int i = c->blk_i + 1; only && i < c->blk->nkids; i++)
      only = trig_only(c, c->blk->kids[i], c->blk, name);
    if (only) {
      const char *re, *im;
      emit_turner(c, sc, tf, pre, &re, &im);
      lput(c, &sc->names, name)->a = "0.0";
      Ent* te = hput(c, &c->turners, F(c, "%p/%s", (void*)sc, name));
      te->a = re; te->b = im;
      return;
    }
  }
  /* A number known now is that number: nothing is computed each sample. */
  double known;
  if (!cname && const_value(c, sc, rhs, &known)) {
    lput(c, &sc->names, name)->a = num_lit(c, known);
    lput(c, &sc->numlits, name)->v = known;
    return;
  }
  V v = emit(c, sc, rhs, pre);
  if (is_pair(v)) {
    static const char* const suffix[4] = {"l", "r", "l2", "r2"};
    const char* t[4];
    for (int k = 0; k < v.w; k++) {
      t[k] = cname ? F(c, "%s_%s", cname, suffix[k]) : fresh(c, F(c, "l_%s_%s", name, suffix[k]));
      badd(c, pre, "  double %s = (%s);\n", t[k], v.x[k]);
    }
    bind_wide(c, sc, name, vlike(v, v.w, t));
    return;
  }
  const char* t = cname ? cname : fresh(c, F(c, "l_%s", name));
  badd(c, pre, "  double %s = (%s);\n", t, v.l);
  lput(c, &sc->names, name)->a = t;
  Node* tf = turning_freq(c, sc, rhs);
  if (tf) {
    const char *re, *im;
    emit_turner(c, sc, tf, pre, &re, &im);
    Ent* te = hput(c, &c->turners, F(c, "%p/%s", (void*)sc, name));
    te->a = re; te->b = im;
  }
  double cv;
  if (const_value(c, sc, rhs, &cv)) lput(c, &sc->numlits, name)->v = cv;
  else note_invariant(c, sc, name, rhs, sc);
}

/* One non-final statement of a body. */
static void emit_local_stmt(Ctx* c, Scope* sc, Node* st, Buf* pre) {
  switch (st->kind) {
  case nkLet:
    emit_let(c, sc, st->str, st->kids[0], pre, NULL);
    break;
  case nkVar: {
    if (is_array_make(st->kids[0])) { declare_state_array(c, sc, st, pre, 0); break; }
    /* `$x` in a def: its first value runs once, then the state keeps. A
     * pair is a "pvar" region (the flag, two sides), a number a "var". */
    Buf init_pre = {0};
    V v = emit(c, sc, st->kids[0], &init_pre);
    /* A first value that is numbers, known now, is given once a block,
     * before any sample (hoist_init), not checked for every sample: the
     * state holds it before its first use either way. */
    int settle = !c->loop_nv && !init_pre.n;
    for (int k = 0; k < v.w; k++) settle = settle && c_number(v.x[k]);
    const char* flag = fresh(c, "sf");
    const char* off = register_region(c, v.w == 4 ? "qvar" : v.w == 2 ? "pvar" : "var", v.w + 1, st->str);
    Buf fill = {0};                      /* the numbers into the region, then its flag */
    const char* base = settle ? off : flag;
    for (int k = 0; k < v.w; k++) badd(c, &fill, "    s->pool[%s + %d] = (%s);\n", base, k + 1, v.x[k]);
    badd(c, &fill, "    s->pool[%s] = 1.0;\n", base);
    if (settle) {
      spush(c, &c->hoist_inits, F(c, "  if (s->pool[%s] == 0.0) {\n%s  }\n", off, bstr(&fill)));
      badd(c, pre, "  long %s = %s;\n", flag, off);
    } else {
      badd(c, pre, "  long %s = %s;\n  if (s->pool[%s] == 0.0) {\n%s%s  }\n", flag, off, flag, bstr(&init_pre), bstr(&fill));
    }
    const char* cell[4];
    for (int k = 0; k < v.w; k++) cell[k] = F(c, "s->pool[%s + %d]", flag, k + 1);
    if (v.w == 1) lput(c, &sc->names, st->str)->a = cell[0];
    else bind_wide(c, sc, st->str, vlike(v, v.w, cell));
    break;
  }
  case nkAssign: {
    Ent* pair = lookup_stereo(sc, st->str);
    if (pair) {
      bcat(c, pre, assign_wide(c, sc, st->str, wide_value(pair), st->kids[0]));
    } else {
      const char* target = lookup(sc, st->str);
      if (!target[0]) fail(c, "unknown assignment target: %s %s", st->str, err_loc(c, st));
      const char* rhs = emit1(c, sc, st->kids[0], pre, F(c, "$%s", st->str));
      badd(c, pre, "  %s = (%s);\n", target, rhs);
    }
    break;
  }
  case nkIdxAssign:
    bcat(c, pre, emit_idx_assign(c, sc, st));
    break;
  default:
    emit(c, sc, st, pre);      /* run for its effects */
  }
}

/* A body: its statements, then the value of its last expression. */
static V emit_block(Ctx* c, Scope* sc, Node* blk, Buf* pre) {
  Node* was = c->blk; int was_i = c->blk_i;
  for (int i = 0; i < blk->nkids - 1; i++) {
    c->blk = blk; c->blk_i = i;
    emit_local_stmt(c, sc, blk->kids[i], pre);
  }
  c->blk = was; c->blk_i = was_i;
  if (blk->nkids == 0) return mono("0.0");
  return emit(c, sc, blk->kids[blk->nkids - 1], pre);
}

/* ---- recursion: a def that calls itself compiles to a loop ---- */

typedef struct {
  Node* def;
  const char** vl; const char** vr;    /* each parameter's loop variable(s); "" when bound by name */
  int* whole;
  int any_pair;                        /* some branch's result is a pair */
  const char* rl; const char* rr;
  int any_lad;                         /* some branch's result is a value on the ladder */
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
      Node* body = def->kids[0];
      if (!def->is_rec && body->kind == nkBlock && body->nkids > 1 &&
          body->kids[body->nkids - 1] == call && call->line > def->line)
        fail(c, "%s's body runs on to the call %s: a body is its lets, $state and stores, ended by "
             "the expression that is its value, and %s's has none before it. End the body with its "
             "value (after a store, 0 will do), or write def rec %s if it is meant to repeat.",
             def->str, err_loc(c, call), def->str, def->str);
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

/* The tail of a recursive def's body, as statements in the loop: a real
 * branch for each `if`, `continue` for a repeat, `break` with the
 * result for anything else. */
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
    const char* cond = emit1(c, sc, n->kids[0], o, "the condition");
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
        if (lp->whole[i]) {
          if (a->kind == nkIdent) {
            Ent* st = lookup_stereo(sc, a->str);
            if (st && eq(st->a, lp->vl[i])) continue;
          }
          V v = emit(c, sc, a, o);
      no_quad(c, v, "def rec", a);
          const char* tl = fresh(c, F(c, "next_%s_l", p));
          const char* tr = fresh(c, F(c, "next_%s_r", p));
          badd(c, o, "  double %s = (%s); double %s = (%s);\n", tl, v.l, tr, v.r);
          badd(c, &assigns, "  %s = %s; %s = %s;\n", lp->vl[i], tl, lp->vr[i], tr);
        } else {
          if (a->kind == nkIdent && eq(lookup(sc, a->str), lp->vl[i])) continue;
          const char* x = emit1(c, sc, a, o, F(c, "%s's %s", def->str, p));
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
  V v = emit(c, sc, n, o);
    no_quad(c, v, "def rec", n);
  if (is_pair(v)) lp->any_pair = 1;
  if (v.lad) lp->any_lad = 1;
  badd(c, o, "  %s = (%s); %s = (%s); break;\n", lp->rl, v.l, lp->rr, v.r);
}

/* A call to a recursive def: the loop's statements into `pre`, and its
 * result. */
static V emit_loop_side(Ctx* c, Scope* sc, Node* def, Node* call, V* args, int* bound, int k, Buf* pre) {
  /* `sc` holds what binds by name (bound[i]) already. */
  if (sfind(&c->def_stack, def->str) >= 0) cycle_error(c, def, call);
  check_repeats(c, def);
  int* changing = changing_params(c, def);
  RecLoop lp = {def, NULL, NULL, takes_pair(c, def), 0, NULL, NULL};
  lp.vl = al(c, (def->nparams + 1) * sizeof(char*));
  lp.vr = al(c, (def->nparams + 1) * sizeof(char*));
  Scope* inner = push(c, sc);
  for (int i = 0; i < def->nparams; i++) {
    const char* p = def->params[i];
    if (bound[i]) { lp.vl[i] = ""; lp.vr[i] = ""; continue; }
    if (args[i].lad) lp.whole[i] = 1;    /* a value is always taken whole */
    if (lp.whole[i]) {
      const char* vl = fresh(c, F(c, "rec_%s_l", p));
      const char* vr = fresh(c, F(c, "rec_%s_r", p));
      badd(c, pre, "  double %s = (%s); double %s = (%s);\n", vl, args[i].l, vr, args[i].r);
      Ent* e = lput(c, &inner->stereo, p);
      e->a = vl; e->b = vr; e->lad = args[i].lad;
      lp.vl[i] = vl; lp.vr[i] = vr;
      continue;
    }
    const char* vn = fresh(c, F(c, "rec_%s", p));
    badd(c, pre, "  double %s = (%s);\n", vn, at(args[i], k));
    lput(c, &inner->names, p)->a = vn;
    if (!changing[i] && !is_pair(args[i])) {
      double cv;
      if (const_value(c, sc, call->kids[i], &cv)) lput(c, &inner->numlits, p)->v = cv;
      else note_invariant(c, inner, p, call->kids[i], sc);
    }
    lp.vl[i] = vn; lp.vr[i] = "";
  }
  lp.rl = fresh(c, F(c, "rec_%s", def->str));
  lp.rr = fresh(c, F(c, "rec_%s_r", def->str));
  const char* steps = fresh(c, "steps");
  const char* give_up = F(c, "%s = %s = 0.0 / 0.0;", lp.rl, lp.rr);
  const char* where = c_literal(c, F(c, "%s %s", def->str, err_loc(c, def)));
  badd(c, pre, "  double %s, %s;\n  long %s = 0;\n", lp.rl, lp.rr, steps);
  badd(c, pre, "  if (s->stuck) { %s } else for (;;) {\n", give_up);
  badd(c, pre, "  if ((++%s & 65535) == 0 && n_overrun((void*)s, %s)) { %s break; }\n", steps, where, give_up);
  spush(c, &c->def_stack, def->str);
  int depth = c->each_depth;
  Buf deferred = c->deferred;
  c->each_depth = 0;
  c->deferred = (Buf){0};
  emit_tail(c, inner, &lp, def->kids[0], pre);
  c->each_depth = depth;
  c->deferred = deferred;
  c->def_stack.n--;
  bcat(c, pre, "  }\n");
  V r = lp.any_pair ? vpair(lp.rl, lp.rr) : mono(lp.rl);
  r.lad = lp.any_lad;
  return r;
}

static V emit_loop(Ctx* c, Scope* sc, Node* def, Node* call, Buf* pre) {
  if (call->nkids != def->nparams)
    fail(c, "wrong arg count for %s: expected %d got %d %s", def->str, def->nparams, call->nkids, err_loc(c, call));
  int* whole = takes_pair(c, def);
  V* args = al(c, (def->nparams + 1) * sizeof(V));
  int* bound = al(c, (def->nparams + 1) * sizeof(int));
  int split = 0;
  Scope* names = push(c, NULL);
  names->isolated = 1;
  for (int i = 0; i < def->nparams; i++) {
    if (bind_by_name(c, sc, names, def->params[i], call->kids[i], pre)) { bound[i] = 1; continue; }
    Buf own = {0};
    args[i] = emit(c, sc, call->kids[i], &own);
    if (own.n) for (int k = 0; k < i; k++) if (!bound[k]) args[k] = capture(c, pre, args[k]);
    bcat(c, pre, bstr(&own));
    if (is_pair(args[i]) && !whole[i] && !args[i].lad) split = 1;
  }
  if (!split) return emit_loop_side(c, names, def, call, args, bound, 0, pre);
  V l = emit_loop_side(c, names, def, call, args, bound, 0, pre);
  V r = emit_loop_side(c, names, def, call, args, bound, 1, pre);
  if (is_pair(l) || is_pair(r))
    fail(c, "%s gives more than a number, and given places it repeats once a place, which isn't built %s",
         def->str, err_loc(c, call));
  return vpair(l.l, r.l);
}

/* phasor(freq): one slot, advanced and wrapped. */
static const char* emit_phasor(Ctx* c, Scope* sc, Node* freq_node, const char* freq, Buf* pre) {
  const char* step;
  if (c->hoist_on && is_invariant(c, sc, freq_node)) step = hoist(c, F(c, "(%s) / s->sr", emit_invariant(c, sc, freq_node)));
  else step = F(c, "(%s) / s->sr", freq);
  const char* slot = fresh(c, "ph");
  const char* off = register_region(c, "phasor", 1, NULL);
  badd(c, pre, "  long %s = %s; s->pool[%s] = fmod(s->pool[%s] + %s, 1.0); "
       "if (s->pool[%s] < 0.0) s->pool[%s] += 1.0;\n", slot, off, slot, slot, step, slot, slot);
  return F(c, "s->pool[%s]", slot);
}

/* noise(): xorshift32 in a slot, seeded from where the call is (its
 * statement, its defs, which noise() there), so each call site is its
 * own stream, the same on every run. It is white noise with a fixed
 * level per Hz, the level ±1 uniform noise has at 48 kHz, so what is
 * heard is the same at any rate: its samples scale by sqrt(sr / 48000). */
static const char* emit_noise(Ctx* c, Buf* pre) {
  const char* slot = fresh(c, "nz");
  const char* off = register_region(c, "noise", 1, NULL);
  Region* r = &c->regions[c->nregions - 1];
  unsigned seed = 2166136261u;
  const char* id = F(c, "%s|%s|%d", r->scope, r->owner, r->scope_idx);
  for (const char* p = id; *p; p++) seed = (seed ^ (unsigned char)*p) * 16777619u;
  if (seed == 0) seed = 2463534242u;
  const char* t = fresh(c, "nzv");
  /* In a loop of voices each voice's seed is its own, as unrolled: an
   * array filled in once the voices' regions are known. */
  const char* sd = c->loop_nv ? F(c, "__SEED_%06d__", c->nregions - 1) : F(c, "%uu", seed);
  badd(c, pre, "  double %s; { long %s = %s; unsigned int r = (unsigned int)s->pool[%s]; if (r == 0) r = %s; "
       "r ^= r << 13; r ^= r >> 17; r ^= r << 5; s->pool[%s] = (double)r; "
       "%s = ((double)r / 4294967295.0) * 2.0 - 1.0; }\n", t, slot, off, slot, sd, slot, t);
  const char* per_hz = c->hoist_on && !c->in_hoist ? hoist(c, "sqrt(s->sr / 48000.0)") : "sqrt(s->sr / 48000.0)";
  return F(c, "(%s * %s)", t, per_hz);
}

/* The slots one call site of a native claims. */
static long native_slot_size(Ctx* c, const char* name, Node** kids) {
  const NativeSpec* spec = native(name);
  if (eq(name, "reverb")) return dsp_reverb_slots(c->sr);   /* its room is in seconds */
  if (spec->buf_arg < 0) return spec->slots;
  Node* a = kids[spec->buf_arg];
  if (a->kind != nkNum) {
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

static const char* const HOISTED_MATH[] = {"exp", "log", "pow", "sin", "cos", "tan", "tanh", "sqrt", "erf", NULL};
static const char* const MIDI_FNS[] = {"midi_cc", "midi_note", "midi_trig", "midi_freq",
                                       "midi_gate", "midi_voice_freq", "midi_voice_gate",
                                       "midi_voice_vel", "midi_voice_key", "midi_vel",
                                       "midi_voice_cc", "midi_voice_press", "midi_voice_bend",
                                       "midi_voice_on", NULL};

/* A call's arguments, each a value. */
static V* emit_args(Ctx* c, Scope* sc, Node* n, Buf* pre, int* any_pair) {
  V* a = al(c, (n->nkids + 1) * sizeof(V));
  emit_seq(c, sc, n->kids, n->nkids, a, pre);
  *any_pair = 0;
  for (int i = 0; i < n->nkids; i++)
    if (is_pair(a[i])) *any_pair = 1;
  return a;
}

/* The first of `a` on the ladder, if any: a function on a value gives a value. */
static V lad_among(V* a, int n) {
  for (int i = 0; i < n; i++) if (a[i].lad) return a[i];
  return mono("");
}

/* How many times a function given `a` runs side by side: once for
 * numbers, twice for a pair, four times for a pair of pairs. A pair beside
 * a pair of pairs is a stand-in error: which halves would the pair meet? */
static int width_of(Ctx* c, V* a, int n, const char* name, Node* at) {
  int quad = 0, pair = 0, value = 0, field = 0;
  for (int i = 0; i < n; i++) {
    if (is_quad(a[i])) quad = 1; else if (is_pair(a[i])) pair = 1;
    if (a[i].lad) value = 1; else if (is_pair(a[i])) field = 1;
  }
  if ((quad && pair) || (value && field)) {
    Buf b = {0};                         /* the ones that clash, as written */
    if (at && at->nkids >= n)
      for (int i = 0, first = 1; i < n; i++)
        if (is_pair(a[i])) { badd(c, &b, "%s%s", first ? "" : ", and ", given(c, at->kids[i], a[i])); first = 0; }
    fail(c, "%s is given %s %s: it runs once a number, and these don't line up number for number, so give it one "
         "shape. " QUAD_STANDIN, name, b.n ? bstr(&b) : "two shapes", at ? err_loc(c, at) : "");
  }
  return quad ? 4 : pair ? 2 : 1;
}

/* The sides of `args` for side k: a number is the same on both. */
static const char** sides(Ctx* c, V* a, int n, int k) {
  const char** s = al(c, (n + 1) * sizeof(char*));
  for (int i = 0; i < n; i++) s[i] = at(a[i], k);
  return s;
}

/* A pure function applied side by side: once for numbers, per side for a
 * pair (a number then read on both). `build` makes one side's text. */
typedef const char* (*Build)(Ctx* c, const char* name, const char** x, int n);
static V map_pure(Ctx* c, const char* name, V* a, int n, int any_pair, Build build, Buf* pre, Node* at) {
  if (!any_pair) return mono(build(c, name, sides(c, a, n, 0), n));
  int wd = width_of(c, a, n, name, at);
  for (int i = 0; i < n; i++)
    if (!is_pair(a[i])) a[i] = mono(once(c, pre, a[i].l));
  const char* x[4];
  for (int k = 0; k < wd; k++) x[k] = build(c, name, sides(c, a, n, k), n);
  return vlike(lad_among(a, n), wd, x);
}

static const char* b_fn1(Ctx* c, const char* name, const char** x, int n) { (void)n; return F(c, "%s(%s)", name, x[0]); }
static const char* b_abs(Ctx* c, const char* name, const char** x, int n) { (void)n; (void)name; return F(c, "fabs(%s)", x[0]); }
static const char* b_shape(Ctx* c, const char* name, const char** x, int n) { (void)n; return F(c, "shape_%s(%s)", name, x[0]); }
static const char* b_pow(Ctx* c, const char* name, const char** x, int n) { (void)n; (void)name; return F(c, "pow(%s, %s)", x[0], x[1]); }
/* log(x, base): C's exact log2 and log10 for those bases, else the quotient. */
static const char* b_log2(Ctx* c, const char* name, const char** x, int n) { (void)n; (void)name; return F(c, "log2(%s)", x[0]); }
static const char* b_log10(Ctx* c, const char* name, const char** x, int n) { (void)n; (void)name; return F(c, "log10(%s)", x[0]); }
static const char* b_logb(Ctx* c, const char* name, const char** x, int n) { (void)n; (void)name; return F(c, "(log(%s) / log(%s))", x[0], x[1]); }
static const char* b_minmax(Ctx* c, const char* name, const char** x, int n) {
  const char* acc = x[0];
  for (int i = 1; i < n; i++) acc = F(c, "f%s(%s, %s)", name, acc, x[i]);
  return acc;
}
static const char* b_clamp(Ctx* c, const char* name, const char** x, int n) { (void)n; (void)name; return F(c, "fmin(fmax(%s, %s), %s)", x[0], x[1], x[2]); }
static const char* b_int(Ctx* c, const char* name, const char** x, int n) { (void)n; (void)name; return F(c, "((double)(long)(%s))", x[0]); }
static const char* b_midi(Ctx* c, const char* name, const char** x, int n) {
  if (n == 2) return F(c, "n_%s((int)(%s), (int)(%s))", name, x[0], x[1]);
  return n ? F(c, "n_%s((int)(%s))", name, x[0]) : F(c, "n_%s()", name);
}

/* A stateful call per side: once for numbers, per side for a pair, each
 * side its own region (left first). `call` makes one side's statement
 * and value. */
typedef const char* (*Stateful)(Ctx* c, Scope* sc, Node* n, const char** x, Buf* pre, void* ud);
static V per_side(Ctx* c, Scope* sc, Node* n, V* a, int any_pair, Stateful call, Buf* pre, void* ud) {
  if (!any_pair) return mono(call(c, sc, n, sides(c, a, n->nkids, 0), pre, ud));
  int wd = width_of(c, a, n->nkids, n->str, n);        /* once a number, each its own region, in order */
  for (int i = 0; i < n->nkids; i++)
    if (is_quad(a[i])) a[i] = capture(c, pre, a[i]);
  const char* x[4];
  for (int k = 0; k < wd; k++) x[k] = call(c, sc, n, sides(c, a, n->nkids, k), pre, ud);
  return vlike(lad_among(a, n->nkids), wd, x);
}

static const char* s_phasor(Ctx* c, Scope* sc, Node* n, const char** x, Buf* pre, void* ud) {
  (void)ud;
  return emit_phasor(c, sc, n->kids[0], x[0], pre);
}
static const char* s_midi_trig(Ctx* c, Scope* sc, Node* n, const char** x, Buf* pre, void* ud) {
  (void)sc; (void)n; (void)ud;
  const char* off = register_region(c, "midi_trig", 1, NULL);
  return native_call(c, pre, off, F(c, "n_midi_trig((DspState*)s, (int)(%s))", x[0]));
}
typedef struct { const char* sym; long length; } WaveTable;
static const char* s_wave(Ctx* c, Scope* sc, Node* n, const char** x, Buf* pre, void* ud) {
  (void)sc;
  WaveTable* w = ud;
  const char* off = register_region(c, "wave", native_slot_size(c, "wave", n->kids), NULL);
  return native_call(c, pre, off, F(c, "n_wave((DspState*)s, %s, (double*)%s, %ld)", x[0], w->sym, w->length));
}
/* A hoisted native: the coefficient once a block, the `_c` step here. */
typedef struct { const char* coef; } Hoisted;
static const char* s_lp1_c(Ctx* c, Scope* sc, Node* n, const char** x, Buf* pre, void* ud) {
  (void)sc;
  const char* off = register_region(c, n->str, native_slot_size(c, n->str, n->kids), NULL);
  return native_call(c, pre, off, F(c, "n_%s_c((DspState*)s, %s, %s)", n->str, x[0], ((Hoisted*)ud)->coef));
}
static const char* s_reverb_c(Ctx* c, Scope* sc, Node* n, const char** x, Buf* pre, void* ud) {
  (void)sc;
  const char* off = register_region(c, "reverb", native_slot_size(c, "reverb", n->kids), NULL);
  return native_call(c, pre, off, F(c, "n_reverb_c((DspState*)s, %s, %s, %s)", x[0], x[2], ((Hoisted*)ud)->coef));
}
static const char* s_native(Ctx* c, Scope* sc, Node* n, const char** x, Buf* pre, void* ud) {
  (void)sc; (void)ud;
  const char* off = register_region(c, n->str, native_slot_size(c, n->str, n->kids), NULL);
  Buf call = {0};
  badd(c, &call, "n_%s((DspState*)s", n->str);
  for (int i = 0; i < n->nkids; i++) badd(c, &call, ", %s", x[i]);
  bcat(c, &call, ")");
  return native_call(c, pre, off, bstr(&call));
}

static void decide_var(Ctx* c, Node* st);
static Node* file_var_decl(Ctx* c, const char* name);

/* A name's value: a scope's binding, then the file's. */
/* A built-in name's value, or NULL. A name the patch binds comes first:
 * `let sweep = ...` is the patch's own. */
static const char* builtin_value(const char* s) {
  if (eq(s, "t")) return "aither_t";
  if (eq(s, "sr")) return "s->sr";
  if (eq(s, "dt")) return "s->dt";
  /* The voice's own time: a sweep from the origin the engine sets (a
   * new voice, `aither sweep`), so a patch never subtracts. */
  if (eq(s, "sweep")) return "((aither_step - s->sweep_origin) / s->sr)";
  if (eq(s, "PI")) return "3.14159265358979323846";
  if (eq(s, "TAU")) return "(2.0 * 3.14159265358979323846)";
  return NULL;
}

static V emit_ident_raw(Ctx* c, Scope* sc, Node* n) {
  const char* s = n->str;
  for (Scope* x = sc; x; x = x->parent) {
    Ent* e = lfind(x->names, s);
    if (e) return mono(e->a);
    e = lfind(x->stereo, s);
    if (e) return wide_value(e);
    if (lfind(x->arrays, s) || lfind(x->larrays, s) || lfind(x->sarrays, s)) break;
  }
  Ent* cq = hfind(&c->const_quads, s);
  if (cq) {                              /* a constant pair of pairs (Hamilton's i): its numbers */
    Node* q = cq->node;
    if (q->kids[0]->kind == nkNum) {
      V z = vpair(num_lit(c, q->kids[0]->num), num_lit(c, q->kids[1]->num));
      z.lad = 1;
      return z;
    }
    V v = vquad(num_lit(c, q->kids[0]->kids[0]->num), num_lit(c, q->kids[0]->kids[1]->num),
                num_lit(c, q->kids[1]->kids[0]->num), num_lit(c, q->kids[1]->kids[1]->num));
    v.lad = cq->lad;
    return v;
  }
  /* A def sees the file's constants (numbers and tables known when it
   * compiles), never its signals: those it is given. */
  if (in_def(sc) && !builtin_value(s) && !hfind(&c->top_arrays, s) && !(c->top && lfind(c->top->numlits, s)) &&
      (has(&c->top_lets, s) || has(&c->stereo_lets, s) || has(&c->play_set, s) || hfind(&c->top_larrays, s)))
    fail(c, "%s sees its arguments and the file's constants, and '%s' is %s: pass it in %s",
         c->def_stack.n ? c->def_stack.v[c->def_stack.n - 1] : "a def", s,
         hfind(&c->top_larrays, s) ? "an array of this sample's values" : "a signal", err_loc(c, n));
  Ent* table = lookup_array(c, sc, s);
  if (has(&c->stereo_lets, s)) {
    V v = has(&c->quad_lets, s) ? vquad(F(c, "l_%s_l", s), F(c, "l_%s_r", s), F(c, "l_%s_l2", s), F(c, "l_%s_r2", s))
                                : vpair(F(c, "l_%s_l", s), F(c, "l_%s_r", s));
    v.lad = has(&c->lad_lets, s);
    return v;
  }
  if (table) fail(c, "%s is a table of %ld numbers, but one number is needed here — index it (%s[i]) "
                  "or hand it to wave %s", s, table->n, s, err_loc(c, n));
  if ((lookup_larray(c, sc, s) || lookup_sarray(c, sc, s)))
    fail(c, "%s is an array, but one number is needed here — index it (%s[i]) %s", s, s, err_loc(c, n));
  if (has(&c->play_set, s)) return vpair(F(c, "p_%s_l", s), F(c, "p_%s_r", s));
  if (has(&c->pair_vars, s)) return file_var(c, s);
  if (has(&c->top_lets, s)) {
    if (!has(&c->lets_done, s)) {
      if (builtin_value(s)) return mono(builtin_value(s));   /* the built-in, until the let */
      Ent* ll = hfind(&c->let_lines, s);
      fail(c, "'%s' is used before its `let` on line %ld: move that let above this line %s",
           s, ll ? ll->n : 0L, err_loc(c, n));
    }
    return mono(F(c, "l_%s", s));
  }
  if (has(&c->top_var_set, s)) return mono(F(c, "s->v_%s", s));
  if (has(&c->file_vars, s)) {
    /* Read before the line that declares it (where it is set once):
     * until then it reads 0. Its shape comes from how its first value is
     * written, a pair literal or a pair builtin. */
    decide_var(c, file_var_decl(c, s));
    return emit_ident_raw(c, sc, n);
  }
  const char* b = builtin_value(s);
  if (b) return mono(b);
  fail(c, "unknown identifier: %s %s", s, err_loc(c, n));
  return mono("");
}

static const char* shared_key(Ctx* c, Scope* sc, const char* s);
static V cross_read(Ctx* c, const char* key, V v);

/* A name as the unit being emitted reads it: from its buffer when its
 * writer is another task (multicore, notes/multicore-2026-10-18.md). */
static V emit_ident(Ctx* c, Scope* sc, Node* n) {
  V v = emit_ident_raw(c, sc, n);
  if (c->cur_unit < 0 || c->in_hoist) return v;
  const char* key = shared_key(c, sc, n->str);
  return key ? cross_read(c, key, v) : v;
}

/* An `if`: only the branch taken runs. Plain values pick with `?:`;
 * a branch with statements becomes a real branch. */
static V emit_if(Ctx* c, Scope* sc, Node* n, Buf* pre) {
  double known;           /* a condition known now: only the branch taken is there */
  if (const_value(c, sc, n->kids[0], &known))
    return known != 0.0 ? emit(c, sc, n->kids[1], pre) : n->kids[2] ? emit(c, sc, n->kids[2], pre) : mono("0.0");
  const char* cond = emit1(c, sc, n->kids[0], pre, "the condition");
  Buf pt = {0}, pe = {0};
  V t = emit(c, sc, n->kids[1], &pt);
  V e = n->kids[2] ? emit(c, sc, n->kids[2], &pe) : mono("0.0");
  V both[2] = {t, e};
  int w, lad;
  common_shape(c, both, 2, &w, &lad, n);
  t = as_shape(c, t, w, lad);
  e = as_shape(c, e, w, lad);
  const char* v[4];
  if (!pt.n && !pe.n) {
    const char* ct = w > 1 ? once(c, pre, cond) : cond;
    for (int k = 0; k < w; k++) v[k] = F(c, "((%s) != 0.0 ? (%s) : (%s))", ct, at(t, k), at(e, k));
    V r = vwide(w, v);
    r.lad = lad;
    return r;
  }
  for (int k = 0; k < w; k++) {
    v[k] = fresh(c, "ifv");
    badd(c, pre, "  double %s;\n", v[k]);
  }
  badd(c, pre, "  if ((%s) != 0.0) {\n%s", cond, bstr(&pt));
  for (int k = 0; k < w; k++) badd(c, pre, "  %s = (%s);\n", v[k], at(t, k));
  badd(c, pre, "  } else {\n%s", bstr(&pe));
  for (int k = 0; k < w; k++) badd(c, pre, "  %s = (%s);\n", v[k], at(e, k));
  bcat(c, pre, "  }\n");
  V r = vwide(w, v);
  r.lad = lad;
  return r;
}

/* x ^ k for a whole k from 0 to 4 known now: the products, so x^2 is x * x
 * to the bit. */
static int small_power(Ctx* c, Scope* sc, Node* e, long* k) {
  double v;
  if (!const_value(c, sc, e, &v)) return 0;
  *k = (long)v;
  return (double)*k == v && *k >= 0 && *k <= 4;
}

static const char* power_of(Ctx* c, const char* x, long k) {
  if (k == 0) return "1.0";
  const char* r = x;
  for (long j = 1; j < k; j++) r = F(c, "((%s) * (%s))", r, x);
  return r;
}

static V hamilton(Ctx* c, V p, V q);

/* The product at width w, the operands fixed first: a complex product, or
 * Hamilton's. */
static V ladder_mul(Ctx* c, V a, V b, int w, Buf* pre) {
  a = capture(c, pre, embed(a, w));
  b = capture(c, pre, embed(b, w));
  V r = w == 4 ? hamilton(c, a, b)
               : vpair(F(c, "(%s * %s - %s * %s)", a.l, b.l, a.r, b.r), F(c, "(%s * %s + %s * %s)", a.l, b.r, a.r, b.l));
  r.lad = 1;
  return r;
}

/* z ^ p for any power: de Moivre's, the size to the power and the angle
 * times it, the principal root where there are several. On Hamilton's
 * numbers the angle is his, 0..pi, about his axis (k for a plain number,
 * as `axis`), as slerp takes a fraction of a rotation. 0 to a power is 0,
 * and 1 at power 0. */
static V ladder_power(Ctx* c, V z, const char* power, Buf* pre) {
  z = capture(c, pre, z);
  const char* p = once(c, pre, power);
  const char* v = z.w == 4 ? once(c, pre, F(c, "sqrt(%s * %s + %s * %s + %s * %s)", z.r, z.r, z.l2, z.l2, z.r2, z.r2))
                           : once(c, pre, F(c, "fabs(%s)", z.r));
  const char* size = once(c, pre, F(c, "sqrt(%s * %s + %s * %s)", z.l, z.l, v, v));
  const char* angle = z.w == 4 ? once(c, pre, F(c, "(atan2(%s, %s) * %s)", v, z.l, p))
                               : once(c, pre, F(c, "(atan2(%s, %s) * %s)", z.r, z.l, p));
  const char* m = once(c, pre, F(c, "(%s == 0.0 ? (%s == 0.0 ? 1.0 : 0.0) : pow(%s, %s))", size, p, size, p));
  const char* re = F(c, "(%s * cos(%s))", m, angle);
  const char* s = once(c, pre, F(c, "(%s * sin(%s))", m, angle));
  if (z.w == 2) { V r = vpair(re, s); r.lad = 1; return r; }
  V r = vquad(re, F(c, "(%s == 0.0 ? 0.0 : %s * %s / %s)", v, s, z.r, v), F(c, "(%s == 0.0 ? 0.0 : %s * %s / %s)", v, s, z.l2, v),
              F(c, "(%s == 0.0 ? %s : %s * %s / %s)", v, s, s, z.r2, v));
  r.lad = 1;
  return r;
}

/* `+ - * / ^` with a value on the ladder (notes/ladder-2026-11-06.md): the
 * narrower sits inside, the rest zero. */
static V ladder_op(Ctx* c, Scope* sc, Node* n, V a, V b, Buf* pre) {
  const char* op = n->str;
  if (!(eq(op, "+") || eq(op, "-") || eq(op, "*") || eq(op, "/") || eq(op, "^")))
    fail(c, "`%s` takes numbers, and is given %s %s", op, givens(c, n->kids, (V[]){a, b}, 2), err_loc(c, n));
  int w = a.w > b.w ? a.w : b.w;
  const char* x[4];
  if (eq(op, "^")) {
    if (b.w != 1)
      fail(c, "`^` on a value takes a number for its power, and is given %s %s", given(c, n->kids[1], b), err_loc(c, n));
    double p;
    if (const_value(c, sc, n->kids[1], &p) && p == floor(p) && p >= 0 && p <= 64) {   /* the products, exact */
      if (p == 0) return embed(mono("1.0"), w);
      V base = capture(c, pre, a), r = base;
      for (long k = 1; k < (long)p; k++) r = ladder_mul(c, r, base, w, pre);
      return r;
    }
    return ladder_power(c, a, b.l, pre);
  }
  if (eq(op, "+") || eq(op, "-")) {
    V p = embed(a, w), q = embed(b, w);
    for (int k = 0; k < w; k++) x[k] = F(c, "((%s) %s (%s))", p.x[k], op, q.x[k]);
    return vlike(p, w, x);
  }
  if (eq(op, "*") && (a.w == 1 || b.w == 1)) {             /* a number scales, as it sits inside */
    V num = mono(once(c, pre, a.w == 1 ? a.l : b.l)), val = a.w == 1 ? b : a;
    for (int k = 0; k < w; k++) x[k] = a.w == 1 ? F(c, "((%s) * (%s))", num.l, val.x[k]) : F(c, "((%s) * (%s))", val.x[k], num.l);
    return vlike(val, w, x);
  }
  if (eq(op, "*")) return ladder_mul(c, a, b, w, pre);
  if (b.w == 1) {                                          /* over a number: x / 0 is 0, as everywhere */
    const char* d = once(c, pre, b.l);
    for (int k = 0; k < w; k++) x[k] = F(c, "((%s) == 0.0 ? 0.0 : (%s) / (%s))", d, a.x[k], d);
    return vlike(a, w, x);
  }
  /* a over b: a conj(b) over the size of b squared, 0 where b is 0 */
  V q = capture(c, pre, embed(b, w));
  const char* cj[4];
  cj[0] = q.l;
  for (int k = 1; k < w; k++) cj[k] = F(c, "(-(%s))", q.x[k]);
  Buf size = {0};
  for (int k = 0; k < w; k++) badd(c, &size, "%s%s * %s", k ? " + " : "", q.x[k], q.x[k]);
  const char* d = once(c, pre, F(c, "(%s)", bstr(&size)));
  V top = ladder_mul(c, a, vwide(w, cj), w, pre);
  for (int k = 0; k < w; k++) x[k] = F(c, "((%s) == 0.0 ? 0.0 : (%s) / (%s))", d, top.x[k], d);
  return vlike(top, w, x);
}

static V binop_at(Ctx* c, Scope* sc, Node* n, V a, V b, Buf* pre) {
  /* Arithmetic at each place, when a place holds more than a number. */
  if ((is_field(a) || is_field(b)) && (a.lad || b.lad || a.w == 4 || b.w == 4)) {
    a = capture(c, pre, a);
    b = capture(c, pre, b);
    V p = binop_at(c, sc, n, place(a, 0), place(b, 0), pre);
    V q = binop_at(c, sc, n, place(a, 1), place(b, 1), pre);
    return two_places(c, p, q, n);
  }
  if (a.lad || b.lad) return ladder_op(c, sc, n, a, b, pre);
  const char* op = n->str;
  const char* fmt = NULL;
  if (eq(op, "^")) {
    long k;
    if (!is_quad(a) && small_power(c, sc, n->kids[1], &k)) {
      if (k >= 2) { a.l = once(c, pre, a.l); a.r = is_pair(a) ? once(c, pre, a.r) : a.l; }
      if (!is_pair(a)) return mono(power_of(c, a.l, k));
      return vpair(power_of(c, a.l, k), power_of(c, a.r, k));
    }
    fmt = "pow((%1$s), (%2$s))";
  }
  else if (eq(op, "+")) fmt = "((%1$s) + (%2$s))";
  else if (eq(op, "-")) fmt = "((%1$s) - (%2$s))";
  else if (eq(op, "*")) fmt = "((%1$s) * (%2$s))";
  else if (eq(op, "/")) fmt = "((%2$s) == 0.0 ? 0.0 : (%1$s) / (%2$s))";
  else if (eq(op, "mod")) fmt = "((%2$s) == 0.0 ? 0.0 : (%1$s) - floor((%1$s) / (%2$s)) * (%2$s))";
  else if (eq(op, "==")) fmt = "((((%1$s) == (%2$s))) ? 1.0 : 0.0)";
  else if (eq(op, "!=")) fmt = "((((%1$s) != (%2$s))) ? 1.0 : 0.0)";
  else if (eq(op, "<")) fmt = "((((%1$s) < (%2$s))) ? 1.0 : 0.0)";
  else if (eq(op, ">")) fmt = "((((%1$s) > (%2$s))) ? 1.0 : 0.0)";
  else if (eq(op, "<=")) fmt = "((((%1$s) <= (%2$s))) ? 1.0 : 0.0)";
  else if (eq(op, ">=")) fmt = "((((%1$s) >= (%2$s))) ? 1.0 : 0.0)";
  /* `and` and `or` don't short-circuit: both sides always run. */
  else if (eq(op, "and")) fmt = "((((%1$s) != 0.0) & ((%2$s) != 0.0)) ? 1.0 : 0.0)";
  else if (eq(op, "or")) fmt = "((((%1$s) != 0.0) | ((%2$s) != 0.0)) ? 1.0 : 0.0)";
  else fail(c, "bad binop: %s %s", op, err_loc(c, n));
  int w = a.w > b.w ? a.w : b.w;
  if (w == 1) return mono(F(c, fmt, a.l, b.l));
  if (a.w == 1) a = mono(once(c, pre, a.l));
  if (b.w == 1) b = mono(once(c, pre, b.l));
  const char* x[4];
  for (int k = 0; k < w; k++) x[k] = F(c, fmt, at(a, k), at(b, k));
  return vwide(w, x);
}

static V emit_binop(Ctx* c, Scope* sc, Node* n, Buf* pre) {
  V ab[2];
  emit_seq(c, sc, n->kids, 2, ab, pre);
  return binop_at(c, sc, n, ab[0], ab[1], pre);
}

static V emit_call(Ctx* c, Scope* sc, Node* n, Buf* pre);

static V emit(Ctx* c, Scope* sc, Node* n, Buf* pre) {
  switch (n->kind) {
  case nkNum: return mono(num_lit(c, n->num));
  case nkIdent: return emit_ident(c, sc, n);
  case nkUnary: {
    V v = emit(c, sc, n->kids[0], pre);
    const char* fmt;
    if (eq(n->str, "-")) fmt = "(-(%s))";
    else if (eq(n->str, "not")) fmt = "((%s) == 0.0 ? 1.0 : 0.0)";
    else { fail(c, "bad unary: %s %s", n->str, err_loc(c, n)); return mono(""); }
    if (!eq(n->str, "-")) no_quad(c, v, "`not`", n);
    const char* x[4];
    if (v.lad && !eq(n->str, "-")) fail(c, "`not` takes a number, and is given %s %s", given(c, n->kids[0], v), err_loc(c, n));
    for (int k = 0; k < v.w; k++) x[k] = F(c, fmt, v.x[k]);
    return vlike(v, v.w, x);
  }
  case nkBinOp: return emit_binop(c, sc, n, pre);
  case nkIf: return emit_if(c, sc, n, pre);
  case nkCall: return emit_call(c, sc, n, pre);
  case nkArr: {
    if (n->nkids == 2) {
      V lr[2];
      emit_seq(c, sc, n->kids, 2, lr, pre);
      if (is_value_lit(n)) {               /* one value */
        if (is_quad(lr[0]) || is_quad(lr[1]))
          fail(c, "a pair of pairs of pairs %s: one more doubling would be the octonions, whose product doesn't "
               "associate, so rotations made of them wouldn't compose. Values nest once", err_loc(c, n));
        if (is_field(lr[0]) || is_field(lr[1]))
          fail(c, "(a, b) of two places %s: a value holds numbers, at one place. Name the sides, let [l, r] = ... "
               , err_loc(c, n));
        if (!is_pair(lr[0]) && !is_pair(lr[1])) { V z = vpair(lr[0].l, lr[1].l); z.lad = 1; return z; }
        V a = embed(lr[0], 2), b = embed(lr[1], 2);      /* a number sits inside as (r, 0) */
        V q = vquad(a.l, a.r, b.l, b.r);
        q.lad = 1;
        return q;
      }
      if (is_field(lr[0]) || is_field(lr[1]))            /* two places: each holds a value */
        fail(c, "[a, b] of [a, b] %s: places hold numbers or values, not places. One of Hamilton's numbers is "
             "((w, x), (y, z))", err_loc(c, n));
      return two_places(c, lr[0], lr[1], n);
    }
    fail(c, "an array of %d can't be a value here %s — two places are [l, r], a value (a, b); a table goes in a let, or to wave",
         n->nkids, err_loc(c, n));
    return mono("");
  }
  case nkStr:
    fail(c, "a string is only an import path here %s", err_loc(c, n));
    return mono("");
  case nkImport:
    fail(c, "import belongs at the top of the file %s", err_loc(c, n));
    return mono("");
  case nkLambda:
    fail(c, "lambda `%s => ...` can only appear as a builtin argument (e.g. sum(N, n => expr)), "
         "not as a standalone value %s", n->nparams > 0 ? n->params[0] : "_", err_loc(c, n));
    return mono("");
  case nkIdx: {
    Node* a = n->kids[0];
    Node* i = n->kids[1];
    if (a->kind == nkIdent) {
      Ent* sa = lookup_sarray(c, sc, a->str);
      double kv;
      const char* x[4];
      if (sa && const_value(c, sc, i, &kv)) {   /* an index known now: the element, or 0 past the ends */
        long k = (long)kv;
        int w = ew(sa);
        for (int j = 0; j < w; j++)
          x[j] = k < 0 || k >= sa->n ? "0.0" : F(c, "s->pool[%s + %ld]", sa->a, 1 + w * k + j);
        return elem_value(sa, w, x);
      }
      if (sa) {
        const char* ix = emit1(c, sc, i, pre, "an index");
        const char* t = fresh(c, "ix");
        badd(c, pre, "  long %s = (long)(%s);\n", t, ix);
        int w = ew(sa);                    /* past either end, 0 */
        for (int j = 0; j < w; j++)
          x[j] = w == 1 ? F(c, "((%s >= 0 && %s < %ld) ? s->pool[%s + 1 + %s] : 0.0)", t, t, sa->n, sa->a, t)
                        : F(c, "((%s >= 0 && %s < %ld) ? s->pool[%s + %d + %d * %s] : 0.0)", t, t, sa->n, sa->a, 1 + j, w, t);
        return elem_value(sa, w, x);
      }
      Ent* la = lookup_larray(c, sc, a->str);
      Ent* found = la ? la : lookup_array(c, sc, a->str);
      if (found && found->edged && found->n > 0 && const_value(c, sc, i, &kv)) {
        long k = (long)kv;           /* made by array(): past either end, 0 */
        int w = ew(found);
        for (int j = 0; j < w; j++) x[j] = k < 0 || k >= found->n ? "0.0" : F(c, "%s[%ld]", found->a, w * k + j);
        return elem_value(found, w, x);
      }
      if (found && found->edged) {
        const char* ix = emit1(c, sc, i, pre, "an index");
        const char* t = fresh(c, "ix");
        badd(c, pre, "  long %s = (long)(%s);\n", t, ix);
        int w = ew(found);
        for (int j = 0; j < w; j++)
          x[j] = w == 1 ? F(c, "((%s >= 0 && %s < %ld) ? %s[%s] : 0.0)", t, t, found->n, found->a, t)
                        : F(c, "((%s >= 0 && %s < %ld) ? %s[%d * %s + %d] : 0.0)", t, t, found->n, found->a, w, t, j);
        return elem_value(found, w, x);
      }
      if (found && found->n > 0 && const_value(c, sc, i, &kv)) {
        long k = (long)kv % found->n;
        if (k < 0) k += found->n;
        int w = ew(found);
        for (int j = 0; j < w; j++) x[j] = F(c, "%s[%ld]", found->a, w * k + j);
        return elem_value(found, w, x);
      }
      if (found) {     /* local arrays and tables wrap round, as a table should */
        const char* ix = emit1(c, sc, i, pre, "an index");
        const char* t = fresh(c, "ix");
        badd(c, pre, "  long %s = (long)(%s) %% %ld; if (%s < 0) %s += %ld;\n", t, ix, found->n, t, t, found->n);
        int w = ew(found);
        for (int j = 0; j < w; j++) x[j] = w == 1 ? F(c, "%s[%s]", found->a, t) : F(c, "%s[%d * %s + %d]", found->a, w, t, j);
        return elem_value(found, w, x);
      }
    }
    /* A pair indexes like a two-entry table: the left side for even i,
     * the right for odd. A pair of pairs gives its halves, each a pair. */
    V v = emit(c, sc, a, pre);
    if (v.lad && !eq(n->str, "()"))
      fail(c, "`%s` is %s, and a value has no places to index %s: name its parts, let (a, b) = %s",
           written(c, a), shape_of(v), err_loc(c, n), written(c, a));
    if (is_quad(v)) {
      double kv;
      if (const_value(c, sc, i, &kv)) {
        long k = (long)kv % 2;
        if (k < 0) k += 2;
        return half_of(v, (int)k);
      }
      const char* ix = emit1(c, sc, i, pre, "an index");
      const char* t = fresh(c, "ix");
      badd(c, pre, "  long %s = (long)(%s) %% 2;\n", t, ix);
      return vpair(F(c, "((%s == 0) ? (%s) : (%s))", t, v.l, v.l2), F(c, "((%s == 0) ? (%s) : (%s))", t, v.r, v.r2));
    }
    if (i->kind == nkNum) {
      long k = (long)i->num % 2;
      if (k < 0) k += 2;
      return mono(at(v, (int)k));
    }
    const char* ix = emit1(c, sc, i, pre, "an index");
    const char* t = fresh(c, "ix");
    badd(c, pre, "  long %s = (long)(%s) %% 2;\n", t, ix);
    return mono(F(c, "((%s == 0) ? (%s) : (%s))", t, v.l, v.r));
  }
  default:
    fail(c, "cannot emit expression node: %s %s", KIND_NAMES[n->kind], err_loc(c, n));
    return mono("");
  }
}

/* Does the expression do costly maths (exp, pow, sin ...), directly or
 * through a pure def it calls? */
static int costly(Ctx* c, Node* n) {
  if (!n) return 0;
  if (n->kind == nkBinOp && eq(n->str, "^")) {
    double v;
    long k = 0;
    if (!(const_value(c, NULL, n->kids[1], &v) && (k = (long)v) == v && k >= 0 && k <= 4)) return 1;
  }
  if (n->kind == nkCall) {
    if (in(n->str, HOISTED_MATH) && !user_def(c, n->str)) return 1;
    Node* d = user_def(c, n->str);
    int seen = 0;
    for (int i = 0; i < pure_depth; i++) if (pure_path[i] == d) seen = 1;
    if (d && !seen && pure_depth < 64 && !d->is_rec && d->kids && d->kids[0] && d->kids[0]->kind != nkBlock) {
      pure_path[pure_depth++] = d;
      int r = costly(c, d->kids[0]);
      pure_depth--;
      if (r) return 1;
    }
  }
  for (int i = 0; i < n->nkids; i++) if (costly(c, n->kids[i])) return 1;
  return 0;
}

/* A pair operation's arguments, each emitted once and fixed, so the
 * operation can look at their shapes (number, pair, pair of pairs)
 * before choosing its formula. */
static V* emit_operands(Ctx* c, Scope* sc, Node* n, Buf* pre, int* any_quad) {
  V* v = al(c, (n->nkids + 1) * sizeof(V));
  *any_quad = 0;
  for (int i = 0; i < n->nkids; i++) {
    v[i] = capture(c, pre, emit(c, sc, n->kids[i], pre));
    if (is_quad(v[i])) *any_quad = 1;
  }
  return v;
}

/* Hamilton's product of two quaternions (a, b)(c, d), the doubling formula
 * written out. Each operand must be one word: it is read several times. */
static V hamilton(Ctx* c, V p, V q) {
  const char *w1 = p.l, *x1 = p.r, *y1 = p.l2, *z1 = p.r2, *w2 = q.l, *x2 = q.r, *y2 = q.l2, *z2 = q.r2;
  return vquad(F(c, "(%s * %s - %s * %s - %s * %s - %s * %s)", w1, w2, x1, x2, y1, y2, z1, z2),
               F(c, "(%s * %s + %s * %s + %s * %s - %s * %s)", w1, x2, x1, w2, y1, z2, z1, y2),
               F(c, "(%s * %s - %s * %s + %s * %s + %s * %s)", w1, y2, x1, z2, y1, w2, z1, x2),
               F(c, "(%s * %s + %s * %s - %s * %s + %s * %s)", w1, z2, x1, y2, y1, x2, z1, w2));
}

static V emit_quad_op(Ctx* c, Node* n, V* q, Buf* pre) {
  const char* f = n->str;
  /* each quaternion as w + x i + y j + z k: (a, b) = (w + x i, y + z i) */
  const char *w1 = q[0].l, *x1 = q[0].r, *y1 = q[0].l2, *z1 = q[0].r2;
  if (eq(f, "conj")) return vquad(w1, F(c, "(-(%s))", x1), F(c, "(-(%s))", y1), F(c, "(-(%s))", z1));
  if (eq(f, "amplitude"))
    return mono(F(c, "sqrt(%s * %s + %s * %s + %s * %s + %s * %s)", w1, w1, x1, x1, y1, y1, z1, z1));
  if (eq(f, "phase") || eq(f, "axis")) {
    /* Hamilton: q = T q (cos t + e sin t), t his angle (0..pi), e his axis. */
    const char* vn = fresh(c, "qv");
    badd(c, pre, "  double %s = sqrt(%s * %s + %s * %s + %s * %s);\n", vn, x1, x1, y1, y1, z1, z1);
    if (eq(f, "phase")) return mono(F(c, "atan2(%s, %s)", vn, w1));
    return vquad("0.0", F(c, "(%s == 0.0 ? 0.0 : %s / %s)", vn, x1, vn), F(c, "(%s == 0.0 ? 0.0 : %s / %s)", vn, y1, vn),
                 F(c, "(%s == 0.0 ? 1.0 : %s / %s)", vn, z1, vn));
  }
  fail(c, "%s isn't a reader of a value %s", f, err_loc(c, n));
  return mono("");
}

/* conj and the readers of a value, on an operand emitted once. */
static V turning_op(Ctx* c, Node* n, const char* name, V* ops, Buf* pre) {
  if (n->nkids != 1 || !ops[0].lad)
    fail(c, "%s reads one value, (a, b) or ((w, x), (y, z)), and is given %s %s%s", name,
         n->nkids ? givens(c, n->kids, ops, n->nkids) : "nothing", err_loc(c, n),
         n->nkids && is_field(ops[0]) ? ": two places aren't a value, so name the sides and write (l, r)" : "");
  if (is_quad(ops[0])) return emit_quad_op(c, n, ops, pre);
  const char *a = ops[0].l, *b = ops[0].r;
  if (eq(name, "axis"))
    fail(c, "axis(q) is the axis of one of Hamilton's numbers (his Ax q), and is given %s %s",
         given(c, n->kids[0], ops[0]), err_loc(c, n));
  if (eq(name, "conj")) return vpair(a, F(c, "(-(%s))", b));
  if (eq(name, "amplitude")) return mono(F(c, "sqrt(%s * %s + %s * %s)", a, a, b, b));
  return mono(F(c, "atan2(%s, %s)", b, a));
}

static V emit_call(Ctx* c, Scope* sc, Node* n, Buf* pre) {
  /* A def that is costly maths over invariant arguments (a frequency from
   * a helper: bandf(i), with i a number): its expression, once a block. */
  if (c->hoist_on && !c->in_hoist && pure_def(c, sc, n) && costly(c, n) &&
      is_invariant(c, sc, n) && !is_const(c, sc, n))
    return mono(hoist(c, emit_invariant(c, sc, n)));
  /* Costly pure maths over invariant inputs: once a block. */
  if (c->hoist_on && !c->in_hoist && in(n->str, HOISTED_MATH) && !user_def(c, n->str) &&
      !lookup_fn(sc, n->str)[0] && !lookup_lambda(sc, n->str) && is_invariant(c, sc, n) &&
      !is_const(c, sc, n))
    return mono(hoist(c, emit_invariant(c, sc, n)));
  /* A lambda-valued param: its body inline. */
  Ent* le = lookup_lambda_ent(sc, n->str);
  if (le) {
    Node* lam = le->node;
    if (n->nkids < lam->nparams)
      fail(c, "lambda '%s' takes %d args, got %d %s", n->str, lam->nparams, n->nkids, err_loc(c, n));
    Scope* inner = push(c, le->sc ? le->sc : sc);   /* the scope that wrote it */
    for (int i = 0; i < lam->nparams; i++) {
      V v = emit(c, sc, n->kids[i], pre);
      bind_value(c, inner, lam->params[i], v, is_pair(v), is_pair(v) ? NULL : n->kids[i], sc, pre);
    }
    Node* body = lam->kids[0];
    return body->kind == nkBlock ? emit_block(c, inner, body, pre) : emit(c, inner, body, pre);
  }
  const char* resolved = lookup_fn(sc, n->str);
  const char* name = resolved[0] ? resolved : n->str;
  /* sin and cos of a steady TAU * phasor(f), or of the clock at a steady
   * rate: a turning point's sides. */
  if ((eq(name, "sin") || eq(name, "cos")) && n->nkids == 1 && !user_def(c, name) && c->hoist_on && !c->in_hoist) {
    Node* a = n->kids[0];
    const char *re = NULL, *im = NULL;
    if (a->kind == nkIdent) {
      Ent* t = find_turner(c, sc, a->str);
      if (t) { re = t->a; im = t->b; }
    } else {
      Node* f = turning_freq(c, sc, a);
      if (f) emit_turner(c, sc, f, pre, &re, &im);
      else if (!turning_off()) {
        const char* rate = clock_rate(c, sc, a);
        if (rate && !eq(rate, "0.0")) emit_clock_turner(c, sc, a, rate, pre, &re, &im);
      }
    }
    if (re) return mono(eq(name, "cos") ? re : im);
  }
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
    if (N == 0) return mono("0.0");
    const char* param = lamarg->params[0];
    Node* body = lamarg->kids[0];
    if (can_loop(c, body, param, N)) return emit_voice_loop(c, sc, n, body, param, N, pre);
    V* terms = al(c, N * sizeof(V));
    for (long k = 1; k <= N; k++) {
      Scope* iter = push(c, sc);
      lput(c, &iter->names, param)->a = num_lit(c, (double)k);
      lput(c, &iter->numlits, param)->v = (double)k;
      Buf own = {0};
      terms[k - 1] = body->kind == nkBlock ? emit_block(c, iter, body, &own) : emit(c, iter, body, &own);
      if (own.n) for (long j = 0; j < k - 1; j++) terms[j] = capture(c, pre, terms[j]);
      bcat(c, pre, bstr(&own));
    }
    int w, lad;
    common_shape(c, terms, N, &w, &lad, n);
    for (long k = 0; k < N; k++) terms[k] = as_shape(c, terms[k], w, lad);
    Buf q[4] = {{0}};
    long kept = 0;
    for (long k = 0; k < N; k++) {        /* a term that is 0 (a folded-away branch) adds nothing */
      int zero = 1;
      for (int m = 0; m < w; m++) zero = zero && (eq(at(terms[k], m), "0.0") || eq(at(terms[k], m), "0.0000000000000000"));
      if (zero && (kept || k < N - 1)) continue;
      for (int m = 0; m < w; m++) { if (kept) bcat(c, &q[m], " + "); bcat(c, &q[m], at(terms[k], m)); }
      kept++;
    }
    const char* x[4];
    for (int m = 0; m < w; m++) x[m] = F(c, "(%s)", bstr(&q[m]));
    V total = vwide(w, x);
    total.lad = lad && w > 1;
    return total;
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
    for (long k = 0; k < N; k++) {
      Scope* iter = push(c, sc);
      lput(c, &iter->names, l->params[0])->a = num_lit(c, (double)k);
      lput(c, &iter->numlits, l->params[0])->v = (double)k;
      Node* b = l->kids[0];
      if (b->kind == nkBlock) {
        for (int i = 0; i < b->nkids - 1; i++) emit_local_stmt(c, iter, b->kids[i], pre);
        Node* last = b->kids[b->nkids - 1];
        if (!(last->kind == nkNum && last->num == 0.0)) emit(c, iter, last, pre);
      } else {
        emit(c, iter, b, pre);
      }
    }
    c->each_depth--;
    bcat(c, pre, bstr(&c->deferred));      /* the step lands together */
    c->deferred = saved;
    return mono("0.0");
  }
  if (eq(name, "len")) {
    if (n->nkids == 1 && n->kids[0]->kind == nkIdent) {
      const char* a = n->kids[0]->str;
      Ent* e;
      if ((e = lookup_sarray(c, sc, a))) return mono(num_lit(c, (double)e->n));
      if ((e = lookup_larray(c, sc, a))) return mono(num_lit(c, (double)e->n));
      if ((e = lookup_array(c, sc, a))) return mono(num_lit(c, (double)e->n));
    }
    fail(c, "len takes the name of an array %s", err_loc(c, n));
  }
  if (eq(name, "array"))
    fail(c, "array(N) declares an array: `$f = array(N)` for state that lasts, `let a = array(N)` "
         "for this sample's scratch %s", err_loc(c, n));
  if (eq(name, "field_places")) {
    double v;
    if (!const_value(c, sc, n, &v))
      fail(c, "a ring's period has to be known when the patch compiles, as its places are, and is given `%s` %s",
           n->nkids ? written(c, n->kids[0]) : "nothing", err_loc(c, n));
    return mono(num_lit(c, v));
  }
  if (eq(name, "noise")) return mono(emit_noise(c, pre));
  if ((eq(name, "turn") || eq(name, "unturn")) && !user_def(c, name))
    fail(c, "%s is `%s` now %s: multiplying is turning, on values written (a, b)", name, eq(name, "turn") ? "*" : "/",
         err_loc(c, n));
  if (eq(name, "conj") || eq(name, "amplitude") || eq(name, "phase") || eq(name, "axis")) {
    /* Emitted once; a pair of pairs among them takes Hamilton's formula. */
    int anyq;
    V* ops = emit_operands(c, sc, n, pre, &anyq);
    V r = turning_op(c, n, name, ops, pre);
    for (int i = 0; i < n->nkids; i++) if (ops[i].lad && r.w > 1) r.lad = 1;
    return r;
  }
  if (in(name, PAIR_CALLS)) {          /* turn, unturn and conj are above */
    const char* f = name;
    if (eq(f, "analytic")) {
      const char** a = pair_operands(c, sc, n, pre, 1, "one signal");
      const char* off_a = register_region(c, "hilbert_a", 17, NULL);
      const char* off_b = register_region(c, "hilbert_b", 16, NULL);
      const char* re = fresh(c, "an_re");
      const char* im = fresh(c, "an_im");
      badd(c, pre, "  double %s = n_hilbert_a((DspState*)s, (long)(%s), %s);\n", re, off_a, a[0]);
      badd(c, pre, "  double %s = -n_hilbert_b((DspState*)s, (long)(%s), %s);\n", im, off_b, a[0]);
      V z = vpair(re, im);
      z.lad = 1;
      return z;
    }
    pair_operands(c, sc, n, pre, 0, "no arguments");     /* input */
    return vpair("aither_in_l", "aither_in_r");
  }
  if (in(name, PAIR_READERS)) {
    const char** a = pair_operands(c, sc, n, pre, 2, "one pair (or two numbers)");
    if (eq(name, "amplitude")) return mono(F(c, "sqrt(%s * %s + %s * %s)", a[0], a[0], a[1], a[1]));
    return mono(F(c, "atan2(%s, %s)", a[1], a[0]));
  }
  /* A medium's natives (dsp.c, the field core's): the first argument is
   * the medium's state array, passed as a pointer to its elements, the rest
   * numbers. The core writes these (medium/system.c); a patch doesn't. */
  if (!strncmp(name, "medium_", 7)) {
    Node* arr = n->nkids ? n->kids[0] : NULL;
    Ent* sa = arr && arr->kind == nkIdent ? lookup_sarray(c, sc, arr->str) : NULL;
    if (!sa) fail(c, "%s takes a medium's state array first %s", name, err_loc(c, n));
    Buf args = {0};
    badd(c, &args, "s->pool + %s + 1 + %ld", sa->a, sa->wo);
    for (int i = 1; i < n->nkids; i++) badd(c, &args, ", %s", emit1(c, sc, n->kids[i], pre, "a number"));
    const char* t = fresh(c, "md");
    badd(c, pre, "  double %s = n_%s(%s);\n", t, name, bstr(&args));
    return mono(t);
  }
  if (eq(name, "wave")) {
    if (n->nkids != 2) fail(c, "wave takes 2 args %s", err_loc(c, n));
    V freq = emit(c, sc, n->kids[0], pre);
    Node* arr = n->kids[1];
    WaveTable w;
    if (arr->kind == nkIdent) {
      Ent* found = lookup_array(c, sc, arr->str);
      if (!found)
        fail(c, "wave: %s must be a constant table of numeric literals%s %s", arr->str,
             lookup_larray(c, sc, arr->str) ? " (it holds runtime values)" : "", err_loc(c, arr));
      w.sym = found->a; w.length = found->n;
    } else if (arr->kind == nkArr) {
      for (int i = 0; i < arr->nkids; i++)
        if (arr->kids[i]->kind != nkNum) fail(c, "wave array must be numeric literals only %s", err_loc(c, arr->kids[i]));
      w.sym = static_table(c, fresh(c, "arr"), arr, NULL);
      w.length = arr->nkids;
    } else {
      fail(c, "wave: second arg must be an array literal or a let binding to one %s", err_loc(c, arr));
      return mono("");
    }
    V a[2] = {freq, mono("0.0")};      /* the table is no value: per_side reads one per kid */
    return per_side(c, sc, n, a, is_pair(freq), s_wave, pre, &w);
  }
  /* Builtins and natives: arguments first, then once, or per side. */
  int any = 0;
  if (eq(name, "phasor")) {
    if (n->nkids != 1) fail(c, "phasor takes 1 arg %s", err_loc(c, n));
    V* a = emit_args(c, sc, n, pre, &any);
    return per_side(c, sc, n, a, any, s_phasor, pre, NULL);
  }
  if (in(name, LIBM1) && n->nkids == 1) { V* a = emit_args(c, sc, n, pre, &any); return map_pure(c, name, a, 1, any, b_fn1, pre, n); }
  if (eq(name, "abs") && n->nkids == 1) { V* a = emit_args(c, sc, n, pre, &any); return map_pure(c, name, a, 1, any, b_abs, pre, n); }
  if ((eq(name, "saw") || eq(name, "tri") || eq(name, "sqr")) && n->nkids == 1) {
    V* a = emit_args(c, sc, n, pre, &any);
    return map_pure(c, name, a, 1, any, b_shape, pre, n);
  }
  if (eq(name, "pow") && n->nkids == 2) { V* a = emit_args(c, sc, n, pre, &any); return map_pure(c, name, a, 2, any, b_pow, pre, n); }
  if (eq(name, "log2") || eq(name, "log10"))
    fail(c, "%s is log(x, %s): one log, its base an argument %s", name, eq(name, "log2") ? "2" : "10", err_loc(c, n));
  if (eq(name, "log") && n->nkids == 2) {
    double b;
    int k = const_value(c, sc, n->kids[1], &b);
    V* a = emit_args(c, sc, n, pre, &any);
    if (k && b == 2.718281828459045) return map_pure(c, name, a, 1, any, b_fn1, pre, n);   /* the default: log(x) */
    return map_pure(c, name, a, 2, any, k && b == 2 ? b_log2 : k && b == 10 ? b_log10 : b_logb, pre, n);
  }
  if ((eq(name, "min") || eq(name, "max")) && n->nkids >= 2) {
    V* a = emit_args(c, sc, n, pre, &any);
    return map_pure(c, name, a, n->nkids, any, b_minmax, pre, n);
  }
  if (eq(name, "clamp") && n->nkids == 3) { V* a = emit_args(c, sc, n, pre, &any); return map_pure(c, name, a, 3, any, b_clamp, pre, n); }
  if (eq(name, "int") && n->nkids == 1) { V* a = emit_args(c, sc, n, pre, &any); return map_pure(c, name, a, 1, any, b_int, pre, n); }
  if (eq(name, "midi_trig") && n->nkids == 1) {
    V* a = emit_args(c, sc, n, pre, &any);
    return per_side(c, sc, n, a, any, s_midi_trig, pre, NULL);
  }
  if (((eq(name, "midi_cc") || eq(name, "midi_note") || eq(name, "midi_voice_freq") ||
        eq(name, "midi_voice_gate") || eq(name, "midi_voice_vel") || eq(name, "midi_voice_key") ||
        eq(name, "midi_voice_press") || eq(name, "midi_voice_bend") || eq(name, "midi_voice_on")) &&
       n->nkids == 1) ||
      (eq(name, "midi_voice_cc") && n->nkids == 2) ||
      ((eq(name, "midi_freq") || eq(name, "midi_gate") || eq(name, "midi_vel")) && n->nkids == 0)) {
    /* What MIDI holds is steady over a block (is_invariant), so with
     * steady arguments it is read once a block: a voice loop's per voice,
     * into an array, instead of an atomic read per voice per sample. */
    int steady = c->hoist_on && !c->in_hoist;
    for (int i = 0; i < n->nkids && steady; i++) steady = is_invariant(c, sc, n->kids[i]);
    if (steady) {
      const char** x = al(c, (n->nkids + 1) * sizeof(char*));
      for (int i = 0; i < n->nkids; i++) x[i] = emit_invariant(c, sc, n->kids[i]);
      if (n->nkids == 0 || x[0]) return mono(hoist(c, b_midi(c, name, x, n->nkids)));
    }
    V* a = emit_args(c, sc, n, pre, &any);
    return map_pure(c, name, a, n->nkids, any, b_midi, pre, n);
  }
  if (in(name, MIDI_FNS)) fail(c, "%s: wrong arg count %s", name, err_loc(c, n));
  /* Hoisted natives: the coefficients once a block, the step per sample. */
  if (c->hoist_on && (eq(name, "lp1") || eq(name, "hp1")) && n->nkids == 2 && is_invariant(c, sc, n->kids[1])) {
    Hoisted h = {hoist(c, F(c, "n_lp1_coef(%s, s->sr)", emit_invariant(c, sc, n->kids[1])))};
    V* a = emit_args(c, sc, n, pre, &any);
    return per_side(c, sc, n, a, is_pair(a[0]), s_lp1_c, pre, &h);
  }
  if (c->hoist_on && (eq(name, "lpf") || eq(name, "hpf") || eq(name, "bpf") || eq(name, "notch")) &&
      n->nkids == 3 && is_invariant(c, sc, n->kids[1]) && is_invariant(c, sc, n->kids[2])) {
    const char* k1 = emit_invariant(c, sc, n->kids[1]);
    const char* k2 = emit_invariant(c, sc, n->kids[2]);
    Hoisted h = {hoist_arr(c, 4, F(c, "n_svf_coef(%s, %s, s->sr, $H)", k1, k2))};
    V* a = emit_args(c, sc, n, pre, &any);
    return per_side(c, sc, n, a, is_pair(a[0]), s_lp1_c, pre, &h);
  }
  if (c->hoist_on && eq(name, "reverb") && n->nkids == 3 && is_invariant(c, sc, n->kids[1])) {
    Hoisted h = {hoist_arr(c, 5, F(c, "n_reverb_coef(%s, s->sr, $H)", emit_invariant(c, sc, n->kids[1])))};
    V* a = emit_args(c, sc, n, pre, &any);
    return per_side(c, sc, n, a, is_pair(a[0]) || is_pair(a[2]), s_reverb_c, pre, &h);
  }
  const NativeSpec* spec = native(name);
  if (spec && !eq(name, "wave")) {
    int arity = native_arity(spec);
    if (n->nkids != arity) fail(c, "%s takes %d args, got %d %s", name, arity, n->nkids, err_loc(c, n));
    V* a = emit_args(c, sc, n, pre, &any);
    return per_side(c, sc, n, a, any, s_native, pre, NULL);
  }
  Node* def = user_def(c, name);
  if (def) {
    if (has(&c->recursive, name)) return emit_loop(c, sc, def, n, pre);
    return emit_def(c, sc, def, n, pre);
  }
  fail(c, "unknown function: %s %s", name, err_loc(c, n));
  return mono("");
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

/* A file-level `$x = init` (top level or play body): its name is known
 * from the start; whether it is a pair, from its first value. */
static void declare_var(Ctx* c, Node* n) {
  if (has(&c->play_set, n->str))
    fail(c, "'%s' is the name of a play block — choose a different variable name %s", n->str, err_loc(c, n));
  if (has(&c->file_vars, n->str)) return;
  incl(c, &c->file_vars, n->str);
  if (c->nvar_decls == c->cap_var_decls) {
    int cap = c->cap_var_decls ? c->cap_var_decls * 2 : 32;
    Node** v = al(c, cap * sizeof(Node*));
    if (c->nvar_decls) memcpy(v, c->var_decls, c->nvar_decls * sizeof(Node*));
    c->var_decls = v; c->cap_var_decls = cap;
  }
  c->var_decls[c->nvar_decls++] = n;
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

/* A file-level `$x`'s struct field(s): two for a pair. */
static void make_var(Ctx* c, const char* name, int w) {
  if (w == 1) {
    incl(c, &c->top_var_set, name);
    spush(c, &c->top_var_names, name);
    return;
  }
  incl(c, &c->pair_vars, name);
  if (w == 4) incl(c, &c->quad_vars, name);
  for (int k = 0; k < w; k++) spush(c, &c->top_var_names, pair_var_field(c, name, k));
}

static Node* file_var_decl(Ctx* c, const char* name) {
  for (int i = 0; i < c->nvar_decls; i++) if (eq(c->var_decls[i]->str, name)) return c->var_decls[i];
  return NULL;
}

/* Shape a file-level `$x` read before its declaration: from how its
 * value is written. */
static void decide_var(Ctx* c, Node* st) {
  Node* v = st->kids[0];
  int w = (v->kind == nkArr && v->nkids == 2) || (v->kind == nkCall && in(v->str, PAIR_CALLS)) ? 2 : 1;
  if (v->kind == nkArr && v->nkids == 2 && v->kids[0]->kind == nkArr && v->kids[1]->kind == nkArr) w = 4;
  make_var(c, st->str, w);
  if (is_value_lit(v)) incl(c, &c->lad_vars, st->str);
}

/* A file-level `$x = init`, where its first declaration is: a struct
 * field (two for a pair), set once from its first value. */
static const char* var_init(Ctx* c, Scope* sc, Node* st) {
  int first = 0;
  for (int i = 0; i < c->nvar_decls; i++) if (c->var_decls[i] == st) first = 1;
  if (!first) return "";
  Buf init_pre = {0};
  if (is_array_make(st->kids[0])) {
    declare_state_array(c, sc, st, &init_pre, 1);
    return sdup(c, bstr(&init_pre));
  }
  V v = emit(c, sc, st->kids[0], &init_pre);
  int decided = has(&c->pair_vars, st->str) || has(&c->top_var_set, st->str);
  if (!decided) {
    make_var(c, st->str, v.w);
    if (v.lad) incl(c, &c->lad_vars, st->str);
  }
  else if (has(&c->quad_vars, st->str) != is_quad(v))
    fail(c, "$%s is read above as %s, but its first value here is %s: write it the same way there, "
         "or declare it before it is read %s", st->str, is_quad(v) ? "less than a pair of pairs" : "a pair of pairs",
         is_quad(v) ? "a pair of pairs" : "not one", err_loc(c, st));
  else if (has(&c->pair_vars, st->str) != is_pair(v))
    fail(c, "$%s is read above as %s, but its first value here is %s: write it as [l, r] there, "
         "or declare it before it is read %s", st->str, is_pair(v) ? "a number" : "a pair",
         is_pair(v) ? "a pair" : "a number", err_loc(c, st));
  if (is_pair(v)) {
    int i0 = sfind(&c->top_var_names, pair_var_field(c, st->str, 0));
    Buf b = {0};
    badd(c, &b, "  if (!(s->inited[%d] & 1)) {\n%s", i0, bstr(&init_pre));
    for (int k = 0; k < v.w; k++) badd(c, &b, "    %s = (%s);\n", pair_var_ref(c, st->str, k), v.x[k]);
    for (int k = 0; k < v.w; k++) badd(c, &b, "    s->inited[%d] |= 1;\n", i0 + k);
    bcat(c, &b, "  }\n");
    return bstr(&b);
  }
  int i = sfind(&c->top_var_names, st->str);
  return F(c, "  if (!(s->inited[%d] & 1)) {\n%s    s->v_%s = (%s); s->inited[%d] |= 1;\n  }\n",
           i, bstr(&init_pre), st->str, v.l, i);
}

/* `$x = rhs` at file level (top or in a play). */
static void file_assign(Ctx* c, Scope* sc, Node* st, Buf* body) {
  if (has(&c->pair_vars, st->str)) {
    bcat(c, body, assign_wide(c, sc, st->str, file_var(c, st->str), st->kids[0]));
    return;
  }
  const char* target;
  if (has(&c->top_var_set, st->str)) target = F(c, "s->v_%s", st->str);
  else if (lookup(sc, st->str)[0]) target = lookup(sc, st->str);
  else if (has(&c->top_lets, st->str)) target = F(c, "l_%s", st->str);
  else if (has(&c->file_vars, st->str)) {
    decide_var(c, file_var_decl(c, st->str));
    file_assign(c, sc, st, body);
    return;
  } else {
    fail(c, "unknown assignment target '%s' %s", st->str, err_loc(c, st));
    return;
  }
  const char* rhs = emit1(c, sc, st->kids[0], body, F(c, "$%s", st->str));
  badd(c, body, "  %s = (%s);\n", target, rhs);
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

/* ---- tasks: which units (a profile slot each) can run apart ----
 *
 * A unit reads and writes the file's shared names: its `$state`, its
 * top-level lets, its plays. A unit needs every other unit that writes
 * a name it reads (one written later in the file is read a sample late,
 * still a need: the writer runs its block first). Units that need each
 * other, and units that write one name, are one task. Def bodies count,
 * through any depth. Nothing here changes the code: it tells `aither
 * profile` how the voice would split. */

static int shared_name(Ctx* c, const char* s) {
  return has(&c->file_vars, s) || has(&c->top_lets, s) || has(&c->play_set, s);
}

/* A unit inside a play: the play, and the names its own lets give. */
typedef struct { const char* play; Map* local; } Within;

static void uses(Ctx* c, Node* n, Map* bound, Use* u, Map* defs_seen, int in_def, Within* w);

static void def_uses(Ctx* c, const char* name, Use* u, Map* defs_seen) {
  Node* d = user_def(c, name);
  if (!d || has(defs_seen, name)) return;
  incl(c, defs_seen, name);
  Map bound = {{0}, 0};
  for (int i = 0; i < d->nparams; i++) incl(c, &bound, d->params[i]);
  for (int i = 0; i < d->nkids; i++) uses(c, d->kids[i], &bound, u, defs_seen, 1, NULL);
}

static void uses(Ctx* c, Node* n, Map* bound, Use* u, Map* defs_seen, int in_def, Within* w) {
  if (!n) return;
  switch (n->kind) {
  case nkIdent:
    if (has(bound, n->str)) break;
    if (w && has(w->local, n->str)) incl(c, &u->reads, F(c, "%s.%s", w->play, n->str));
    else if (shared_name(c, n->str)) incl(c, &u->reads, n->str);
    break;
  case nkLambda:
    for (int i = 0; i < n->nparams; i++) incl(c, bound, n->params[i]);
    break;
  case nkCall:
    def_uses(c, n->str, u, defs_seen);
    /* A medium's step and kick change its state array, and a reading must see
     * it sample by sample, not as a block left it: each counts as a write, so
     * every line that touches a medium is one task, in order. */
    if (!strncmp(n->str, "medium_", 7) && n->nkids && n->kids[0]->kind == nkIdent && shared_name(c, n->kids[0]->str))
      incl(c, &u->writes, n->kids[0]->str);
    break;
  case nkLet:
    /* its value is read before its name is bound */
    for (int i = 0; i < n->nkids; i++) uses(c, n->kids[i], bound, u, defs_seen, in_def, w);
    incl(c, bound, n->str);
    return;
  case nkVar:
    if (in_def) {
      for (int i = 0; i < n->nkids; i++) uses(c, n->kids[i], bound, u, defs_seen, in_def, w);
      incl(c, bound, n->str);
      return;
    }
    incl(c, &u->writes, n->str);
    break;
  case nkAssign: case nkIdxAssign:
    if (!has(bound, n->str) && shared_name(c, n->str)) {
      incl(c, &u->writes, n->str);
      if (n->kind == nkIdxAssign) incl(c, &u->reads, n->str);
    }
    break;
  default: break;
  }
  for (int i = 0; i < n->nkids; i++) uses(c, n->kids[i], bound, u, defs_seen, in_def, w);
}

static int task_root(int* up, int i) {
  while (up[i] != i) i = up[i] = up[up[i]];
  return i;
}

static void task_join(int* up, int a, int b) {
  a = task_root(up, a); b = task_root(up, b);
  if (a != b) up[a > b ? a : b] = a < b ? a : b;
}


static int array_rhs(Node* rhs) {
  if (!rhs) return 0;
  if (rhs->kind == nkArr) return rhs->nkids != 2 && rhs->nkids > 0 && !all_nums(rhs);
  return is_array_make(rhs);
}

static void collect_idx_targets(Ctx* c, Node* n, Map* out) {
  if (!n) return;
  if (n->kind == nkIdxAssign && n->str[0]) incl(c, out, n->str);
  for (int i = 0; i < n->nkids; i++) collect_idx_targets(c, n->kids[i], out);
}

static int play_nst(Node* play) {
  Node* bn = play->kids[0];
  return bn->kind == nkBlock ? bn->nkids : 1;
}

static Node* play_stmt(Node* play, int si) {
  Node* bn = play->kids[0];
  return bn->kind == nkBlock ? bn->kids[si] : bn;
}

/* The writer a unit reads `w`'s name from: the last one above it in the
 * file, as it stands after that unit this sample; with none above, the
 * last one of all, as it stood after it the sample before (`late`). */
static int pick_writer(Ent* w, int u, int* late) {
  int* ws = w->p;
  int wu = -1;
  for (int k = 0; k < w->n; k++) if (ws[k] < u) wu = ws[k];
  *late = wu < 0;
  return wu < 0 ? ws[w->n - 1] : wu;
}

/* ---- clustering ----
 *
 * The finest split gives a task to everything that can run apart. A
 * task costs a buffer per value crossing out of it and a pass over the
 * block, so on few cores the tasks are merged back by what they cost,
 * estimated from the tree: a node is 1, a transcendental 4, a def call
 * its body's cost, `sum`/`each` N times their body's. */
static double node_cost(Ctx* c, Node* n, int depth);

static double def_cost(Ctx* c, Node* d, int depth) {
  Ent* e = hfind(&c->unit_costs, d->str);
  if (e) return e->v;
  e = hput(c, &c->unit_costs, d->str);
  e->v = 1.0;  /* while its body is counted: a recursive def counts once */
  double v = 0.0;
  for (int i = 0; i < d->nkids; i++) v += node_cost(c, d->kids[i], depth + 1);
  e->v = v > 1.0 ? v : 1.0;
  return e->v;
}

static double node_cost(Ctx* c, Node* n, int depth) {
  if (!n || depth > 64) return 0.0;
  double v = 1.0;
  if (n->kind == nkCall) {
    Node* d = user_def(c, n->str);
    if (d) v += def_cost(c, d, depth);
    else if (eq(n->str, "sin") || eq(n->str, "cos") || eq(n->str, "exp") || eq(n->str, "log") ||
             eq(n->str, "tanh") || eq(n->str, "pow") || eq(n->str, "atan2") || eq(n->str, "tan") || eq(n->str, "erf"))
      v += 3.0;
    if ((eq(n->str, "sum") || eq(n->str, "each")) && n->nkids == 2) {
      double N;
      if (!const_value(c, c->top, n->kids[0], &N) || N < 1) N = 8;
      return v + node_cost(c, n->kids[0], depth + 1) + N * node_cost(c, n->kids[1], depth + 1);
    }
  }
  for (int i = 0; i < n->nkids; i++) v += node_cost(c, n->kids[i], depth + 1);
  return v;
}

/* The same estimate, and how much of it is inside repeated instances
 * (`sum`, `each`, with a count of 2 or more), for the graph report
 * (AITHER_GRAPH_REPORT, notes/graph-2026-11-02.md). */
/* Whether n, its defs followed, calls a native other than a phasor or a
 * sine: what a vector lane can't take as it stands. */
static int calls_native(Ctx* c, Node* n, int depth) {
  if (!n || depth > 64) return 0;
  if (n->kind == nkCall) {
    Node* d = user_def(c, n->str);
    if (d) { for (int i = 0; i < d->nkids; i++) if (calls_native(c, d->kids[i], depth + 1)) return 1; }
    else if (native(n->str) && !eq(n->str, "phasor")) return 1;
  }
  for (int i = 0; i < n->nkids; i++) if (calls_native(c, n->kids[i], depth + 1)) return 1;
  return 0;
}

static double inst_plain;   /* the graph report: instance cost with no natives */

static double inst_cost(Ctx* c, Node* n, int depth, int inside, double* in_inst) {
  if (!n || depth > 64) return 0.0;
  double v = 1.0;
  if (n->kind == nkCall) {
    Node* d = user_def(c, n->str);
    if (d) {
      double dv = 0.0;
      for (int i = 0; i < d->nkids; i++) dv += inst_cost(c, d->kids[i], depth + 1, inside, in_inst);
      v += dv;
    } else if (eq(n->str, "sin") || eq(n->str, "cos") || eq(n->str, "exp") || eq(n->str, "log") ||
               eq(n->str, "tanh") || eq(n->str, "pow") || eq(n->str, "atan2") || eq(n->str, "tan") || eq(n->str, "erf"))
      v += 3.0;
    if ((eq(n->str, "sum") || eq(n->str, "each")) && n->nkids == 2) {
      double N;
      if (!const_value(c, c->top, n->kids[0], &N) || N < 1) N = 8;
      double body = N * inst_cost(c, n->kids[1], depth + 1, 1, NULL);
      if (in_inst && N >= 2 && !inside) {
        *in_inst += body;
        if (!calls_native(c, n->kids[1], depth + 1)) inst_plain += body;
      }
      return v + inst_cost(c, n->kids[0], depth + 1, inside, in_inst) + body;
    }
  }
  for (int i = 0; i < n->nkids; i++) v += inst_cost(c, n->kids[i], depth + 1, inside, in_inst);
  return v;
}

static void graph_report(Ctx* c) {
  double total = 0.0, inst = 0.0;
  inst_plain = 0.0;
  double* w = al(c, (c->ntasks ? c->ntasks : 1) * sizeof(double));
  for (int i = 0; i < c->nunits; i++) {
    double k = inst_cost(c, c->units[i].st, 0, 0, &inst);
    total += k;
    w[c->unit_task[i]] += k;
  }
  double big = 0.0;
  for (int t = 0; t < c->ntasks; t++) if (w[t] > big) big = w[t];
  fprintf(stderr, "graph %.0f %.0f %.0f %d %.0f\n", total, inst, big, c->ntasks, inst_plain);
}

/* A task merges no further than this. A single huge function is slow
 * under TCC (bush_doof_e's blocks: 28 s in one task, 19 s in tasks of
 * about this size, per 10 s of sound on the uConsole) and no faster
 * under -O2; a patch below it is one task. */
enum { TASK_COST_CAP = 2000 };

/* What a task costs beyond its work, each run: being taken, its loop,
 * its buffers crossing cores. In the units of node_cost. */
enum { TASK_OVERHEAD = 20 };

typedef unsigned long long Bits;
#define BIT(m, i, j) ((m)[(size_t)(i) * words + (j) / 64] >> ((j) % 64) & 1)
#define SET(m, i, j) ((m)[(size_t)(i) * words + (j) / 64] |= 1ull << ((j) % 64))
#define CLR(m, i, j) ((m)[(size_t)(i) * words + (j) / 64] &= ~(1ull << ((j) % 64)))

typedef struct {
  int nt, words, cores;
  Bits* need;        /* need[a] has bit w: a reads what w writes */
  char* alive;
  double* cost;
  double* fin;       /* scratch: when each task can finish, cores unlimited */
  char* mark;        /* scratch: 0 not seen, 1 on the path, 2 done */
} Cl;

/* When x can finish with b merged into a (b < 0: as it is); -1 on a loop. */
static double cl_finish(Cl* g, int x, int a, int b) {
  int words = g->words;
  if (g->mark[x] == 2) return g->fin[x];
  if (g->mark[x] == 1) return -1.0;
  g->mark[x] = 1;
  double start = 0.0;
  for (int pass = 0; pass < (x == a && b >= 0 ? 2 : 1); pass++) {
    const Bits* row = g->need + (size_t)(pass ? b : x) * words;
    for (int k = 0; k < words; k++)
      for (Bits m = row[k]; m; m &= m - 1) {
        int y = k * 64 + __builtin_ctzll(m);
        if (y == b) y = a;
        if (y == x) continue;
        double f = cl_finish(g, y, a, b);
        if (f < 0) return -1.0;
        if (f > start) start = f;
      }
  }
  double c = g->cost[x] + (x == a && b >= 0 ? g->cost[b] : 0.0) + TASK_OVERHEAD;
  g->mark[x] = 2;
  return g->fin[x] = start + c;
}

/* How long the tasks take on g->cores, at least: the longest path, or
 * the work shared out evenly, whichever is more. -1 if merging b into a
 * makes a loop. */
static double cl_bound(Cl* g, int a, int b) {
  double cp = 0.0, work = 0.0;
  memset(g->mark, 0, g->nt);
  for (int x = 0; x < g->nt; x++) {
    if (!g->alive[x] || x == b) continue;
    double f = cl_finish(g, x, a, b);
    if (f < 0) return -1.0;
    if (f > cp) cp = f;
    work += g->cost[x] + TASK_OVERHEAD;
  }
  if (b >= 0) work += g->cost[b];
  work /= g->cores;
  return cp > work ? cp : work;
}

typedef struct { int a, b; double c; } Pair;

static int by_pair_cost(const void* x, const void* y) {
  double d = ((const Pair*)x)->c - ((const Pair*)y)->c;
  return d < 0 ? -1 : d > 0;
}

/* Merge the finest tasks for `cores`, none past the cap. Candidates are a
 * task and one it reads from, or two read by the same task (or by the
 * output), cheapest first; a merge is made when it doesn't lengthen how
 * long the tasks take on that many cores, at least (cl_bound), and
 * makes no loop. A chain merges, as it saves the overhead; small tasks
 * side by side merge while the work outweighs the longest path; big
 * ones side by side stay apart, to run at once. On one core everything
 * merges, up to the cap. Merged units keep the file's order, which is
 * what one task means, so any clustering computes the same bits. */
static void cluster_tasks(Ctx* c, int cores) {
  const double cap = TASK_COST_CAP;
  int nt = c->ntasks, n = c->nunits;
  if (cores < 1 || nt < 2) return;
  int words = (nt + 63) / 64;
  Cl g = {nt, words, cores, al(c, (size_t)nt * words * 8), al(c, nt), al(c, nt * sizeof(double)),
          al(c, nt * sizeof(double)), al(c, nt)};
  for (int i = 0; i < n; i++)
    for (int b = 0; b < NBUCKETS; b++)
      for (Ent* e = c->use[i].reads.b[b]; e; e = e->next) {
        Ent* f = hfind(&c->writers, e->key);
        if (!f) continue;
        int a = c->unit_task[i], w = c->unit_task[((int*)f->p)[0]];
        if (a != w) SET(g.need, a, w);
      }
  int* to = al(c, nt * sizeof(int));     /* each fine task's cluster */
  for (int t = 0; t < nt; t++) { to[t] = t; g.alive[t] = 1; }
  for (int i = 0; i < n; i++) g.cost[c->unit_task[i]] += node_cost(c, c->units[i].st, 0);
  double cur = cl_bound(&g, -1, -1);
  int* preds = al(c, (nt + 1) * sizeof(int));
  char* fed = al(c, nt);
  for (int changed = 1; changed;) {
    changed = 0;
    /* the candidates */
    int np = 0, capp = 64;
    Pair* pr = al(c, capp * sizeof(Pair));
    #define ADD(x, y) do { \
      if (np == capp) { Pair* q = al(c, (capp *= 2) * sizeof(Pair)); memcpy(q, pr, np * sizeof(Pair)); pr = q; } \
      pr[np++] = (Pair){x, y, g.cost[x] + g.cost[y]}; } while (0)
    memset(fed, 0, nt);
    for (int x = 0; x <= nt; x++) {
      if (x < nt && !g.alive[x]) continue;
      int k = 0;
      for (int y = 0; y < nt; y++) {
        if (!g.alive[y]) continue;
        if (x < nt ? BIT(g.need, x, y) : !fed[y]) preds[k++] = y;
        if (x < nt && BIT(g.need, x, y)) { fed[y] = 1; ADD(x, y); }
      }
      /* side by side: each with the next cheapest */
      for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && g.cost[preds[j]] < g.cost[preds[j - 1]]; j--) {
          int t = preds[j]; preds[j] = preds[j - 1]; preds[j - 1] = t;
        }
      for (int i = 0; i + 1 < k; i++) ADD(preds[i], preds[i + 1]);
    }
    #undef ADD
    qsort(pr, np, sizeof(Pair), by_pair_cost);
    for (int q = 0; q < np; q++) {
      int a = pr[q].a, b = pr[q].b;
      if (!g.alive[a] || !g.alive[b] || g.cost[a] + g.cost[b] > cap) continue;
      if (a > b) { int t = a; a = b; b = t; }
      double nb = cl_bound(&g, a, b);
      if (nb < 0 || nb > cur + 1e-9) continue;
      /* a takes in b */
      cur = nb;
      changed = 1;
      g.alive[b] = 0;
      g.cost[a] += g.cost[b];
      for (int x = 0; x < nt; x++) {
        if (BIT(g.need, b, x)) SET(g.need, a, x);
        if (BIT(g.need, x, b)) { SET(g.need, x, a); CLR(g.need, x, b); }
      }
      for (int x = 0; x < words; x++) g.need[(size_t)b * words + x] = 0;
      CLR(g.need, a, a);
      for (int k = 0; k < nt; k++) if (to[k] == b) to[k] = a;
    }
  }
  /* Number the clusters in file order. */
  int* id = al(c, nt * sizeof(int));
  for (int k = 0; k < nt; k++) id[k] = -1;
  c->ntasks = 0;
  for (int i = 0; i < n; i++) {
    int k = to[c->unit_task[i]];
    if (id[k] < 0) id[k] = c->ntasks++;
    c->unit_task[i] = id[k];
  }
}
#undef BIT
#undef SET
#undef CLR

/* How many cores a voice's tasks are planned for: AITHER_TASKS if set
 * (0: none, the finest split), else four per thread the pool runs on. */
static int tasks_wanted(void) {
  const char* e = getenv("AITHER_TASKS");
  /* four times the threads: a schedule with slack (bush_doof_e on two
   * cores: planned for 2, 6.4 s; for 8, 5.1 s; for 16, 5.0 s) */
  return e && *e ? atoi(e) : pool_threads() > 1 ? 4 * pool_threads() : 1;
}

/* What a unit belongs to, for `check`: its play (>= 0, an index into
 * the units' plays by first unit), or -1 for a line of its own. */
static int unit_play(Ctx* c, int u) {
  if (!c->units[u].play) return -1;
  for (int k = 0; k <= u; k++) if (c->units[k].play == c->units[u].play) return k;
  return -1;
}

static int same_owner(Ctx* c, int a, int b) {
  int pa = unit_play(c, a), pb = unit_play(c, b);
  return pa >= 0 ? pa == pb : pb < 0 && a == b;
}

static const char* shown_name(Ctx* c, const char* key) {
  if (has(&c->file_vars, key)) return F(c, "$%s", key);
  const char* dot = strchr(key, '.');
  if (dot) return F(c, "%.*s's %s", (int)(dot - key), key, dot + 1);
  return key;
}

/* Up to four names, then how many more. */
static void name_list(Ctx* c, Buf* o, Strs* v) {
  for (int i = 0; i < v->n && i < 4; i++) badd(c, o, i ? ", %s" : "%s", v->v[i]);
  if (v->n > 4) badd(c, o, " and %d more", v->n - 4);
}

static void push_once(Ctx* c, Strs* v, const char* s) {
  for (int i = 0; i < v->n; i++) if (eq(v->v[i], s)) return;
  spush(c, v, s);
}

/* Why the finest split kept parts together, for `check`: each task of
 * more than one owner (play, or top-level line) that holds a tenth of
 * the voice's estimated cost or more, heaviest first, five at most. A
 * loop is named by the values read from below their writer, the reads
 * that close it. */
static void explain_groups(Ctx* c, Map* arrays) {
  int n = c->nunits, nt = c->ntasks;
  double total = 0.0;
  double* w = al(c, (nt ? nt : 1) * sizeof(double));
  for (int i = 0; i < n; i++) { double k = node_cost(c, c->units[i].st, 0); w[c->unit_task[i]] += k; total += k; }
  char* shown = al(c, nt ? nt : 1);
  for (int round = 0; round < 5; round++) {
    int g = -1;
    for (int t = 0; t < nt; t++)
      if (!shown[t] && (g < 0 || w[t] > w[g])) g = t;
    if (g < 0 || total <= 0 || w[g] < 0.1 * total) break;
    shown[g] = 1;
    /* its owners */
    Strs plays = {0};
    int lo = -1, hi = -1, lines = 0, first = -1, owners = 0;
    for (int i = 0; i < n; i++) {
      if (c->unit_task[i] != g) continue;
      if (first < 0) first = i;
      if (!same_owner(c, first, i)) owners = 1;
      if (c->units[i].play) push_once(c, &plays, c->units[i].play->str);
      else {
        int ln = c->units[i].st->line;
        if (lo < 0 || ln < lo) lo = ln;
        if (ln > hi) hi = ln;
        lines++;
      }
    }
    if (!owners) continue;
    /* why */
    Strs both = {0}, arr = {0}, loop = {0};
    for (int i = 0; i < n; i++) {
      if (c->unit_task[i] != g) continue;
      for (int b = 0; b < NBUCKETS; b++) {
        for (Ent* e = c->use[i].writes.b[b]; e; e = e->next) {
          Ent* f = hfind(&c->writers, e->key);
          int* ws = f->p;
          if (ws[0] != i) continue;
          for (int k = 1; k < f->n; k++)
            if (c->unit_task[ws[k]] == g && !same_owner(c, i, ws[k])) { push_once(c, &both, shown_name(c, e->key)); break; }
        }
        for (Ent* e = c->use[i].reads.b[b]; e; e = e->next) {
          Ent* f = hfind(&c->writers, e->key);
          if (!f) continue;
          int late, wu = pick_writer(f, i, &late);
          if (c->unit_task[wu] != g || same_owner(c, i, wu)) continue;
          if (has(arrays, e->key)) push_once(c, &arr, shown_name(c, e->key));
          else if (late) push_once(c, &loop, shown_name(c, e->key));
        }
      }
    }
    Buf o = {0};
    for (int i = 0; i < plays.n && i < 6; i++) badd(c, &o, i ? ", %s" : "%s", plays.v[i]);
    if (plays.n > 6) badd(c, &o, " and %d more plays", plays.n - 6);
    if (lines) {
      if (plays.n) bcat(c, &o, ", ");
      if (lines == 1) badd(c, &o, "line %d", lo);
      else badd(c, &o, "%d lines from %d to %d", lines, lo, hi);
    }
    char pc[32];
    snprintf(pc, sizeof pc, "%.0f", 100 * w[g] / total);
    badd(c, &o, " (%s%%):", pc);
    const char* sep = " ";
    if (loop.n) { badd(c, &o, "%sa loop through ", sep); name_list(c, &o, &loop); sep = "; "; }
    if (both.n) { badd(c, &o, "%sboth write ", sep); name_list(c, &o, &both); sep = "; "; }
    if (arr.n) { badd(c, &o, "%sreads the array ", sep); name_list(c, &o, &arr); sep = "; "; }
    if (eq(sep, " ")) bcat(c, &o, " read one another");
    spush(c, &c->glue, bstr(&o));
  }
}

/* The units in the order they are emitted, what each reads and writes,
 * and their tasks. Arrays don't cross tasks: a unit reading another's
 * array joins it. */
static void plan_tasks(Ctx* c, Node* program) {
  int n = 0;
  for (int i = 0; i < program->nkids; i++) {
    Node* s = program->kids[i];
    if (s->kind == nkDef || s->kind == nkImport) continue;
    n += s->kind == nkPlay ? play_nst(s) : 1;
  }
  c->nunits = n;
  c->units = al(c, (n ? n : 1) * sizeof(Unit));
  c->use = al(c, (n ? n : 1) * sizeof(Use));
  c->unit_task = al(c, (n ? n : 1) * sizeof(int));
  Map arrays = {{0}, 0};
  collect_idx_targets(c, program, &arrays);
  for (int k = 0; k < c->nvar_decls; k++)
    if (c->var_decls[k]->nkids && array_rhs(c->var_decls[k]->kids[0])) incl(c, &arrays, c->var_decls[k]->str);
  int u = 0;
  for (int i = 0; i < program->nkids; i++) {
    Node* s = program->kids[i];
    if (s->kind == nkDef || s->kind == nkImport) continue;
    if (s->kind == nkLet && array_rhs(s->kids[0])) incl(c, &arrays, s->str);
    if (s->kind != nkPlay) { c->units[u++] = (Unit){s, NULL, 1}; continue; }
    int nst = play_nst(s);
    for (int si = 0; si < nst; si++) {
      Node* st = play_stmt(s, si);
      if (st->kind == nkLet && array_rhs(st->kids[0])) incl(c, &arrays, F(c, "%s.%s", s->str, st->str));
      c->units[u++] = (Unit){st, s, si == nst - 1};
    }
  }
  Use* use = c->use;
  Node* play = NULL;
  Within w = {0};
  for (int i = 0; i < n; i++) {
    Node* s = c->units[i].st;
    Map bound = {{0}, 0}, defs_seen = {{0}, 0};
    if (!c->units[i].play) {
      if (s->kind == nkLet) {
        incl(c, &use[i].writes, s->str);
        uses(c, s->kids[0], &bound, &use[i], &defs_seen, 0, NULL);
      } else {
        uses(c, s, &bound, &use[i], &defs_seen, 0, NULL);
      }
      continue;
    }
    if (c->units[i].play != play) {
      play = c->units[i].play;
      w.play = play->str;
      w.local = al(c, sizeof(Map));
      for (int k = 0; k < play_nst(play); k++)
        if (play_stmt(play, k)->kind == nkLet) incl(c, w.local, play_stmt(play, k)->str);
    }
    if (s->kind == nkLet) {
      incl(c, &use[i].writes, F(c, "%s.%s", w.play, s->str));
      uses(c, s->kids[0], &bound, &use[i], &defs_seen, 0, &w);
    } else {
      if (c->units[i].last) incl(c, &use[i].writes, w.play);
      uses(c, s, &bound, &use[i], &defs_seen, 0, &w);
    }
  }
  /* Each name's writers, in file order; two or more are one task. */
  int* up = al(c, (n ? n : 1) * sizeof(int));
  for (int i = 0; i < n; i++) up[i] = i;
  for (int i = 0; i < n; i++)
    for (int b = 0; b < NBUCKETS; b++)
      for (Ent* e = use[i].writes.b[b]; e; e = e->next) {
        Ent* f = hfind(&c->writers, e->key);
        if (!f) { f = hput(c, &c->writers, e->key); f->p = al(c, (n ? n : 1) * sizeof(int)); }
        int* ws = f->p;
        ws[f->n++] = i;
        task_join(up, ws[0], i);
      }
  /* Arrays stay with their writers. */
  for (int i = 0; i < n; i++)
    for (int b = 0; b < NBUCKETS; b++)
      for (Ent* e = use[i].reads.b[b]; e; e = e->next) {
        Ent* f = hfind(&c->writers, e->key);
        if (f && has(&arrays, e->key)) task_join(up, ((int*)f->p)[0], i);
      }
  /* Needs, then the cycles among them: reachability, n is a few hundred. */
  int words = (n + 63) / 64;
  unsigned long long* reach = al(c, (size_t)(n ? n : 1) * words * 8);
  #define REACH(i, j) (reach[(size_t)(i) * words + (j) / 64] >> ((j) % 64) & 1)
  for (int i = 0; i < n; i++)
    for (int b = 0; b < NBUCKETS; b++)
      for (Ent* e = use[i].reads.b[b]; e; e = e->next) {
        Ent* f = hfind(&c->writers, e->key);
        if (!f) continue;
        int wr = task_root(up, ((int*)f->p)[0]);
        reach[(size_t)i * words + wr / 64] |= 1ull << (wr % 64);
      }
  /* A unit merged into a task needs what the task needs. */
  for (int i = 0; i < n; i++) {
    int r = task_root(up, i);
    if (r != i) for (int k = 0; k < words; k++) reach[(size_t)r * words + k] |= reach[(size_t)i * words + k];
  }
  for (int k = 0; k < n; k++)
    for (int i = 0; i < n; i++)
      if (REACH(i, k))
        for (int x = 0; x < words; x++) reach[(size_t)i * words + x] |= reach[(size_t)k * words + x];
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (REACH(i, j) && REACH(j, i)) task_join(up, i, j);
  #undef REACH
  /* Number the tasks in file order. */
  int* id = al(c, (n ? n : 1) * sizeof(int));
  for (int i = 0; i < n; i++) id[i] = -1;
  c->ntasks = 0;
  for (int i = 0; i < n; i++) { int r = task_root(up, i); if (id[r] < 0) id[r] = c->ntasks++; }
  for (int i = 0; i < n; i++) c->unit_task[i] = id[task_root(up, i)];
  explain_groups(c, &arrays);
  if (getenv("AITHER_GRAPH_REPORT")) graph_report(c);
  c->nfine = c->ntasks;
  c->fine_task = al(c, (n ? n : 1) * sizeof(int));
  memcpy(c->fine_task, c->unit_task, (n ? n : 1) * sizeof(int));
  cluster_tasks(c, tasks_wanted());
  /* The values that cross from one task to another, each from the writer
   * its reader reads. A play's sound crosses in its part's buffers. */
  for (int i = 0; i < n; i++)
    for (int b = 0; b < NBUCKETS; b++)
      for (Ent* e = use[i].reads.b[b]; e; e = e->next) {
        Ent* f = hfind(&c->writers, e->key);
        if (!f || has(&c->play_set, e->key) || c->unit_task[((int*)f->p)[0]] == c->unit_task[i]) continue;
        int late;
        int wu = pick_writer(f, i, &late);
        Ent* x = hput(c, &c->xbufs, F(c, "%s#%d", e->key, wu));
        if (!x->a) { x->n = -1; x->a = ""; x->b = ""; }
      }
}

/* Which task needs which, as pairs, once each. */
static int task_edges_of(Ctx* c, const int* of, int nt, int* out) {
  int ne = 0;
  unsigned char* seen = al(c, (size_t)(nt ? nt : 1) * (nt ? nt : 1));
  for (int i = 0; i < c->nunits; i++)
    for (int b = 0; b < NBUCKETS; b++)
      for (Ent* e = c->use[i].reads.b[b]; e; e = e->next) {
        Ent* f = hfind(&c->writers, e->key);
        if (!f) continue;
        int a = of[i], w = of[((int*)f->p)[0]];
        if (a == w || seen[(size_t)a * nt + w]) continue;
        seen[(size_t)a * nt + w] = 1;
        if (out) { out[2 * ne] = a; out[2 * ne + 1] = w; }
        ne++;
      }
  return ne;
}

static int task_edges(Ctx* c, int* out) { return task_edges_of(c, c->unit_task, c->ntasks, out); }

/* The tables `aither profile` reads: each slot's task in the finest
 * split, and which task needs which, as pairs. */
static void emit_task_tables(Ctx* c, int nslots, Buf* out) {
  bcat(c, out, "const int task_of[] = {");
  for (int i = 0; i < nslots; i++) badd(c, out, i ? ", %d" : "%d", c->fine_task[i]);
  if (!nslots) bcat(c, out, "0");
  bcat(c, out, "};\n");
  badd(c, out, "const long task_count = %d;\n", c->nfine);
  int ne = task_edges_of(c, c->fine_task, c->nfine, NULL);
  int* ed = al(c, (ne ? ne : 1) * 2 * sizeof(int));
  task_edges_of(c, c->fine_task, c->nfine, ed);
  bcat(c, out, "const int task_needs[] = {");
  for (int k = 0; k < ne; k++) badd(c, out, k ? ", %d, %d" : "%d, %d", ed[2 * k], ed[2 * k + 1]);
  if (!ne) bcat(c, out, "0");
  bcat(c, out, "};\n");
  badd(c, out, "const long task_nneeds = %d;\n", ne);
  /* how it runs: each slot's task after clustering, and what each costs */
  bcat(c, out, "const int run_of[] = {");
  for (int i = 0; i < nslots; i++) badd(c, out, i ? ", %d" : "%d", i < c->nunits ? c->unit_task[i] : 0);
  if (!nslots) bcat(c, out, "0");
  badd(c, out, "};\nconst long run_count = %d;\nconst double unit_cost[] = {", c->ntasks);
  for (int i = 0; i < nslots; i++) badd(c, out, i ? ", %.0f" : "%.0f", i < c->nunits ? node_cost(c, c->units[i].st, 0) : 0.0);
  if (!nslots) bcat(c, out, "0");
  bcat(c, out, "};\nconst char* const task_glue[] = {");
  for (int i = 0; i < c->glue.n; i++) badd(c, out, i ? ", \"%s\"" : "\"%s\"", escape_prof(c, c->glue.v[i]));
  if (!c->glue.n) bcat(c, out, "0");
  badd(c, out, "};\nconst long task_glue_count = %d;\n\n", c->glue.n);
}

/* ---- the buffers between tasks ----
 *
 * One array, `aither_B`, a row of `aither_S` samples per buffer: the
 * clock, the input, the output, each part's gain, park flag and sound,
 * then the values that cross. A crossing value's row holds, at i + 1,
 * the value after its writer at sample i, and at 0 the value when the
 * block began (a read from above its writer reads one sample back). */
enum { XB_T, XB_IN_L, XB_IN_R, XB_OUT_L, XB_OUT_R, XB_FIXED };
static int xb_gain(Ctx* c, int part) { (void)c; return XB_FIXED + part; }
static int xb_park(Ctx* c, int part) { return XB_FIXED + c->nplays + part; }
static int xb_part(Ctx* c, int part, int side) { return XB_FIXED + 2 * c->nplays + 2 * part + side; }

static const char* xb(Ctx* c, int row, const char* idx) {
  return F(c, "aither_r%d[%s]", row, idx);
}

/* A pointer to each row `text` reads or writes, made once: TCC doesn't
 * fold row * S + i out of the loop itself. */
static void declare_rows(Ctx* c, const char* text, Buf* out) {
  Map seen = {{0}, 0};
  for (const char* p = strstr(text, "aither_r"); p; p = strstr(p + 1, "aither_r")) {
    const char* q = p + 8;
    if (*q < '0' || *q > '9') continue;
    int row = atoi(q);
    while (*q >= '0' && *q <= '9') q++;
    if (*q != '[') continue;
    const char* k = F(c, "%d", row);
    if (has(&seen, k)) continue;
    incl(c, &seen, k);
    badd(c, out, "  double* const aither_r%d = aither_B + %d * aither_S;\n", row, row);
  }
}

/* `width`: 1 a number, 2 a pair, 4 a pair of pairs. */
static int xbuf(Ctx* c, const char* key, int wu, int width) {
  Ent* e = hfind(&c->xbufs, F(c, "%s#%d", key, wu));
  if (!e) fail(c, "internal: '%s' crosses between tasks without a buffer", key);
  if (e->n < 0) { e->n = c->nbufs; e->wd = width; c->nbufs += width; }
  return (int)e->n;
}

/* The shared name an identifier read here stands for: a play's own let
 * ("play.name"), a file-level name, or NULL for anything local. */
static const char* shared_key(Ctx* c, Scope* sc, const char* s) {
  for (Scope* x = sc; x && x != c->top; x = x->parent)
    if (lfind(x->names, s) || lfind(x->stereo, s) || lfind(x->arrays, s) || lfind(x->larrays, s) ||
        lfind(x->sarrays, s) || lfind(x->numlits, s) || lfind(x->fns, s) || lfind(x->lambdas, s))
      return x == c->play_scope ? F(c, "%s.%s", c->play_name, s) : NULL;
  return shared_name(c, s) ? s : NULL;
}

static int play_index(Ctx* c, const char* name) {
  for (int i = 0; i < c->play_names.n; i++) if (eq(c->play_names.v[i], name)) return i;
  fail(c, "internal: play '%s' read before it is emitted", name);
  return -1;
}

/* A read of `key`, as `v` in its writer's task, from the unit being
 * emitted: unchanged in that task, its buffer from any other. */
static V cross_read(Ctx* c, const char* key, V v) {
  Ent* w = hfind(&c->writers, key);
  if (!w || c->cur_unit < 0) return v;
  int* ws = w->p;
  if (c->unit_task[ws[0]] == c->unit_task[c->cur_unit]) return v;
  if (has(&c->play_set, key)) {
    int part = play_index(c, key);
    return vpair(xb(c, xb_part(c, part, 0), "aither_i"), xb(c, xb_part(c, part, 1), "aither_i"));
  }
  int late, wu = pick_writer(w, c->cur_unit, &late);
  int b = xbuf(c, key, wu, v.w);
  const char* idx = late ? "aither_i" : "aither_i + 1";
  const char* x[4];
  for (int k = 0; k < v.w; k++) x[k] = xb(c, b + k, idx);
  return vlike(v, v.w, x);
}

static V emit_ident_raw(Ctx* c, Scope* sc, Node* n);

/* A play's let read through sin and cos from another task: its turning
 * point's two sides, from the rows its writer stored them in; NULL when
 * it doesn't cross (or has no turning point). */
static Ent* turner_across(Ctx* c, const char* name) {
  const char* key = F(c, "%s.%s", c->play_name, name);
  Ent* w = hfind(&c->writers, key);
  if (!w || c->unit_task[((int*)w->p)[0]] == c->unit_task[c->cur_unit]) return NULL;
  int late, wu = pick_writer(w, c->cur_unit, &late);
  Ent* x = hfind(&c->xbufs, F(c, "%s~turn#%d", key, wu));
  if (!x || x->n < 0) return NULL;
  Ent* t = al(c, sizeof(Ent));
  t->a = xb(c, (int)x->n, "aither_i + 1");
  t->b = xb(c, (int)x->n + 1, "aither_i + 1");
  return t;
}

/* Is `s`, seen from `sc`, a value (a number or a pair) rather than a
 * table, an array or a function? */
static int value_name(Ctx* c, Scope* sc, const char* s) {
  for (Scope* x = sc; x; x = x->parent) {
    if (lfind(x->names, s) || lfind(x->stereo, s)) return 1;
    if (lfind(x->arrays, s) || lfind(x->larrays, s) || lfind(x->sarrays, s) || lfind(x->fns, s) ||
        lfind(x->lambdas, s)) return 0;
  }
  if (has(&c->stereo_lets, s) || has(&c->pair_vars, s)) return 1;
  if (hfind(&c->top_arrays, s) || has(&c->top_larrays, s) || has(&c->top_sarrays, s)) return 0;
  return has(&c->top_lets, s) || has(&c->top_var_set, s) || has(&c->file_vars, s);
}

/* After unit `u`: what it wrote that another task reads, into its rows.
 * `park` gets the stores of its `$state` alone, for a parked play, where
 * the unit doesn't run and the value stands. */
static void store_crossing(Ctx* c, int u, Scope* sc, Buf* out, Buf* park) {
  for (int b = 0; b < NBUCKETS; b++)
    for (Ent* e = c->use[u].writes.b[b]; e; e = e->next) {
      Ent* x = hfind(&c->xbufs, F(c, "%s#%d", e->key, u));
      if (!x) continue;
      const char* dot = strchr(e->key, '.');
      const char* name = dot ? dot + 1 : e->key;
      if (!value_name(c, sc, name)) continue;
      Node id = {0};
      id.kind = nkIdent; id.str = name; id.source = ""; id.line = c->units[u].st->line;
      int was = c->cur_unit;
      c->cur_unit = -1;
      V v = emit_ident_raw(c, sc, &id);
      c->cur_unit = was;
      int row = xbuf(c, e->key, u, v.w);
      x->a = v.l; x->b = v.r; x->c2 = v.l2; x->d2 = v.r2;
      Buf stores = {0};
      for (int k = 0; k < v.w; k++) badd(c, &stores, "  %s = (%s);\n", xb(c, row + k, "aither_i + 1"), v.x[k]);
      const char* st = bstr(&stores);
      bcat(c, out, st);
      if (park && has(&c->file_vars, e->key)) bcat(c, park, st);
      /* a play's let of a turning: its turning point crosses with it */
      Ent* tn = dot && sc == c->play_scope ? hfind(&c->turners, F(c, "%p/%s", (void*)sc, name)) : NULL;
      if (tn) {
        Ent* tx = hput(c, &c->xbufs, F(c, "%s~turn#%d", e->key, u));
        tx->n = c->nbufs; tx->wd = 2; tx->a = ""; tx->b = "";
        c->nbufs += 2;
        badd(c, out, "  %s = (%s); %s = (%s);\n", xb(c, (int)tx->n, "aither_i + 1"), tn->a,
             xb(c, (int)tx->n + 1, "aither_i + 1"), tn->b);
      }
    }
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
      scan_vars(c, s);
    }
  }

  Buf body = {0};
  Strs prof_names = {0};
  Scope* top = push(c, NULL);
  c->top = top;
  c->nplays = c->play_set.n;
  c->cur_unit = -1;
  plan_tasks(c, program);
  c->nbufs = XB_FIXED + 4 * c->nplays;

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

  /* Each unit's code on its own, to be laid into its task's loop. */
  int nu = c->nunits;
  Buf* chunk = al(c, (nu ? nu : 1) * sizeof(Buf));
  Buf* park_store = al(c, (nu ? nu : 1) * sizeof(Buf));
  int* unit_part = al(c, (nu ? nu : 1) * sizeof(int));
  int u = 0;
  #define PROF_LINE(B) do { if (u < MAX_PROF_SLOTS) badd(c, (B), "  if (s->profiling) { double prof_now = n_now(); " \
    "s->prof[%d] += prof_now - prof_last; prof_last = prof_now; }\n", u); } while (0)
  for (int i = 0; i < program->nkids; i++) {
    Node* s = program->kids[i];
    if (i > 0 && program->kids[i - 1]->kind == nkLet) incl(c, &c->lets_done, program->kids[i - 1]->str);
    if (s->kind == nkDef) continue;
    if (s->kind == nkImport)
      fail(c, "import \"%s\" was not resolved — the program was built without import expansion %s",
           s->str, err_loc(c, s));
    const char* src = s->source[0] ? s->source : c->patch_path;
    const char* loc = (s->source[0] && !eq(s->source, c->patch_path))
                        ? F(c, "%s %d", s->source, s->line) : F(c, "line %d", s->line);
    c->scope = stmt_scope(c, s, i == final_idx);
    if (s->kind == nkPlay) {
      int part = c->play_names.n;
      spush(c, &c->play_names, s->str);
      Scope* inner = push(c, top);
      const char* nm = s->str;
      c->play_scope = inner;
      c->play_name = nm;
      int nst = play_nst(s);
      for (int si = 0; si < nst; si++, u++) {
        Node* st = play_stmt(s, si);
        Buf* body = &chunk[u];
        unit_part[u] = part;
        c->cur_unit = u;
        if (src[0] && st->line > 0) badd(c, body, "#line %d \"%s\"\n", st->line, src);
        c->scope = F(c, "play %s/%s", nm, stmt_scope(c, st, si == nst - 1));
        if (si == nst - 1 && st->kind != nkLet && st->kind != nkVar && st->kind != nkAssign && st->kind != nkIdxAssign) {
          V v = emit(c, inner, st, body);
          if (is_quad(v))
            fail(c, "play %s ends in %s, `%s` %s, and a play is heard as two numbers: say which two, its first half, "
                 "or a projection of your own. " QUAD_STANDIN, nm, shape_of(v), written(c, st), err_loc(c, st));
          const char* l = is_pair(v) ? v.l : once(c, body, v.l);
          const char* r = is_pair(v) ? v.r : l;
          badd(c, body, "    p_%s_l = (%s) * %s;\n", nm, l, xb(c, xb_gain(c, part), "aither_i"));
          badd(c, body, "    p_%s_r = (%s) * %s;\n", nm, r, xb(c, xb_gain(c, part), "aither_i"));
        } else switch (st->kind) {
        case nkLet:
          if (has(&c->play_set, st->str))
            fail(c, "'%s' is the name of a play block — choose a different local variable name %s",
                 st->str, err_loc(c, st));
          emit_let(c, inner, st->str, st->kids[0], body, NULL);
          break;
        case nkVar: bcat(c, body, var_init(c, inner, st)); break;
        case nkAssign: file_assign(c, inner, st, body); break;
        case nkIdxAssign: bcat(c, body, emit_idx_assign(c, inner, st)); break;
        default: emit(c, inner, st, body);
        }
        store_crossing(c, u, inner, body, &park_store[u]);
        if (u < MAX_PROF_SLOTS)
          spush(c, &prof_names, si == nst - 1 ? F(c, "play %s (%s)", nm, loc)
                                              : F(c, "play %s/%s (line %d)", nm, stmt_scope(c, st, 0), st->line));
        PROF_LINE(body);
      }
      c->play_scope = NULL;
      c->play_name = NULL;
      continue;
    }
    Buf* body = &chunk[u];
    unit_part[u] = -1;
    c->cur_unit = u;
    if (src[0] && s->line > 0) badd(c, body, "#line %d \"%s\"\n", s->line, src);
    switch (s->kind) {
    case nkVar: bcat(c, body, var_init(c, top, s)); break;
    case nkLet: {
      Node* rhs = s->kids[0];
      int numeric = rhs->kind == nkArr && all_nums(rhs) && !is_value_lit(rhs);
      if ((rhs->kind == nkArr && rhs->nkids != 2 && rhs->nkids > 0 && !numeric) || is_array_make(rhs)) {
        declare_local_array(c, top, s->str, rhs, body, 1);
      } else if (numeric) {
        const char** items = al(c, (rhs->nkids + 1) * sizeof(char*));
        Ent* e = hput(c, &c->top_arrays, s->str);
        e->a = static_table(c, F(c, "arr_%s", s->str), rhs, items);
        e->n = rhs->nkids;
        e->node = rhs;
        if (rhs->nkids == 2) {
          badd(c, body, "  double l_%s_l = %s;\n  double l_%s_r = %s;\n", s->str, items[0], s->str, items[1]);
          incl(c, &c->stereo_lets, s->str);
        }
      } else {
        V v = emit(c, top, rhs, body);
        if ((is_quad(v) && quad_literal(rhs)) || (is_value_lit(rhs) && !is_quad(v) && all_nums(rhs))) {     /* a constant, as a number is */
          Ent* q = hput(c, &c->const_quads, s->str);
          q->node = rhs; q->lad = v.lad;
        }
        if (v.lad) incl(c, &c->lad_lets, s->str);
        if (is_pair(v)) {
          static const char* const suffix[4] = {"l", "r", "l2", "r2"};
          for (int k = 0; k < v.w; k++) badd(c, body, "  double l_%s_%s = (%s);\n", s->str, suffix[k], v.x[k]);
          incl(c, &c->stereo_lets, s->str);
          if (is_quad(v)) incl(c, &c->quad_lets, s->str);
        } else {
          badd(c, body, "  double l_%s = (%s);\n", s->str, v.l);
          double cv;
          if (const_value(c, top, rhs, &cv)) lput(c, &top->numlits, s->str)->v = cv;
          else note_invariant(c, top, s->str, rhs, top);
        }
      }
      break;
    }
    case nkAssign: file_assign(c, top, s, body); break;
    case nkIdxAssign: bcat(c, body, emit_idx_assign(c, top, s)); break;
    default:
      if (i == final_idx) {
        V v = emit(c, top, s, body);
        if (is_quad(v))
          fail(c, "the patch ends in %s, `%s` %s, and the pipe carries two numbers (a speaker's two sides, a scope's "
               "x and y, an IQ stream): say which two, its first half, or a projection of your own. " QUAD_STANDIN,
               shape_of(v), written(c, s), err_loc(c, s));
        const char* l = is_pair(v) ? v.l : once(c, body, v.l);
        badd(c, body, "  %s = (%s); %s = (%s);\n", xb(c, XB_OUT_L, "aither_i"), l,
             xb(c, XB_OUT_R, "aither_i"), is_pair(v) ? v.r : l);
      } else {
        emit(c, top, s, body);
      }
    }
    if (s->kind == nkLet) incl(c, &c->lets_done, s->str);
    store_crossing(c, u, top, body, NULL);
    if (u < MAX_PROF_SLOTS) spush(c, &prof_names, F(c, "%s (%s)", stmt_scope(c, s, i == final_idx), loc));
    PROF_LINE(body);
    u++;
  }
  #undef PROF_LINE
  c->cur_unit = -1;
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
  /* The plays nothing else reads but their sound: skipping one changes
   * nothing else, so --solo can park it rather than run it silent. */
  c->apart = (Strs){0};
  for (int i = 0; i < program->nkids; i++) {
    Node* p = program->kids[i];
    if (p->kind != nkPlay) continue;
    Map w = {{0}, 0};
    for (int k = 0; k < c->nunits; k++)
      if (c->units[k].play == p)
        for (int b = 0; b < NBUCKETS; b++)
          for (Ent* e = c->use[k].writes.b[b]; e; e = e->next)
            if (!eq(e->key, p->str)) incl(c, &w, e->key);
    int read = 0;
    for (int k = 0; k < c->nunits && !read; k++)
      if (c->units[k].play != p)
        for (int b = 0; b < NBUCKETS && !read; b++)
          for (Ent* e = c->use[k].reads.b[b]; e && !read; e = e->next) read = has(&w, e->key);
    if (!read) spush(c, &c->apart, p->str);
  }

  /* The tasks, in an order that runs each after the tasks it needs (the
   * lowest-numbered ready one first, so the file's order where it can). */
  int nt = c->ntasks;
  int ne = task_edges(c, NULL);
  int* ed = al(c, (ne ? ne : 1) * 2 * sizeof(int));
  task_edges(c, ed);
  int* order = al(c, (nt ? nt : 1) * sizeof(int));
  char* placed = al(c, nt ? nt : 1);
  for (int k = 0; k < nt; k++) {
    int pick = -1;
    for (int t = 0; t < nt && pick < 0; t++) {
      if (placed[t]) continue;
      int ready = 1;
      for (int e = 0; e < ne && ready; e++) if (ed[2 * e] == t && !placed[ed[2 * e + 1]]) ready = 0;
      if (ready) pick = t;
    }
    if (pick < 0) fail(c, "internal: the tasks need each other in a ring");
    placed[pick] = 1;
    order[k] = pick;
  }
  for (int t = 0; t < nt; t++) {
    Buf whole = body;
    body = (Buf){0};
    /* Where its `$state` stood when the block began, for reads from above. */
    for (int b = 0; b < NBUCKETS; b++)
      for (Ent* x = c->xbufs.b[b]; x; x = x->next) {
        const char* hash = strrchr(x->key, '#');
        const char* key = F(c, "%.*s", (int)(hash - x->key), x->key);
        if (x->n < 0 || !has(&c->file_vars, key) || c->unit_task[atoi(hash + 1)] != t) continue;
        const char* first[4] = {x->a, x->b, x->c2, x->d2};
        for (int k = 0; k < ew(x); k++) badd(c, &body, "  %s = (%s);\n", xb(c, (int)x->n + k, "0"), first[k]);
      }
    Buf pro = body;
    body = (Buf){0};
    for (int k = 0; k < nu; ) {
      if (c->unit_task[k] != t) { k++; continue; }
      int part = unit_part[k];
      if (part < 0) { bcat(c, &body, bstr(&chunk[k])); k++; continue; }
      /* A run of one play's units: skipped while the play is parked. */
      Node* play = c->units[k].play;
      const char* nm = play->str;
      int end = k, has_last = 0;
      while (end < nu && c->units[end].play == play) {
        if (c->unit_task[end] == t && c->units[end].last) has_last = 1;
        end++;
      }
      if (has_last) badd(c, &body, "  double p_%s_l, p_%s_r;\n", nm, nm);
      badd(c, &body, "  if (%s != 0.0) {\n", xb(c, xb_park(c, part), "aither_i"));
      if (has_last) badd(c, &body, "    p_%s_l = 0.0; p_%s_r = 0.0;\n", nm, nm);
      for (int j = k; j < end; j++) if (c->unit_task[j] == t) bcat(c, &body, bstr(&park_store[j]));
      bcat(c, &body, "  } else {\n");
      for (int j = k; j < end; j++) if (c->unit_task[j] == t) bcat(c, &body, bstr(&chunk[j]));
      bcat(c, &body, "  }\n");
      if (has_last)
        badd(c, &body, "  %s = p_%s_l; %s = p_%s_r;\n", xb(c, xb_part(c, part, 0), "aither_i"), nm,
             xb(c, xb_part(c, part, 1), "aither_i"), nm);
      k = end;
    }
    /* The loop's head: the clock and the input where its units read them. */
    Buf loop = body, head = {0};
    const char* lt = bstr(&loop);
    /* The clock is the step count, a whole number; t is it over the rate,
     * the same division a render's sample i always had (i / sr). */
    int uses_t = strstr(lt, "aither_t") != NULL;
    if (uses_t || strstr(lt, "aither_step"))
      badd(c, &head, "  const double aither_step = %s; (void)aither_step;\n", xb(c, XB_T, "aither_i"));
    if (uses_t) bcat(c, &head, "  const double aither_t = aither_step / s->sr; (void)aither_t;\n");
    if (strstr(lt, "aither_in_"))
      badd(c, &head, "  const double aither_in_l = %s, aither_in_r = %s; (void)aither_in_l; (void)aither_in_r;\n",
           xb(c, XB_IN_L, "aither_i"), xb(c, XB_IN_R, "aither_i"));
    body = whole;
    badd(c, &body, "/*@@task@@*/\nAITHER_TASKFN void task_%d(VoiceState* s, long aither_n, long aither_S, double* aither_B) {\n", t);
    bcat(c, &body, "  double prof_last = 0.0; (void)prof_last; (void)aither_B; (void)aither_S;\n");
    declare_rows(c, F(c, "%s%s%s", bstr(&pro), bstr(&head), lt), &body);
    bcat(c, &body, bstr(&pro));
    bcat(c, &body, "  for (long aither_i = 0; aither_i < aither_n; aither_i++) {\n");
    bcat(c, &body, bstr(&head));
    if (strstr(lt, "prof_last")) bcat(c, &body, "  if (s->profiling) prof_last = n_now();\n");
    bcat(c, &body, lt);
    bcat(c, &body, "  }\n}\n\n");
  }
  /* Where a parallel -O2 build cuts the file (voice.c): the tasks above,
   * each its own; what follows, with the prelude, one more. */
  bcat(c, &body, "/*@@tail@@*/\n");
  for (int t = 0; t < nt; t++) badd(c, &body, "AITHER_TASKFN void task_%d(VoiceState*, long, long, double*);\n", t);
  bcat(c, &body, "static void run_tasks(VoiceState* s, long aither_n, long aither_S, double* aither_B) {\n");
  for (int k = 0; k < nt; k++) badd(c, &body, "  task_%d(s, aither_n, aither_S, aither_B);\n", order[k]);
  bcat(c, &body, "}\n\n");
  /* The same tasks for the pool: each one's function, how many it waits
   * for, and the ones it feeds. */
  bcat(c, &body, "static void* const aither_task_fns[] = {");
  for (int t = 0; t < nt; t++) badd(c, &body, t ? ", (void*)task_%d" : "(void*)task_%d", t);
  bcat(c, &body, "};\nstatic const int aither_task_waits[] = {");
  for (int t = 0; t < nt; t++) {
    int w = 0;
    for (int e = 0; e < ne; e++) if (ed[2 * e] == t) w++;
    badd(c, &body, t ? ", %d" : "%d", w);
  }
  bcat(c, &body, "};\nstatic const int aither_task_feeds_at[] = {0");
  {
    int at = 0;
    for (int t = 0; t < nt; t++) {
      for (int e = 0; e < ne; e++) if (ed[2 * e + 1] == t) at++;
      badd(c, &body, ", %d", at);
    }
  }
  bcat(c, &body, "};\nstatic const int aither_task_feeds[] = {");
  {
    int any = 0;
    for (int t = 0; t < nt; t++)
      for (int e = 0; e < ne; e++)
        if (ed[2 * e + 1] == t) badd(c, &body, any++ ? ", %d" : "%d", ed[2 * e]);
    if (!any) bcat(c, &body, "0");
  }
  badd(c, &body, "};\n#define AITHER_NT %d\n\n", nt);
  /* The voice's sound at sample i: the final expression's, or with none
   * the plays', added in the file's order. */
  bcat(c, &body, "static void outs(long aither_S, double* aither_B, long aither_i, double* outL, double* outR) {\n");
  Buf whole = body;
  body = (Buf){0};
  if (final_idx >= 0) {
    badd(c, &body, "  *outL = %s; *outR = %s;\n", xb(c, XB_OUT_L, "aither_i"), xb(c, XB_OUT_R, "aither_i"));
  } else if (c->play_names.n > 0) {
    bcat(c, &body, "  *outL = ");
    for (int i = 0; i < c->play_names.n; i++) badd(c, &body, i ? " + %s" : "%s", xb(c, xb_part(c, i, 0), "aither_i"));
    bcat(c, &body, ";\n  *outR = ");
    for (int i = 0; i < c->play_names.n; i++) badd(c, &body, i ? " + %s" : "%s", xb(c, xb_part(c, i, 1), "aither_i"));
    bcat(c, &body, ";\n");
  } else {
    bcat(c, &body, "  (void)aither_S; (void)aither_B; (void)aither_i;\n  *outL = 0.0; *outR = 0.0;\n");
  }
  {
    Buf o = body;
    body = whole;
    declare_rows(c, bstr(&o), &body);
    bcat(c, &body, bstr(&o));
  }
  bcat(c, &body, "}\n\n");

  bcat(c, &body, "const char* const prof_names[] = {");
  for (int i = 0; i < prof_names.n; i++) badd(c, &body, i ? ", \"%s\"" : "\"%s\"", escape_prof(c, prof_names.v[i]));
  if (prof_names.n == 0) bcat(c, &body, "0");
  bcat(c, &body, "};\n");
  badd(c, &body, "const long prof_count = %d;\n\n", prof_names.n);
  emit_task_tables(c, prof_names.n, &body);
  badd(c, &body, "#define AITHER_NB %d\n#define AITHER_NP %d\n#define AITHER_MAXN 128\n", c->nbufs, c->nplays);
  badd(c, &body, "#define XB_T %d\n#define XB_IN_L %d\n#define XB_IN_R %d\n#define XB_GAIN %d\n#define XB_PARK %d\n#define XB_PART %d\n",
       XB_T, XB_IN_L, XB_IN_R, XB_FIXED, XB_FIXED + c->nplays, XB_FIXED + 2 * c->nplays);
  bcat(c, &body, BLOCK_TEMPLATE);

  /* The prelude, now that the state's shape is known. */
  Buf pre = {0};
  bcat(c, &pre, "#include <math.h>\n");
  /* One file: the block's hoisted values and the tasks are its own. A
   * parallel -O2 build (voice.c) defines these to share them between
   * files: the values once, the tasks callable from the tail. */
  bcat(c, &pre, "#ifndef AITHER_SHARED\n#define AITHER_SHARED static\n#endif\n"
                "#ifndef AITHER_TASKFN\n#define AITHER_TASKFN static\n#endif\n");
  bcat(c, &pre, "typedef struct DspState DspState;\n");
  for (int i = 0; dsp_natives[i].name; i++) {
    const NativeSpec* sp = &dsp_natives[i];
    if (eq(sp->name, "wave")) continue;
    badd(c, &pre, "extern double n_%s(DspState*,long", sp->name);
    for (int k = 0; k < native_arity(sp); k++) bcat(c, &pre, ",double");
    bcat(c, &pre, ");\n");
  }
  bcat(c, &pre,
    "extern double n_lp1_coef(double,double);\n"
    "extern double n_medium_turn(double*,double,double,double,double,double,double,double,double,double);\n"
    "extern double n_medium_kick(double*,double,double);\n"
    "extern double n_medium_at(double*,double);\n"
    "extern double n_medium_velocity_at(double*,double);\n"
    /* The turning step: TCC calls dsp.c's, compiled at -O2; the -O2 build
     * inlines the same code (dsp.c's turn_step, kept identical), so the
     * two give the same bits. */
    "#ifdef __TINYC__\n"
    "extern void n_turn(double*,double,double);\n"
    "extern void n_turn_clock(double*,double,double);\n"
    "#else\n"
    "static inline void turn_step(double* p, double cs, double sn) {\n"
    "  double r = p[0] * cs - p[1] * sn;\n"
    "  double i = p[0] * sn + p[1] * cs;\n"
    "  double g = 1.5 - 0.5 * (r * r + i * i);\n"
    "  p[0] = r * g; p[1] = i * g;\n"
    "}\n"
    "static inline void n_turn(double* p, double cs, double sn) {\n"
    "  if (p[2] == 0.0) { p[0] = 1.0; p[1] = 0.0; p[2] = 1.0; }\n"
    "  turn_step(p, cs, sn);\n"
    "}\n"
    "static inline void n_turn_clock(double* p, double cs, double sn) { turn_step(p, cs, sn); p[2] = p[2] - 1.0; }\n"
    "#endif\n"
    "extern double n_lp1_c(DspState*,long,double,double);\n"
    "extern double n_hp1_c(DspState*,long,double,double);\n"
    "extern void n_svf_coef(double,double,double,double*);\n"
    "extern double n_lpf_c(DspState*,long,double,const double*);\n"
    "extern double n_hpf_c(DspState*,long,double,const double*);\n"
    "extern double n_bpf_c(DspState*,long,double,const double*);\n"
    "extern double n_notch_c(DspState*,long,double,const double*);\n"
    "extern void n_reverb_coef(double,double,double*);\n"
    "extern double n_reverb_c(DspState*,long,double,double,const double*);\n"
    "extern double n_wave(DspState*,long,double,double*,int);\n"
    "extern double n_hilbert_a(DspState*,long,double);\n"
    "extern double n_hilbert_b(DspState*,long,double);\n"
    "extern double shape_saw(double);\n"
    "extern double shape_tri(double);\n"
    "extern double shape_sqr(double);\n"
    "extern double n_midi_cc(int);\n"
    "extern double n_midi_note(int);\n"
    "extern double n_midi_freq(void);\n"
    "extern double n_midi_gate(void);\n"
    "extern double n_midi_trig(DspState*,long,int);\n"
    "extern double n_midi_voice_freq(int);\n"
    "extern double n_midi_voice_gate(int);\n"
    "extern double n_midi_voice_vel(int);\n"
    "extern double n_midi_voice_key(int);\n"
    "extern double n_midi_voice_cc(int,int);\n"
    "extern double n_midi_voice_press(int);\n"
    "extern double n_midi_voice_bend(int);\n"
    "extern double n_midi_voice_on(int);\n"
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
    "  double step, dt, sweep_origin;\n"
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
    "  double  peak_decay, rms_alpha;\n"
    "  double  peak_raw;\n"
    "  long    bad;\n"
    "  long    n_parts;\n"
    "  double  deadline;\n"
    "  long    stuck;\n"
    "  const char* stuck_at;\n"
    "  double* prof;\n"
    "  long    profiling;\n"
    "  long    prof_ticks;\n"
    "  double* raw_out;\n"
    "  double* part_buf;\n"
    "  int (*par)(void*, long, void* const*, const int*, const int*, const int*, long, long, double*);\n");
  for (int i = 0; i < c->top_var_names.n; i++) badd(c, &pre, "  double v_%s;\n", c->top_var_names.v[i]);
  badd(c, &pre, "  unsigned char inited[%d];\n", c->top_var_names.n > 1 ? c->top_var_names.n : 1);
  bcat(c, &pre, "} VoiceState;\n\n");

  Buf hoistc = {0};
  for (int i = 0; i < c->hoist_decls.n; i++) bcat(c, &hoistc, c->hoist_decls.v[i]);
  bcat(c, &hoistc, "/*@@cut@@*/\n");
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
  if (eq(name, "log")) return "x,base=2.718281828459045";
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

/* ---- one name, one meaning: state and a parameter or let can't share ----
 *
 * To the compiler `$x` and `x` are one name (the `$` is for the reader),
 * so a def's parameter `on` and its state `$on` were one value, silently,
 * and a top-level `let x` took over `$x`'s reads. Now it is an error, at
 * compile time: in a live set a send that fails changes nothing and the
 * version playing plays on, so the cost is a rename.
 *
 * Frames: the top level (with every play's state, which is file-level by
 * name; a play's lets are its own frame), each def (a boundary: a def
 * sees its arguments, not the file's state) and each lambda (which sees
 * the frames it is written in). A name bound in a frame (a parameter or
 * a let) clashes with state of that name in the same frame, or in one it
 * is written in, up to the def or top level that bounds them: there `$x`
 * meant the outer state and would read the inner name. State declared
 * inside a frame with a let of its name outside is shadowing, as any
 * language has it: inside, `$x` and `x` both mean the inner state. */
typedef struct NameFrame NameFrame;
struct NameFrame {
  NameFrame* up;
  int boundary;                           /* a def, or the top level */
  const char** bound; Node** bnode; int nb, cb;
  const char** state; Node** snode; int ns, cs;
};

static void frame_add(Ctx* c, const char*** v, Node*** nodes, int* n, int* cap, const char* name, Node* at) {
  if (!name || !*name) return;
  if (*n == *cap) {
    int nc = *cap ? *cap * 2 : 8;
    const char** nv = al(c, nc * sizeof(char*));
    Node** nn = al(c, nc * sizeof(Node*));
    if (*n) { memcpy(nv, *v, *n * sizeof(char*)); memcpy(nn, *nodes, *n * sizeof(Node*)); }
    *v = nv; *nodes = nn; *cap = nc;
  }
  (*v)[*n] = name; (*nodes)[*n] = at; (*n)++;
}

static NameFrame** name_frames;
static int n_name_frames, cap_name_frames;

static NameFrame* new_frame(Ctx* c, NameFrame* up, int boundary) {
  NameFrame* f = al(c, sizeof(NameFrame));
  memset(f, 0, sizeof *f);
  f->up = up;
  f->boundary = boundary;
  if (n_name_frames == cap_name_frames) {
    cap_name_frames = cap_name_frames ? cap_name_frames * 2 : 64;
    NameFrame** nf = al(c, cap_name_frames * sizeof(NameFrame*));
    if (n_name_frames) memcpy(nf, name_frames, n_name_frames * sizeof(NameFrame*));
    name_frames = nf;
  }
  name_frames[n_name_frames++] = f;
  return f;
}

static void gather_names(Ctx* c, Node* n, NameFrame* f, NameFrame* top) {
  if (!n) return;
  switch (n->kind) {
  case nkDef: {
    NameFrame* d = new_frame(c, f, 1);
    for (int i = 0; i < n->nparams; i++) frame_add(c, &d->bound, &d->bnode, &d->nb, &d->cb, n->params[i], n);
    for (int i = 0; i < n->nkids; i++) gather_names(c, n->kids[i], d, top);
    return;
  }
  case nkLambda: {
    NameFrame* l = new_frame(c, f, 0);
    for (int i = 0; i < n->nparams; i++) frame_add(c, &l->bound, &l->bnode, &l->nb, &l->cb, n->params[i], n);
    for (int i = 0; i < n->nkids; i++) gather_names(c, n->kids[i], l, top);
    return;
  }
  case nkPlay: {
    NameFrame* pl = new_frame(c, f, 0);
    pl->ns = -1;                          /* its state is the top level's: marks a play frame */
    for (int i = 0; i < n->nkids; i++) gather_names(c, n->kids[i], pl, top);
    pl->ns = 0;
    return;
  }
  case nkLet:
    frame_add(c, &f->bound, &f->bnode, &f->nb, &f->cb, n->str, n);
    break;
  case nkVar: {
    /* A state array is read by index, through the arrays, never as a
     * plain name: `let z = $z[k]`, this element of it, is the idiom. */
    if (n->nkids > 0 && is_array_make(n->kids[0])) break;
    /* A play's state is the file's, by name. */
    NameFrame* owner = f;
    for (NameFrame* x = f; x; x = x->up) {
      if (x->boundary) break;
      if (x->ns == -1) { owner = top; break; }
    }
    if (owner->ns == -1) owner = top;
    frame_add(c, &owner->state, &owner->snode, &owner->ns, &owner->cs, n->str, n);
    break;
  }
  default: break;
  }
  for (int i = 0; i < n->nkids; i++) gather_names(c, n->kids[i], f, top);
}

static void clash(Ctx* c, const char* name, Node* bind, Node* st) {
  int is_param = bind->kind == nkDef || bind->kind == nkLambda;
  fail(c, "'%s' is both %s %s and the state $%s %s: to the compiler $%s and %s are one name, so they "
       "would be one value. Rename one", name, is_param ? "a parameter" : "a let", err_loc(c, bind), name,
       err_loc(c, st), name, name);
}

static void check_state_names(Ctx* c, Node* program) {
  name_frames = NULL;
  n_name_frames = cap_name_frames = 0;
  NameFrame* top = new_frame(c, NULL, 1);
  gather_names(c, program, top, top);
  for (int k = 0; k < n_name_frames; k++) {
    NameFrame* f = name_frames[k];
    for (NameFrame* g = f; g; g = g->up) {
      for (int i = 0; i < f->nb; i++)
        for (int j = 0; j < g->ns; j++)
          if (eq(f->bound[i], g->state[j])) clash(c, f->bound[i], f->bnode[i], g->snode[j]);
      /* State inside, a parameter or let outside, is plain shadowing:
       * there `$x` and `x` both mean the inner state, as written. */
      if (g == f)
        for (int i = 0; i < f->ns; i++)
          for (int j = 0; j < g->nb; j++)
            if (eq(f->state[i], g->bound[j])) clash(c, f->state[i], g->bnode[j], f->snode[i]);
      if (g->boundary) break;
    }
  }
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
  check_state_names(c, program);
  normalize_calls(c, program, NULL);
  mark_recursion(c);
  const char* src = emit_program(c, program);
  src = finalise_layout(c, src);
  out->csrc = src;
  out->var_names = c->top_var_names.v; out->nvars = c->top_var_names.n;
  out->part_names = c->play_names.v; out->nparts = c->play_names.n;
  out->unheard = c->unheard.v; out->nunheard = c->unheard.n;
  out->apart = c->apart.v; out->napart = c->apart.n;
  out->regions = c->regions; out->nregions = c->nregions;
  free(c);
  return 0;
}
